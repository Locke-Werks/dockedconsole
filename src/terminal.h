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

#include "config.h"

#include <functional>
#include <string>
#include <vector>

namespace dock {

/// Launches Windows Terminal and reparents its window into the dock.
///
/// The window is created by the WT "monarch" process, which is very often an
/// ALREADY RUNNING WindowsTerminal.exe hosting the user's other windows. Killing
/// that process would take their other terminals down with it, so this class
/// never kills anything: it closes its own window and nothing else. Health is
/// tracked by watching the window handle, not a process.
class Terminal {
public:
    Terminal(HWND host, const Config& cfg);
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// Reports a non-fatal problem, for a tray balloon.
    std::function<void(const std::wstring&)> on_fault;

    /// The terminal window closed. The argument is empty for an ordinary exit,
    /// or a message when it died so soon after starting that it looks like a
    /// launch failure rather than the user typing exit.
    std::function<void(const std::wstring&)> on_closed;

    /// Launches and embeds. Safe to call again after the terminal has gone.
    void Start();

    /// Detaches the current terminal and starts a fresh one.
    void Restart();

    /// Sizes the terminal to fill the dock's client area.
    void Fit();

    /// Called on the health timer. Returns false once the terminal is gone and
    /// the owner should act on it.
    void CheckHealth();

    void FocusTerminal();

    [[nodiscard]] HWND Hwnd() const { return terminal_; }

    /// Detaches the terminal window and asks it to close.
    ///
    /// Detaching first is not tidiness. A child window is destroyed with its
    /// parent, and letting that happen to a Windows Terminal window inside a
    /// shared process would tear it down without any chance to shut its panes
    /// down cleanly, taking the user's other terminals with it.
    void Release();

private:
    [[nodiscard]] bool Launch();
    [[nodiscard]] HWND LocateWindow(const std::vector<HWND>& existing, DWORD& owner_pid);
    void WaitUntilReady(HWND hwnd);
    [[nodiscard]] std::wstring Embed(HWND child, DWORD owner_pid);
    void ReassertChildState();
    [[nodiscard]] int ChromeTrim();

    HWND host_;
    const Config& cfg_;

    HWND terminal_ = nullptr;
    int chrome_trim_ = -1;
    ULONGLONG embedded_at_ = 0;
    ULONGLONG last_start_ = 0;
    int rapid_failures_ = 0;
    bool starting_ = false;
    bool released_ = false;
};

/// Distance from the top of a Windows Terminal window to the bottom of its drag
/// bar, or 0 if the bar is not there yet.
///
/// Deliberately not the drag bar's own height: WT's window rect starts one pixel
/// above the bar, and trimming only the bar's height leaves that pixel row
/// showing along the top of the dock.
[[nodiscard]] int MeasureChrome(HWND terminal);

} // namespace dock
