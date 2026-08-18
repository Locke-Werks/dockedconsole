# Contributing

## Building

Needs Visual Studio 2022 with the C++ workload, CMake 3.28 or newer, and
Windows 11.

```powershell
cmake --preset vs        # works from any shell, no vcvars needed
cmake --build --preset vs
```

Four presets. `vs`, `ci` and `release` name no generator at all, so CMake picks
the newest Visual Studio installed and that generator locates its own toolset:
they work from any shell with no `vcvars` to source, on whatever VS you have.
`dev` uses Ninja for faster incremental builds and does need an x64 Native Tools
prompt, because Ninja will not find `cl.exe` on its own.

`ci` is what CI runs and turns on `/WX`, so build it before opening a pull
request rather than finding out from the runner:

```powershell
cmake --preset ci
cmake --build --preset ci
```

Leaving the generator unset is not laziness, it is the fix for two separate
failures. A GitHub `windows-latest` runner has neither `cl.exe` nor `ninja` on
`PATH` in a plain step, so a Ninja preset fails there with `CMAKE_CXX_COMPILER
not set`. Naming a specific Visual Studio year fails differently and later:
`Visual Studio 17 2022` stopped resolving the day `windows-latest` became
`windows-2025-vs2026`, with `could not find any instance of Visual Studio`.
Letting CMake choose survives both. The architecture is left unset for the same
reason: VS generators already default to the host platform, and `CMakeLists.txt`
fails the configure outright if the result is ever not x64.

Whichever VS it picks, the generator is multi-config, which is why
`cmake --install` on those build trees needs `--config`.

The build is x64 only and links the C runtime statically, so the shippable
artifact is one file of about 325 KB that imports nothing but Windows system
libraries. CI asserts that: if a change reintroduces a `VCRUNTIME` import, the
build fails rather than shipping something that needs a redistributable.

## Conventions

### Code

- C++20, `/W4 /permissive-`, four spaces, braces on their own line.
- `PascalCase` for functions and types, `snake_case_` for private members,
  `kConstantCase` for constants. Everything lives in `namespace dock`.
- There is no marshalling layer to centralise, so Win32 is called directly where
  it is used. The rule that survives from the C# version is the next one.
- **Every call that can fail gets its result checked**, and where the return
  value is ambiguous, check the thing itself. `SetParent` returns null both for
  failure and for a window that had no parent, so the test is `GetParent`. This
  class of thing is the entire project.
- Comments explain why, not what. If a comment restates the line below it, delete
  it. If a line looks wrong until you know a Win32 rule, write the rule down. If
  a line exists because three other approaches were measured and failed, write
  down what failed.

### DPI

Everything is physical pixels. The AppBar API is physical, the process is
PerMonitorV2 from `app.manifest`, and any coordinate that reaches
`SHAppBarMessage` or `SetWindowPos` must not have been through a DPI-unaware
round trip. If you add a measurement, take it from a PerMonitorV2 context.

This bites in testing more than in the product: a PowerShell script that has not
called `SetProcessDpiAwarenessContext` reads virtualised coordinates and every
number it prints will be wrong by the scale factor.

### Things that will bite you

- Windows Terminal's tab strip is client-drawn. Window styles will not remove it.
- The terminal window is usually owned by a pre-existing `WindowsTerminal.exe`
  shared with the user's other windows. Never kill that process, and never let
  `DestroyWindow` on the host reach it as a child: detach it first.
- `SetParent` across integrity levels is blocked by UIPI.
- `GetWindowRect` on a maximized window overhangs the monitor by the invisible
  resize border, about 9 physical pixels per side at 125%. Anything asking "does
  this window cover the display" must read `DWMWA_EXTENDED_FRAME_BOUNDS`.
- `WS_CLIPCHILDREN` on the host is load-bearing. Without it the host erases over
  the embedded terminal and it strobes on every resize.
- `GetMessageW` returns `-1` on error, and `-1` is truthy.
- A leaked AppBar registration shrinks the desktop with no window left to explain
  it. Every exit path must reach `ABM_REMOVE`. The same goes for a taskbar clip,
  which is worse because it does not self-heal.

### Teardown, honestly

`TerminateProcess` runs no code, and this program deliberately swallows
`WM_CLOSE`, so Task Manager's "End task" escalates to exactly that. There is no
in-process guarantee against a hard kill and the C# version's `ProcessExit`
handler never provided one either.

What exists instead: `Teardown()` is idempotent and reachable from every intended
exit, startup runs a work-area recompute that prunes a dead predecessor's
registration, and `--reclaim` recovers both the reserved strip and a leftover
taskbar clip from geometry alone, with no state from the process that left them.

### Commits

Imperative mood, concise subject line, body only when the change needs
explanation.

```
Refuse WM_CLOSE in WndProc

There is no close button, and a WM_CLOSE posted directly by anything else
would otherwise close the dock.
```

No emoji. No trailers.

### Pull requests

Change code and the strings that code needs. Release notes, changelogs, version
numbers and contributor lists belong to the maintainer at release time.

## Verifying a change

There is no unit test suite: nearly everything here is an interaction with the
window manager, and a mock of the window manager would only prove the mock works.
Verification is done against a live desktop. If you change window handling,
re-check the table at the bottom of the README, in particular:

1. Work area shrinks by exactly `widthPhysicalPx` while docked, via
   `SystemParametersInfo(SPI_GETWORKAREA)` from a PerMonitorV2 thread.
2. A maximized window's DWM extended frame bounds stop at the dock edge.
3. `WM_CLOSE`, `SC_CLOSE` and `SC_MINIMIZE` sent to the host all leave it alive.
4. `--stop` restores the work area and leaves other terminal windows running.
5. A borderless window sized to the whole monitor gets clamped out of the strip,
   and an ordinary maximized window does not.
6. The enforcer costs no measurable CPU at idle and stays around 1% of one core
   under a continuous window-drag storm.

Cold start and warm start are different code paths in `terminal.cpp`, and
historically only the cold one was broken. Test both: with no
`WindowsTerminal.exe` running, and with one already up.

State what you actually ran. "Should work" is not a verification.

## Releasing

Tagging `v*` builds, signs, packages and publishes. See `.github/workflows/release.yml`.

1. Bump the version in `cmake/DockVersion.cmake` and `version` in
   `installer.toml`. They must match, and CI checks that on every push.
2. Commit, tag `vX.Y.Z`, push the tag.
3. CI signs the payload, forges the installer, signs the installer, verifies both
   signatures, verifies the container survived signing, and publishes.

The signing order is not adjustable. Payload binaries are hashed into the
installer and extracted verbatim, so anything unsigned going in stays unsigned on
the user's disk. Sign the payload first, forge second, sign the installer last.
