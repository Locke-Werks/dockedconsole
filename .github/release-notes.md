Signed installer. Run it, and the terminal docks to the right edge of the primary display.

Fixed since 0.2.1: the notification area's overflow, the panel behind the taskbar chevron, opened underneath the dock. It is not a topmost window and never needed to be, because it opens above the taskbar rather than over it, so it had nothing to outrank. A topmost dock standing on that corner of the screen covered it outright, and nothing in the ordinary z-order band can rise above a topmost window. The visible half of that was a flyout that never appeared. The half that bit was the clicks meant for its icons landing on the dock instead, which left an app whose only interface is a tray icon unreachable. The dock now steps out of the topmost band while that flyout is open and returns when it closes.

The README gained the Windows Terminal keybindings worth knowing in a strip this shape. The tab strip is clipped out of the dock, so tabs are driven from the command palette and from Ctrl+Alt+n, and panes are what a tall narrow window is actually good for.

Docked Console registers a Windows AppBar, so maximized windows stop at its edge instead of covering it. No title bar, no minimize, no close, no taskbar or Alt+Tab entry.

One native executable of about 330 KB, no .NET runtime to install.

Fullscreen apps are pushed out of the strip instead of painting over it. Maximized windows already stopped at its edge; this covers the ones that size themselves to the whole monitor. DXGI exclusive fullscreen cannot be contained from user mode and is documented as a known limit.

Off by default, `pushTaskbar` clips the taskbar so it ends at the dock rather than running underneath it.

Requires Windows 11 and the Windows Terminal that ships with it.

Getting out: type `exit` in the shell, use the tray icon, or run `dockedconsole.exe --stop`.

The publisher on the signature is Specter Point Intelligence, LLC, the parent organisation. That is expected.
