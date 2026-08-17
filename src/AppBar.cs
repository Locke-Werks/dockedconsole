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

namespace DockedConsole;

internal enum DockEdge
{
    Left,
    Top,
    Right,
    Bottom,
}

/// <summary>
/// Wraps SHAppBarMessage. Registering an AppBar makes the shell subtract the reserved
/// rect from the desktop work area, so maximized windows stop at its edge instead of
/// covering it. This is the same mechanism the taskbar uses.
/// </summary>
internal sealed class AppBar
{
    private readonly IntPtr _hWnd;
    private readonly uint _callbackMessage;
    private bool _registered;

    public AppBar(IntPtr hWnd, uint callbackMessage)
    {
        _hWnd = hWnd;
        _callbackMessage = callbackMessage;
    }

    public bool IsRegistered => _registered;

    /// <summary>The rect the shell last granted us, in physical pixels.</summary>
    public RECT ReservedRect { get; private set; }

    public bool Register()
    {
        if (_registered)
        {
            return true;
        }

        var abd = NewData();
        abd.uCallbackMessage = _callbackMessage;

        if (Native.SHAppBarMessage(Native.ABM_NEW, ref abd) == UIntPtr.Zero)
        {
            return false;
        }

        _registered = true;
        return true;
    }

    /// <summary>
    /// Ask the shell where we may sit, pin our thickness back on, then commit. QUERYPOS
    /// only clamps the docked edge, so the opposite edge has to be recomputed afterwards
    /// or the bar drifts to whatever thickness the shell felt like handing back.
    /// </summary>
    public void Reposition(DockEdge edge, int thicknessPx, Rectangle monitorBounds)
    {
        if (!_registered)
        {
            return;
        }

        var abd = NewData();
        abd.uEdge = ToNativeEdge(edge);
        abd.rc = DesiredRect(edge, thicknessPx, monitorBounds);

        Native.SHAppBarMessage(Native.ABM_QUERYPOS, ref abd);
        abd.rc = PinThickness(edge, thicknessPx, abd.rc);

        Native.SHAppBarMessage(Native.ABM_SETPOS, ref abd);
        abd.rc = PinThickness(edge, thicknessPx, abd.rc);

        ReservedRect = abd.rc;

        Native.SetWindowPos(
            _hWnd,
            IntPtr.Zero,
            abd.rc.Left,
            abd.rc.Top,
            abd.rc.Width,
            abd.rc.Height,
            Native.SWP_NOZORDER | Native.SWP_NOACTIVATE);

        NotifyWindowPosChanged();
    }

    public void NotifyActivate()
    {
        if (!_registered)
        {
            return;
        }

        var abd = NewData();
        Native.SHAppBarMessage(Native.ABM_ACTIVATE, ref abd);
    }

    public void NotifyWindowPosChanged()
    {
        if (!_registered)
        {
            return;
        }

        var abd = NewData();
        Native.SHAppBarMessage(Native.ABM_WINDOWPOSCHANGED, ref abd);
    }

    /// <summary>
    /// Idempotent. A leaked registration leaves the desktop work area permanently
    /// shrunk, so this has to be reachable from every exit path.
    /// </summary>
    public void Remove()
    {
        if (!_registered)
        {
            return;
        }

        _registered = false;

        var abd = NewData();
        Native.SHAppBarMessage(Native.ABM_REMOVE, ref abd);
    }

    private APPBARDATA NewData() => new()
    {
        cbSize = System.Runtime.InteropServices.Marshal.SizeOf<APPBARDATA>(),
        hWnd = _hWnd,
    };

    private static RECT DesiredRect(DockEdge edge, int thickness, Rectangle mon) => edge switch
    {
        // Span the full monitor on the long axis and let QUERYPOS trim for the taskbar.
        DockEdge.Left => new RECT { Left = mon.Left, Top = mon.Top, Right = mon.Left + thickness, Bottom = mon.Bottom },
        DockEdge.Right => new RECT { Left = mon.Right - thickness, Top = mon.Top, Right = mon.Right, Bottom = mon.Bottom },
        DockEdge.Top => new RECT { Left = mon.Left, Top = mon.Top, Right = mon.Right, Bottom = mon.Top + thickness },
        DockEdge.Bottom => new RECT { Left = mon.Left, Top = mon.Bottom - thickness, Right = mon.Right, Bottom = mon.Bottom },
        _ => throw new ArgumentOutOfRangeException(nameof(edge)),
    };

    private static RECT PinThickness(DockEdge edge, int thickness, RECT rc)
    {
        switch (edge)
        {
            case DockEdge.Left:
                rc.Right = rc.Left + thickness;
                break;
            case DockEdge.Right:
                rc.Left = rc.Right - thickness;
                break;
            case DockEdge.Top:
                rc.Bottom = rc.Top + thickness;
                break;
            case DockEdge.Bottom:
                rc.Top = rc.Bottom - thickness;
                break;
        }

        return rc;
    }

    private static uint ToNativeEdge(DockEdge edge) => edge switch
    {
        DockEdge.Left => Native.ABE_LEFT,
        DockEdge.Top => Native.ABE_TOP,
        DockEdge.Right => Native.ABE_RIGHT,
        DockEdge.Bottom => Native.ABE_BOTTOM,
        _ => Native.ABE_RIGHT,
    };

    /// <summary>
    /// Recovery for a hard kill. If the process died without unregistering, the shell may
    /// still be holding the reserved strip. Registering and immediately removing a
    /// throwaway AppBar forces a work-area recompute, during which the shell prunes
    /// registrations whose window no longer exists.
    /// </summary>
    public static void ForceWorkAreaRecompute()
    {
        using var scratch = new Form
        {
            FormBorderStyle = FormBorderStyle.None,
            ShowInTaskbar = false,
            StartPosition = FormStartPosition.Manual,
            Location = new Point(-32000, -32000),
            Size = new Size(1, 1),
        };

        // Force handle creation without ever showing the window.
        var handle = scratch.Handle;

        var bar = new AppBar(handle, Native.RegisterWindowMessage("DockedConsoleReclaim"));
        if (!bar.Register())
        {
            return;
        }

        var mon = Screen.PrimaryScreen?.Bounds ?? new Rectangle(0, 0, 1, 1);
        bar.Reposition(DockEdge.Right, 1, mon);
        Thread.Sleep(120);
        bar.Remove();

        Debug.WriteLine("work area recompute forced");
    }
}
