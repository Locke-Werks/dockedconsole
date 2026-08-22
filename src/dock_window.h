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
#include <string>
#include <vector>

namespace dock {

/// The docked window itself.
///
/// It carries no chrome of its own and hosts the reparented console as a child,
/// so everything the user sees belongs to the console and everything the window
/// manager sees belongs to this window.
///
/// The strip is divided into one to three columns, each a panel window with its
/// own terminal in it. Starting the executable again while a dock is up adds a
/// column rather than refusing: the strip grows inboard by one more width, the
/// shell slides the desktop work area over to match, and the columns already on
/// screen do not move. A column whose shell exits takes its width back the same
/// way, and the last one to go takes the dock with it.
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
    /// One column: a panel window, and the terminal reparented into it.
    ///
    /// The panel is not decoration. Windows Terminal's tab strip is hidden by
    /// pushing the terminal up past the top of its parent's client area and
    /// letting the parent clip it, so every terminal needs a parent whose client
    /// area is its own column and nothing else.
    struct Column {
        int id = 0;
        HWND panel = nullptr;
        std::unique_ptr<Terminal> terminal;
        /// Why the terminal went, if it went badly. Read once, by the handler
        /// that takes the column down.
        std::wstring failure;
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
    LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam);

    /// The panels forward almost everything to DefWindowProc. They exist to clip
    /// and to paint, not to behave.
    static LRESULT CALLBACK ColumnProcThunk(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

    void Reposition();
    void ScheduleReposition();

    // -- columns -----------------------------------------------------------
    [[nodiscard]] int ColumnCount() const { return static_cast<int>(columns_.size()); }

    /// Total thickness of the reserved strip: one width per column.
    [[nodiscard]] int StripThickness() const;

    /// Answers the message a second instance sends. Reserves the column and
    /// returns immediately; the terminal is started from the posted follow-up,
    /// because the caller is blocked in SendMessage and a cold terminal takes
    /// seconds to appear.
    [[nodiscard]] LRESULT OnAddColumnRequest();

    /// Appends a column with its panel created but no terminal yet. Null if the
    /// panel could not be created.
    [[nodiscard]] Column* AppendColumn();

    void StartColumn(int id);
    void OnColumnGone(int id);
    void OnColumnTerminalClosed(int id, const std::wstring& failure);
    void RemoveColumn(int id);

    /// Moves every panel onto its slice of the client area and re-fits the
    /// terminal inside it.
    void LayoutColumns();
    void FitColumns();

    /// Hands the enforcer the current panel and terminal handles so it keeps
    /// rejecting our own windows on its cheapest tier.
    void PublishOwnedWindows();

    /// Destroys what a removed column left behind, once its terminal is no
    /// longer inside its own Start(). See Teardown for why that matters.
    void DrainRetiredColumns();

    [[nodiscard]] Column* FindColumn(int id);
    [[nodiscard]] Column* FindColumnByPanel(HWND panel);
    [[nodiscard]] Column* ActiveColumn();
    void FocusActiveColumn();

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
    UINT add_column_message_ = 0;

    std::unique_ptr<AppBar> app_bar_;

    /// Held behind a unique_ptr so a Column's address survives a different
    /// column being added or removed while this one's terminal is still
    /// starting, which pumps the message queue and can dispatch either.
    std::vector<std::unique_ptr<Column>> columns_;

    /// A column that has been taken out of the layout but not yet taken apart.
    ///
    /// The panel waits with the terminal rather than being destroyed on the
    /// spot, because destroying a window destroys its children: if the terminal
    /// were mid-embed when the column went, its window could still become a
    /// child of this panel a moment after Release() detached it.
    struct Retired {
        std::unique_ptr<Terminal> terminal;
        HWND panel = nullptr;
    };
    std::vector<Retired> retired_;

    int next_column_id_ = 1;

    /// The column the tray's "Restart shell" acts on and the one focus goes to.
    int active_column_id_ = 0;

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
