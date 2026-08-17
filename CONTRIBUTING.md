# Contributing

## Building

Needs the .NET 9 SDK and Windows 11.

```powershell
dotnet build -c Release          # development loop, framework dependent, fast
dotnet publish -c Release -o dist # the shippable artifact, self contained
```

`dotnet publish` is what CI runs. It produces one self-contained
`dist\dockedconsole.exe` with no runtime prerequisite, which is why it takes
about forty seconds rather than four. Use `build` while working.

## Conventions

### Code

- Target `net9.0-windows`, nullable enabled, C# 13.
- Four spaces, braces on their own line, `_camelCase` for private fields.
- P/Invoke declarations live in `Native.cs`. Nowhere else.
- Every P/Invoke that can fail gets its result checked. `SetParent` returning null
  is ambiguous, so verify with `GetParent`. This class of thing is the entire
  project.
- Comments explain why, not what. If a comment restates the line below it, delete
  it. If a line looks wrong until you know a Win32 rule, write the rule down.

### DPI

Everything is physical pixels. The AppBar API is physical, the process is
PerMonitorV2 in both the manifest and the csproj, and any coordinate that reaches
`SHAppBarMessage` or `SetWindowPos` must not have been through a DPI-unaware
round trip. If you add a measurement, take it from a PerMonitorV2 context.

### Things that will bite you

Documented at length in the README, listed here so you do not rediscover them:

- Windows Terminal's tab strip is client-drawn. Window styles will not remove it.
- The terminal window is usually owned by a pre-existing `WindowsTerminal.exe`
  shared with the user's other windows. Never kill that process.
- `SetParent` across integrity levels is blocked by UIPI.
- On the conhost path, the console window is attributed to the attached shell
  process, not to conhost.
- A leaked AppBar registration shrinks the desktop with no window left to explain
  it. Every exit path must reach `ABM_REMOVE`.

### Commits

Imperative mood, concise subject line, body only when the change needs
explanation.

```
Refuse WM_CLOSE in WndProc

WinForms only reports CloseReason.UserClosing for SC_CLOSE, so a WM_CLOSE
posted directly arrived as CloseReason.None and closed the dock.
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

State what you actually ran. "Should work" is not a verification.

## Releasing

Tagging `v*` builds, signs, packages and publishes. See `.github/workflows/release.yml`.

1. Bump `Version` in `DockedConsole.csproj` and `version` in `installer.toml`.
   They must match.
2. Commit, tag `vX.Y.Z`, push the tag.
3. CI signs the payload, forges the installer, signs the installer, verifies both
   signatures, verifies the container survived signing, and publishes.

The signing order is not adjustable. Payload binaries are hashed into the
installer and extracted verbatim, so anything unsigned going in stays unsigned on
the user's disk. Sign the payload first, forge second, sign the installer last.
