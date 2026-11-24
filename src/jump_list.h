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

namespace dock::jump_list {

/// The AppUserModelID the installer stamps on the Start menu shortcut. The
/// process has to claim the same one, or a pinned icon and the running window
/// are two different apps to the taskbar and the pinned one has no jump list.
inline constexpr wchar_t kAppUserModelId[] = L"LockeWerks.DockedConsole";

/// Claims kAppUserModelId for this process. Before any window exists.
void ClaimAppId();

/// Rewrites the taskbar jump list: one entry per Windows Terminal profile, each
/// starting the dock with that profile or adding a pane of it to the running
/// dock. Failures are logged and otherwise ignored; a dock with a stale or
/// empty jump list still works.
void Publish();

} // namespace dock::jump_list
