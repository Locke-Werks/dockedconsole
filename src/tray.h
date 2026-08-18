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
#include <shellapi.h> // NOTIFYICONDATAW

#include <string_view>

namespace dock {

/// The dock has no chrome, so the notification area is where its controls live.
class Tray {
public:
    Tray() = default;
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool Create(HWND host);

    /// Idempotent.
    void Destroy();

    /// Re-adds the icon after an explorer restart. Without this the icon is gone
    /// for good and the only remaining ways out are typing exit and --stop.
    void Readd();

    /// Handles the tray callback message. Returns the chosen menu command, or 0.
    [[nodiscard]] UINT OnCallback(WPARAM wparam, LPARAM lparam);

    /// Balloon notification. Silently does nothing when the icon is not up.
    void Notify(std::wstring_view message);

    /// True while the context menu is on screen.
    ///
    /// Showing that menu requires SetForegroundWindow on the host, which fires
    /// WM_ACTIVATE, whose handler would otherwise punt focus into the terminal
    /// and dismiss the menu the user just opened.
    [[nodiscard]] bool MenuIsUp() const { return menu_up_; }

private:
    [[nodiscard]] NOTIFYICONDATAW BaseData() const;
    [[nodiscard]] UINT ShowMenu();

    HWND host_ = nullptr;
    HICON icon_ = nullptr;
    bool added_ = false;
    bool menu_up_ = false;
};

/// The broadcast explorer sends when it restarts and every tray icon must be
/// re-added. Registered once; the value is stable for the session.
[[nodiscard]] UINT TaskbarCreatedMessage();

} // namespace dock
