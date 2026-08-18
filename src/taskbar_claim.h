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
#pragma once

#include <windows.h>

#include "config.h"

#include <string>
#include <vector>

namespace dock {

/// Stops the taskbar running underneath the dock, by clipping it.
///
/// Three mechanisms were measured on Windows 11 26200 before settling on this
/// one, because the obvious two do not work:
///
///  - Registering our AppBar without ABM_QUERYPOS reserves the correct work
///    area, but ABM_SETPOS still trims OUR rect at the taskbar and the taskbar
///    window never moves. A full-height right-edge claim on a 2752x2032 display
///    came back as 735x1972 with Shell_TrayWnd still spanning all 2752.
///  - SetWindowPos on Shell_TrayWnd returns TRUE and changes nothing: explorer
///    rewrites the geometry in its WM_WINDOWPOSCHANGING handler. Adding
///    SWP_NOSENDCHANGING gets past that and the rect really does change, but
///    explorer re-applies its own layout within about 50ms. MoveWindow and
///    SWP_ASYNCWINDOWPOS fail outright. Holding it would mean re-forcing faster
///    than explorer relayouts, which is a flicker war nobody wins.
///  - SetWindowRgn clips the window instead of moving it. Explorer's layout code
///    sets position and size, not the region, so the clip survives. A window
///    region bounds hit-testing as well as painting, so the clipped strip stops
///    accepting clicks too and they fall through to the dock.
///
/// The cost of clipping rather than resizing is that the taskbar does not reflow
/// its contents, so with enough windows open a button can be cut off at the
/// boundary rather than moved. That is the honest trade for a taskbar the shell
/// will not let anyone resize.
///
/// Cleanup is the most important thing in this file. A leaked AppBar
/// registration self-heals on the next work-area recompute; a leaked clip does
/// NOT, and leaves a permanently truncated taskbar with nothing on screen to
/// explain it. Every exit path clears it, and --reclaim clears it with no state
/// at all from the process that set it.
class TaskbarClaim {
public:
    TaskbarClaim() = default;
    ~TaskbarClaim();

    TaskbarClaim(const TaskbarClaim&) = delete;
    TaskbarClaim& operator=(const TaskbarClaim&) = delete;

    /// Finds the taskbars belonging to `monitor` and records their original
    /// rects. Safe to call again; re-resolving after an explorer restart is
    /// required, because the old handles are meaningless by then.
    void Resolve(HMONITOR monitor, const RECT& monitor_bounds, DockEdge dock_edge);

    /// Clips any taskbar that overlaps `strip` back out of it. No-op when the
    /// taskbar auto-hides, sits on the dock's own edge, or the config forbids it.
    void Apply(const RECT& strip, const Config& cfg);

    /// True when no taskbar's VISIBLE area overlaps the strip any more. Reads
    /// the window region, not the window rect: clipping leaves the rect alone by
    /// design, so a rect-based check would always report failure.
    [[nodiscard]] bool Verify(const RECT& strip) const;

    /// Releases every clip. Idempotent.
    void Restore();

    /// Recovery with no prior state, for --reclaim after a hard kill. Clearing a
    /// window region is unconditional and safe, so this needs no record of what
    /// the dead process did and works against a crash of any build.
    static int RestoreAllFromGeometry(std::wstring& report);

private:
    struct Bar {
        HWND hwnd = nullptr;
        std::wstring cls;
        RECT original{};
        RECT clip{};
        DockEdge edge = DockEdge::Bottom;
        bool auto_hide = false;
        bool clipped = false;
    };

    std::vector<Bar> bars_;
    HMONITOR resolved_monitor_ = nullptr;
    RECT monitor_bounds_{};
    DockEdge dock_edge_ = DockEdge::Right;
    bool disabled_this_session_ = false;
};

/// The edge of `monitor` that `rect` hugs.
[[nodiscard]] DockEdge EdgeOf(const RECT& rect, const RECT& monitor);

} // namespace dock
