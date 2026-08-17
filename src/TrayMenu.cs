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
using System.Drawing.Drawing2D;

namespace DockedConsole;

/// <summary>
/// The dock has no chrome, so the notification area is where its controls live.
/// </summary>
internal sealed class TrayMenu : IDisposable
{
    private readonly NotifyIcon _icon;
    private readonly ContextMenuStrip _menu;
    private readonly DockConfig _cfg;
    private IntPtr _iconHandle;
    private bool _disposed;

    public TrayMenu(DockConfig cfg)
    {
        _cfg = cfg;
        _menu = new ContextMenuStrip();

        var header = new ToolStripMenuItem("Docked Console") { Enabled = false };
        _menu.Items.Add(header);
        _menu.Items.Add(new ToolStripSeparator());

        var restart = new ToolStripMenuItem("Restart shell");
        restart.Click += (_, _) => RestartShellRequested?.Invoke();
        _menu.Items.Add(restart);

        var reload = new ToolStripMenuItem("Reload config");
        reload.Click += (_, _) => ReloadConfigRequested?.Invoke();
        _menu.Items.Add(reload);

        var edit = new ToolStripMenuItem("Edit config");
        edit.Click += (_, _) => OpenConfig();
        _menu.Items.Add(edit);

        _menu.Items.Add(new ToolStripSeparator());

        var exit = new ToolStripMenuItem("Undock and exit");
        exit.Click += (_, _) => ExitRequested?.Invoke();
        _menu.Items.Add(exit);

        _icon = new NotifyIcon
        {
            Icon = LoadIcon(out _iconHandle),
            Text = "Docked Console",
            Visible = true,
            ContextMenuStrip = _menu,
        };
    }

    public event Action? RestartShellRequested;

    public event Action? ReloadConfigRequested;

    public event Action? ExitRequested;

    public void Notify(string message)
    {
        if (_disposed)
        {
            return;
        }

        _icon.BalloonTipTitle = "Docked Console";
        _icon.BalloonTipText = message;
        _icon.BalloonTipIcon = ToolTipIcon.Warning;
        _icon.ShowBalloonTip(5000);
    }

    private void OpenConfig()
    {
        try
        {
            if (!File.Exists(DockConfig.Path))
            {
                _cfg.Save();
            }

            Process.Start(new ProcessStartInfo
            {
                FileName = DockConfig.Path,
                UseShellExecute = true,
            });
        }
        catch (Exception ex)
        {
            Notify($"could not open the config: {ex.Message}");
        }
    }

    /// <summary>
    /// The executable's own icon, so the tray matches the Start Menu and the installer.
    /// Falls back to drawing one, which keeps the tray populated even if extraction fails.
    /// </summary>
    private static Icon LoadIcon(out IntPtr handle)
    {
        handle = IntPtr.Zero;

        try
        {
            string? exe = Environment.ProcessPath;
            if (!string.IsNullOrEmpty(exe))
            {
                var extracted = Icon.ExtractAssociatedIcon(exe);
                if (extracted is not null)
                {
                    return extracted;
                }
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"icon extraction failed, drawing one instead: {ex.Message}");
        }

        return BuildIcon(out handle);
    }

    private static Icon BuildIcon(out IntPtr handle)
    {
        using var bmp = new Bitmap(32, 32);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);

            using var back = new SolidBrush(Color.FromArgb(0x1E, 0x1E, 0x1E));
            using var border = new Pen(Color.FromArgb(0x3C, 0x3C, 0x3C), 2f);
            g.FillRectangle(back, 2, 4, 28, 24);
            g.DrawRectangle(border, 2, 4, 28, 24);

            using var fore = new Pen(Color.FromArgb(0x4E, 0xC9, 0xB0), 2.4f);
            // A ">_" prompt glyph.
            g.DrawLines(fore, [new PointF(9, 12), new PointF(14, 16), new PointF(9, 20)]);
            g.DrawLine(fore, 17, 21, 24, 21);
        }

        handle = bmp.GetHicon();
        return Icon.FromHandle(handle);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _icon.Visible = false;
        _icon.Dispose();
        _menu.Dispose();

        if (_iconHandle != IntPtr.Zero)
        {
            Native.DestroyIcon(_iconHandle);
            _iconHandle = IntPtr.Zero;
        }
    }
}
