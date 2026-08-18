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

#include <string>
#include <vector>

namespace dock {

enum class DockEdge { Left, Top, Right, Bottom };

enum class ElevationPolicy {
    /// Elevate only when the terminal we would embed is going to be elevated.
    Auto,
    /// Never elevate. Report and stop if the terminal turns out to be elevated.
    Never,
    /// Always elevate at startup.
    Always,
};

enum class ShellExitAction {
    /// Typing exit closes the dock, as it would close an ordinary terminal.
    Quit,
    /// Typing exit gets a fresh shell and the dock stays put.
    Relaunch,
};

/// How Load() got the values it is handing back. The caller decides what to say
/// and, importantly, whether writing the file back is safe.
enum class ConfigStatus {
    /// Parsed cleanly.
    Loaded,
    /// No file yet. Writing defaults is correct here and only here.
    Missing,
    /// The file exists but could not be read or parsed. Defaults are in memory
    /// and the file is LEFT ALONE, because it is the only copy of something a
    /// person typed and a stray comma should not cost them their settings.
    Unreadable,
};

struct Config {
    // -- geometry ----------------------------------------------------------
    std::wstring edge = L"right";

    /// Thickness of the reserved strip in PHYSICAL pixels. The AppBar API works
    /// in physical pixels, so this is not scaled by the monitor's DPI setting.
    int width_physical_px = 735;

    /// Device name such as \\.\DISPLAY1. Empty means the primary monitor.
    std::wstring monitor_device_name;

    // -- terminal ----------------------------------------------------------
    /// Command line for wt.exe. "-w new" forces a fresh window rather than a tab
    /// in an existing one. Add -p "Profile Name" to pick a specific profile.
    std::wstring terminal_args = L"-w new";

    /// Pixels of terminal chrome hidden above the top of the dock. -1 measures
    /// Windows Terminal's tab strip at runtime, which is right at any DPI.
    int chrome_trim_px = -1;

    /// auto, never or always. Embedding reparents the terminal's window, and
    /// UIPI forbids that across integrity levels, so "auto" elevates only when
    /// an elevated Windows Terminal is running and the window we are handed
    /// would therefore be elevated too.
    std::wstring elevation = L"auto";

    /// quit or relaunch. "quit" makes typing exit undock and close the whole
    /// thing, the way exit closes an ordinary terminal window.
    std::wstring on_shell_exit = L"quit";

    // -- appearance --------------------------------------------------------
    std::wstring background_color = L"#0C0C0C";

    bool topmost = true;

    // -- the screen block --------------------------------------------------
    /// Push fullscreen windows out of the strip instead of letting them cover
    /// it. Replaces yieldToFullscreenApps, which did the opposite.
    bool block_fullscreen = true;

    /// Take the whole monitor edge and clip the taskbar so it stops at the dock.
    ///
    /// Off by default, which leaves the layout the way it has always been: the
    /// dock stops where the taskbar begins and the taskbar keeps its full width.
    ///
    /// Windows does not allow the taskbar to be resized by anyone, so the only
    /// mechanism that works is SetWindowRgn, which clips it. Clipping does not
    /// reflow the taskbar's buttons, so the last one is cut off at the boundary
    /// rather than the row re-packing. That cosmetic cost is why this is opt-in
    /// rather than on. See taskbar_claim.h for what else was tried.
    bool push_taskbar = false;

    /// Process image names the fullscreen clamp leaves alone. Shell surfaces are
    /// excluded by window class in the enforcer regardless of this list, because
    /// that is correctness rather than preference.
    std::vector<std::wstring> fullscreen_exclusions{
        L"explorer.exe",
        L"ShellExperienceHost.exe",
        L"StartMenuExperienceHost.exe",
        L"SearchHost.exe",
        L"TextInputHost.exe",
        L"LockApp.exe",
        L"LogonUI.exe",
    };

    // -- derived -----------------------------------------------------------
    [[nodiscard]] DockEdge ParsedEdge() const;
    [[nodiscard]] ElevationPolicy ParsedElevation() const;
    [[nodiscard]] ShellExitAction ParsedOnShellExit() const;
    [[nodiscard]] COLORREF Background() const;

    /// wt.exe, an app execution alias in the per-user WindowsApps folder.
    [[nodiscard]] std::wstring ResolveTerminalPath() const;

    /// True when `image` is on the exclusion list, case-insensitively.
    [[nodiscard]] bool IsExcluded(std::wstring_view image) const;

    /// Keeps the strip from swallowing the monitor. A strip wider than half the
    /// display would leave nowhere to work, which is unrecoverable without
    /// editing the file by hand.
    void Clamp();

    // -- persistence -------------------------------------------------------
    /// Beside the executable when a config is already sitting there, which
    /// covers portable use and the development build. Otherwise the per-user
    /// data folder, because an installed copy lives in Program Files and a
    /// normal user cannot write to it.
    [[nodiscard]] static std::wstring FilePath();

    /// Never fails. On any problem the defaults come back and `status` says why.
    [[nodiscard]] static Config Load(ConfigStatus& status, std::wstring& detail);

    bool Save() const;
};

} // namespace dock
