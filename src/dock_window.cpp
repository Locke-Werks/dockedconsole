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

#include <algorithm>
#include <vector>

namespace dock {
namespace {

constexpr UINT kRepositionDelayMs = 200;

} // namespace

DockWindow::DockWindow(Config cfg) : cfg_(std::move(cfg))
{
    appbar_message_ = RegisterWindowMessageW(L"DockedConsole_AppBarCallback");
    taskbar_created_ = TaskbarCreatedMessage();
    add_column_message_ = RegisterWindowMessageW(kAddColumnMessageName);
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

    WNDCLASSEXW column_class{};
    column_class.cbSize = sizeof(column_class);
    column_class.lpfnWndProc = ColumnProcThunk;
    column_class.hInstance = instance;
    column_class.lpszClassName = kColumnClass;
    column_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Null for the same reason as the host class: WM_ERASEBKGND paints, so a
    // config reload does not have to swap a class brush and delete the old one.
    column_class.hbrBackground = nullptr;

    if (!RegisterClassExW(&column_class)) {
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

    // The same deafness, in the direction that matters for columns: an elevated
    // dock would drop the add-column message a second instance sends from the
    // user's ordinary token, and the launch would look like it did nothing.
    ChangeWindowMessageFilterEx(hwnd_, add_column_message_, MSGFLT_ALLOW, nullptr);

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

    Column* first = AppendColumn();
    if (!first) {
        log::Write(L"column: could not create the first panel; refusing to start");
        return false;
    }
    LayoutColumns();
    StartColumn(first->id);

    // StartColumn pumps the message queue, so a full Teardown and DestroyWindow
    // may already have happened by the time it returns. Continuing would install
    // WinEvent hooks and timers that Teardown has just removed and will never
    // remove again, on a window that no longer exists.
    if (shutting_down_) {
        log::Write(L"create: torn down during start-up; not finishing initialisation");
        return true;
    }

    // Last, so a regression in the new behaviour is unambiguous: everything
    // above this line is the same product the C# version was.
    enforcer_.Start(hwnd_, cfg_);
    Reposition(); // hands the enforcer its target now that it is listening

    SetTimer(hwnd_, kTimerHealth, 1000, nullptr);
    return true;
}

int DockWindow::StripThickness() const
{
    const int columns = (std::max)(1, ColumnCount());
    return columns * cfg_.width_physical_px;
}

DockWindow::Column* DockWindow::FindColumn(int id)
{
    for (const auto& column : columns_) {
        if (column->id == id) {
            return column.get();
        }
    }
    return nullptr;
}

DockWindow::Column* DockWindow::FindColumnByPanel(HWND panel)
{
    for (const auto& column : columns_) {
        if (column->panel == panel) {
            return column.get();
        }
    }
    return nullptr;
}

DockWindow::Column* DockWindow::ActiveColumn()
{
    if (Column* column = FindColumn(active_column_id_)) {
        return column;
    }
    return columns_.empty() ? nullptr : columns_.front().get();
}

DockWindow::Column* DockWindow::AppendColumn()
{
    auto column = std::make_unique<Column>();
    column->id = next_column_id_++;

    // Sized by LayoutColumns a moment later. Created empty rather than at a
    // guess, so a panel that never gets laid out is invisible instead of a black
    // rectangle sitting over the column beside it.
    column->panel = CreateWindowExW(
        0, kColumnClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwnd_, nullptr, instance_, this);

    if (!column->panel) {
        log::Writef(L"column: CreateWindowExW failed (%lu)", GetLastError());
        return nullptr;
    }

    Column* raw = column.get();
    columns_.push_back(std::move(column));
    active_column_id_ = raw->id;
    PublishOwnedWindows();
    return raw;
}

void DockWindow::StartColumn(int id)
{
    Column* column = FindColumn(id);
    if (!column || column->terminal || shutting_down_) {
        return;
    }

    column->terminal = std::make_unique<Terminal>(column->panel, cfg_);
    column->terminal->on_fault = [this](const std::wstring& message) {
        tray_.Notify(message);
    };
    column->terminal->on_closed = [this, id](const std::wstring& failure) {
        OnColumnTerminalClosed(id, failure);
    };
    column->terminal->Start();

    // Start() pumps, so a teardown may have run inside it, and so may the
    // removal of this very column. Nothing below may reuse `column`.
    if (shutting_down_) {
        return;
    }
    PublishOwnedWindows();

    // Both the terminal and the shell settle their own geometry after being
    // sized, so the fit is re-applied a few times rather than once.
    fit_ticks_ = 0;
    SetTimer(hwnd_, kTimerFit, 250, nullptr);
}

LRESULT DockWindow::OnAddColumnRequest()
{
    if (shutting_down_) {
        // The sender falls back to reporting that an instance is already
        // running, which is true and is about to stop being true.
        return kAddColumnNotHandled;
    }

    if (ColumnCount() >= kMaxColumns) {
        log::Writef(L"column: refused, already at %d", kMaxColumns);
        tray_.Notify(L"Already at the maximum of three columns.");
        return kAddColumnAtMax;
    }

    const MonitorInfo monitor = ResolveMonitor(cfg_.monitor_device_name);
    const DockEdge edge = cfg_.ParsedEdge();
    const int span = (edge == DockEdge::Left || edge == DockEdge::Right)
                         ? Width(monitor.bounds)
                         : Height(monitor.bounds);
    const int wanted = (ColumnCount() + 1) * cfg_.width_physical_px;

    if (span - wanted < kMinFreeWorkAreaPx) {
        log::Writef(L"column: refused, a %d px strip would leave %d px of desktop",
                    wanted, span - wanted);
        tray_.Notify(L"No room for another column on this display.");
        return kAddColumnNoRoom;
    }

    Column* column = AppendColumn();
    if (!column) {
        return kAddColumnNotHandled;
    }

    // Reposition early-outs while it is already on the stack, and it can be:
    // this runs on a SendMessage from the process the user has just started, and
    // a sent message is dispatched inline, including from inside our own
    // SHAppBarMessage. The timer picks it up once that has unwound.
    if (repositioning_) {
        ScheduleReposition();
    } else {
        Reposition();
    }

    // Posted, not called. The sender is blocked in SendMessage until this
    // returns, and a cold Windows Terminal takes seconds to appear.
    //
    // Checked, because a reserved column that never gets its terminal is a strip
    // of desktop given up for an empty black rectangle, and nothing else would
    // ever come along to notice.
    const int id = column->id;
    if (!PostMessageW(hwnd_, WM_APP_COLUMN_START, static_cast<WPARAM>(id), 0)) {
        log::Writef(L"column: could not post the start for #%d (%lu); giving the "
                    L"width back", id, GetLastError());
        RemoveColumn(id);
        return kAddColumnNotHandled;
    }

    log::Writef(L"column: added #%d, now %d of %d", id, ColumnCount(), kMaxColumns);
    return kAddColumnAdded;
}

void DockWindow::OnColumnTerminalClosed(int id, const std::wstring& failure)
{
    if (shutting_down_) {
        return;
    }

    Column* column = FindColumn(id);
    if (!column) {
        return;
    }
    column->failure = failure;

    // Posted, never handled inline. This arrives from inside the Terminal's own
    // code, and taking the column apart here would free the object whose call is
    // still on the stack.
    PostMessageW(hwnd_, WM_APP_COLUMN_GONE, static_cast<WPARAM>(id), 0);
}

void DockWindow::OnColumnGone(int id)
{
    if (shutting_down_) {
        return;
    }

    Column* column = FindColumn(id);
    if (!column) {
        return;
    }

    const std::wstring failure = column->failure;

    if (ColumnCount() <= 1) {
        if (!failure.empty()) {
            // Shown before shutdown rather than as a tray balloon, because the
            // balloon would be destroyed along with the tray icon a moment later.
            MessageBoxW(hwnd_, failure.c_str(), L"Docked Console",
                        MB_OK | MB_ICONWARNING);
        }

        // The last shell exited. Typing exit is how you close any other terminal,
        // so it closes this one too: undock, restore the desktop, quit.
        RequestShutdown();
        return;
    }

    if (!failure.empty()) {
        tray_.Notify(failure);
    }
    RemoveColumn(id);
}

void DockWindow::RemoveColumn(int id)
{
    auto it = columns_.begin();
    for (; it != columns_.end(); ++it) {
        if ((*it)->id == id) {
            break;
        }
    }
    if (it == columns_.end()) {
        return;
    }

    Retired retired;
    retired.panel = (*it)->panel;
    retired.terminal = std::move((*it)->terminal);

    // Detached, not destroyed, and for the same reason Teardown gives: Start()
    // pumps, so this can be reached from inside the very Start() belonging to
    // this terminal, and freeing it there would free the frame about to resume.
    // Release() is safe from anywhere; DrainRetiredColumns does the destroying
    // once nothing is on the stack.
    if (retired.terminal) {
        retired.terminal->Release();
    }

    columns_.erase(it);

    // Hidden now, destroyed later. Out of the layout either way, since
    // LayoutColumns only knows about columns_.
    if (retired.panel) {
        ShowWindow(retired.panel, SW_HIDE);
    }
    retired_.push_back(std::move(retired));

    if (!FindColumn(active_column_id_) && !columns_.empty()) {
        active_column_id_ = columns_.back()->id;
    }

    PublishOwnedWindows();
    log::Writef(L"column: removed #%d, %d left", id, ColumnCount());

    // Same reasoning as the add path: Reposition early-outs while it is already
    // on the stack, so hand it to the timer when it is.
    if (repositioning_) {
        ScheduleReposition();
    } else {
        Reposition();
    }
    FocusActiveColumn();
}

void DockWindow::DrainRetiredColumns()
{
    std::erase_if(retired_, [](Retired& retired) {
        if (retired.terminal && retired.terminal->Starting()) {
            return false;
        }
        if (retired.panel) {
            DestroyWindow(retired.panel);
        }
        return true;
    });
}

void DockWindow::LayoutColumns()
{
    if (columns_.empty()) {
        return;
    }

    RECT client{};
    if (!GetClientRect(hwnd_, &client) || IsEmptyRect(client)) {
        return;
    }

    const DockEdge edge = cfg_.ParsedEdge();
    const int count = ColumnCount();

    for (int i = 0; i < count; ++i) {
        const RECT slot = ColumnSlot(edge, client, i, count);
        SetWindowPos(columns_[static_cast<size_t>(i)]->panel, nullptr,
                     slot.left, slot.top, Width(slot), Height(slot),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    FitColumns();
}

void DockWindow::FitColumns()
{
    for (const auto& column : columns_) {
        if (column->terminal) {
            column->terminal->Fit();
        }
    }
}

void DockWindow::PublishOwnedWindows()
{
    std::vector<HWND> owned;
    owned.reserve(columns_.size() * 2);
    for (const auto& column : columns_) {
        owned.push_back(column->panel);
        if (column->terminal && column->terminal->Hwnd()) {
            owned.push_back(column->terminal->Hwnd());
        }
    }
    enforcer_.SetOwnedWindows(std::move(owned));
}

void DockWindow::FocusActiveColumn()
{
    // Not while the tray menu is up. Showing that menu requires
    // SetForegroundWindow on the host, which lands in WM_ACTIVATE, and punting
    // focus into a terminal from there would dismiss the menu the user just
    // opened.
    if (tray_.MenuIsUp()) {
        return;
    }
    if (Column* column = ActiveColumn(); column && column->terminal) {
        column->terminal->FocusTerminal();
    }
}

LRESULT CALLBACK DockWindow::ColumnProcThunk(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, w, l);
    }

    auto* self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, w, l);
    }

    switch (msg) {
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(w), &client, self->background_);
        return 1;
    }

    case WM_PARENTNOTIFY:
        // Best effort, and treated as such. A click on a child is sent up the
        // parent chain as WM_PARENTNOTIFY, and it is the only signal we get that
        // the user chose a column: the terminal belongs to another process, so
        // its focus is not ours to read. If a terminal build stops sending it,
        // the active column stays the last one added, which is where a freshly
        // added column wants focus anyway.
        if (LOWORD(w) == WM_LBUTTONDOWN || LOWORD(w) == WM_RBUTTONDOWN
            || LOWORD(w) == WM_MBUTTONDOWN) {
            if (Column* column = self->FindColumnByPanel(hwnd)) {
                self->active_column_id_ = column->id;
            }
        }
        break;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, w, l);
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

    if (msg == add_column_message_) {
        // Another copy of the executable was started while this one is docked.
        // The reply travels back out of that process's SendMessage and decides
        // what, if anything, it puts on screen.
        return OnAddColumnRequest();
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

    case WM_APP_COLUMN_START:
        if (!shutting_down_) {
            StartColumn(static_cast<int>(wparam));
        }
        return 0;

    case WM_APP_COLUMN_GONE:
        OnColumnGone(static_cast<int>(wparam));
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
            if (shutting_down_ || columns_.empty()) {
                KillTimer(hwnd_, kTimerFit);
                return 0;
            }
            FitColumns();
            if (++fit_ticks_ >= 6) {
                KillTimer(hwnd_, kTimerFit);
            }
            return 0;

        case kTimerHealth: {
            if (shutting_down_) {
                return 0;
            }
            // By id, not by iterator. With onShellExit: relaunch, CheckHealth
            // starts a replacement terminal, starting one pumps, and a message
            // dispatched from inside that pump can remove a column out from
            // under the loop.
            std::vector<int> ids;
            ids.reserve(columns_.size());
            for (const auto& column : columns_) {
                ids.push_back(column->id);
            }
            for (const int id : ids) {
                if (shutting_down_) {
                    return 0;
                }
                Column* column = FindColumn(id);
                if (column && column->terminal) {
                    column->terminal->CheckHealth();
                }
            }
            DrainRetiredColumns();

            // Backstop for a shell flyout that vanished without the hide event
            // that would have brought us back up. Being stuck below every window
            // on the desktop is the one failure this feature could cause, so it
            // gets a second way out that does not depend on a hook.
            enforcer_.CheckYield();
            return 0;
        }

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
        if (LOWORD(wparam) != WA_INACTIVE) {
            FocusActiveColumn();
        }
        return 0;

    case WM_SETFOCUS:
        FocusActiveColumn();
        return 0;

    case WM_SIZE:
        // The panels are sized off the client area, so they move with it.
        LayoutColumns();
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
            // The session really is ending. Tear down, then end the message loop
            // rather than sitting here torn-down but alive: Windows will kill us
            // shortly either way, but a process that returns from its own loop
            // releases the single-instance mutex instead of holding it while the
            // shell waits.
            Teardown();
            PostQuitMessage(0);
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
    app_bar_->Reposition(cfg_.ParsedEdge(), StripThickness(), monitor.bounds,
                         cfg_.push_taskbar);

    if (cfg_.push_taskbar) {
        taskbar_.Resolve(monitor.handle, monitor.bounds, cfg_.ParsedEdge());
        taskbar_.Apply(app_bar_->ReservedRect(), cfg_);
        // Explorer applies its relayout asynchronously, so reading the rect back
        // inline reports a stale value and a false failure.
        SetTimer(hwnd_, kTimerTaskbarVerify, 250, nullptr);
    }

    enforcer_.SetTarget(monitor.handle, monitor.bounds, app_bar_->ReservedRect(),
                        cfg_.ParsedEdge());

    if (cfg_.topmost) {
        ReassertTopmost();
    }

    LayoutColumns();

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
    case kMenuRestartShell: {
        // Checked BEFORE the call, not after. The menu runs inside a nested modal
        // loop, so a teardown can already have happened by the time the command
        // comes back, and restarting a terminal into a dock that is going away
        // is not something to discover afterwards.
        Column* column = ActiveColumn();
        if (!column || !column->terminal || shutting_down_) {
            break;
        }
        const int id = column->id;
        column->terminal->Restart();
        if (shutting_down_ || !FindColumn(id)) {
            break; // Restart pumps too, so re-check on the way out
        }
        // The old HWND is gone. Without this the enforcer keeps filtering
        // against a destroyed handle and stops recognising its own children.
        PublishOwnedWindows();
        fit_ticks_ = 0;
        SetTimer(hwnd_, kTimerFit, 250, nullptr);
        break;
    }
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
    // A reload dispatched after teardown would re-hook the desktop and re-create
    // brushes for a window that is gone.
    if (shutting_down_) {
        return;
    }

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

    if (was_blocking != cfg_.block_fullscreen || was_topmost != cfg_.topmost) {
        // The hooks are installed once at startup, so toggling this did nothing
        // in either direction until now. Topmost is in the condition because the
        // flyout yield is hooked for its sake alone: turning topmost on in a dock
        // that started with blockFullscreen off would otherwise leave nothing
        // listening.
        enforcer_.Stop();
        enforcer_.Start(hwnd_, cfg_);
        PublishOwnedWindows();
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

    // By value, so the thread never touches this object. See StopChannel.
    auto* channel = new StopChannel{hwnd_, stop_event_.Get(), stop_quit_event_.Get()};

    stop_thread_.Reset(CreateThread(nullptr, 0, StopChannelThread, channel, 0, nullptr));
    if (!stop_thread_) {
        delete channel;
    }
}

DWORD WINAPI DockWindow::StopChannelThread(LPVOID param)
{
    // Owned by this thread from here on. Leaked only on the path where the thread
    // never runs to completion, which is a process that is exiting anyway.
    std::unique_ptr<StopChannel> channel(static_cast<StopChannel*>(param));

    const HANDLE waits[2] = {channel->stop, channel->quit};

    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        // PostMessage rather than SendMessage: it is the cross-thread-safe one,
        // and a posted message is dispatched by the inner pump loops that
        // TrackPopupMenuEx and MessageBoxW run, so --stop is honoured even with
        // the tray menu open. If the window has since been destroyed this simply
        // fails, which is the point of holding an HWND rather than a pointer.
        PostMessageW(channel->hwnd, WM_APP_STOP_REQUESTED, 0, 0);
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

    // Every terminal is detached before anything can destroy this window. Each
    // is a WS_CHILD of its column panel, the panels are children of this window,
    // and a child is destroyed with its parent: letting that happen to a Windows
    // Terminal window living inside a process shared with the user's other
    // terminals would take all of them down.
    //
    // Release() but deliberately NOT reset(). Terminal::Start pumps the message
    // queue while it waits for a cold-starting terminal, so Teardown can be
    // re-entered from inside a Terminal method that is still on the stack:
    // tray Exit, --stop, or WM_CLOSE arriving during that window all land here.
    // Destroying the object then would free it underneath its own Start(), which
    // resumes and writes to the freed memory. Release() is idempotent and sets a
    // flag Start() checks after every pump, so it unwinds cleanly instead.
    //
    // The objects are destroyed with this window, after the message loop has
    // ended and no Start() can still be running. That covers the retired list
    // too, which is why nothing here touches it.
    for (const auto& column : columns_) {
        if (column->terminal) {
            column->terminal->Release();
        }
    }

    // The retired ones too. They were released when their column went, but a
    // release that lost a race with an embed still in flight would leave a
    // terminal parented to a hidden panel that is about to be destroyed with
    // this window. Release() is idempotent, so asking twice costs nothing.
    for (const auto& retired : retired_) {
        if (retired.terminal) {
            retired.terminal->Release();
        }
    }

    tray_.Destroy();

    if (app_bar_) {
        app_bar_->Remove();
        app_bar_.reset();
    }

    log::Write(L"teardown: complete");
}

} // namespace dock
