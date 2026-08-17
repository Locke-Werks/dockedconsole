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

internal enum ElevationPolicy
{
    /// <summary>Elevate only when the terminal we would embed is going to be elevated.</summary>
    Auto,

    /// <summary>Never elevate. Report and stop if the terminal turns out to be elevated.</summary>
    Never,

    /// <summary>Always elevate at startup.</summary>
    Always,
}

/// <summary>
/// Integrity level matching.
///
/// Embedding works by reparenting the terminal's window into the dock, and UIPI forbids a
/// medium-integrity process from calling SetParent on a window owned by an elevated one.
/// That is not hypothetical here: `wt -w new` is serviced by whichever Windows Terminal
/// "monarch" process already exists, so if ANY elevated terminal is running, the window we
/// are handed belongs to it and an unelevated dock cannot touch it. SetParent fails, the
/// terminal is left floating, and the dock has nothing to show.
///
/// So the dock matches the terminal instead of fighting it: if the window is going to be
/// elevated, relaunch elevated first. On a machine with no elevated terminal, nothing
/// elevates and no prompt appears.
/// </summary>
internal static class Elevation
{
    private const string TerminalProcessName = "WindowsTerminal";

    /// <summary>Marks a relaunch so the new instance waits for the old one's mutex.</summary>
    public const string RelaunchFlag = "--elevated-relaunch";

    public static bool IsCurrentProcessElevated()
    {
        try
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            var principal = new System.Security.Principal.WindowsPrincipal(identity);
            return principal.IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>Null when the process cannot be queried, which is not the same as false.</summary>
    public static bool? IsProcessElevated(int pid)
    {
        IntPtr process = Native.OpenProcess(Native.PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (process == IntPtr.Zero)
        {
            return null;
        }

        IntPtr token = IntPtr.Zero;
        IntPtr buffer = IntPtr.Zero;
        try
        {
            if (!Native.OpenProcessToken(process, Native.TOKEN_QUERY, out token))
            {
                return null;
            }

            buffer = Marshal.AllocHGlobal(sizeof(int));
            if (!Native.GetTokenInformation(token, Native.TokenElevation, buffer, sizeof(int), out _))
            {
                return null;
            }

            return Marshal.ReadInt32(buffer) != 0;
        }
        catch (Exception)
        {
            return null;
        }
        finally
        {
            if (buffer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(buffer);
            }

            if (token != IntPtr.Zero)
            {
                Native.CloseHandle(token);
            }

            Native.CloseHandle(process);
        }
    }

    /// <summary>
    /// True when a Windows Terminal process is running elevated. Any such process can be
    /// the monarch that services our window request, which would hand us a window we
    /// cannot reparent unless we elevate too.
    /// </summary>
    public static bool AnyElevatedTerminalRunning()
    {
        try
        {
            foreach (var proc in Process.GetProcessesByName(TerminalProcessName))
            {
                using (proc)
                {
                    if (IsProcessElevated(proc.Id) == true)
                    {
                        return true;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"terminal elevation scan failed: {ex.Message}");
        }

        return false;
    }

    public static bool ShouldElevate(DockConfig cfg)
    {
        if (cfg.ParsedTerminal != TerminalKind.WindowsTerminal)
        {
            // conhost is launched by us and inherits our integrity level, so there is
            // never a mismatch to correct.
            return false;
        }

        if (IsCurrentProcessElevated())
        {
            return false;
        }

        return cfg.ParsedElevation switch
        {
            ElevationPolicy.Always => true,
            ElevationPolicy.Never => false,
            _ => AnyElevatedTerminalRunning(),
        };
    }

    /// <summary>
    /// True when the dock cannot work but the configuration forbids fixing it. Checked so
    /// the refusal happens before a terminal window is opened that could never be captured.
    /// </summary>
    public static bool BlockedByPolicy(DockConfig cfg) =>
        cfg.ParsedElevation == ElevationPolicy.Never
        && cfg.ParsedTerminal == TerminalKind.WindowsTerminal
        && !IsCurrentProcessElevated()
        && AnyElevatedTerminalRunning();

    /// <summary>
    /// Restarts this executable elevated, passing the original arguments through. Returns
    /// false if the user declined the prompt, which is a decision rather than an error.
    /// </summary>
    public static bool Relaunch(string[] args, out string message)
    {
        string? exe = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exe))
        {
            message = "could not determine this executable's path";
            return false;
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = exe,
            UseShellExecute = true,
            Verb = "runas",
        };

        foreach (var arg in args)
        {
            if (!string.Equals(arg, RelaunchFlag, StringComparison.OrdinalIgnoreCase))
            {
                startInfo.ArgumentList.Add(arg);
            }
        }

        startInfo.ArgumentList.Add(RelaunchFlag);

        try
        {
            Process.Start(startInfo);
            message = "relaunched elevated";
            return true;
        }
        catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223)
        {
            message = "The elevation prompt was declined, so Docked Console did not start.";
            return false;
        }
        catch (Exception ex)
        {
            message = $"could not relaunch elevated: {ex.Message}";
            return false;
        }
    }
}
