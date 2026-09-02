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

/// Panes visible in a Windows Terminal window, counted through UI Automation.
///
/// Windows Terminal publishes no pane count, but it does publish its panes: every
/// terminal control has an automation peer whose class name is "TermControl",
/// and a screen reader walks them. So can we. Only the active tab's visible panes
/// are in the tree, so the number can run low and never high, which is the safe
/// direction for a cap that otherwise only ever rises.
///
/// Returns -1 when the count could not be taken: UI Automation refused, the walk
/// timed out, or it found nothing at all, which for a window that is showing a
/// terminal means the walk never reached the XAML tree rather than that the
/// window is empty. The call goes into another process's UI thread, so the
/// timeout is the ceiling on how long a hung terminal can hold this one.
[[nodiscard]] int CountTerminalPanes(HWND terminal, DWORD timeout_ms = 3000);

} // namespace dock
