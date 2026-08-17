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
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace DockedConsole;

internal readonly record struct FontResult(
    bool Applied,
    string ActualFace,
    int CellWidth,
    int CellHeight,
    string? Error)
{
    public override string ToString() =>
        Applied
            ? $"font '{ActualFace}' cell {CellWidth}x{CellHeight}"
            : $"font not applied: {Error}";
}

/// <summary>
/// Sets the embedded console's font and code page.
///
/// conhost does almost no font fallback: whatever single face it is given has to carry
/// every glyph the shell emits, or missing ones render as tofu. That makes the face a
/// correctness setting here, not decoration, because a Nerd Font prompt is unreadable in
/// Consolas.
///
/// The host process is a WinExe and owns no console of its own, so it can borrow the
/// embedded shell's console with AttachConsole, adjust it, and detach.
/// </summary>
internal static class ConsoleAppearance
{
    public static FontResult Apply(uint shellPid, string face, int sizePx, bool utf8)
    {
        if (string.IsNullOrWhiteSpace(face))
        {
            return new FontResult(false, string.Empty, 0, 0, "no face configured");
        }

        if (!Native.AttachConsole(shellPid))
        {
            return new FontResult(false, string.Empty, 0, 0,
                $"AttachConsole failed ({Marshal.GetLastWin32Error()})");
        }

        // While attached we are a client of that console and would otherwise receive its
        // Ctrl+C events. Ignore them for the moment we are borrowing it.
        Native.SetConsoleCtrlHandler(IntPtr.Zero, true);

        IntPtr conout = IntPtr.Zero;
        try
        {
            conout = Native.CreateFile(
                "CONOUT$",
                Native.GENERIC_READ | Native.GENERIC_WRITE,
                Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE,
                IntPtr.Zero,
                Native.OPEN_EXISTING,
                0,
                IntPtr.Zero);

            if (conout == Native.INVALID_HANDLE_VALUE || conout == IntPtr.Zero)
            {
                return new FontResult(false, string.Empty, 0, 0,
                    $"could not open CONOUT$ ({Marshal.GetLastWin32Error()})");
            }

            if (utf8)
            {
                Native.SetConsoleOutputCP(65001);
                Native.SetConsoleCP(65001);
            }

            var desired = new CONSOLE_FONT_INFOEX
            {
                cbSize = (uint)Marshal.SizeOf<CONSOLE_FONT_INFOEX>(),
                nFont = 0,
                // X = 0 lets the font pick its own advance width for the requested height.
                dwFontSize = new COORD { X = 0, Y = (short)sizePx },
                FontFamily = Native.FF_MODERN_TRUETYPE,
                FontWeight = Native.FW_NORMAL,
                FaceName = Truncate(face),
            };

            bool set = Native.SetCurrentConsoleFontEx(conout, false, ref desired);
            int err = Marshal.GetLastWin32Error();

            // conhost silently ignores a face it does not consider a valid console font,
            // so trust the read-back rather than the return value.
            var actual = new CONSOLE_FONT_INFOEX
            {
                cbSize = (uint)Marshal.SizeOf<CONSOLE_FONT_INFOEX>(),
            };
            Native.GetCurrentConsoleFontEx(conout, false, ref actual);

            bool took = string.Equals(actual.FaceName?.Trim(), face.Trim(), StringComparison.OrdinalIgnoreCase);

            return new FontResult(
                took,
                actual.FaceName ?? string.Empty,
                actual.dwFontSize.X,
                actual.dwFontSize.Y,
                took ? null : $"conhost kept '{actual.FaceName}' (SetCurrentConsoleFontEx returned {set}, error {err})");
        }
        catch (Exception ex)
        {
            return new FontResult(false, string.Empty, 0, 0, ex.Message);
        }
        finally
        {
            if (conout != IntPtr.Zero && conout != Native.INVALID_HANDLE_VALUE)
            {
                Native.CloseHandle(conout);
            }

            Native.SetConsoleCtrlHandler(IntPtr.Zero, false);
            Native.FreeConsole();
            Debug.WriteLine("detached from embedded console");
        }
    }

    /// <summary>FaceName is a fixed 32-wchar buffer; an over-long name corrupts the struct.</summary>
    private static string Truncate(string face) => face.Length <= 31 ? face : face[..31];
}
