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
#include "monitors.h"

#include "win32.h"

namespace dock {
namespace {

BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param)
{
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(param);
    MonitorInfo info;
    if (DescribeMonitor(handle, info)) {
        out->push_back(std::move(info));
    }
    return TRUE;
}

} // namespace

bool DescribeMonitor(HMONITOR handle, MonitorInfo& out)
{
    if (!handle) {
        return false;
    }

    // cbSize must be sizeof(MONITORINFOEXW) and the cast is required. Get either
    // wrong and the call fails silently, leaving a zeroed struct that reads as a
    // 0x0 monitor at the origin.
    MONITORINFOEXW info{};
    info.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(handle, reinterpret_cast<LPMONITORINFO>(&info))) {
        return false;
    }

    out.handle = handle;
    out.bounds = info.rcMonitor;
    out.work = info.rcWork;
    out.device = info.szDevice;
    out.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    return true;
}

std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> found;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                        reinterpret_cast<LPARAM>(&found));
    return found;
}

MonitorInfo PrimaryMonitor()
{
    MonitorInfo info;
    const POINT origin{0, 0};
    if (DescribeMonitor(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), info)) {
        return info;
    }

    // No display at all. Hand back something with a non-empty rect so callers
    // that divide by the width do not have to special-case it.
    info.bounds = RECT{0, 0, 1, 1};
    info.work = info.bounds;
    return info;
}

MonitorInfo ResolveMonitor(std::wstring_view device)
{
    if (!device.empty()) {
        for (auto& monitor : EnumerateMonitors()) {
            if (EqualsNoCase(monitor.device, device)) {
                return monitor;
            }
        }
    }

    return PrimaryMonitor();
}

} // namespace dock
