Signed installer. Run it, and the terminal docks to the right edge of the primary display.

New in 0.3.0: columns. Running `dockedconsole.exe` while a dock is already up used to put a dialog on screen saying so and exit. It now adds a column instead. The strip grows inboard by another `widthPhysicalPx`, the shell slides the desktop work area over to match, and the column already on screen does not move. Three is the ceiling, and a column is also refused when another one would leave less than 320 physical pixels of desktop. A refusal is reported from the notification area rather than a dialog, and the copy you launched exits with code 5.

Each column is a separate Windows Terminal with its own tabs and panes. Typing `exit` in one closes that column and gives its width back, and the surviving columns close the gap. The last column to close still takes the dock with it, so a single-column dock behaves exactly as it did. The tray icon, `--stop` and the AppBar registration are all still one per dock, not one per column.

Docked Console registers a Windows AppBar, so maximized windows stop at its edge instead of covering it. No title bar, no minimize, no close, no taskbar or Alt+Tab entry.

One native executable of about 330 KB, no .NET runtime to install.

Fullscreen apps are pushed out of the strip instead of painting over it. Maximized windows already stopped at its edge; this covers the ones that size themselves to the whole monitor. DXGI exclusive fullscreen cannot be contained from user mode and is documented as a known limit.

Off by default, `pushTaskbar` clips the taskbar so it ends at the dock rather than running underneath it.

Requires Windows 11 and the Windows Terminal that ships with it.

Getting out: type `exit` in the last shell, use the tray icon, or run `dockedconsole.exe --stop`.

The publisher on the signature is Specter Point Intelligence, LLC, the parent organisation. That is expected.
