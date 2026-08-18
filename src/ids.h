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

namespace dock {

/// Local\ scopes both to the interactive session, which is what we want: one
/// dock per logged-on user, not one per machine.
inline constexpr wchar_t kMutexName[] = L"Local\\DockedConsole.Instance";
inline constexpr wchar_t kStopEventName[] = L"Local\\DockedConsole.Stop";

inline constexpr wchar_t kWindowClass[] = L"DockedConsole.Host";
inline constexpr wchar_t kWindowTitle[] = L"Docked Console";

// Private messages.
inline constexpr UINT WM_APP_TRAY = WM_APP + 1;
inline constexpr UINT WM_APP_STOP_REQUESTED = WM_APP + 2;
inline constexpr UINT WM_APP_TERMINAL_GONE = WM_APP + 3;

// Timer ids. Restarting a live id resets its interval, which is how the
// reposition timer coalesces a burst of shell notifications into one call.
inline constexpr UINT_PTR kTimerReposition = 1;
inline constexpr UINT_PTR kTimerFit = 2;
inline constexpr UINT_PTR kTimerHealth = 3;
inline constexpr UINT_PTR kTimerTaskbarVerify = 4;
inline constexpr UINT_PTR kTimerEnforcePoll = 5;
inline constexpr UINT_PTR kTimerClampVerify = 6;
inline constexpr UINT_PTR kTimerTerminalStart = 7;

// Tray menu commands. TrackPopupMenuEx returns these directly, so they never
// travel as WM_COMMAND.
inline constexpr UINT kMenuRestartShell = 40001;
inline constexpr UINT kMenuReloadConfig = 40002;
inline constexpr UINT kMenuEditConfig = 40003;
inline constexpr UINT kMenuExit = 40004;

/// Icon resource id. Must be the lowest ICON id in the image, or Explorer picks
/// a different one for the file icon than the tray shows.
inline constexpr int kIconResourceId = 1;

} // namespace dock
