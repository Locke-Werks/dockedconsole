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

#include <string_view>

namespace dock::console {

/// The dock is a GUI-subsystem executable so that running it never opens a
/// console of its own. That also means its control commands have nowhere to
/// print. Borrowing the calling shell's console gives --stop, --reclaim and
/// --help somewhere to report.

/// Prints to the calling shell if there is one and stays SILENT otherwise.
///
/// Used by anything an installer hook can invoke. A hook has no console, and the
/// dialog fallback in Report would block until the hook's timeout expired and
/// fail the install with nobody there to click OK. The distinction between this
/// and Report is load-bearing; do not collapse them.
void Write(std::wstring_view message, bool error = false);

/// Prints to the calling shell, falling back to a dialog when there is none.
void Report(std::wstring_view message, bool error = false);

/// Flushes and releases the borrowed console. Safe to call when nothing was
/// ever attached.
void Detach();

} // namespace dock::console
