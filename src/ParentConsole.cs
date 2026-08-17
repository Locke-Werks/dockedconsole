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
namespace DockedConsole;

/// <summary>
/// The dock is a GUI-subsystem executable so that running it never opens a console of its
/// own. That also means its control commands have nowhere to print. Borrowing the calling
/// shell's console gives --stop, --reclaim and --help somewhere to report.
/// </summary>
internal static class ParentConsole
{
    private static bool _attached;

    public static bool Attach()
    {
        if (_attached)
        {
            return true;
        }

        if (!Native.AttachConsole(Native.ATTACH_PARENT_PROCESS))
        {
            return false;
        }

        try
        {
            // The runtime bound stdout to a null writer at startup because there was no
            // console then. Rebind it now that there is one.
            var stdout = new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true };
            var stderr = new StreamWriter(Console.OpenStandardError()) { AutoFlush = true };
            Console.SetOut(stdout);
            Console.SetError(stderr);
            _attached = true;
        }
        catch (IOException)
        {
            return false;
        }

        return _attached;
    }

    public static void Detach()
    {
        if (!_attached)
        {
            return;
        }

        try
        {
            Console.Out.Flush();
            Console.Error.Flush();
        }
        catch (IOException)
        {
            // Nothing to do if the parent's console has already gone away.
        }

        Native.FreeConsole();
        _attached = false;
    }

    /// <summary>
    /// Prints to the calling shell if there is one and stays silent otherwise.
    ///
    /// Used by anything an installer hook can invoke. A hook has no console, and the
    /// dialog fallback in Report would block until the hook's timeout expires and fail
    /// the install with no one there to click OK.
    /// </summary>
    public static void Write(string message, bool error = false)
    {
        if (!Attach())
        {
            return;
        }

        if (error)
        {
            Console.Error.WriteLine(message);
        }
        else
        {
            Console.WriteLine(message);
        }
    }

    /// <summary>Prints to the calling shell, falling back to a dialog when there is none.</summary>
    public static void Report(string message, bool error = false)
    {
        if (Attach())
        {
            if (error)
            {
                Console.Error.WriteLine(message);
            }
            else
            {
                Console.WriteLine(message);
            }

            return;
        }

        MessageBox.Show(
            message,
            "Docked Console",
            MessageBoxButtons.OK,
            error ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
    }
}
