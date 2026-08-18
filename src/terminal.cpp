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
#include "terminal.h"

#include "diag_log.h"
#include "elevation.h"
#include "monitors.h"
#include "win32.h"

#include <algorithm> // std::find, std::min, std::max
#include <vector>

namespace dock {
namespace {

constexpr wchar_t kTerminalClass[] = L"CASCADIA_HOSTING_WINDOW_CLASS";
constexpr wchar_t kDragBarClass[] = L"DRAG_BAR_WINDOW_CLASS";

constexpr ULONGLONG kLocateTimeoutMs = 15000;
constexpr ULONGLONG kReadyTimeoutMs = 10000;
constexpr ULONGLONG kRapidFailureWindowMs = 3000;
constexpr ULONGLONG kStartupFailureWindowMs = 2000;
constexpr int kMaxRapidFailures = 5;

struct ClassSearch {
    const wchar_t* wanted;
    std::vector<HWND>* found;
};

BOOL CALLBACK CollectByClass(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<ClassSearch*>(param);
    if (ClassNameOf(hwnd) == search->wanted) {
        search->found->push_back(hwnd);
    }
    return TRUE;
}

std::vector<HWND> FindWindowsByClass(const wchar_t* class_name)
{
    std::vector<HWND> found;
    ClassSearch search{class_name, &found};
    EnumWindows(CollectByClass, reinterpret_cast<LPARAM>(&search));
    return found;
}

struct DragBarSearch {
    RECT window_rect;
    int trim;
};

BOOL CALLBACK FindDragBar(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<DragBarSearch*>(param);
    RECT bar{};
    if (ClassNameOf(hwnd) == kDragBarClass && GetWindowRect(hwnd, &bar)) {
        search->trim = bar.bottom - search->window_rect.top;
        return FALSE;
    }
    return TRUE;
}

/// Pumps the queue for `ms` so the UI thread stays responsive while we wait.
///
/// The C# version slept here. Sleeping blocks the message loop, which means the
/// dock stops repainting, the tray menu stops responding, and once the enforcer
/// is installed its WinEvent callbacks queue up undelivered, because
/// WINEVENT_OUTOFCONTEXT hooks are dispatched from inside GetMessage. A cold
/// start could therefore have meant a ten-second enforcement blackout.
/// Returns false when the process is quitting and the caller must unwind.
///
/// PeekMessage with PM_REMOVE takes WM_QUIT off the queue like any other message,
/// and DispatchMessage does nothing with it. Dropping it here would mean the
/// GetMessage loop in DockWindow::Run never sees it and never returns 0, so the
/// process would hang forever still holding the single-instance mutex, with no
/// window on screen. Re-post it and stop pumping.
[[nodiscard]] bool PumpFor(ULONGLONG ms)
{
    const ULONGLONG deadline = GetTickCount64() + ms;
    MSG msg;
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    return true;
}

} // namespace

int MeasureChrome(HWND terminal)
{
    DragBarSearch search{};
    if (!GetWindowRect(terminal, &search.window_rect)) {
        return 0;
    }
    EnumChildWindows(terminal, FindDragBar, reinterpret_cast<LPARAM>(&search));
    return search.trim;
}

Terminal::Terminal(HWND host, const Config& cfg) : host_(host), cfg_(cfg) {}

Terminal::~Terminal()
{
    Release();
}

bool Terminal::Launch()
{
    const std::wstring wt = cfg_.ResolveTerminalPath();

    // wt.exe is an app execution alias: a zero-length reparse point. Any check
    // based on file size would call it missing.
    if (GetFileAttributesW(wt.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (on_fault) {
            on_fault(L"wt.exe was not found at " + wt);
        }
        return false;
    }

    const std::wstring home = KnownFolder(FOLDERID_Profile);

    // CreateProcessW writes into lpCommandLine, so it must be a writable buffer.
    // Passing a literal or a c_str() is undefined behaviour and corrupts memory
    // on some paths rather than failing cleanly.
    std::wstring command_line = L"\"" + wt + L"\" " + cfg_.terminal_args;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(wt.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, home.empty() ? nullptr : home.c_str(),
                        &startup, &process)) {
        log::Writef(L"launch: CreateProcessW failed (%lu)", GetLastError());
        if (on_fault) {
            on_fault(L"could not start Windows Terminal");
        }
        return false;
    }

    // The launcher is a throwaway stub that exits as soon as it has handed off
    // to the monarch. We own nothing here and must keep nothing.
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void Terminal::Start()
{
    // Start pumps while it waits, so a stray timer tick could re-enter it.
    if (starting_ || released_) {
        return;
    }
    starting_ = true;

    const std::vector<HWND> existing = FindWindowsByClass(kTerminalClass);
    last_start_ = GetTickCount64();

    if (!Launch()) {
        starting_ = false;
        // Same reasoning as a locate timeout: an empty dock that reserves desktop
        // space forever is worse than not starting.
        if (on_closed) {
            on_closed(L"Windows Terminal could not be started, so the dock has undocked.");
        }
        return;
    }

    DWORD owner_pid = 0;
    HWND hwnd = LocateWindow(existing, owner_pid);

    // Everything from here down runs after PumpFor has dispatched messages, so
    // teardown may already have happened underneath us. Release() sets
    // released_, and DockWindow deliberately keeps this object alive until its
    // own destructor, so checking the flag is sufficient and safe.
    if (released_) {
        log::Write(L"locate: abandoned, the dock is shutting down");
        starting_ = false;
        return;
    }

    if (!hwnd) {
        log::Write(L"locate: timed out with no new terminal window");
        starting_ = false;
        // Fatal, not a warning. A balloon used to leave the dock sitting there
        // reserving 735px of desktop with nothing in it and no way back: the
        // health check watches a window handle, and there is no handle to watch,
        // so nothing would ever retry or clean up.
        if (on_closed) {
            on_closed(L"The terminal window never appeared, so the dock has undocked.\n\n"
                      L"Check that Windows Terminal starts on its own, then start the "
                      L"dock again.");
        }
        return;
    }

    log::Writef(L"locate: hwnd=0x%p ownerPid=%lu ownerElevated=%s weAreElevated=%s",
                hwnd, owner_pid, log::Describe(IsProcessElevated(owner_pid)),
                log::Describe(IsCurrentProcessElevated() ? 1 : 0));

    // A cold-started terminal hands back a window handle well before the window
    // has finished building itself. Reparenting into that gap is the difference
    // between a dock that works when a terminal was already running and one that
    // does not.
    WaitUntilReady(hwnd);

    if (released_) {
        log::Write(L"ready: abandoned, the dock is shutting down");
        starting_ = false;
        return;
    }

    terminal_ = hwnd;
    chrome_trim_ = -1;

    // One retry. If the first attempt lost a race with the terminal's own
    // start-up, the second runs against a window that has finished settling.
    std::wstring error = Embed(hwnd, owner_pid);
    if (!error.empty()) {
        log::Writef(L"embed: first attempt failed, retrying once. %s", error.c_str());
        const bool keep_going = PumpFor(600);
        if (released_ || !keep_going) {
            log::Write(L"embed: abandoned between attempts, the dock is shutting down");
            terminal_ = nullptr;
            starting_ = false;
            return;
        }
        error = Embed(hwnd, owner_pid);
    }

    if (!error.empty()) {
        log::Writef(L"embed: FAILED. %s", error.c_str());
        terminal_ = nullptr;
        starting_ = false;
        if (on_closed) {
            on_closed(error + L"\n\nDetails were written to:\n" + log::Path());
        }
        return;
    }

    log::Writef(L"embed: ok, chromeTrim=%dpx", ChromeTrim());
    embedded_at_ = GetTickCount64();
    starting_ = false;
}

void Terminal::Restart()
{
    // Refused while a start is already in flight. Start() pumps, so a tray
    // "Restart shell" can be dispatched from inside one, and going ahead would
    // Release() the window that Start() is still in the middle of embedding and
    // then clear released_ out from under it: the outer Start would resume and
    // re-embed a window that had already been detached and sent WM_CLOSE, and
    // the terminal would end up untracked while still a live child of the host.
    if (starting_) {
        log::Write(L"restart: a start is already in flight; ignoring");
        return;
    }

    // Release() sets released_, which is also the flag Start() uses to abandon a
    // launch that a teardown interrupted. Clearing it unconditionally would let
    // a Restart arriving during shutdown revive a torn-down dock, so only a
    // caller that is genuinely still running gets a fresh start.
    const bool host_alive = host_ && IsWindow(host_);

    Release();
    if (!host_alive) {
        log::Write(L"restart: the dock is gone; not relaunching");
        return;
    }

    released_ = false;
    Start();
}

HWND Terminal::LocateWindow(const std::vector<HWND>& existing, DWORD& owner_pid)
{
    const ULONGLONG deadline = GetTickCount64() + kLocateTimeoutMs;

    while (GetTickCount64() < deadline) {
        for (HWND candidate : FindWindowsByClass(kTerminalClass)) {
            const bool was_there_before =
                std::find(existing.begin(), existing.end(), candidate) != existing.end();
            if (was_there_before) {
                continue;
            }
            GetWindowThreadProcessId(candidate, &owner_pid);
            return candidate;
        }
        if (!PumpFor(50) || released_) {
            break;
        }
    }

    owner_pid = 0;
    return nullptr;
}

void Terminal::WaitUntilReady(HWND hwnd)
{
    // The readiness signal is the DRAG_BAR_WINDOW_CLASS child. That window is
    // created as part of the terminal's normal build-out, so its presence means
    // the frame has settled, and it is also the thing the chrome trim is
    // measured from. Waiting for it fixes two cold-start problems at once:
    // reparenting a half-built window, and measuring a trim that does not exist
    // yet and falling back to a computed guess.
    const ULONGLONG started = GetTickCount64();

    while (GetTickCount64() - started < kReadyTimeoutMs) {
        if (!IsWindow(hwnd)) {
            log::Writef(L"ready: window vanished while waiting after %llums",
                        GetTickCount64() - started);
            return;
        }

        if (IsWindowVisible(hwnd) && MeasureChrome(hwnd) > 0) {
            log::Writef(L"ready: terminal settled after %llums", GetTickCount64() - started);
            // The drag bar can appear a frame before the first layout finishes.
            // Either way we are done waiting; Start re-checks released_ next.
            (void)PumpFor(120);
            return;
        }

        if (!PumpFor(50) || released_) {
            return;
        }
    }

    log::Writef(L"ready: TIMED OUT after %llums, visible=%d, dragBar=%dpx. Embedding anyway.",
                GetTickCount64() - started, IsWindowVisible(hwnd) ? 1 : 0,
                MeasureChrome(hwnd));
}

std::wstring Terminal::Embed(HWND child, DWORD owner_pid)
{
    const LONG_PTR original_style = GetWindowLongPtrW(child, GWL_STYLE);
    const LONG_PTR original_ex = GetWindowLongPtrW(child, GWL_EXSTYLE);

    // Remembered for Release, which has to hand a usable window back to the user
    // if it cannot close it.
    original_style_ = original_style;
    original_ex_ = original_ex;

    ShowWindow(child, SW_HIDE);

    LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU
                                    | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER
                                    | WS_DLGFRAME | WS_POPUP);
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(child, GWL_STYLE, style);

    LONG_PTR ex = GetWindowLongPtrW(child, GWL_EXSTYLE);
    ex &= ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_APPWINDOW
                                 | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME);
    SetWindowLongPtrW(child, GWL_EXSTYLE, ex);

    SetLastError(0);
    SetParent(child, host_);
    const DWORD set_parent_error = GetLastError();
    const HWND actual_parent = GetParent(child);

    log::Writef(L"embed: style 0x%08llX->0x%08llX SetParent err=%lu parent=0x%p want=0x%p",
                static_cast<unsigned long long>(original_style),
                static_cast<unsigned long long>(GetWindowLongPtrW(child, GWL_STYLE)),
                set_parent_error, actual_parent, host_);

    // GetParent is the unambiguous test. SetParent returns null both for failure
    // and for a window that had no parent, which every top-level window did.
    if (actual_parent != host_) {
        SetWindowLongPtrW(child, GWL_STYLE, original_style);
        SetWindowLongPtrW(child, GWL_EXSTYLE, original_ex);
        SetWindowPos(child, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ShowWindow(child, SW_SHOWNORMAL);

        const int terminal_elevated = IsProcessElevated(owner_pid);
        if (terminal_elevated == 1 && !IsCurrentProcessElevated()) {
            return L"The terminal window belongs to an elevated Windows Terminal, and "
                   L"Windows does not allow an unelevated program to take over an "
                   L"elevated window.\n\n"
                   L"This happens when any elevated terminal is already running, because "
                   L"Windows Terminal creates new windows inside whichever process it "
                   L"already has. Run Docked Console as administrator, or close the "
                   L"elevated terminal windows first.\n\n"
                   L"A terminal window was opened and could not be captured. Close it "
                   L"manually.";
        }
        return L"The terminal window could not be captured, so the dock has nothing to "
               L"show. A terminal window may have been left open.";
    }

    SetWindowPos(child, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    Fit();
    ShowWindow(child, SW_SHOWNA);
    return {};
}

void Terminal::ReassertChildState()
{
    // A terminal that is still starting up can re-apply its own window styles
    // after we have set ours, which silently undoes the reparent and leaves the
    // window floating beside an empty dock. Re-assert rather than assume.
    LONG_PTR style = GetWindowLongPtrW(terminal_, GWL_STYLE);
    const bool is_child = (style & WS_CHILD) != 0;
    const bool parented = GetParent(terminal_) == host_;

    if (is_child && parented) {
        return;
    }

    log::Writef(L"reassert: child=%d parented=%d style=0x%08llX, re-applying",
                is_child ? 1 : 0, parented ? 1 : 0,
                static_cast<unsigned long long>(style));

    style &= ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU
                                    | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER
                                    | WS_DLGFRAME | WS_POPUP);
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(terminal_, GWL_STYLE, style);

    if (!parented) {
        SetParent(terminal_, host_);
    }
}

int Terminal::ChromeTrim()
{
    if (cfg_.chrome_trim_px >= 0) {
        return cfg_.chrome_trim_px;
    }
    if (chrome_trim_ >= 0) {
        return chrome_trim_;
    }

    int measured = MeasureChrome(terminal_);

    // A tab strip is tens of pixels tall at any sane DPI. Anything outside that
    // means the measurement caught the window mid-layout, so prefer the computed
    // value: WT's tab strip is 40 DIP.
    if (measured <= 0 || measured > 200) {
        UINT dpi = GetDpiForWindow(terminal_);
        if (dpi == 0) {
            dpi = 96;
        }
        measured = static_cast<int>((40.0 * dpi / 96.0) + 0.5) + 1;
    }

    chrome_trim_ = measured;
    return chrome_trim_;
}

void Terminal::Fit()
{
    if (!terminal_ || !IsWindow(terminal_)) {
        return;
    }

    RECT client{};
    if (!GetClientRect(host_, &client) || IsEmptyRect(client)) {
        return;
    }

    ReassertChildState();

    // Windows Terminal draws its tab strip inside its own client area, so
    // stripping the native caption does not remove it. Instead the window is
    // pushed up by exactly the height of the tab strip and made correspondingly
    // taller: child windows are clipped to the parent's client area, so the
    // strip falls outside the dock and disappears while the terminal grid still
    // fills it edge to edge.
    const int trim = ChromeTrim();

    SetWindowPos(terminal_, nullptr, 0, -trim,
                 Width(client), Height(client) + trim,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Terminal::FocusTerminal()
{
    if (!terminal_ || !IsWindow(terminal_)) {
        return;
    }

    const DWORD host_thread = GetCurrentThreadId();
    const DWORD child_thread = GetWindowThreadProcessId(terminal_, nullptr);

    if (host_thread == child_thread) {
        SetFocus(terminal_);
        return;
    }

    // Focus cannot be handed across threads without attaching their input
    // queues first.
    if (AttachThreadInput(host_thread, child_thread, TRUE)) {
        SetFocus(terminal_);
        AttachThreadInput(host_thread, child_thread, FALSE);
    }
}

void Terminal::CheckHealth()
{
    if (released_ || starting_) {
        return;
    }

    // The window handle is the authority. Watching a process would be wrong,
    // because the window can live inside a process we did not start and must not
    // touch.
    if (terminal_ && IsWindow(terminal_)) {
        return;
    }
    if (!terminal_) {
        return;
    }

    const bool immediate = (GetTickCount64() - embedded_at_) < kStartupFailureWindowMs;
    terminal_ = nullptr;

    if (cfg_.ParsedOnShellExit() == ShellExitAction::Quit) {
        if (on_closed) {
            // Nobody can type exit that fast. A terminal that vanishes this soon
            // after being embedded failed to launch, and quitting silently would
            // look like the dock itself refusing to start.
            on_closed(immediate
                          ? std::wstring(
                                L"The terminal closed immediately after starting, so the "
                                L"dock has undocked.\n\nCheck that the terminal launches "
                                L"on its own, then start the dock again.")
                          : std::wstring());
        }
        return;
    }

    if (GetTickCount64() - last_start_ < kRapidFailureWindowMs) {
        ++rapid_failures_;
    } else {
        rapid_failures_ = 0;
    }

    if (rapid_failures_ >= kMaxRapidFailures) {
        if (on_fault) {
            on_fault(L"the terminal keeps closing immediately; stopped relaunching it");
        }
        return;
    }

    Start();
}

void Terminal::Release()
{
    released_ = true;

    HWND hwnd = terminal_;
    terminal_ = nullptr;

    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    SetParent(hwnd, nullptr);

    // Put back what Embed took away before asking it to close.
    //
    // WM_CLOSE is a request, and Windows Terminal declines it when a pane has a
    // running process and "warn before closing" is on. A declined close used to
    // leave the window hidden, parentless and still carrying WS_CHILD with no
    // caption: an invisible window the user cannot reach, with their shells
    // still alive inside it. Restoring the frame means the worst case is a
    // normal terminal window sitting on the desktop.
    //
    // The exact captured styles, not an invented WS_OVERLAPPEDWINDOW: this
    // window belongs to another application and it is not ours to redecorate.
    if (original_style_ != 0) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, original_style_ | WS_VISIBLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, original_ex_);
    } else {
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        style &= ~static_cast<LONG_PTR>(WS_CHILD);
        style |= WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    }

    // Reposition, because the geometry is still the dock's. Fit() had put the
    // window at y = -chromeTrim in the host's CLIENT coordinates so the tab strip
    // was clipped away; as a top-level window those become SCREEN coordinates and
    // the caption lands above the top of the display, where it cannot be grabbed.
    // Put it somewhere the user can actually reach it.
    RECT work{};
    const MonitorInfo monitor = ResolveMonitor(cfg_.monitor_device_name);
    work = monitor.work;

    const int width = (std::min)(1100, (std::max)(640, Width(work) / 2));
    const int height = (std::min)(800, (std::max)(400, Height(work) / 2));

    SetWindowPos(hwnd, nullptr,
                 work.left + (Width(work) - width) / 2,
                 work.top + (Height(work) - height) / 2,
                 width, height,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNA);

    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

} // namespace dock
