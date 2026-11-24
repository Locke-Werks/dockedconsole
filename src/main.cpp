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
#include "jump_list.h"
#include "parent_console.h"
#include "taskbar_claim.h"
#include "win32.h"
#include "wt_settings.h"

#include <shellapi.h>

#include <cstring>
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

/// What --pane-dir and --pane-run asked for, if either was given.
struct PaneArgs {
    std::wstring cwd;
    std::wstring command;
    std::wstring profile;
    bool present = false;
};

PaneArgs ReadPaneArgs(const std::vector<std::wstring>& args)
{
    PaneArgs out;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (EqualsNoCase(args[i], L"--pane-dir")) {
            out.cwd = args[i + 1];
            out.present = true;
        } else if (EqualsNoCase(args[i], L"--pane-run")) {
            out.command = args[i + 1];
            out.present = true;
        } else if (EqualsNoCase(args[i], L"--pane-profile")) {
            out.profile = args[i + 1];
            out.present = true;
        }
    }
    return out;
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

    const PaneArgs pane = ReadPaneArgs(args);
    cfg.initial_pane_cwd = pane.cwd;
    cfg.initial_pane_command = pane.command;
    cfg.initial_pane_profile = pane.profile;

    cfg.Clamp();
}

void ShowUsage()
{
    console::Report(
        L"dockedconsole.exe            Dock the console and reserve the strip. Run it\r\n"
        L"                             again while docked to add a column, up to three.\r\n"
        L"dockedconsole.exe --stop     Undock and exit the running instance.\r\n"
        L"dockedconsole.exe --reclaim  Recover desktop space left reserved by a hard kill.\r\n"
        L"dockedconsole.exe --explain-elevation\r\n"
        L"                             Say whether the dock would elevate, and why.\r\n"
        L"\r\n"
        L"Overrides, not saved to the config file, and read only by the first instance:\r\n"
        L"  --width <physical px>\r\n"
        L"  --edge <left|top|right|bottom>\r\n"
        L"\r\n"
        L"A pane, run in the first column when this starts the dock, or asked of the\r\n"
        L"dock already running: a column with room takes a split, a full one gets a\r\n"
        L"new column. Exit 0 added, 5 refused, 6 window naming is off, 2 no answer.\r\n"
        L"  --pane-dir <path>          Open it in this directory.\r\n"
        L"  --pane-run <command line>  Run this instead of a shell.\r\n"
        L"  --pane-profile <name>      Use this Windows Terminal profile. The taskbar\r\n"
        L"                             jump list lists one entry per profile.\r\n"
        L"\r\n"
        L"Settings live in dockedconsole.json next to this executable, or in\r\n"
        L"%LOCALAPPDATA%\\DockedConsole when there is none there.\r\n"
        L"\r\n"
        L"This is a GUI executable, so PowerShell does not wait for it. If you need the\r\n"
        L"exit code, use: Start-Process -Wait dockedconsole.exe -ArgumentList --stop");
    console::Detach();
}

/// Prints the elevation decision the dock would make right now, and the facts
/// it rests on, without starting anything.
int ExplainElevation()
{
    ConfigStatus status{};
    std::wstring detail;
    const Config cfg = Config::Load(status, detail);

    const ProfileElevation column = PredictProfileElevation(cfg, ProfileTarget::Column);

    std::wstring report;
    report += L"wt settings   "
              + (column.settings_path.empty() ? std::wstring(L"(none found)")
                                              : column.settings_path)
              + L"\r\n";
    report += L"column        " + DescribeProfileElevation(column) + L"\r\n";
    if (!cfg.terminal_profile.empty()) {
        const ProfileElevation pane = PredictProfileElevation(cfg, ProfileTarget::Pane);
        report += L"pane          " + DescribeProfileElevation(pane) + L"\r\n";
    }

    DWORD elevated_pid = 0;
    report += L"terminals     ";
    report += AnyElevatedTerminalRunning(&elevated_pid)
                  ? L"an elevated Windows Terminal is running (pid "
                        + std::to_wstring(elevated_pid) + L")"
                  : std::wstring(L"no elevated Windows Terminal is running");
    report += L"\r\n";

    report += L"this process  ";
    report += IsCurrentProcessElevated() ? L"elevated" : L"not elevated";
    report += L"\r\n";

    const ElevationDecision decision = DecideElevation(cfg);
    report += L"elevation     " + cfg.elevation + L"\r\n";
    report += L"decision      ";
    if (decision.elevate) {
        report += L"relaunch elevated, because " + decision.reason;
    } else if (decision.blocked) {
        report += L"refuse to start, because " + decision.reason
                  + L" and \"elevation\" is \"never\"";
    } else if (IsCurrentProcessElevated()) {
        report += L"already elevated";
    } else {
        report += L"stay unelevated, nothing requires more";
    }

    console::Report(report);
    console::Detach();
    return 0;
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

/// Asks the dock that is already running for another column.
///
/// Exit codes are the useful part, because this process has no console to write
/// to: 0 added, 5 refused with the reason shown in the notification area, 2 when
/// the running instance could not be reached at all.
int RequestColumn()
{
    HWND dock = FindWindowW(kWindowClass, kWindowTitle);
    if (dock) {
        DWORD_PTR answer = kAddColumnNotHandled;

        // SendMessageTimeout, not SendMessage. The dock could be inside a modal
        // loop of its own, and a plain SendMessage would hang this process
        // behind it with nothing on screen to say why.
        //
        // The handler on the other side reserves the column and returns; the
        // terminal is launched after it has answered, so this wait is short even
        // on a cold start.
        const LRESULT delivered = SendMessageTimeoutW(
            dock, RegisterWindowMessageW(kAddColumnMessageName), 0, 0,
            SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 4000, &answer);

        if (delivered != 0) {
            switch (answer) {
            case kAddColumnAdded:
                return 0;
            case kAddColumnAtMax:
            case kAddColumnNoRoom:
                // The running dock has already said so in the notification area,
                // where a message about a dock belongs. Another dialog here
                // would be the second time the user is told.
                return 5;
            default:
                // Zero: a build that predates the message, a message UIPI
                // dropped on the way to an elevated dock, or an instance that is
                // in the middle of shutting down. Fall through and say so.
                break;
            }
        }
    }

    MessageBoxW(nullptr,
                L"Docked Console is already running and did not answer a request for "
                L"another column.\n\n"
                L"It is probably shutting down, or is an older build. Try again, or run "
                L"dockedconsole.exe --stop and start it fresh.",
                L"Docked Console", MB_OK | MB_ICONINFORMATION);
    return 2;
}

/// Asks the dock that is already running for a pane, the way ProjectMan does.
///
/// Same block, same message: the header from ids.h, then the two strings back
/// to back with no terminators. Exit codes follow RequestColumn's: 0 added,
/// 5 refused with the reason already shown from the notification area, 6 when
/// window naming is off, 7 when the dock called the request malformed, 2 when
/// it could not be reached at all.
int RequestPane(const PaneArgs& pane)
{
    HWND dock = FindWindowW(kWindowClass, kWindowTitle);
    if (dock) {
        const size_t cwd_bytes = pane.cwd.size() * sizeof(wchar_t);
        const size_t cmd_bytes = pane.command.size() * sizeof(wchar_t);
        const size_t profile_bytes = pane.profile.size() * sizeof(wchar_t);

        std::vector<char> block(sizeof(SplitRequest) + cwd_bytes + cmd_bytes + profile_bytes);

        SplitRequest header{};
        header.version = kSplitRequestVersion;
        header.column_id = 0;
        header.cwd_offset = static_cast<UINT32>(sizeof(SplitRequest));
        header.cwd_length = static_cast<UINT32>(pane.cwd.size());
        header.cmd_offset = static_cast<UINT32>(sizeof(SplitRequest) + cwd_bytes);
        header.cmd_length = static_cast<UINT32>(pane.command.size());
        header.profile_offset =
            static_cast<UINT32>(sizeof(SplitRequest) + cwd_bytes + cmd_bytes);
        header.profile_length = static_cast<UINT32>(pane.profile.size());

        memcpy(block.data(), &header, sizeof(header));
        if (cwd_bytes) {
            memcpy(block.data() + header.cwd_offset, pane.cwd.data(), cwd_bytes);
        }
        if (cmd_bytes) {
            memcpy(block.data() + header.cmd_offset, pane.command.data(), cmd_bytes);
        }
        if (profile_bytes) {
            memcpy(block.data() + header.profile_offset, pane.profile.data(), profile_bytes);
        }

        COPYDATASTRUCT data{};
        data.dwData = kSplitCopyDataId;
        data.cbData = static_cast<DWORD>(block.size());
        data.lpData = block.data();

        DWORD_PTR answer = kSplitNotHandled;
        const LRESULT delivered = SendMessageTimeoutW(
            dock, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
            SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 8000, &answer);

        if (delivered != 0) {
            switch (answer) {
            case kSplitAccepted:
            case kSplitNewColumn:
                return 0;
            case kSplitAtMax:
            case kSplitNoRoom:
                return 5;
            case kSplitUnnamed:
                MessageBoxW(nullptr,
                            L"Docked Console has window naming turned off, so its columns "
                            L"cannot be addressed.\n\n"
                            L"Set \"nameWindows\" to true in dockedconsole.json and start "
                            L"it again.",
                            L"Docked Console", MB_OK | MB_ICONINFORMATION);
                return 6;
            case kSplitRejected:
                MessageBoxW(nullptr,
                            L"Docked Console refused the pane request as malformed, which "
                            L"means the two are different versions.",
                            L"Docked Console", MB_OK | MB_ICONINFORMATION);
                return 7;
            default:
                break;
            }
        }
    }

    MessageBoxW(nullptr,
                L"Docked Console is already running and did not answer a request for "
                L"a pane.\n\n"
                L"It is probably shutting down, or is an older build. Try again, or run "
                L"dockedconsole.exe --stop and start it fresh.",
                L"Docked Console", MB_OK | MB_ICONINFORMATION);
    return 2;
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
        // Not an error. Starting the dock again is how you ask for another
        // column, or with pane arguments for a pane, so hand the request over
        // and get out of the way.
        const PaneArgs pane = ReadPaneArgs(args);
        return pane.present ? RequestPane(pane) : RequestColumn();
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

    const ElevationDecision decision = DecideElevation(cfg);
    if (decision.elevate) {
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

        log::Writef(L"elevation: %s, relaunching elevated because %s",
                    cfg.elevation.c_str(), decision.reason.c_str());

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

    if (decision.blocked) {
        log::Writef(L"elevation: never, refusing to start because %s",
                    decision.reason.c_str());
        const std::wstring message =
            L"Docked Console would have to run elevated because " + decision.reason
            + L", and \"elevation\" is \"never\".\n\n"
              L"Set it to \"auto\" in dockedconsole.json, run Docked Console as "
              L"administrator, or point terminalArgs -p at a profile that does not "
              L"elevate.";
        MessageBoxW(nullptr, message.c_str(), L"Docked Console", MB_OK | MB_ICONWARNING);
        return 3;
    }

    // Self-heal a previous hard kill before claiming anything of our own. The
    // shell only prunes registrations whose window is gone during a work-area
    // recompute, and this provokes one. Costs about 120ms, once.
    AppBar::ForceWorkAreaRecompute();

    // Whatever Load had to say, not only when it failed: an obsolete key it
    // ignored is exactly the kind of thing a person reads the log to find.
    if (!detail.empty()) {
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

    // Every start, so a profile added to Windows Terminal shows up the next
    // time the dock comes up.
    jump_list::Publish();

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

    // Before any window, so the taskbar files this process under the same app
    // as the pinned shortcut the installer made.
    dock::jump_list::ClaimAppId();

    const std::vector<std::wstring> args = dock::CommandLineArgs();

    int exit_code = 0;
    if (dock::HasFlag(args, L"--help") || dock::HasFlag(args, L"-h")
        || dock::HasFlag(args, L"/?")) {
        dock::ShowUsage();
    } else if (dock::HasFlag(args, L"--stop")) {
        exit_code = dock::SignalStop();
    } else if (dock::HasFlag(args, L"--reclaim")) {
        exit_code = dock::Reclaim();
    } else if (dock::HasFlag(args, L"--explain-elevation")) {
        exit_code = dock::ExplainElevation();
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
