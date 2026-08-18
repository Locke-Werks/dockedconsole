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

#include "app_bar.h"
#include "config.h"
#include "enforcer.h"
#include "taskbar_claim.h"
#include "terminal.h"
#include "tray.h"
#include "win32.h" // Handle

#include <memory>

namespace dock {

/// The docked window itself.
///
/// It carries no chrome of its own and hosts the reparented console as a child,
/// so everything the user sees belongs to the console and everything the window
/// manager sees belongs to this window.
class DockWindow {
public:
    explicit DockWindow(Config cfg);
    ~DockWindow();

    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;

    /// Creates the window and registers the AppBar. False if either fails, in
    /// which case the caller should report and exit rather than run headless.
    bool Create(HINSTANCE instance);

    /// Pumps until the dock is asked to shut down. Returns the exit code.
    int Run();

    /// Run-once cleanup, reachable from every exit path.
    ///
    /// A missed ABM_REMOVE leaves the desktop work area shrunk with no window
    /// left to explain why, so this is idempotent and called from more places
    /// than strictly look necessary.
    void Teardown();

    /// Named Hwnd rather than Handle on purpose: a member function called
    /// Handle would hide the dock::Handle type inside this class scope, and the
    /// resulting errors point at the member declarations rather than at the
    /// name that caused them.
    [[nodiscard]] HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
    LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam);

    void Reposition();
    void ScheduleReposition();
    void StartTerminal();
    void OnTerminalClosed(const std::wstring& failure);
    void OnAppBarNotification(WPARAM notification, LPARAM lparam);
    void OnMenuCommand(UINT command);
    void OnReloadConfig();
    void OpenConfigFile();
    void ReassertTopmost();

    void InstallStopChannel();
    void StopChannelShutdown();

    /// Everything the wait thread needs, by value.
    ///
    /// It deliberately holds no pointer to the DockWindow. On the path where the
    /// thread does not exit in time, teardown leaks its handles and returns, and
    /// this object outlives the window: dereferencing a DockWindow there would be
    /// a use-after-free, whereas an HWND is just a number and PostMessage to a
    /// destroyed window merely fails.
    struct StopChannel {
        HWND hwnd = nullptr;
        HANDLE stop = nullptr;
        HANDLE quit = nullptr;
    };

    static DWORD WINAPI StopChannelThread(LPVOID param);

    /// Begins an orderly shutdown. The one supported way out.
    void RequestShutdown();

    Config cfg_;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;

    UINT appbar_message_ = 0;
    UINT taskbar_created_ = 0;

    std::unique_ptr<AppBar> app_bar_;
    std::unique_ptr<Terminal> terminal_;
    TaskbarClaim taskbar_;
    Enforcer enforcer_;
    Tray tray_;
    HBRUSH background_ = nullptr;

    int fit_ticks_ = 0;

    Handle stop_event_;
    Handle stop_quit_event_;
    Handle stop_thread_;

    bool shutting_down_ = false;
    bool torn_down_ = false;
    bool repositioning_ = false;
};

} // namespace dock
