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
    /// window_name is substituted for {window} in Config::terminal_args, giving
    /// this column's Windows Terminal window a name another process can address
    /// with `wt -w <name>`. Passed at construction because it is fixed for the
    /// life of the column; a setter would only invite it arriving late, after
    /// Launch has already built its command line.
    Terminal(HWND host, const Config& cfg, std::wstring window_name);
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// Reports a non-fatal problem, for a tray balloon.
    std::function<void(const std::wstring&)> on_fault;

    /// The terminal window closed. The argument is empty for an ordinary exit,
    /// or a message when it died so soon after starting that it looks like a
    /// launch failure rather than the user typing exit.
    std::function<void(const std::wstring&)> on_closed;

    /// The -w name this terminal's window answers to. Empty when naming is off,
    /// which is the one case where nothing can be added to it afterwards.
    [[nodiscard]] const std::wstring& WindowName() const { return window_name_; }

    /// Adds a pane below the focused one, running `command` in `cwd`.
    ///
    /// Runs wt.exe from THIS process, which is the whole point. Windows
    /// Terminal keeps elevated and unelevated windows in separate monarch
    /// worlds, so only a process at the same integrity level as the window we
    /// embedded can reach it. A medium-integrity caller aiming at an elevated
    /// window does not get an error, it gets a brand new window carrying our
    /// name, which then owns that name for good.
    /// An empty profile falls back to terminalProfile, then wt's default.
    [[nodiscard]] bool SplitPane(const std::wstring& cwd, const std::wstring& command,
                                 const std::wstring& profile, std::wstring& error);

    /// Run this instead of a plain shell when the terminal starts.
    ///
    /// Set before Start(), and kept across a Restart so a relaunched column
    /// comes back as what it was rather than reverting to a shell.
    void SetInitialCommand(std::wstring cwd, std::wstring command, std::wstring profile);

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

    /// Panes the embedded window is showing, or -1 when that cannot be told.
    /// Asks the terminal through UI Automation, which is the only place Windows
    /// Terminal publishes them; see pane_probe.h for what the number can miss.
    [[nodiscard]] int LivePaneCount() const;

    /// True when the terminal is still our child and sits exactly where Fit()
    /// put it. A split, a summon or the terminal's own start-up can move it,
    /// and a moved terminal shows its tab strip or leaves a gap at the bottom.
    [[nodiscard]] bool IsFitted() const;

    /// True while Start() is on the stack. Start pumps the message queue, so a
    /// message dispatched from inside it can decide to throw this object away,
    /// and destroying it there frees the Start() that is still running. Callers
    /// that destroy a Terminal on a message have to wait for this to go false.
    [[nodiscard]] bool Starting() const { return starting_; }

    /// Detaches the terminal window and asks it to close.
    ///
    /// Detaching first is not tidiness. A child window is destroyed with its
    /// parent, and letting that happen to a Windows Terminal window inside a
    /// shared process would tear it down without any chance to shut its panes
    /// down cleanly, taking the user's other terminals with it.
    void Release();

private:
    [[nodiscard]] bool Launch();
    [[nodiscard]] HWND LocateWindow(const std::vector<HWND>& existing, DWORD& owner_pid,
                                    ULONGLONG timeout_ms);

    enum class Ready { Settled, Vanished, TimedOut };
    [[nodiscard]] Ready WaitUntilReady(HWND hwnd);

    [[nodiscard]] std::wstring Embed(HWND child, DWORD owner_pid);
    void ReassertChildState();
    [[nodiscard]] int ChromeTrim();

    HWND host_;
    const Config& cfg_;
    std::wstring window_name_;
    std::wstring initial_cwd_;
    std::wstring initial_command_;
    std::wstring initial_profile_;

    HWND terminal_ = nullptr;

    /// What the terminal's frame looked like before Embed took it apart, so
    /// Release can put back exactly that rather than inventing a frame.
    LONG_PTR original_style_ = 0;
    LONG_PTR original_ex_ = 0;

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
