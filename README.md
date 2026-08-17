<div align="center">

<img src="assets/dockedconsole.ico" width="96" alt="Docked Console">

# Docked Console

**A terminal that owns its strip of desktop and does not negotiate.**

[![release](https://img.shields.io/github/v/release/Locke-Werks/dockedconsole?style=flat-square&color=d6262a)](https://github.com/Locke-Werks/dockedconsole/releases)
[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%2011-d6262a?style=flat-square)](#requirements)

</div>

---

You know the drill. You snap a terminal to the right edge. It looks great for
about eleven seconds. Then you maximize a browser and Windows cheerfully paints
straight over it, because as far as the shell is concerned that terminal is just
another window sitting in a place it happens to like.

"Always on top" does not fix this. Always on top keeps the pixels visible while
everything else still maximizes *underneath*, so you get a terminal hovering over
a window whose top-right corner is now unreachable. Congratulations, you have
invented a floating obstruction.

Docked Console does the thing the taskbar does. It registers a Windows **AppBar**,
which tells the shell to subtract that strip from the desktop work area outright.
Maximized windows stop at its edge because, as far as they know, the desktop ends
there. Nothing goes over it. Nothing hides under it. There is no title bar, no
minimize button, no close button, and no entry in Alt+Tab or the taskbar.

It is a real Windows Terminal in there, with your profile, your fonts, your
colors, your prompt. Not a reimplementation, not a pale imitation with three
supported escape sequences.

```
┌──────────────────────────────────────────┬─────────────────┐
│                                          │ PS C:\> _       │
│   your maximized window stops here  ───► │                 │
│                                          │  a real         │
│   it does not go under                   │  terminal       │
│   it does not go over                    │                 │
│   it does not know this strip exists     │  no chrome      │
│                                          │  no close       │
│                                          │  no escape      │
└──────────────────────────────────────────┴─────────────────┘
                                            ▲
                                            └── subtracted from the work area
```

## Install

Grab the signed installer from [Releases](https://github.com/Locke-Werks/dockedconsole/releases)
and run it. It is signed by Specter Point Intelligence, LLC, which is the parent
organization, so that publisher name is correct and not a mis-signing.

Nothing else is required. No .NET runtime to chase down: the binary is
self-contained.

Or build it yourself, which takes about four seconds:

```powershell
git clone https://github.com/Locke-Werks/dockedconsole
cd dockedconsole
dotnet publish -c Release -o dist
.\dist\dockedconsole.exe
```

## Use

Run it. It docks to the right edge at 735 physical pixels and gets out of your way
forever.

```powershell
dockedconsole.exe                     # dock
dockedconsole.exe --stop              # undock and exit
dockedconsole.exe --reclaim           # recover space after a hard kill
dockedconsole.exe --help              # the rest

dockedconsole.exe --width 900         # try a width without saving it
dockedconsole.exe --edge left         # try the other side
```

### Getting out

There is no close button, on purpose. Three ways out:

- Type `exit` in the shell, like any other terminal
- Tray icon → **Undock and exit**
- `dockedconsole.exe --stop`

All three unregister the AppBar and give you your desktop back. If you kill the
process outright instead, `--reclaim` forces the shell to recompute the work area.

## Configure

`dockedconsole.json`, in `%LOCALAPPDATA%\DockedConsole\`, or beside the executable
if you drop one there (portable mode wins). Tray → **Reload config** applies
geometry changes live.

| Key | Default | What it does |
|---|---|---|
| `edge` | `right` | `left`, `top`, `right`, `bottom` |
| `widthPhysicalPx` | `735` | Strip thickness in **physical** pixels, not scaled by DPI |
| `monitorDeviceName` | `null` | e.g. `\\.\DISPLAY2`, or null for primary |
| `terminal` | `windowsterminal` | or `conhost`, if you enjoy suffering |
| `terminalArgs` | `-w new` | wt.exe args. `-p "Profile"` to pick a profile |
| `chromeTrimPx` | `-1` | `-1` measures the tab strip at runtime |
| `elevation` | `auto` | `auto`, `never`, `always`. See below |
| `onShellExit` | `quit` | `quit` closes the dock, `relaunch` gives you a fresh shell |
| `backgroundColor` | `#0C0C0C` | Behind the terminal |
| `topmost` | `true` | |
| `yieldToFullscreenApps` | `true` | Gets out of the way of fullscreen apps, like the taskbar |
| `shell` / `shellArgs` / `fontFace` / `fontSizePx` / `utf8Console` | | conhost path only |

Width is physical pixels because the AppBar API is. At 125% scaling, 735 physical
is 588 logical.

## Requirements

Windows 11. Windows Terminal (which ships with it). That is the list.

## Three things that are less obvious than they look

**The tab strip is clipped, not disabled.** Windows Terminal draws its tabs inside
its own client area, so stripping `WS_CAPTION` does exactly nothing to them.
Instead the terminal is placed at `y = -chromeTrim` and made that much taller.
Child windows are clipped to the parent's client area, so the tab strip falls
outside the dock and ceases to exist as far as you are concerned. `chromeTrim` is
measured at runtime as the distance from the terminal's top edge to the bottom of
its `DRAG_BAR_WINDOW_CLASS` child, which absorbs both the 40 DIP tab row and the
one-pixel border above it at any DPI. Trimming the bar's *height* instead leaves a
one-pixel scar along the top, which is how this was found.

**The terminal process is usually not ours, and killing it would be rude.**
`wt -w new` asks the Windows Terminal "monarch" for a window, and the monarch is
very often an already-running `WindowsTerminal.exe` that also owns every other
terminal you have open. Killing that process to clean up would take all of them
with it. So the Windows Terminal path never kills a process: teardown detaches the
window with `SetParent(hwnd, null)` and posts `WM_CLOSE` to that window alone.
Health checks watch the window handle, not a process.

**Elevation is not a power grab, it is integrity-level matching.** Reparenting is
`SetParent`, and UIPI forbids a medium-integrity process from reparenting a window
owned by an elevated one. Because new terminal windows are born inside whichever
monarch already exists, *one* elevated terminal anywhere means the window you are
handed is elevated, `SetParent` fails, and the terminal is left floating with the
dock empty beside it. So `elevation: "auto"` checks for an elevated Windows
Terminal at startup and relaunches itself elevated only when that is the case. On
a machine with no elevated terminal, nothing elevates and nothing prompts. Set it
to `never` if you would rather be told than helped; you will get a dialog
explaining the situation instead of a broken dock.

## Verified, not asserted

Measured on a 2752x2032 display at 125% scaling with a 60px bottom taskbar:

| Check | Result |
|---|---|
| Work area while docked | 2017 x 1972, down from 2752 x 1972 |
| Maximized window visible bounds | `(0,0)-(2017,1972)`, stops exactly at the dock |
| `WS_CAPTION` / `WS_SYSMENU` / `WS_THICKFRAME` | all absent |
| `WS_EX_TOOLWINDOW` set, `WS_EX_APPWINDOW` clear | not in Alt+Tab, not in the taskbar |
| `WM_CLOSE`, `SC_CLOSE`, `SC_MINIMIZE` | all survived |
| Typing `exit` | dock exits, work area restored, no orphaned shells |
| `--stop` | work area restored, other terminal windows untouched |
| Hard kill | shell reclaims the strip on next recompute |

`WM_CLOSE` is refused in `WndProc` rather than in `OnFormClosing`, because WinForms
only reports `CloseReason.UserClosing` for `SC_CLOSE`. A `WM_CLOSE` posted directly
arrives as `CloseReason.None` and walks straight past a reason-based guard. That
one was found by a test, not by reading.

## Why not conhost

There is a `terminal: "conhost"` path. It works. You will not enjoy it:

- **No font fallback.** The single configured face must contain every glyph you
  emit or you get tofu boxes.
- **No characters outside the Basic Multilingual Plane. Ever.** conhost stores one
  UTF-16 code unit per cell, so a surrogate pair is split across two cells and
  renders as two boxes. Nerd Fonts v3 moved its Material Design icons to U+F0001
  and up, which is precisely that range, so a modern prompt cannot render there at
  any font size with any font.

It exists as a fallback. Use Windows Terminal.

## License

GPLv3. See [LICENSE](LICENSE).

Copyright (C) 2026 Locke Werks.
