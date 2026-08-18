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
#include <string_view>
#include <vector>

namespace dock {

/// Replaces System.Windows.Forms.Screen.
///
/// Every rect here is physical pixels, because the process is PerMonitorV2 from
/// the manifest. That is what SHAppBarMessage wants, so no conversion happens
/// anywhere in this program and none should be introduced.
struct MonitorInfo {
    HMONITOR handle = nullptr;

    /// rcMonitor: the whole display.
    RECT bounds{};

    /// rcWork: what the shell has left after every registered appbar, including
    /// the taskbar and, once we are running, ourselves.
    RECT work{};

    /// Adapter name such as \\.\DISPLAY1. Matches Screen.DeviceName exactly, so
    /// a monitorDeviceName written by the C# version still resolves.
    std::wstring device;

    bool primary = false;
};

[[nodiscard]] std::vector<MonitorInfo> EnumerateMonitors();

/// The monitor named by `device`, or the primary when the name is empty or no
/// longer matches anything. A display that has been unplugged since the config
/// was written must not leave the dock with nowhere to go.
[[nodiscard]] MonitorInfo ResolveMonitor(std::wstring_view device);

[[nodiscard]] MonitorInfo PrimaryMonitor();

/// Fills `out` from a handle. False if the handle is stale, which happens across
/// a display change.
bool DescribeMonitor(HMONITOR handle, MonitorInfo& out);

} // namespace dock
