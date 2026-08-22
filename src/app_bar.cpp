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
#include "app_bar.h"

#include "diag_log.h"
#include "monitors.h"
#include "win32.h"

#include <shellapi.h>

namespace dock {
namespace {

UINT ToNativeEdge(DockEdge edge)
{
    switch (edge) {
    case DockEdge::Left:   return ABE_LEFT;
    case DockEdge::Top:    return ABE_TOP;
    case DockEdge::Bottom: return ABE_BOTTOM;
    case DockEdge::Right:  break;
    }
    return ABE_RIGHT;
}

} // namespace

RECT DesiredStrip(DockEdge edge, int thickness, const RECT& monitor)
{
    switch (edge) {
    case DockEdge::Left:
        return RECT{monitor.left, monitor.top, monitor.left + thickness, monitor.bottom};
    case DockEdge::Right:
        return RECT{monitor.right - thickness, monitor.top, monitor.right, monitor.bottom};
    case DockEdge::Top:
        return RECT{monitor.left, monitor.top, monitor.right, monitor.top + thickness};
    case DockEdge::Bottom:
        return RECT{monitor.left, monitor.bottom - thickness, monitor.right, monitor.bottom};
    }
    return monitor;
}

RECT PinThickness(DockEdge edge, int thickness, RECT rc)
{
    switch (edge) {
    case DockEdge::Left:   rc.right = rc.left + thickness;   break;
    case DockEdge::Right:  rc.left = rc.right - thickness;   break;
    case DockEdge::Top:    rc.bottom = rc.top + thickness;   break;
    case DockEdge::Bottom: rc.top = rc.bottom - thickness;   break;
    }
    return rc;
}

RECT SubtractStrip(const RECT& monitor, const RECT& strip, DockEdge edge)
{
    RECT out = monitor;
    switch (edge) {
    case DockEdge::Left:   out.left = strip.right;    break;
    case DockEdge::Right:  out.right = strip.left;    break;
    case DockEdge::Top:    out.top = strip.bottom;    break;
    case DockEdge::Bottom: out.bottom = strip.top;    break;
    }
    return out;
}

RECT ColumnSlot(DockEdge edge, const RECT& client, int index, int count)
{
    if (count <= 1 || index < 0 || index >= count) {
        return client;
    }

    const bool vertical = (edge == DockEdge::Left || edge == DockEdge::Right);
    const int span = vertical ? Width(client) : Height(client);

    // Offset of the i'th boundary from the low end of the span.
    const auto boundary = [span, count](int i) {
        return static_cast<int>(static_cast<long long>(span) * i / count);
    };

    int low = 0;
    int high = 0;
    if (edge == DockEdge::Left || edge == DockEdge::Top) {
        // Column 0 is at the low end, where the monitor edge is.
        low = boundary(index);
        high = boundary(index + 1);
    } else {
        low = span - boundary(index + 1);
        high = span - boundary(index);
    }

    if (vertical) {
        return RECT{client.left + low, client.top, client.left + high, client.bottom};
    }
    return RECT{client.left, client.top + low, client.right, client.top + high};
}

AppBar::AppBar(HWND hwnd, UINT callback_message)
    : hwnd_(hwnd), callback_message_(callback_message)
{
}

AppBar::~AppBar()
{
    Remove();
}

APPBARDATA AppBar::NewData() const
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    return data;
}

bool AppBar::Register()
{
    if (registered_) {
        return true;
    }

    APPBARDATA data = NewData();
    data.uCallbackMessage = callback_message_;

    if (SHAppBarMessage(ABM_NEW, &data) == 0) {
        return false;
    }

    registered_ = true;
    return true;
}

bool AppBar::Reregister()
{
    // The shell has restarted, so whatever it knew about us is gone. Clear the
    // flag rather than calling Remove, which would post an ABM_REMOVE for a
    // registration that no longer exists.
    registered_ = false;
    const bool ok = Register();
    log::Writef(L"appbar: re-registered after an explorer restart: %s",
                ok ? L"ok" : L"FAILED");
    return ok;
}

void AppBar::Reposition(DockEdge edge, int thickness_px, const RECT& monitor_bounds,
                        bool claim_full_edge)
{
    if (!registered_) {
        return;
    }

    const RECT wanted = DesiredStrip(edge, thickness_px, monitor_bounds);

    APPBARDATA data = NewData();
    data.uEdge = ToNativeEdge(edge);
    data.rc = wanted;

    // No ABM_QUERYPOS. See the header: that call is what made the taskbar win
    // the corner, because it trims the incoming rect around every appbar that
    // registered earlier and the taskbar always did.
    SHAppBarMessage(ABM_SETPOS, &data);

    if (claim_full_edge) {
        // Measured: the shell trims the long axis on SETPOS too, handing back a
        // rect that stops at the taskbar. Taking its answer here is what made
        // the dock 60px short at the bottom. Keep our own span; the taskbar gets
        // moved out of the way separately.
        if (!(data.rc == wanted)) {
            log::Writef(L"appbar: shell trimmed (%d,%d)-(%d,%d) to (%d,%d)-(%d,%d); "
                        L"keeping the full edge",
                        wanted.left, wanted.top, wanted.right, wanted.bottom,
                        data.rc.left, data.rc.top, data.rc.right, data.rc.bottom);
        }
        data.rc = wanted;
    } else {
        data.rc = PinThickness(edge, thickness_px, data.rc);
    }

    reserved_ = data.rc;

    SetWindowPos(hwnd_, nullptr, reserved_.left, reserved_.top,
                 Width(reserved_), Height(reserved_),
                 SWP_NOZORDER | SWP_NOACTIVATE);

    NotifyWindowPosChanged();
}

void AppBar::NotifyActivate()
{
    if (!registered_) {
        return;
    }
    APPBARDATA data = NewData();
    SHAppBarMessage(ABM_ACTIVATE, &data);
}

void AppBar::NotifyWindowPosChanged()
{
    if (!registered_) {
        return;
    }
    APPBARDATA data = NewData();
    SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
}

void AppBar::Remove()
{
    if (!registered_) {
        return;
    }

    registered_ = false;
    APPBARDATA data = NewData();
    SHAppBarMessage(ABM_REMOVE, &data);
}

void AppBar::ForceWorkAreaRecompute()
{
    static constexpr wchar_t kScratchClass[] = L"DockedConsole.Reclaim";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kScratchClass;
    RegisterClassExW(&wc); // harmless if it is already registered

    // A real window, off-screen and never shown. It must be a genuine top-level
    // window rather than message-only: SHAppBarMessage will not accept an
    // HWND_MESSAGE window, and the shell needs somewhere to send its
    // notifications.
    HWND scratch = CreateWindowExW(WS_EX_TOOLWINDOW, kScratchClass, L"", WS_POPUP,
                                   -32000, -32000, 1, 1, nullptr, nullptr,
                                   wc.hInstance, nullptr);
    if (!scratch) {
        return;
    }

    {
        AppBar bar(scratch, RegisterWindowMessageW(L"DockedConsoleReclaim"));
        if (bar.Register()) {
            const MonitorInfo monitor = PrimaryMonitor();
            // Polite on purpose: this is a throwaway 1px bar whose only job is
            // to make the shell recompute, so it has no corner to win.
            bar.Reposition(DockEdge::Right, 1, monitor.bounds, /*claim_full_edge=*/false);

            // Pump for a moment instead of sleeping. SHAppBarMessage is
            // synchronous against the shell, and the shell can SendMessage back
            // to us during the recompute; a window that is not pumping is a
            // hang waiting to happen.
            const ULONGLONG deadline = GetTickCount64() + 120;
            MSG msg;
            while (GetTickCount64() < deadline) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(10);
            }
        }
        // bar.Remove() runs here, in the destructor.
    }

    DestroyWindow(scratch);
    log::Write(L"reclaim: forced a work-area recompute");
}

} // namespace dock
