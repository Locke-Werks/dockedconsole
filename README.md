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

Docked Console takes the strip and keeps it. It registers a Windows **AppBar**,
which tells the shell to subtract that strip from the desktop work area outright,
so maximized windows stop at its edge. Then it deals with the thing an AppBar does
nothing about: fullscreen apps, which size themselves to the whole monitor and
paint over everything regardless of what the shell reserved.

It is a real Windows Terminal in there, with your profile, your fonts, your
colors, your prompt. Not a reimplementation, not a pale imitation with three
supported escape sequences.

```
┌──────────────────────────────────────────┬─────────────────┐
│                                          │ PS C:\> _       │
│   your maximized window stops here  ───► │                 │
│                                          │  a real         │
│   your fullscreen app gets pushed over   │  terminal       │
│                                          │                 │
│   it does not go under                   │  no chrome      │
│   it does not go over                    │  no close       │
│                                          │  no escape      │
├──────────────────────────────────────────┴─────────────────┤
│  taskbar, full width, untouched by default                 │
└────────────────────────────────────────────────────────────┘
```

## Install

Grab the signed installer from [Releases](https://github.com/Locke-Werks/dockedconsole/releases)
and run it. It is signed by Specter Point Intelligence, LLC, which is the parent
organization, so that publisher name is correct and not a mis-signing.

Nothing else is required. No runtime to chase down: it is a single native
executable of about 325 KB with the C runtime linked in, and it imports nothing
but Windows system libraries.

Or build it yourself:

```powershell
git clone https://github.com/Locke-Werks/dockedconsole
cd dockedconsole
cmake --preset vs
cmake --build --preset vs
.\build\vs\Debug\dockedconsole.exe
```

The `vs` preset uses the Visual Studio generator, so it works from any shell with
no `vcvars` to source. Needs Visual Studio 2022 with the C++ workload, and CMake
3.28 or newer.

## Use

Run it. It docks to the right edge at 735 physical pixels and gets out of your way
forever.

```powershell
dockedconsole.exe                     # dock
dockedconsole.exe --stop              # undock and exit
dockedconsole.exe --reclaim           # recover after a hard kill
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
process outright instead, `--reclaim` puts everything back.

## Configure

`dockedconsole.json`, in `%LOCALAPPDATA%\DockedConsole\`, or beside the executable
if you drop one there (portable mode wins). Tray → **Reload config** applies
geometry changes live.

| Key | Default | What it does |
|---|---|---|
| `edge` | `right` | `left`, `top`, `right`, `bottom` |
| `widthPhysicalPx` | `735` | Strip thickness in **physical** pixels, not scaled by DPI |
| `monitorDeviceName` | `null` | e.g. `\\.\DISPLAY2`, or null for primary |
| `terminalArgs` | `-w new` | wt.exe args. `-p "Profile"` to pick a profile |
| `chromeTrimPx` | `-1` | `-1` measures the tab strip at runtime |
| `elevation` | `auto` | `auto`, `never`, `always`. See below |
| `onShellExit` | `quit` | `quit` closes the dock, `relaunch` gives you a fresh shell |
| `backgroundColor` | `#0C0C0C` | Behind the terminal |
| `topmost` | `true` | |
| `blockFullscreen` | `true` | Push fullscreen windows out of the strip |
| `pushTaskbar` | `false` | Take the whole edge and clip the taskbar. See below |
| `fullscreenExclusions` | shell processes | Image names the fullscreen block leaves alone |

Width is physical pixels because the AppBar API is. At 125% scaling, 735 physical
is 588 logical.

The config parser accepts comments and trailing commas, because you are going to
hand-edit this file. A config it cannot parse is reported and **left alone**: it
is the only copy of something you typed, and a stray comma should not cost you
your settings.

## Requirements

Windows 11. Windows Terminal (which ships with it). That is the list.

## Four things that are less obvious than they look

**The tab strip is clipped, not disabled.** Windows Terminal draws its tabs inside
its own client area, so stripping `WS_CAPTION` does exactly nothing to them.
Instead the terminal is placed at `y = -chromeTrim` and made that much taller.
Child windows are clipped to the parent's client area, so the tab strip falls
outside the dock and ceases to exist as far as you are concerned. `chromeTrim` is
measured at runtime as the distance from the terminal's top edge to the bottom of
its `DRAG_BAR_WINDOW_CLASS` child, which absorbs both the 40 DIP tab row and the
one-pixel border above it at any DPI. It measures 51px on the machine this was
written on. Trimming the bar's *height* instead leaves a one-pixel scar along the
top, which is how this was found.

**The taskbar cannot be resized by anyone, which is why `pushTaskbar` is off.**
By default the dock stops where the taskbar begins and they sit side by side, the
way they always have. Setting `pushTaskbar` to `true` makes the dock take the
whole monitor edge, and then something has to give.

Four mechanisms were measured. Registering the AppBar without `ABM_QUERYPOS`
reserves the right work area but `ABM_SETPOS` still trims *our* rect at the
taskbar: a full-height claim came back 60 pixels short. `SetWindowPos` on
`Shell_TrayWnd` returns `TRUE` and changes nothing, because explorer rewrites the
geometry in its `WM_WINDOWPOSCHANGING` handler. Adding `SWP_NOSENDCHANGING` gets
past that and the rect really does change, but explorer re-applies its own layout
within about 50ms. `MoveWindow` and `SWP_ASYNCWINDOWPOS` fail outright.

What works is `SetWindowRgn`. Explorer's layout code sets position and size, not
the window region, so a clip survives indefinitely, and a window region bounds
hit-testing as well as painting, so the clipped strip stops taking clicks too.
The cost is that clipping does not reflow the taskbar's buttons: the last one is
cut off at the boundary rather than the row re-packing. Holding a real resize
would mean re-forcing faster than explorer relayouts, which is a permanent
flicker war for a cosmetic difference. Hence opt-in, and hence off.

**Fullscreen is a rect, not a window state.** Windows has no "fullscreen" flag to
ask about. An app goes fullscreen by sizing itself to the monitor, so the block is
a `SetWinEventHook` that notices when a window covers the display and moves it
into what is left. The test has to read `DWMWA_EXTENDED_FRAME_BOUNDS` rather than
`GetWindowRect`, and that is not a detail: since Windows 10 a resizable window's
rect includes an invisible border outside the visible frame, so an ordinary
**maximized** window reports `(-9,-9)-(2761,1981)` on a 2752x2032 display. It
overhangs the monitor on all four edges. A block built on `GetWindowRect` would
classify every maximized window as fullscreen and shove it, breaking the one
behaviour that already worked.

**Elevation is not a power grab, it is integrity-level matching.** Reparenting is
`SetParent`, and UIPI forbids a medium-integrity process from reparenting a window
owned by an elevated one. Because new terminal windows are born inside whichever
monarch already exists, *one* elevated terminal anywhere means the window you are
handed is elevated, `SetParent` fails, and the terminal is left floating with the
dock empty beside it. So `elevation: "auto"` checks for an elevated Windows
Terminal at startup and relaunches itself elevated only when that is the case. On
a machine with no elevated terminal, nothing elevates and nothing prompts. Set it
to `never` if you would rather be told than helped.

## What it cannot do

**DXGI exclusive fullscreen.** When an app calls
`IDXGISwapChain::SetFullscreenState(TRUE, ...)` it takes ownership of the display's
scanout and composition is bypassed for that output. The dock is not behind such a
window, it is absent from the pipeline, and no `SetWindowPos`, z-order call or
AppBar reservation changes that. The dock disappears until the app leaves exclusive
mode, then comes back. This is a kernel and display-driver boundary, not a window
manager one, and `uiAccess` would not help.

Borderless fullscreen, which is what browsers at F11, video players and most
modern game engines actually use, is handled. Exclusive fullscreen is now the
minority path, mostly older D3D11 titles and explicit "Exclusive Fullscreen"
options. A window that re-asserts its own rect faster than the dock corrects it
wins after five attempts in three seconds, by design: the alternative is a
flicker war that burns a core and helps nobody. It is logged when it happens.

## Verified, not asserted

Measured on a 2752x2032 display at 125% scaling with a 60px bottom taskbar.

| Check | Result |
|---|---|
| Work area while docked | 2017 x 1972, down from 2752 x 1972 |
| Dock window rect | `(2017,0)-(2752,1972)`, stopping at the taskbar |
| Taskbar, default | `(0,1972)-(2752,2032)`, untouched |
| Borderless fullscreen window | created 2752 wide, clamped to `(0,0)-(2017,2032)` |
| Fullscreen clamp, all four edges | right `(0,0)-(2017,2032)`, left `(735,0)-(2752,2032)`, top `(0,735)-(2752,2032)`, bottom `(0,0)-(2752,1237)` |
| Maximized window, `GetWindowRect` | `(-9,-9)-(2026,1981)`, overhanging the monitor on every edge |
| Maximized window, DWM frame | `(0,0)-(2017,1972)`, stopping at the dock, and correctly left alone |
| `WM_CLOSE`, `SC_CLOSE`, `SC_MINIMIZE`, `SC_MAXIMIZE`, `SC_MOVE` | all survived |
| `WS_EX_TOOLWINDOW` set, `WS_EX_APPWINDOW` clear | not in Alt+Tab, not in the taskbar |
| Typing `exit` | dock exits, work area restored, no orphaned shells |
| `--stop` | work area restored, any taskbar clip released, other terminal windows untouched |
| `--stop` during a cold start, at 6 points | no crash, no leaked strip, no leaked clip |
| 12 malformed configs, incl. 500 nested brackets and `1e999` | all rejected, file left on disk, defaults used |
| Enforcer CPU, idle | 0 ms over 5 s |
| Enforcer CPU, 2000 window moves in 1.5 s | 15.6 ms, about 1% of one core |
| With `pushTaskbar: true`, dock rect | `(2017,0)-(2752,2032)`, the full monitor height |
| With `pushTaskbar: true`, taskbar visible area | `(0,1972)-(2017,2032)`, stops at the dock |
| With `pushTaskbar: true`, clicks in the clipped strip | fall through to the dock, not to `Shell_TrayWnd` |
| Binary | 323 KB, imports only Windows system DLLs |

## License

GPLv3. See [LICENSE](LICENSE).

Copyright (C) 2026 Locke Werks.
