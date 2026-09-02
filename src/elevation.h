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

struct Config;

/// Integrity level matching.
///
/// Embedding works by reparenting the terminal's window, and UIPI forbids a
/// medium-integrity process from calling SetParent on a window owned by an
/// elevated one. That is not hypothetical here: `wt -w new` is serviced by
/// whichever Windows Terminal "monarch" process already exists, so if ANY
/// elevated terminal is running, the window we are handed belongs to it and an
/// unelevated dock cannot touch it. SetParent fails, the terminal is left
/// floating, and the dock has nothing to show.
///
/// So the dock matches the terminal instead of fighting it: if the window is
/// going to be elevated, relaunch elevated first. On a machine with no elevated
/// terminal, nothing elevates and no prompt appears.

/// Marks a relaunch so the new instance waits for the old one's mutex.
inline constexpr wchar_t kRelaunchFlag[] = L"--elevated-relaunch";

/// Asks whether our own token is unfiltered.
///
/// Not literally the same question as WindowsPrincipal.IsInRole(Administrator),
/// which the C# version asked: that reports whether the Administrators SID is
/// present and enabled. TokenIsElevated reports whether the token is unfiltered,
/// which is the question that actually decides whether SetParent will work.
[[nodiscard]] bool IsCurrentProcessElevated();

/// Tri-state, because "could not query" is genuinely not "not elevated" and the
/// embed-failure message depends on telling them apart.
/// Returns 1 for elevated, 0 for not, -1 for unknown.
[[nodiscard]] int IsProcessElevated(DWORD pid);

/// True when a Windows Terminal process is running elevated. Any such process
/// can be the monarch that services our window request. The pid of the first
/// one found is reported when asked, for the log.
[[nodiscard]] bool AnyElevatedTerminalRunning(DWORD* pid = nullptr);

/// What to do about integrity levels before a window is opened.
///
/// The window we are about to be handed is going to be elevated in two cases:
/// an elevated Windows Terminal is already running and will service the
/// request, or the profile Windows Terminal is going to launch for us carries
/// "elevate": true, in which case it opens the tab in an elevated instance and
/// closes the window it opened first. Either way an unelevated dock cannot
/// capture the result, so under "auto" it relaunches itself elevated and under
/// "never" it refuses up front rather than opening a window it can never take.
struct ElevationDecision {
    bool elevate = false;
    bool blocked = false;

    /// Why the window would be elevated, for the log and the refusal dialog.
    /// Empty when nothing requires it.
    std::wstring reason;
};

[[nodiscard]] ElevationDecision DecideElevation(const Config& cfg);

/// Restarts this executable elevated, passing the original arguments through.
/// False if the user declined the prompt, which is a decision rather than an
/// error; `message` explains which happened.
bool Relaunch(const std::vector<std::wstring>& args, std::wstring& message);

} // namespace dock
