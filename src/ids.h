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

/// Child of the host, one per column, and the reason a column can hold a
/// terminal at all: the tab strip is hidden by pushing the terminal up past the
/// top of its parent's client area, and children are clipped to their own
/// parent. A single host shared by every column would clip against the whole
/// strip, so on a top or bottom edge the second column's tab strip would show
/// through over the first.
inline constexpr wchar_t kColumnClass[] = L"DockedConsole.Column";

/// Asks a running dock to add a column. Registered rather than WM_APP+n because
/// it crosses processes: the second instance sends it and exits.
inline constexpr wchar_t kAddColumnMessageName[] = L"DockedConsole_AddColumn";

/// What the running dock answers. This crosses a process boundary between two
/// builds that need not be the same version, so the numbers are contract: add
/// to the end, never renumber. Zero is deliberately "not handled", because that
/// is what DefWindowProc returns in a build that predates the message and what a
/// message UIPI dropped looks like from the sender's side.
enum AddColumnResult : LRESULT {
    kAddColumnNotHandled = 0,
    kAddColumnAdded = 1,
    kAddColumnAtMax = 2,
    kAddColumnNoRoom = 3,
};

/// Prefix of a column's Windows Terminal window name. The rest is a token
/// unique to this dock process plus the column id: "dock-3fa91c02-2".
///
/// The token is not decoration. Release detaches a terminal without destroying
/// it, so a Windows Terminal window outlives the dock that made it whenever an
/// embed is abandoned. Without the token a later run would hand out a name that
/// stale window still answers to, and every pane meant for the dock would land
/// in a window sitting behind it.
inline constexpr wchar_t kWindowNamePrefix[] = L"dock-";

/// Identifies our WM_COPYDATA, so a stray one from anything else is ignored.
/// Bumped if SplitRequest ever changes shape, because the sender and the
/// receiver need not be the same build.
inline constexpr ULONG_PTR kSplitCopyDataId = 0x44434B31;   // 'DCK1'

/// A request to run something in a new pane of one of our columns.
///
/// Fixed header, then two NUL-terminated wide strings laid out after it. Every
/// offset and length is in WCHARs and is checked against cbData before a single
/// character is read: this arrives from a process we did not start.
struct SplitRequest {
    UINT32 version;      // 1
    UINT32 column_id;    // 0 means any column with room
    UINT32 cwd_offset;
    UINT32 cwd_length;
    UINT32 cmd_offset;
    UINT32 cmd_length;
};

inline constexpr UINT32 kSplitRequestVersion = 1;

/// What the dock answers a split request. Same contract discipline as
/// AddColumnResult: add to the end, never renumber, and zero stays "not
/// handled" because that is both what DefWindowProc returns in a build that
/// predates the message and what a UIPI-dropped message looks like.
enum SplitResult : LRESULT {
    kSplitNotHandled = 0,   // older build, dropped by UIPI, or shutting down
    kSplitAccepted = 1,     // queued for a column that already exists
    kSplitNewColumn = 2,    // a column was added and the pane is queued behind it
    kSplitAtMax = 3,        // every column is at its pane cap
    kSplitNoRoom = 4,       // the display cannot take another column
    kSplitRejected = 5,     // malformed request
    kSplitUnnamed = 6,      // naming is off, so no column can be addressed
};

// Private messages.
inline constexpr UINT WM_APP_TRAY = WM_APP + 1;
inline constexpr UINT WM_APP_STOP_REQUESTED = WM_APP + 2;
inline constexpr UINT WM_APP_TERMINAL_GONE = WM_APP + 3;

/// Posted to ourselves after a column has been reserved. The terminal launch is
/// deliberately not done in the handler that answers the add request: that
/// handler is running on a SendMessage from the process the user just started,
/// and a cold terminal takes seconds to appear.
inline constexpr UINT WM_APP_COLUMN_START = WM_APP + 4;

/// Posted after a split request has been accepted. Same reason as
/// WM_APP_COLUMN_START: the handler that answers is running on a SendMessage
/// from another process, and launching wt.exe there would block it.
inline constexpr UINT WM_APP_SPLIT_RUN = WM_APP + 6;

/// A column's terminal has gone. Posted, never called directly: the notification
/// arrives from inside the Terminal's own code, so the column cannot be taken
/// apart until that call has returned.
inline constexpr UINT WM_APP_COLUMN_GONE = WM_APP + 5;

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
