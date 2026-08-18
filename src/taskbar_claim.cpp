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
#include "taskbar_claim.h"

#include "app_bar.h"
#include "diag_log.h"
#include "monitors.h"
#include "win32.h"

#include <shellapi.h>

namespace dock {
namespace {

constexpr wchar_t kPrimaryClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryClass[] = L"Shell_SecondaryTrayWnd";

/// A few pixels of slack, so a rounding difference is not read as an overlap.
constexpr int kOverlapTolerance = 2;

bool TaskbarAutoHides()
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    // Auto-hide is a global setting on Windows 11, so one query answers for
    // every bar.
    return (SHAppBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE) != 0;
}

void CollectTaskbars(std::vector<HWND>& out)
{
    if (HWND primary = FindWindowW(kPrimaryClass, nullptr)) {
        out.push_back(primary);
    }
    HWND next = nullptr;
    while ((next = FindWindowExW(nullptr, next, kSecondaryClass, nullptr)) != nullptr) {
        out.push_back(next);
    }
}

/// Clears any window region, so the taskbar paints and accepts input over its
/// whole rect again. Harmless on a window that never had one.
void ClearRegion(HWND hwnd)
{
    SetWindowRgn(hwnd, nullptr, TRUE);
}

} // namespace

DockEdge EdgeOf(const RECT& rect, const RECT& monitor)
{
    // Whichever monitor edge the bar is flush against. A taskbar is always
    // docked to one, and it is wider than it is tall or the other way round,
    // which disambiguates the corners.
    if (Width(rect) >= Height(rect)) {
        return (rect.top - monitor.top) <= (monitor.bottom - rect.bottom)
                   ? DockEdge::Top
                   : DockEdge::Bottom;
    }
    return (rect.left - monitor.left) <= (monitor.right - rect.right) ? DockEdge::Left
                                                                     : DockEdge::Right;
}

TaskbarClaim::~TaskbarClaim()
{
    Restore();
}

void TaskbarClaim::Resolve(HMONITOR monitor, const RECT& monitor_bounds,
                           DockEdge dock_edge)
{
    // Reposition runs on every shell notification, and re-resolving each time
    // would release and re-apply the clip on every pass, which the user sees as
    // a flicker along the taskbar. Nothing here changes unless the monitor, the
    // edge or the set of taskbar windows does.
    if (monitor == resolved_monitor_ && dock_edge == dock_edge_
        && monitor_bounds == monitor_bounds_ && !bars_.empty()) {
        bool all_alive = true;
        for (const Bar& bar : bars_) {
            if (!IsWindow(bar.hwnd)) {
                all_alive = false;
                break;
            }
        }
        if (all_alive) {
            return;
        }
    }

    // Anything already clipped is released before the handles are discarded.
    Restore();

    bars_.clear();
    resolved_monitor_ = monitor;
    monitor_bounds_ = monitor_bounds;
    dock_edge_ = dock_edge;

    std::vector<HWND> candidates;
    CollectTaskbars(candidates);

    const bool auto_hide = TaskbarAutoHides();

    for (HWND hwnd : candidates) {
        // Ownership is geometric. MonitorFromWindow uses the largest-intersection
        // rule, which stays correct even after we have clipped a bar.
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != monitor) {
            continue;
        }

        Bar bar;
        bar.hwnd = hwnd;
        bar.cls = ClassNameOf(hwnd);
        if (!GetWindowRect(hwnd, &bar.original)) {
            continue;
        }
        bar.edge = EdgeOf(bar.original, monitor_bounds);
        bar.auto_hide = auto_hide;
        bars_.push_back(std::move(bar));
    }

    log::Writef(L"taskbar: %zu bar(s) on this monitor, autoHide=%d",
                bars_.size(), auto_hide ? 1 : 0);
}

void TaskbarClaim::Apply(const RECT& strip, const Config& cfg)
{
    if (!cfg.push_taskbar || disabled_this_session_) {
        return;
    }

    for (Bar& bar : bars_) {
        if (!IsWindow(bar.hwnd)) {
            continue;
        }

        if (bar.auto_hide) {
            // An auto-hide bar reserves nothing and slides in and out on its
            // own. Clipping it means fighting the animation, and losing that
            // fight leaves it stuck half-visible.
            continue;
        }

        if (bar.edge == dock_edge_) {
            // Same edge: there is nowhere to push it. The shell stacks the two
            // and that is the right answer.
            continue;
        }

        RECT now{};
        if (!GetWindowRect(bar.hwnd, &now)) {
            continue;
        }

        const RECT wanted = SubtractStrip(now, strip, dock_edge_);
        if (IsEmptyRect(wanted)) {
            continue;
        }

        if (wanted == now) {
            // No overlap. Make sure we are not still holding a clip from a
            // previous geometry.
            if (bar.clipped) {
                ClearRegion(bar.hwnd);
                bar.clipped = false;
            }
            continue;
        }

        // A window region is in WINDOW-relative coordinates, not screen ones.
        const RECT relative{wanted.left - now.left, wanted.top - now.top,
                            wanted.right - now.left, wanted.bottom - now.top};

        HRGN region = CreateRectRgn(relative.left, relative.top,
                                    relative.right, relative.bottom);
        if (!region) {
            continue;
        }

        // SetWindowRgn takes ownership on success, so the region must not be
        // deleted afterwards; on failure it is still ours.
        if (SetWindowRgn(bar.hwnd, region, TRUE) == 0) {
            DeleteObject(region);
            log::Writef(L"taskbar: SetWindowRgn refused for %s (%lu)",
                        bar.cls.c_str(), GetLastError());
            continue;
        }

        bar.clipped = true;
        bar.clip = wanted;

        log::Writef(L"taskbar: clipped %s from (%d,%d)-(%d,%d) to (%d,%d)-(%d,%d)",
                    bar.cls.c_str(), now.left, now.top, now.right, now.bottom,
                    wanted.left, wanted.top, wanted.right, wanted.bottom);
    }
}

bool TaskbarClaim::Verify(const RECT& strip) const
{
    for (const Bar& bar : bars_) {
        if (!IsWindow(bar.hwnd) || bar.auto_hide || bar.edge == dock_edge_) {
            continue;
        }

        // The visible area is the window rect intersected with its region, so
        // read the region back rather than the window rect: clipping leaves the
        // rect alone by design.
        RECT visible{};
        HRGN probe = CreateRectRgn(0, 0, 0, 0);
        if (!probe) {
            continue;
        }

        const int kind = GetWindowRgn(bar.hwnd, probe);
        if (kind == ERROR) {
            // No region at all: the whole window rect is visible.
            if (!GetWindowRect(bar.hwnd, &visible)) {
                DeleteObject(probe);
                continue;
            }
        } else {
            RECT box{};
            GetRgnBox(probe, &box);
            RECT window{};
            if (!GetWindowRect(bar.hwnd, &window)) {
                DeleteObject(probe);
                continue;
            }
            // Region coordinates are window-relative; put them back on screen.
            visible = RECT{window.left + box.left, window.top + box.top,
                           window.left + box.right, window.top + box.bottom};
        }
        DeleteObject(probe);

        RECT overlap{};
        if (IntersectRect(&overlap, &visible, &strip)
            && Width(overlap) > kOverlapTolerance
            && Height(overlap) > kOverlapTolerance) {
            return false;
        }
    }
    return true;
}

void TaskbarClaim::Restore()
{
    for (Bar& bar : bars_) {
        if (!bar.clipped || !IsWindow(bar.hwnd)) {
            continue;
        }
        ClearRegion(bar.hwnd);
        bar.clipped = false;
        log::Writef(L"taskbar: released the clip on %s", bar.cls.c_str());
    }
}

int TaskbarClaim::RestoreAllFromGeometry(std::wstring& report)
{
    // Clearing a window region is unconditional and safe: removing one that was
    // never set does nothing. That makes recovery after a hard kill trivial and,
    // more importantly, correct without any record of what a dead process did.
    std::vector<HWND> candidates;
    CollectTaskbars(candidates);

    int cleared = 0;
    for (HWND hwnd : candidates) {
        HRGN probe = CreateRectRgn(0, 0, 0, 0);
        if (!probe) {
            continue;
        }
        const bool had_region = GetWindowRgn(hwnd, probe) != ERROR;
        DeleteObject(probe);

        if (!had_region) {
            continue;
        }

        ClearRegion(hwnd);
        ++cleared;
        report += L"Released a leftover clip on " + ClassNameOf(hwnd) + L".\r\n";
    }

    if (cleared == 0) {
        report = L"The taskbar was not clipped; nothing to release.\r\n";
    }
    return cleared;
}

} // namespace dock
