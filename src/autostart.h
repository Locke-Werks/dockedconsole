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

namespace dock::autostart {

/// The HKCU Run entry.
///
/// This is deliberately a command on the product rather than something the
/// installer writes directly. A machine-scope installer runs elevated, and under
/// over-the-shoulder UAC its HKCU is the administrator's, not the person
/// installing. The installer calls these through a hook declared `as = "user"`
/// so the entry lands in the right hive.
///
/// Neither of these may ever put a dialog on screen: the hook has no one to
/// dismiss it and would fail the install on its timeout.

bool Register(std::wstring& message);
bool Unregister(std::wstring& message);
[[nodiscard]] bool IsRegistered();

} // namespace dock::autostart
