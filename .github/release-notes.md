Signed installer. Run it, and the terminal docks to the right edge of the primary display.

Fixed since 0.2.0: undocking logged a failure to detach the terminal on every clean exit. The check compared the window's parent against null, but a window still carrying `WS_CHILD` reports the desktop, so it could never pass. The detach itself always worked; only the check was wrong. It cost three retries and a misleading log line, nothing more.

Docked Console registers a Windows AppBar, so maximized windows stop at its edge instead of covering it. No title bar, no minimize, no close, no taskbar or Alt+Tab entry.

Rewritten in C++. One native executable of about 320 KB, no .NET runtime to install.

Fullscreen apps are pushed out of the strip instead of painting over it. Maximized windows already stopped at its edge; this covers the ones that size themselves to the whole monitor. DXGI exclusive fullscreen cannot be contained from user mode and is documented as a known limit.

Off by default, `pushTaskbar` clips the taskbar so it ends at the dock rather than running underneath it.

The conhost terminal path is gone; Windows Terminal only. Six conhost config keys were removed, and `blockFullscreen`, `pushTaskbar` and `fullscreenExclusions` added. A config that fails to parse is now reported and left alone instead of being overwritten with defaults.

Requires Windows 11.

Getting out: type `exit` in the shell, use the tray icon, or run `dockedconsole.exe --stop`.

The publisher on the signature is Specter Point Intelligence, LLC, the parent organisation. That is expected.
