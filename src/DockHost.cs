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
/// The docked window itself. It carries no chrome of its own and hosts the reparented
/// console as a child, so everything the user sees belongs to the console and everything
/// the window manager sees belongs to this form.
/// </summary>
internal sealed class DockHost : Form
{
    private readonly DockConfig _cfg;
    private readonly uint _appBarMessage = Native.RegisterWindowMessage("DockedConsole_AppBarCallback");
    private readonly System.Windows.Forms.Timer _repositionTimer;

    private AppBar? _appBar;
    private TerminalEmbedder? _embedder;
    private TrayMenu? _tray;
    private EventWaitHandle? _stopEvent;
    private RegisteredWaitHandle? _stopWait;

    private bool _shuttingDown;
    private bool _tornDown;
    private bool _repositioning;

    public DockHost(DockConfig cfg)
    {
        _cfg = cfg;

        FormBorderStyle = FormBorderStyle.None;
        ControlBox = false;
        MinimizeBox = false;
        MaximizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.Manual;
        TopMost = cfg.Topmost;
        BackColor = cfg.ParsedBackground;
        Text = "Docked Console";
        KeyPreview = false;

        // Somewhere off-screen until the shell tells us where the strip actually is.
        Location = new Point(-32000, -32000);
        Size = new Size(cfg.WidthPhysicalPx, 200);

        _repositionTimer = new System.Windows.Forms.Timer { Interval = 200 };
        _repositionTimer.Tick += (_, _) =>
        {
            _repositionTimer.Stop();
            Reposition();
        };
    }

    protected override CreateParams CreateParams
    {
        get
        {
            var cp = base.CreateParams;
            // Tool window keeps it out of Alt+Tab; clearing APPWINDOW keeps it off the taskbar.
            cp.ExStyle |= Native.WS_EX_TOOLWINDOW;
            cp.ExStyle &= ~Native.WS_EX_APPWINDOW;
            return cp;
        }
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);

        _appBar = new AppBar(Handle, _appBarMessage);
        if (!_appBar.Register())
        {
            MessageBox.Show(
                "Could not register the AppBar with the shell. The dock would not reserve any desktop space, so it is not starting.",
                "Docked Console",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
            _shuttingDown = true;
            BeginInvoke(Close);
            return;
        }

        Reposition();

        _embedder = new TerminalEmbedder(this, _cfg);
        _embedder.Fault += OnEmbedderFault;
        _embedder.Closed += OnTerminalClosed;
        _embedder.Start();

        _tray = new TrayMenu(_cfg);
        _tray.RestartShellRequested += () => _embedder?.Restart();
        _tray.ReloadConfigRequested += OnReloadConfig;
        _tray.ExitRequested += RequestShutdown;

        InstallStopChannel();

        SystemEvents.SessionEnding += OnSessionEnding;
        SystemEvents.DisplaySettingsChanged += OnDisplaySettingsChanged;
    }

    private void InstallStopChannel()
    {
        try
        {
            _stopEvent = new EventWaitHandle(false, EventResetMode.AutoReset, AppIdentity.StopEventName);
            _stopWait = ThreadPool.RegisterWaitForSingleObject(
                _stopEvent,
                (_, _) =>
                {
                    if (IsHandleCreated)
                    {
                        BeginInvoke(RequestShutdown);
                    }
                },
                null,
                Timeout.Infinite,
                executeOnlyOnce: true);
        }
        catch (Exception)
        {
            // Without the channel --stop will not work, but the tray menu still will.
        }
    }

    /// <summary>Recomputes the reserved strip and moves the window onto it.</summary>
    private void Reposition()
    {
        if (_appBar is null || _shuttingDown || _repositioning)
        {
            return;
        }

        _repositioning = true;
        try
        {
            var screen = _cfg.ResolveScreen();
            _appBar.Reposition(_cfg.ParsedEdge, _cfg.WidthPhysicalPx, screen.Bounds);

            if (_cfg.Topmost)
            {
                Native.SetWindowPos(
                    Handle,
                    Native.HWND_TOPMOST,
                    0, 0, 0, 0,
                    Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOACTIVATE);
            }

            _embedder?.Fit();
        }
        finally
        {
            _repositioning = false;
        }
    }

    private void ScheduleReposition()
    {
        if (_shuttingDown)
        {
            return;
        }

        // Coalesce: a taskbar move or resolution change emits a burst of these.
        _repositionTimer.Stop();
        _repositionTimer.Start();
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == (int)_appBarMessage)
        {
            HandleAppBarNotification(m.WParam.ToInt64(), m.LParam);
            return;
        }

        switch (m.Msg)
        {
            case Native.WM_SYSCOMMAND:
                int command = (int)(m.WParam.ToInt64() & 0xFFF0);
                if (command is Native.SC_MINIMIZE or Native.SC_MAXIMIZE or Native.SC_CLOSE
                    or Native.SC_MOVE or Native.SC_SIZE)
                {
                    // Swallowed: the dock is not minimizable, closable, movable or resizable.
                    return;
                }

                break;

            case Native.WM_ACTIVATE:
                _appBar?.NotifyActivate();
                if ((m.WParam.ToInt64() & 0xFFFF) != Native.WA_INACTIVE)
                {
                    _embedder?.FocusTerminal();
                }

                break;

            case Native.WM_SETFOCUS:
                _embedder?.FocusTerminal();
                break;

            case Native.WM_WINDOWPOSCHANGED:
                _appBar?.NotifyWindowPosChanged();
                break;

            case Native.WM_DISPLAYCHANGE:
            case Native.WM_DPICHANGED:
                ScheduleReposition();
                break;

            case Native.WM_SETTINGCHANGE:
                // Our own SETPOS changes the work area and echoes back here, so ignore
                // the echo while a reposition is in flight.
                if (m.WParam.ToInt32() == Native.SPI_SETWORKAREA && !_repositioning)
                {
                    ScheduleReposition();
                }

                break;

            case Native.WM_CLOSE:
                // Swallowed at the message level rather than relying on CloseReason:
                // WinForms only reports UserClosing for SC_CLOSE, so a WM_CLOSE posted
                // directly arrives as CloseReason.None and would otherwise close the dock.
                if (!_shuttingDown)
                {
                    return;
                }

                break;

            case Native.WM_ENDSESSION:
                if (m.WParam != IntPtr.Zero)
                {
                    Teardown();
                }

                break;
        }

        base.WndProc(ref m);
    }

    private void HandleAppBarNotification(long notification, IntPtr lParam)
    {
        switch ((int)notification)
        {
            case Native.ABN_POSCHANGED:
            case Native.ABN_WINDOWARRANGE:
                ScheduleReposition();
                break;

            case Native.ABN_STATECHANGE:
                if (_cfg.Topmost)
                {
                    Native.SetWindowPos(
                        Handle,
                        Native.HWND_TOPMOST,
                        0, 0, 0, 0,
                        Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOACTIVATE);
                }

                break;

            case Native.ABN_FULLSCREENAPP:
                if (!_cfg.YieldToFullscreenApps)
                {
                    break;
                }

                // Match taskbar behaviour: get out of the way of fullscreen apps.
                bool fullscreen = lParam != IntPtr.Zero;
                Native.SetWindowPos(
                    Handle,
                    fullscreen ? Native.HWND_BOTTOM : Native.HWND_TOPMOST,
                    0, 0, 0, 0,
                    Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOACTIVATE);
                break;
        }
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        _embedder?.Fit();
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        // Second line of defence behind the WM_CLOSE guard. Anything that is not the OS
        // shutting us down and not our own undock request is refused, whatever reason
        // WinForms attributes to it.
        bool systemInitiated = e.CloseReason is CloseReason.WindowsShutDown
            or CloseReason.TaskManagerClosing;

        if (!_shuttingDown && !systemInitiated)
        {
            e.Cancel = true;
            return;
        }

        Teardown();
        base.OnFormClosing(e);
    }

    private void OnSessionEnding(object sender, SessionEndingEventArgs e) => Teardown();

    private void OnDisplaySettingsChanged(object? sender, EventArgs e) => ScheduleReposition();

    private void OnReloadConfig()
    {
        var fresh = DockConfig.Load();

        _cfg.Edge = fresh.Edge;
        _cfg.WidthPhysicalPx = fresh.WidthPhysicalPx;
        _cfg.MonitorDeviceName = fresh.MonitorDeviceName;
        _cfg.BackgroundColor = fresh.BackgroundColor;
        _cfg.OnShellExit = fresh.OnShellExit;
        _cfg.Topmost = fresh.Topmost;
        _cfg.YieldToFullscreenApps = fresh.YieldToFullscreenApps;

        BackColor = _cfg.ParsedBackground;
        TopMost = _cfg.Topmost;
        Reposition();

        _tray?.Notify("Config reloaded. Shell settings apply on the next restart.");
    }

    private void OnEmbedderFault(string message) => _tray?.Notify(message);

    /// <summary>
    /// The shell exited. Typing exit is how you close any other terminal, so it closes this
    /// one too: undock, restore the desktop work area, quit.
    /// </summary>
    private void OnTerminalClosed(string? failureMessage)
    {
        if (_shuttingDown)
        {
            return;
        }

        if (failureMessage is not null)
        {
            // Shown before shutdown, not as a tray balloon, because the balloon would be
            // destroyed along with the tray icon a moment later.
            MessageBox.Show(failureMessage, "Docked Console", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }

        RequestShutdown();
    }

    /// <summary>The one supported way out: unregister cleanly, then exit.</summary>
    private void RequestShutdown()
    {
        _shuttingDown = true;
        Close();
    }

    /// <summary>
    /// Run-once cleanup. Reachable from form close, session end, WM_ENDSESSION and the
    /// process-exit handler in Program, because a missed ABM_REMOVE leaves the desktop
    /// work area shrunk with no window left to explain why.
    /// </summary>
    public void Teardown()
    {
        if (_tornDown)
        {
            return;
        }

        _tornDown = true;
        _shuttingDown = true;

        try
        {
            SystemEvents.SessionEnding -= OnSessionEnding;
            SystemEvents.DisplaySettingsChanged -= OnDisplaySettingsChanged;
        }
        catch (Exception)
        {
            // Nothing useful to do if the event source is already gone.
        }

        _repositionTimer.Stop();
        _stopWait?.Unregister(null);
        _stopEvent?.Dispose();

        _embedder?.Dispose();
        _embedder = null;

        _appBar?.Remove();
        _appBar = null;

        _tray?.Dispose();
        _tray = null;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            Teardown();
            _repositionTimer.Dispose();
        }

        base.Dispose(disposing);
    }
}
