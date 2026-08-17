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

internal static class AppIdentity
{
    // Local\ scopes both to the interactive session, which is what we want: one dock
    // per logged-on user, not one per machine.
    public const string MutexName = @"Local\DockedConsole.Instance";
    public const string StopEventName = @"Local\DockedConsole.Stop";
}

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (HasFlag(args, "--help") || HasFlag(args, "-h") || HasFlag(args, "/?"))
        {
            ShowUsage();
            return 0;
        }

        if (HasFlag(args, "--stop"))
        {
            return SignalStop();
        }

        if (HasFlag(args, "--reclaim"))
        {
            return Reclaim();
        }

        // Invoked by the installer through a hook declared as = "user", so it must never
        // put a dialog on screen: there is nobody to dismiss it and the hook would time out.
        if (HasFlag(args, "--register-autostart"))
        {
            bool ok = Autostart.Register(out string message);
            ParentConsole.Write(message, !ok);
            ParentConsole.Detach();
            return ok ? 0 : 1;
        }

        if (HasFlag(args, "--unregister-autostart"))
        {
            bool ok = Autostart.Unregister(out string message);
            ParentConsole.Write(message, !ok);
            ParentConsole.Detach();
            return ok ? 0 : 1;
        }

        // The instance check comes before the elevation check so that launching a second
        // copy says so instead of raising a UAC prompt and only then saying so.
        var mutex = AcquireInstanceLock(waitForPredecessor: HasFlag(args, Elevation.RelaunchFlag));
        if (mutex is null)
        {
            MessageBox.Show(
                "Docked Console is already running. Use the tray icon, or run dockedconsole.exe --stop.",
                "Docked Console",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return 2;
        }

        ApplicationConfiguration.Initialize();

        var cfg = DockConfig.Load();
        ApplyOverrides(cfg, args);

        if (Elevation.ShouldElevate(cfg))
        {
            // Hand the lock over before spawning, or the elevated copy races the instance
            // that started it and loses.
            mutex.Dispose();

            if (Elevation.Relaunch(args, out string relaunchMessage))
            {
                return 0;
            }

            MessageBox.Show(relaunchMessage, "Docked Console", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return 3;
        }

        if (Elevation.BlockedByPolicy(cfg))
        {
            mutex.Dispose();
            MessageBox.Show(
                "An elevated Windows Terminal is running, so any new terminal window belongs "
                + "to it and an unelevated Docked Console cannot take it over.\n\n"
                + "Set \"elevation\" to \"auto\" in dockedconsole.json, run Docked Console as "
                + "administrator, or close the elevated terminal windows.",
                "Docked Console",
                MessageBoxButtons.OK,
                MessageBoxIcon.Warning);
            return 3;
        }

        using var instanceLock = mutex;
        using var host = new DockHost(cfg);

        // Last-resort backstop. If the message loop unwinds by a route that skips
        // OnFormClosing, the AppBar still has to come off or the desktop stays shrunk.
        void OnProcessExit(object? sender, EventArgs e) => host.Teardown();
        AppDomain.CurrentDomain.ProcessExit += OnProcessExit;

        try
        {
            Application.Run(host);
        }
        finally
        {
            host.Teardown();
            AppDomain.CurrentDomain.ProcessExit -= OnProcessExit;
        }

        return 0;
    }

    /// <summary>
    /// Null when another instance holds the lock. An elevated relaunch waits, because the
    /// instance that spawned it is still exiting and still owns the mutex for a moment.
    /// </summary>
    private static Mutex? AcquireInstanceLock(bool waitForPredecessor)
    {
        var deadline = DateTime.UtcNow + (waitForPredecessor ? TimeSpan.FromSeconds(6) : TimeSpan.Zero);

        while (true)
        {
            try
            {
                var mutex = new Mutex(true, AppIdentity.MutexName, out bool createdNew);
                if (createdNew)
                {
                    return mutex;
                }

                mutex.Dispose();
            }
            catch (UnauthorizedAccessException)
            {
                // An elevated instance owns it and will not let us open it. Still running.
            }

            if (DateTime.UtcNow >= deadline)
            {
                return null;
            }

            Thread.Sleep(150);
        }
    }

    private static bool HasFlag(string[] args, string flag) =>
        args.Any(a => string.Equals(a, flag, StringComparison.OrdinalIgnoreCase));

    private static void ApplyOverrides(DockConfig cfg, string[] args)
    {
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], "--width", StringComparison.OrdinalIgnoreCase)
                && int.TryParse(args[i + 1], out int width))
            {
                // One-shot: deliberately not persisted, so --width is safe for trying sizes.
                cfg.WidthPhysicalPx = width;
            }
            else if (string.Equals(args[i], "--edge", StringComparison.OrdinalIgnoreCase))
            {
                cfg.Edge = args[i + 1];
            }
        }
    }

    private static int SignalStop()
    {
        if (!EventWaitHandle.TryOpenExisting(AppIdentity.StopEventName, out var handle))
        {
            ParentConsole.Report("Docked Console is not running.", error: true);
            ParentConsole.Detach();
            return 1;
        }

        using (handle)
        {
            handle.Set();
        }

        ParentConsole.Report("Docked Console undocked.");
        ParentConsole.Detach();
        return 0;
    }

    private static int Reclaim()
    {
        ApplicationConfiguration.Initialize();
        AppBar.ForceWorkAreaRecompute();
        ParentConsole.Report("Forced a desktop work-area recompute.");
        ParentConsole.Detach();
        return 0;
    }

    private static void ShowUsage()
    {
        ParentConsole.Report(
            """
            dockedconsole.exe            Dock the console and reserve the strip.
            dockedconsole.exe --stop     Undock and exit the running instance.
            dockedconsole.exe --reclaim  Recover desktop space left reserved by a hard kill.

            Overrides, not saved to the config file:
              --width <physical px>
              --edge <left|top|right|bottom>

            Settings live in dockedconsole.json next to this executable.

            This is a GUI executable, so PowerShell does not wait for it. If you need the
            exit code, use: Start-Process -Wait dockedconsole.exe -ArgumentList --stop
            """);
        ParentConsole.Detach();
    }
}
