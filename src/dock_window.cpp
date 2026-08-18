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
#include "dock_window.h"

#include "diag_log.h"
#include "ids.h"
#include "monitors.h"
#include "win32.h"

#include <shellapi.h>

namespace dock {
namespace {

constexpr UINT kRepositionDelayMs = 200;

} // namespace

DockWindow::DockWindow(Config cfg) : cfg_(std::move(cfg))
{
    appbar_message_ = RegisterWindowMessageW(L"DockedConsole_AppBarCallback");
    taskbar_created_ = TaskbarCreatedMessage();
}

DockWindow::~DockWindow()
{
    Teardown();
    if (background_) {
        DeleteObject(background_);
        background_ = nullptr;
    }
}

bool DockWindow::Create(HINSTANCE instance)
{
    instance_ = instance;
    background_ = CreateSolidBrush(cfg_.Background());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId));
    wc.hIconSm = wc.hIcon;
    // Null, because WM_ERASEBKGND paints. A class brush would have to be swapped
    // with SetClassLongPtr on a config reload and the old one deleted by hand,
    // which is the same work with more ways to leak a GDI object.
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // WS_EX_TOOLWINDOW keeps it out of Alt+Tab. WS_EX_APPWINDOW is simply absent
    // rather than cleared, which is the Win32 equivalent of what the WinForms
    // CreateParams override was doing.
    //
    // WS_CLIPCHILDREN is not decoration: without it the host erases over the
    // embedded terminal on every resize and the terminal strobes during a
    // taskbar move.
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowClass, kWindowTitle,
        WS_POPUP | WS_CLIPCHILDREN,
        -32000, -32000, cfg_.width_physical_px, 200,
        nullptr, nullptr, instance, this);

    if (!hwnd_) {
        return false;
    }

    // When elevation: auto has relaunched us elevated, our high-integrity window
    // silently drops broadcasts from medium-integrity explorer. Without this an
    // elevated dock goes deaf to ABN_POSCHANGED and TaskbarCreated, which shows
    // up as "the taskbar creeps back, but only on some machines".
    ChangeWindowMessageFilterEx(hwnd_, appbar_message_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, taskbar_created_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_SETTINGCHANGE, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_DISPLAYCHANGE, MSGFLT_ALLOW, nullptr);

    log::WriteBanner(cfg_);

    app_bar_ = std::make_unique<AppBar>(hwnd_, appbar_message_);
    if (!app_bar_->Register()) {
        log::Write(L"appbar: ABM_NEW refused; refusing to start");
        return false;
    }

    Reposition();

    tray_.Create(hwnd_);
    InstallStopChannel();

    ShowWindow(hwnd_, SW_SHOWNA);

    StartTerminal();

    // Last, so a regression in the new behaviour is unambiguous: everything
    // above this line is the same product the C# version was.
    enforcer_.Start(hwnd_, cfg_);
    Reposition(); // hands the enforcer its target now that it is listening

    SetTimer(hwnd_, kTimerHealth, 1000, nullptr);
    return true;
}

void DockWindow::StartTerminal()
{
    terminal_ = std::make_unique<Terminal>(hwnd_, cfg_);
    terminal_->on_fault = [this](const std::wstring& message) { tray_.Notify(message); };
    terminal_->on_closed = [this](const std::wstring& failure) { OnTerminalClosed(failure); };
    terminal_->Start();

    // Start() pumps, so a teardown may have run inside it. Everything after this
    // point has to tolerate that.
    if (shutting_down_) {
        return;
    }
    enforcer_.SetTerminal(terminal_->Hwnd());

    // Both the terminal and the shell settle their own geometry after being
    // sized, so the fit is re-applied a few times rather than once.
    fit_ticks_ = 0;
    SetTimer(hwnd_, kTimerFit, 250, nullptr);
}

void DockWindow::OnTerminalClosed(const std::wstring& failure)
{
    if (shutting_down_) {
        return;
    }

    if (!failure.empty()) {
        // Shown before shutdown rather than as a tray balloon, because the
        // balloon would be destroyed along with the tray icon a moment later.
        MessageBoxW(hwnd_, failure.c_str(), L"Docked Console", MB_OK | MB_ICONWARNING);
    }

    // The shell exited. Typing exit is how you close any other terminal, so it
    // closes this one too: undock, restore the desktop, quit.
    RequestShutdown();
}

int DockWindow::Run()
{
    MSG msg;
    for (;;) {
        // GetMessageW returns -1 on error, and -1 is truthy: the idiomatic
        // `while (GetMessage(...))` spins forever on a bad HWND.
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result == 0 || result == -1) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Teardown();
    return 0;
}

LRESULT CALLBACK DockWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<DockWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return self->WndProc(msg, w, l);
    }

    auto* self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        // Messages that arrive before WM_NCCREATE, WM_GETMINMAXINFO among them,
        // have no instance to route to yet.
        return DefWindowProcW(hwnd, msg, w, l);
    }
    return self->WndProc(msg, w, l);
}

LRESULT DockWindow::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == appbar_message_) {
        OnAppBarNotification(wparam, lparam);
        return 0;
    }

    if (msg == taskbar_created_) {
        // Explorer restarted. Both the tray icon and the AppBar registration
        // lived inside it and are gone.
        tray_.Readd();
        if (app_bar_) {
            app_bar_->Reregister();
        }
        ScheduleReposition();
        return 0;
    }

    switch (msg) {
    case WM_APP_TRAY:
        OnMenuCommand(tray_.OnCallback(wparam, lparam));
        return 0;

    case WM_APP_STOP_REQUESTED:
        log::Write(L"stop: requested over the named event");
        RequestShutdown();
        return 0;

    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(hwnd_, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, background_);
        // 1 means "erased". Returning 0 tells Windows to erase it again itself.
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_TIMER:
        // KillTimer does not purge a WM_TIMER already sitting in the queue, so a
        // stale tick can arrive after the kill. Every case re-checks.
        switch (wparam) {
        case kTimerReposition:
            KillTimer(hwnd_, kTimerReposition);
            if (!shutting_down_) {
                Reposition();
            }
            return 0;

        case kTimerFit:
            if (shutting_down_ || !terminal_) {
                KillTimer(hwnd_, kTimerFit);
                return 0;
            }
            terminal_->Fit();
            if (++fit_ticks_ >= 6) {
                KillTimer(hwnd_, kTimerFit);
            }
            return 0;

        case kTimerHealth:
            if (!shutting_down_ && terminal_) {
                terminal_->CheckHealth();
            }
            return 0;

        case kTimerTaskbarVerify: {
            KillTimer(hwnd_, kTimerTaskbarVerify);
            if (shutting_down_ || !app_bar_ || !cfg_.push_taskbar) {
                return 0;
            }
            const RECT& strip = app_bar_->ReservedRect();
            if (!taskbar_.Verify(strip)) {
                log::Write(L"taskbar: still overlapping after the first pass, retrying");
                taskbar_.Apply(strip, cfg_);
            }
            return 0;
        }

        default:
            return 0;
        }

    case WM_SYSCOMMAND: {
        const WPARAM command = wparam & 0xFFF0;
        if (command == SC_MINIMIZE || command == SC_MAXIMIZE || command == SC_CLOSE
            || command == SC_MOVE || command == SC_SIZE) {
            // Swallowed: the dock is not minimizable, closable, movable or
            // resizable.
            return 0;
        }
        break;
    }

    case WM_ACTIVATE:
        if (app_bar_) {
            app_bar_->NotifyActivate();
        }
        // Not while the tray menu is up. Showing that menu requires
        // SetForegroundWindow on this window, which lands here, and punting
        // focus into the terminal would dismiss the menu the user just opened.
        if (LOWORD(wparam) != WA_INACTIVE && terminal_ && !tray_.MenuIsUp()) {
            terminal_->FocusTerminal();
        }
        return 0;

    case WM_SETFOCUS:
        if (terminal_ && !tray_.MenuIsUp()) {
            terminal_->FocusTerminal();
        }
        return 0;

    case WM_SIZE:
        if (terminal_) {
            terminal_->Fit();
        }
        return 0;

    case WM_WINDOWPOSCHANGED: {
        // Forwarding every one of these would feed back on itself: a z-order
        // re-assert produces a WM_WINDOWPOSCHANGED, which pokes the shell, which
        // can answer with ABN_POSCHANGED, which schedules a reposition. A
        // z-order-only change is not something the shell needs to hear about.
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lparam);
        const bool geometry_changed =
            !pos || (pos->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE);
        if (geometry_changed && app_bar_) {
            app_bar_->NotifyWindowPosChanged();
        }
        break;
    }

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // The suggested rect in lParam is ignored on purpose: the AppBar owns
        // this window's geometry, not the DPI change.
        ScheduleReposition();
        return 0;

    case WM_SETTINGCHANGE:
        // Our own SETPOS changes the work area and echoes back here, so ignore
        // the echo while a reposition is in flight.
        if (wparam == SPI_SETWORKAREA && !repositioning_) {
            ScheduleReposition();
        }
        break;

    case WM_CLOSE:
        // Swallowed at the message level. There is no close button, and a
        // WM_CLOSE posted directly by anything else is refused too.
        if (!shutting_down_) {
            return 0;
        }
        break;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wparam) {
            Teardown();
        }
        return 0;

    case WM_DESTROY:
        // Teardown has already run by this point on every intended path. It is
        // idempotent, so calling it again costs nothing and covers the paths
        // that are not intended.
        Teardown();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void DockWindow::Reposition()
{
    if (!app_bar_ || shutting_down_ || repositioning_) {
        return;
    }

    repositioning_ = true;

    const MonitorInfo monitor = ResolveMonitor(cfg_.monitor_device_name);
    app_bar_->Reposition(cfg_.ParsedEdge(), cfg_.width_physical_px, monitor.bounds,
                         cfg_.push_taskbar);

    if (cfg_.push_taskbar) {
        taskbar_.Resolve(monitor.handle, monitor.bounds, cfg_.ParsedEdge());
        taskbar_.Apply(app_bar_->ReservedRect(), cfg_);
        // Explorer applies its relayout asynchronously, so reading the rect back
        // inline reports a stale value and a false failure.
        SetTimer(hwnd_, kTimerTaskbarVerify, 250, nullptr);
    }

    enforcer_.SetTarget(monitor.handle, monitor.bounds, app_bar_->ReservedRect());

    if (cfg_.topmost) {
        ReassertTopmost();
    }

    if (terminal_) {
        terminal_->Fit();
    }

    repositioning_ = false;
}

void DockWindow::ScheduleReposition()
{
    if (shutting_down_) {
        return;
    }
    // Restarting a live timer resets its interval, which coalesces the burst of
    // notifications a taskbar move or a resolution change produces.
    SetTimer(hwnd_, kTimerReposition, kRepositionDelayMs, nullptr);
}

void DockWindow::ReassertTopmost()
{
    // Debounced inside the enforcer, so calling this from every notification
    // costs nothing.
    enforcer_.ReassertTopmost();
}

void DockWindow::OnAppBarNotification(WPARAM notification, LPARAM lparam)
{
    switch (notification) {
    case ABN_POSCHANGED:
    case ABN_WINDOWARRANGE:
        ScheduleReposition();
        break;

    case ABN_STATECHANGE:
        if (cfg_.topmost) {
            ReassertTopmost();
        }
        break;

    case ABN_FULLSCREENAPP:
        // Inverted from the C# version, which dropped to HWND_BOTTOM here and
        // handed the screen to the fullscreen app. Now it is a signal that one
        // has appeared, and the answer is to stay put and push it aside.
        log::Writef(L"appbar: ABN_FULLSCREENAPP %s", lparam ? L"entered" : L"left");
        ReassertTopmost();
        if (lparam) {
            // This can arrive before the location change we would otherwise wait
            // for, so it shaves a frame or two off containment.
            enforcer_.ScanForeground();
        } else {
            ScheduleReposition();
        }
        break;

    default:
        break;
    }
}

void DockWindow::OnMenuCommand(UINT command)
{
    switch (command) {
    case kMenuRestartShell:
        if (terminal_) {
            terminal_->Restart();
            if (shutting_down_) {
                break; // Restart pumps, so teardown may have run inside it
            }
            // The old HWND is gone. Without this the enforcer keeps filtering
            // against a destroyed handle and stops recognising its own child.
            enforcer_.SetTerminal(terminal_->Hwnd());
            fit_ticks_ = 0;
            SetTimer(hwnd_, kTimerFit, 250, nullptr);
        }
        break;
    case kMenuReloadConfig:
        OnReloadConfig();
        break;
    case kMenuEditConfig:
        OpenConfigFile();
        break;
    case kMenuExit:
        RequestShutdown();
        break;
    default:
        break;
    }
}

void DockWindow::OnReloadConfig()
{
    ConfigStatus status{};
    std::wstring detail;
    Config fresh = Config::Load(status, detail);

    if (status == ConfigStatus::Unreadable) {
        tray_.Notify(L"Config not reloaded: " + detail);
        return;
    }

    // Settings that have to be actively undone when they are turned OFF. Reading
    // the new value and repositioning is not enough for any of these three:
    // whatever the old value did is still in effect until something reverses it,
    // and the failure is silent, which is the worst kind.
    const bool was_pushing_taskbar = cfg_.push_taskbar;
    const bool was_topmost = cfg_.topmost;
    const bool was_blocking = cfg_.block_fullscreen;

    cfg_ = std::move(fresh);

    if (was_pushing_taskbar && !cfg_.push_taskbar) {
        // The clip lives on explorer's window, not ours, so it survives until we
        // clear it.
        taskbar_.Restore();
    }

    if (was_topmost && !cfg_.topmost) {
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    if (was_blocking != cfg_.block_fullscreen) {
        // The hooks are installed once at startup, so toggling this did nothing
        // in either direction until now.
        enforcer_.Stop();
        enforcer_.Start(hwnd_, cfg_);
        if (terminal_) {
            enforcer_.SetTerminal(terminal_->Hwnd());
        }
    }

    if (background_) {
        DeleteObject(background_);
    }
    background_ = CreateSolidBrush(cfg_.Background());
    InvalidateRect(hwnd_, nullptr, TRUE);

    Reposition();
    tray_.Notify(detail.empty()
                     ? L"Config reloaded."
                     : L"Config reloaded. " + detail);
}

void DockWindow::OpenConfigFile()
{
    const std::wstring path = Config::FilePath();
    if (!FileExists(path)) {
        cfg_.Save();
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&info)) {
        return;
    }

    // No handler for .json on this machine. Notepad always exists.
    const std::wstring quoted = L"\"" + path + L"\"";
    SHELLEXECUTEINFOW fallback{};
    fallback.cbSize = sizeof(fallback);
    fallback.fMask = SEE_MASK_FLAG_NO_UI;
    fallback.lpVerb = L"open";
    fallback.lpFile = L"notepad.exe";
    fallback.lpParameters = quoted.c_str();
    fallback.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&fallback)) {
        tray_.Notify(L"Could not open " + path);
    }
}

void DockWindow::InstallStopChannel()
{
    stop_event_.Reset(CreateEventW(nullptr, FALSE, FALSE, kStopEventName));
    if (!stop_event_) {
        log::Write(L"stop: could not create the stop event; --stop will not work");
        return;
    }

    // A second, manual-reset event is what makes the waiter joinable. Without it
    // the thread blocks on the stop event forever and teardown has to choose
    // between leaking it and TerminateThread.
    stop_quit_event_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_quit_event_) {
        return;
    }

    stop_thread_.Reset(CreateThread(nullptr, 0, StopChannelThread, this, 0, nullptr));
}

DWORD WINAPI DockWindow::StopChannelThread(LPVOID param)
{
    auto* self = static_cast<DockWindow*>(param);
    const HANDLE waits[2] = {self->stop_event_.Get(), self->stop_quit_event_.Get()};

    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        // PostMessage rather than SendMessage: it is the cross-thread-safe one,
        // and a posted message is dispatched by the inner pump loops that
        // TrackPopupMenuEx and MessageBoxW run, so --stop is honoured even with
        // the tray menu open.
        PostMessageW(self->hwnd_, WM_APP_STOP_REQUESTED, 0, 0);
    }
    return 0;
}

void DockWindow::StopChannelShutdown()
{
    if (stop_quit_event_) {
        SetEvent(stop_quit_event_.Get());
    }

    if (stop_thread_) {
        if (WaitForSingleObject(stop_thread_.Get(), 2000) != WAIT_OBJECT_0) {
            // The waiter did not come home. Closing the events it is still
            // blocked on would hand it two dangling handles, and the handle
            // values can be reused immediately, so it could wake on an unrelated
            // object and post into a destroyed window. Leaking three handles in
            // a process that is exiting anyway is the cheaper mistake.
            log::Write(L"stop: the wait thread did not exit; leaking its handles "
                       L"rather than closing them underneath it");
            (void)stop_thread_.Release();
            (void)stop_quit_event_.Release();
            (void)stop_event_.Release();
            return;
        }
    }

    stop_thread_.Reset();
    stop_quit_event_.Reset();
    stop_event_.Reset();
}

void DockWindow::RequestShutdown()
{
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;

    // Teardown BEFORE DestroyWindow, and this ordering is load-bearing.
    //
    // DestroyWindow destroys the window's WS_CHILD children, and one of those
    // children is a Windows Terminal window that usually belongs to a process
    // shared with every other terminal the user has open. Letting it be
    // destroyed as a child would tear it down with no chance to close its panes.
    // Teardown detaches it first.
    Teardown();
    DestroyWindow(hwnd_);
}

void DockWindow::Teardown()
{
    if (torn_down_) {
        return;
    }
    torn_down_ = true;
    shutting_down_ = true;

    KillTimer(hwnd_, kTimerReposition);
    KillTimer(hwnd_, kTimerFit);
    KillTimer(hwnd_, kTimerHealth);
    KillTimer(hwnd_, kTimerTaskbarVerify);

    StopChannelShutdown();

    // Hooks first: nothing below should be re-entered by a WinEvent callback
    // while it is dismantling the thing that callback depends on.
    enforcer_.Stop();

    // Order matters here.
    //
    // The taskbar goes back first, so the work-area recompute that ABM_REMOVE
    // provokes already sees the final geometry and explorer relays out once
    // rather than twice.
    taskbar_.Restore();

    // The terminal is detached before anything can destroy this window. It is a
    // WS_CHILD of ours, and a child is destroyed with its parent: letting that
    // happen to a Windows Terminal window living inside a process shared with
    // the user's other terminals would take all of them down.
    //
    // Release() but deliberately NOT reset(). Terminal::Start pumps the message
    // queue while it waits for a cold-starting terminal, so Teardown can be
    // re-entered from inside a Terminal method that is still on the stack:
    // tray Exit, --stop, or WM_CLOSE arriving during that window all land here.
    // Destroying the object then would free it underneath its own Start(), which
    // resumes and writes to the freed memory. Release() is idempotent and sets a
    // flag Start() checks after every pump, so it unwinds cleanly instead.
    //
    // The object is destroyed with this window, after the message loop has ended
    // and no Start() can still be running.
    if (terminal_) {
        terminal_->Release();
    }

    tray_.Destroy();

    if (app_bar_) {
        app_bar_->Remove();
        app_bar_.reset();
    }

    log::Write(L"teardown: complete");
}

} // namespace dock
