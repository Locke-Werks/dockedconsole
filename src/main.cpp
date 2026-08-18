// Docked Console, a terminal docked to the edge of the Windows desktop.
// Copyright (C) 2026 Locke Werks
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
#include <windows.h>

#include "app_bar.h"
#include "autostart.h"
#include "config.h"
#include "diag_log.h"
#include "dock_window.h"
#include "elevation.h"
#include "ids.h"
#include "parent_console.h"
#include "taskbar_claim.h"
#include "win32.h"

#include <shellapi.h>

#include <string>
#include <vector>

namespace dock {
namespace {

std::vector<std::wstring> CommandLineArgs()
{
    std::vector<std::wstring> args;
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw) {
        return args;
    }
    // Skip argv[0].
    for (int i = 1; i < count; ++i) {
        args.emplace_back(raw[i]);
    }
    LocalFree(raw);
    return args;
}

bool HasFlag(const std::vector<std::wstring>& args, std::wstring_view flag)
{
    for (const auto& arg : args) {
        if (EqualsNoCase(arg, flag)) {
            return true;
        }
    }
    return false;
}

/// One-shot overrides. Deliberately not persisted, so --width is safe for trying
/// sizes without editing the config back afterwards.
void ApplyOverrides(Config& cfg, const std::vector<std::wstring>& args)
{
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (EqualsNoCase(args[i], L"--width")) {
            const int width = _wtoi(args[i + 1].c_str());
            if (width > 0) {
                cfg.width_physical_px = width;
            }
        } else if (EqualsNoCase(args[i], L"--edge")) {
            cfg.edge = args[i + 1];
        }
    }
    cfg.Clamp();
}

void ShowUsage()
{
    console::Report(
        L"dockedconsole.exe            Dock the console and reserve the strip.\r\n"
        L"dockedconsole.exe --stop     Undock and exit the running instance.\r\n"
        L"dockedconsole.exe --reclaim  Recover desktop space left reserved by a hard kill.\r\n"
        L"\r\n"
        L"Overrides, not saved to the config file:\r\n"
        L"  --width <physical px>\r\n"
        L"  --edge <left|top|right|bottom>\r\n"
        L"\r\n"
        L"Settings live in dockedconsole.json next to this executable, or in\r\n"
        L"%LOCALAPPDATA%\\DockedConsole when there is none there.\r\n"
        L"\r\n"
        L"This is a GUI executable, so PowerShell does not wait for it. If you need the\r\n"
        L"exit code, use: Start-Process -Wait dockedconsole.exe -ArgumentList --stop");
    console::Detach();
}

int SignalStop()
{
    Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName));
    if (!event) {
        // A dock running elevated labels its named objects High, and a
        // medium-integrity caller cannot get write access to them. That reads
        // the same as "not running" from here and always has.
        console::Report(L"Docked Console is not running, or is running elevated "
                        L"and cannot be signalled from here. Use the tray icon.",
                        /*error=*/true);
        console::Detach();
        return 1;
    }

    SetEvent(event.Get());
    console::Report(L"Docked Console undocked.");
    console::Detach();
    return 0;
}

int Reclaim()
{
    // Two jobs, because there are two things a hard kill can leave behind.
    //
    // A leaked AppBar registration shrinks the desktop, and the shell prunes
    // registrations whose window is gone only during a work-area recompute.
    // A shortened taskbar is worse: it does not self-heal at all, and there is
    // nothing on screen to explain it. That one is restored from geometry alone,
    // with no state from the process that moved it, so this works even after a
    // crash of a build that kept no records.
    std::wstring report;
    TaskbarClaim::RestoreAllFromGeometry(report);

    AppBar::ForceWorkAreaRecompute();

    console::Report(report + L"Forced a desktop work-area recompute.");
    console::Detach();
    return 0;
}

enum class InstanceLock { Acquired, AlreadyRunning };

InstanceLock AcquireInstanceLock(Handle& out, bool wait_for_predecessor)
{
    const ULONGLONG deadline =
        GetTickCount64() + (wait_for_predecessor ? 6000ULL : 0ULL);

    for (;;) {
        HANDLE raw = CreateMutexW(nullptr, TRUE, kMutexName);
        if (raw) {
            // CreateMutexW hands back a valid handle without ownership when the
            // object already exists, so this check has to follow a success.
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                out.Reset(raw);
                return InstanceLock::Acquired;
            }
            CloseHandle(raw);
        } else if (GetLastError() != ERROR_ACCESS_DENIED) {
            // Anything other than access-denied is a real failure, but the safe
            // reading is still "somebody else has it".
        }
        // ERROR_ACCESS_DENIED means an elevated instance owns the mutex and will
        // not let us open it. Still running.

        if (GetTickCount64() >= deadline) {
            return InstanceLock::AlreadyRunning;
        }
        Sleep(150);
    }
}

int RunDock(HINSTANCE instance, const std::vector<std::wstring>& args)
{
    // An elevated relaunch waits, because the instance that spawned it is still
    // exiting and still owns the mutex for a moment.
    Handle instance_lock;
    if (AcquireInstanceLock(instance_lock, HasFlag(args, kRelaunchFlag))
        == InstanceLock::AlreadyRunning) {
        MessageBoxW(nullptr,
                    L"Docked Console is already running. Use the tray icon, or run "
                    L"dockedconsole.exe --stop.",
                    L"Docked Console", MB_OK | MB_ICONINFORMATION);
        return 2;
    }

    ConfigStatus status{};
    std::wstring detail;
    Config cfg = Config::Load(status, detail);

    if (status == ConfigStatus::Missing) {
        // Only ever write the file when there was not one. An unreadable config
        // is left exactly as the user typed it.
        cfg.Save();
    }

    ApplyOverrides(cfg, args);

    if (ShouldElevate(cfg)) {
        // We are the product of a relaunch and still not elevated, so elevating
        // again would spawn another copy that reaches this same line: an endless
        // chain of processes and, with elevation "always", an endless chain of
        // UAC prompts. That happens when runas succeeds but the resulting token
        // is not actually elevated, which policy can arrange.
        if (HasFlag(args, kRelaunchFlag)) {
            log::Write(L"elevation: relaunched but still not elevated; not trying again");
            MessageBoxW(nullptr,
                        L"Docked Console tried to restart with administrator rights but "
                        L"came back without them, so it has stopped rather than trying "
                        L"again.\n\n"
                        L"Set \"elevation\" to \"never\" in dockedconsole.json to run "
                        L"without elevating.",
                        L"Docked Console", MB_OK | MB_ICONWARNING);
            return 3;
        }

        // Hand the lock over before spawning, or the elevated copy races the
        // instance that started it and loses.
        instance_lock.Reset();

        std::wstring message;
        if (Relaunch(args, message)) {
            return 0;
        }
        MessageBoxW(nullptr, message.c_str(), L"Docked Console", MB_OK | MB_ICONWARNING);
        return 3;
    }

    if (BlockedByPolicy(cfg)) {
        MessageBoxW(nullptr,
                    L"An elevated Windows Terminal is running, so any new terminal "
                    L"window belongs to it and an unelevated Docked Console cannot "
                    L"take it over.\n\n"
                    L"Set \"elevation\" to \"auto\" in dockedconsole.json, run Docked "
                    L"Console as administrator, or close the elevated terminal windows.",
                    L"Docked Console", MB_OK | MB_ICONWARNING);
        return 3;
    }

    // Self-heal a previous hard kill before claiming anything of our own. The
    // shell only prunes registrations whose window is gone during a work-area
    // recompute, and this provokes one. Costs about 120ms, once.
    AppBar::ForceWorkAreaRecompute();

    if (status == ConfigStatus::Unreadable) {
        log::Writef(L"config: %s", detail.c_str());
    }

    DockWindow dock(std::move(cfg));
    if (!dock.Create(instance)) {
        MessageBoxW(nullptr,
                    L"Could not register the AppBar with the shell. The dock would not "
                    L"reserve any desktop space, so it is not starting.",
                    L"Docked Console", MB_OK | MB_ICONERROR);
        return 4;
    }

    return dock.Run();
}

} // namespace
} // namespace dock

int APIENTRY wWinMain(_In_ HINSTANCE instance,
                      _In_opt_ HINSTANCE previous,
                      _In_ LPWSTR command_line,
                      _In_ int show)
{
    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command_line);
    UNREFERENCED_PARAMETER(show);

    // First call in the process, before anything can load a DLL on our behalf.
    // /DEPENDENTLOADFLAG covers implicit imports; this covers the rest, which
    // matters because people run this from a Downloads folder in portable mode.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32
                             | LOAD_LIBRARY_SEARCH_USER_DIRS
                             | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    // What [STAThread] was buying in the C# version. ShellExecuteExW wants it.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const std::vector<std::wstring> args = dock::CommandLineArgs();

    int exit_code = 0;
    if (dock::HasFlag(args, L"--help") || dock::HasFlag(args, L"-h")
        || dock::HasFlag(args, L"/?")) {
        dock::ShowUsage();
    } else if (dock::HasFlag(args, L"--stop")) {
        exit_code = dock::SignalStop();
    } else if (dock::HasFlag(args, L"--reclaim")) {
        exit_code = dock::Reclaim();
    } else if (dock::HasFlag(args, L"--register-autostart")) {
        // Invoked by the installer through a hook declared as = "user", so it
        // must never put a dialog on screen: there is nobody to dismiss it and
        // the hook would fail on its timeout. console::Write is the silent one.
        std::wstring message;
        const bool ok = dock::autostart::Register(message);
        dock::console::Write(message, !ok);
        dock::console::Detach();
        exit_code = ok ? 0 : 1;
    } else if (dock::HasFlag(args, L"--unregister-autostart")) {
        std::wstring message;
        const bool ok = dock::autostart::Unregister(message);
        dock::console::Write(message, !ok);
        dock::console::Detach();
        exit_code = ok ? 0 : 1;
    } else {
        exit_code = dock::RunDock(instance, args);
    }

    CoUninitialize();
    return exit_code;
}
