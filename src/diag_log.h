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

#include <string>
#include <string_view>

namespace dock {

struct Config;

/// Append-only diagnostic log.
///
/// Embedding depends on the state of another process's window at a moment we do
/// not control, and the interesting failures happen on machines that are not
/// this one. A screenshot of a dialog cannot say which window was found, what
/// its integrity level was, or whether SetParent was refused or quietly undone
/// afterwards. This can.
///
/// Best effort throughout: logging must never be the reason the dock fails to
/// start, so every function here swallows its own failures.
namespace log {

/// %LOCALAPPDATA%\DockedConsole\dockedconsole.log
[[nodiscard]] const std::wstring& Path();

void Write(std::wstring_view message);

/// printf-style, for the call sites that would otherwise build a string by hand.
void Writef(const wchar_t* format, ...);

/// Called once at startup so every log opens with the environment it ran in.
void WriteBanner(const Config& cfg);

/// "yes" / "no" / "unknown", for the tri-state elevation queries.
[[nodiscard]] const wchar_t* Describe(int tristate);

} // namespace log
} // namespace dock
