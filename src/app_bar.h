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
#include <shellapi.h> // APPBARDATA

#include "config.h"

namespace dock {

/// Wraps SHAppBarMessage. Registering an AppBar makes the shell subtract the
/// reserved rect from the desktop work area, so maximized windows stop at its
/// edge instead of covering it. This is the same mechanism the taskbar uses.
class AppBar {
public:
    AppBar(HWND hwnd, UINT callback_message);
    ~AppBar();

    AppBar(const AppBar&) = delete;
    AppBar& operator=(const AppBar&) = delete;

    [[nodiscard]] bool IsRegistered() const { return registered_; }

    /// The rect we asked the shell for, in physical pixels. This is the strip
    /// the window occupies and the rect the enforcer defends.
    [[nodiscard]] const RECT& ReservedRect() const { return reserved_; }

    bool Register();

    /// Re-registers after an explorer restart. The registration lives inside
    /// explorer, so it dies with it and the strip silently stops reserving
    /// anything until this runs.
    bool Reregister();

    /// Claims the monitor edge and moves the window onto it.
    ///
    /// Deliberately does NOT call ABM_QUERYPOS. That call asks the shell where
    /// we may sit given every appbar already registered, and since the taskbar
    /// is an appbar, it is exactly the step that made the dock yield the corner.
    ///
    /// Dropping it is necessary but not sufficient, which was worth measuring
    /// rather than assuming. On Windows 11, ABM_SETPOS trims the rect it is
    /// handed anyway: a full-height right-edge claim on a 2752x2032 display came
    /// back as 735x1972, stopping exactly at the top of the taskbar. So when
    /// `claim_full_edge` is set we keep our own span and let TaskbarClaim push
    /// the taskbar out of the corner. Without that second half the two windows
    /// would simply overlap.
    void Reposition(DockEdge edge, int thickness_px, const RECT& monitor_bounds,
                    bool claim_full_edge);

    void NotifyActivate();
    void NotifyWindowPosChanged();

    /// Idempotent. A leaked registration leaves the desktop work area
    /// permanently shrunk, so this has to be reachable from every exit path.
    void Remove();

    /// Recovery for a hard kill. If a previous run died without unregistering,
    /// the shell may still be holding a reserved strip for a window that no
    /// longer exists. Registering and immediately removing a throwaway AppBar
    /// forces a work-area recompute, during which the shell prunes registrations
    /// whose window is gone.
    static void ForceWorkAreaRecompute();

private:
    [[nodiscard]] APPBARDATA NewData() const;

    HWND hwnd_;
    UINT callback_message_;
    bool registered_ = false;
    RECT reserved_{};
};

/// The full-span rect for an edge, before the shell has had any say.
[[nodiscard]] RECT DesiredStrip(DockEdge edge, int thickness, const RECT& monitor);

/// Re-pins the thickness after the shell has returned a rect. SETPOS may adjust
/// the docked edge, and without this the bar drifts to whatever thickness the
/// shell felt like handing back.
[[nodiscard]] RECT PinThickness(DockEdge edge, int thickness, RECT rc);

/// `monitor` minus the reserved strip. What a fullscreen window gets clamped to.
[[nodiscard]] RECT SubtractStrip(const RECT& monitor, const RECT& strip, DockEdge edge);

} // namespace dock
