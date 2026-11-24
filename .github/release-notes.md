Signed installer. Run it, and the terminal docks to the right edge of the primary display.

New in 0.5.0: the taskbar icon has a jump list. Right-click it for one entry per Windows Terminal profile; picking one starts the dock with that profile, or adds a pane of it to the dock already running. `--pane-profile <name>` does the same from a command line, and the `WM_COPYDATA` request gains an optional profile in version 2 of its block, with version 1 still accepted.

Fixed in 0.4.2: a cold start no longer needs an elevated terminal to already be open. When the profile Windows Terminal launches carries `"elevate": true`, it opens the tab in an elevated instance and closes the window it opened first, so an unelevated dock found its window gone and could not take the replacement. `elevation: "auto"` now reads the profile it is going to get, the `-p` in `terminalArgs` or the default, plus `terminalProfile` and `profiles.defaults`, and elevates up front when that profile elevates. When the prediction still misses, the failure names the profile instead of reporting a dead window handle. `--explain-elevation` prints the decision without starting anything.

Also fixed in 0.4.2: the pane count is checked against the terminal before a column is called full. The count only ever rose, so a column whose panes had been closed by hand looked full for good and every later request opened a new column. The dock now asks the terminal how many panes it is showing, through UI Automation, and lowers the count to match; "Restart shell" resets it, which the README had claimed and the code had not done. A pane split in after start-up is re-fitted for long enough to catch Windows Terminal moving the window, and a terminal that drifts afterwards is put back within a second. `--pane-dir` and `--pane-run` against a running dock ask it for a pane instead of being silently dropped. The log records the command every column and pane was asked to run, with the exact `wt` line.

New in 0.4.0: another program can ask for a pane. Send the dock a `WM_COPYDATA` carrying a working directory and a command line, and it splits one of its columns and runs that there. A column with room takes a horizontal split, a full column gets a new column, and three columns at their cap refuse. The reply says which of those happened. Three panes per column is the ceiling. [ProjectMan](https://github.com/Locke-Werks/ProjectMan) uses this to put a Claude Code session in the dock instead of opening a window of its own; the block layout, the reply codes and the rule that they are additive and never renumbered are in the README.

The dock runs the `wt` command itself rather than handing the window name out, and that is the point. Windows Terminal keeps elevated and unelevated windows in separate monarch worlds, so a program at a different integrity level cannot address the dock's window. Aiming `wt -w <name>` at it from the wrong level does not fail: Windows Terminal makes a new window carrying that name, which then owns the name for good and swallows every later pane meant for that column. `WM_COPYDATA` is allowed through UIPI explicitly, so the request itself crosses levels fine.

Also new in 0.4.0: `--pane-dir` and `--pane-run` make the first column start as that command in that directory rather than as a shell, so a dock started for one job does not open an unwanted shell above it. `nameWindows`, on by default, gives each column's window a name so panes can be addressed to it; turning it off disables pane requests and says so.

Fixed in 0.4.1: a pane request carrying a directory but no command. That is how another program asks for an ordinary shell that happens to open in the right place, and 0.4.0 rejected it outright, appended a bare `--` to the `wt` command line, and dropped `-d` whenever there was no command to go with it. All three are corrected, so a directory and a command are now independent. A request carrying neither is still refused, because it asks for nothing.

Columns, from 0.3.0. Running `dockedconsole.exe` while a dock is already up used to put a dialog on screen saying so and exit. It adds a column instead. The strip grows inboard by another `widthPhysicalPx`, the shell slides the desktop work area over to match, and the column already on screen does not move. Three is the ceiling, and a column is also refused when another one would leave less than 320 physical pixels of desktop. A refusal is reported from the notification area rather than a dialog, and the copy you launched exits with code 5.

Each column is a separate Windows Terminal with its own tabs and panes. Typing `exit` in one closes that column and gives its width back, and the surviving columns close the gap. The last column to close still takes the dock with it, so a single-column dock behaves exactly as it did. The tray icon, `--stop` and the AppBar registration are all still one per dock, not one per column.

Docked Console registers a Windows AppBar, so maximized windows stop at its edge instead of covering it. No title bar, no minimize, no close, no taskbar or Alt+Tab entry.

One native executable of about 330 KB, no .NET runtime to install.

Fullscreen apps are pushed out of the strip instead of painting over it. Maximized windows already stopped at its edge; this covers the ones that size themselves to the whole monitor. DXGI exclusive fullscreen cannot be contained from user mode and is documented as a known limit.

Off by default, `pushTaskbar` clips the taskbar so it ends at the dock rather than running underneath it.

Requires Windows 11 and the Windows Terminal that ships with it.

Getting out: type `exit` in the last shell, use the tray icon, or run `dockedconsole.exe --stop`.

The publisher on the signature is Specter Point Intelligence, LLC, the parent organisation. That is expected.
