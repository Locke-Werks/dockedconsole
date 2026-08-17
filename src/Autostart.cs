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
using Microsoft.Win32;

namespace DockedConsole;

/// <summary>
/// The HKCU Run entry.
///
/// This is deliberately a command on the product rather than something the installer
/// writes directly. A machine-scope installer runs elevated, and under over-the-shoulder
/// UAC its HKCU is the administrator's, not the person installing. The installer calls
/// these through a hook declared `as = "user"` so the entry lands in the right hive.
/// </summary>
internal static class Autostart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "DockedConsole";

    public static bool Register(out string message)
    {
        string? exe = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exe))
        {
            message = "could not determine this executable's path";
            return false;
        }

        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKey, writable: true);
            if (key is null)
            {
                message = $@"could not open HKCU\{RunKey}";
                return false;
            }

            key.SetValue(ValueName, $"\"{exe}\"", RegistryValueKind.String);
            message = $"registered autostart for {Environment.UserName}: {exe}";
            return true;
        }
        catch (Exception ex)
        {
            message = $"could not register autostart: {ex.Message}";
            return false;
        }
    }

    public static bool Unregister(out string message)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
            if (key is null)
            {
                message = "no Run key, nothing to remove";
                return true;
            }

            if (key.GetValue(ValueName) is null)
            {
                message = "autostart was not registered";
                return true;
            }

            key.DeleteValue(ValueName, throwOnMissingValue: false);
            message = $"removed autostart for {Environment.UserName}";
            return true;
        }
        catch (Exception ex)
        {
            message = $"could not remove autostart: {ex.Message}";
            return false;
        }
    }

    public static bool IsRegistered()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(ValueName) is not null;
        }
        catch (Exception)
        {
            return false;
        }
    }
}
