Signed installer. Run it, and the terminal docks to the right edge of the primary display.

New in 0.4.0: another program can ask for a pane. Send the dock a `WM_COPYDATA` carrying a working directory and a command line, and it splits one of its columns and runs that there. A column with room takes a horizontal split, a full column gets a new column, and three columns at their cap refuse. The reply says which of those happened. Three panes per column is the ceiling. [ProjectMan](https://github.com/Locke-Werks/ProjectMan) uses this to put a Claude Code session in the dock instead of opening a window of its own; the block layout, the reply codes and the rule that they are additive and never renumbered are in the README.

The dock runs the `wt` command itself rather than handing the window name out, and that is the point. Windows Terminal keeps elevated and unelevated windows in separate monarch worlds, so a program at a different integrity level cannot address the dock's window. Aiming `wt -w <name>` at it from the wrong level does not fail: Windows Terminal makes a new window carrying that name, which then owns the name for good and swallows every later pane meant for that column. `WM_COPYDATA` is allowed through UIPI explicitly, so the request itself crosses levels fine.

Also new in 0.4.0: `--pane-dir` and `--pane-run` make the first column start as that command in that directory rather than as a shell, so a dock started for one job does not open an unwanted shell above it. `nameWindows`, on by default, gives each column's window a name so panes can be addressed to it; turning it off disables pane requests and says so.

Columns, from 0.3.0. Running `dockedconsole.exe` while a dock is already up used to put a dialog on screen saying so and exit. It adds a column instead. The strip grows inboard by another `widthPhysicalPx`, the shell slides the desktop work area over to match, and the column already on screen does not move. Three is the ceiling, and a column is also refused when another one would leave less than 320 physical pixels of desktop. A refusal is reported from the notification area rather than a dialog, and the copy you launched exits with code 5.

Each column is a separate Windows Terminal with its own tabs and panes. Typing `exit` in one closes that column and gives its width back, and the surviving columns close the gap. The last column to close still takes the dock with it, so a single-column dock behaves exactly as it did. The tray icon, `--stop` and the AppBar registration are all still one per dock, not one per column.

Docked Console registers a Windows AppBar, so maximized windows stop at its edge instead of covering it. No title bar, no minimize, no close, no taskbar or Alt+Tab entry.

One native executable of about 330 KB, no .NET runtime to install.

Fullscreen apps are pushed out of the strip instead of painting over it. Maximized windows already stopped at its edge; this covers the ones that size themselves to the whole monitor. DXGI exclusive fullscreen cannot be contained from user mode and is documented as a known limit.

Off by default, `pushTaskbar` clips the taskbar so it ends at the dock rather than running underneath it.

Requires Windows 11 and the Windows Terminal that ships with it.

Getting out: type `exit` in the last shell, use the tray icon, or run `dockedconsole.exe --stop`.

The publisher on the signature is Specter Point Intelligence, LLC, the parent organisation. That is expected.
