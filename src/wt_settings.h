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
#include <vector>

namespace dock {

struct Config;

/// Which of the dock's two wt invocations a question is about: the terminal a
/// column starts with, or a pane split into it later. They can name different
/// profiles.
enum class ProfileTarget { Column, Pane };

/// What Windows Terminal's own settings say about the profile it is going to
/// launch for us.
///
/// A profile can carry "elevate": true. Windows Terminal honours that by opening
/// the tab in an elevated instance of itself and closing the unelevated window
/// it opened first, so an unelevated dock that launched it finds its window gone
/// and its replacement out of reach. Reading the setting up front is the only
/// way to know that before a window has been opened that cannot be captured.
///
/// Measured, not assumed: the setting applies to the profile's own command
/// line. A window or pane given a command of its own on the wt command line
/// stays at the launcher's level, while one given only a directory, or
/// nothing, elevates. The prediction still follows the profile even for a
/// column that starts as a command, because the shell panes asked for later
/// carry no command, and Windows Terminal will not put an elevated pane inside
/// an unelevated window: it opens a new elevated window for it instead, and
/// the dock never sees that pane at all.
struct ProfileElevation {
    /// 1 when the profile elevates, 0 when it does not, -1 when that could not
    /// be determined. `detail` says why when it is -1.
    int elevates = -1;

    /// The settings.json that was read, empty when none was found.
    std::wstring settings_path;

    /// The resolved profile's name, or its GUID when it has none.
    std::wstring profile;

    /// How the profile was chosen, for a log line: "the default profile",
    /// "-p PowerShell in terminalArgs", "terminalProfile PowerShell".
    std::wstring selector;

    /// Anything a reader of the log would want to know: why the answer is
    /// unknown, or that the named profile was not found and the default stood
    /// in for it, which is what Windows Terminal itself does.
    std::wstring detail;
};

/// The settings.json Windows Terminal is using: the packaged stable build, then
/// Preview, then the unpackaged location. Empty when none exists.
[[nodiscard]] std::wstring TerminalSettingsPath();

/// Resolves the profile wt would launch for `target` the way Windows Terminal
/// resolves it, a name match first, then a GUID, then the default profile, and
/// reports whether it elevates. Never fails: an unreadable file or an unknown
/// profile comes back as -1 with the reason in `detail`.
///
/// Setting DOCKEDCONSOLE_NO_PROFILE_CHECK in the environment makes this answer
/// unknown without reading anything, so the path that recovers from a wrong
/// prediction can be exercised on a machine where the prediction is right.
[[nodiscard]] ProfileElevation PredictProfileElevation(const Config& cfg, ProfileTarget target);

/// One clause for a log line or a dialog: "profile 'PowerShell' (the default
/// profile) has \"elevate\": true".
[[nodiscard]] std::wstring DescribeProfileElevation(const ProfileElevation& elevation);

/// A profile as the taskbar jump list shows it.
struct ProfileEntry {
    std::wstring name;
    /// A local .ico, .exe or .dll the shell can draw, or empty. Windows Terminal
    /// also accepts emoji, ms-appx URIs and images, none of which a shell link
    /// can carry, so those come back empty and the app icon stands in.
    std::wstring icon;
};

/// Every profile Windows Terminal shows in its own new-tab menu, in its order:
/// named, and not marked "hidden". Empty when settings.json is missing or does
/// not parse; the jump list then simply has no profile entries.
[[nodiscard]] std::vector<ProfileEntry> ListProfiles();

} // namespace dock
