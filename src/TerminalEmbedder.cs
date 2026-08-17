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
using System.Text;

namespace DockedConsole;

internal enum TerminalKind
{
    WindowsTerminal,
    Conhost,
}

internal enum ShellExitAction
{
    /// <summary>Typing exit closes the dock, as it would close an ordinary terminal.</summary>
    Quit,

    /// <summary>Typing exit gets a fresh shell and the dock stays put.</summary>
    Relaunch,
}

/// <summary>
/// Launches a terminal and reparents its window into the dock.
///
/// Two hosts are supported and they differ in ways that matter for cleanup:
///
/// Windows Terminal renders the full Unicode range including astral-plane glyphs and
/// honours the user's profile, fonts and theme. Its window is created by the WT "monarch"
/// process, which is very often an ALREADY RUNNING WindowsTerminal.exe hosting the user's
/// other windows. Killing that process would take their other terminals down with it, so
/// the WT path never kills anything: it closes its own window and nothing else.
///
/// conhost is the fallback. It is a dedicated process tree that can be killed safely, but
/// it does no font fallback and cannot render characters outside the BMP at all, because
/// it stores one UTF-16 code unit per cell.
/// </summary>
internal sealed class TerminalEmbedder : IDisposable
{
    private const string ConhostWindowClass = "ConsoleWindowClass";
    private const string TerminalWindowClass = "CASCADIA_HOSTING_WINDOW_CLASS";
    private const string TerminalDragBarClass = "DRAG_BAR_WINDOW_CLASS";

    private static readonly TimeSpan LocateTimeout = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan RapidFailureWindow = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan StartupFailureWindow = TimeSpan.FromSeconds(2);
    private const int MaxRapidFailures = 5;

    private readonly Form _host;
    private readonly DockConfig _cfg;
    private readonly System.Windows.Forms.Timer _healthTimer;
    private readonly System.Windows.Forms.Timer _fitTimer;

    private Process? _ownedProcess;
    private IntPtr _terminalHwnd;
    private int _chromeTrim = -1;
    private DateTime _lastStartUtc;
    private DateTime _embeddedAtUtc;
    private int _rapidFailures;
    private int _fitTicks;
    private bool _disposed;
    private bool _forwardingFocus;

    public TerminalEmbedder(Form host, DockConfig cfg)
    {
        _host = host;
        _cfg = cfg;

        _healthTimer = new System.Windows.Forms.Timer { Interval = 1000 };
        _healthTimer.Tick += (_, _) => CheckHealth();

        // Both hosts settle their own geometry after being sized, so the fit is re-applied
        // a few times rather than once.
        _fitTimer = new System.Windows.Forms.Timer { Interval = 250 };
        _fitTimer.Tick += (_, _) =>
        {
            Fit();
            if (++_fitTicks >= 6)
            {
                _fitTimer.Stop();
            }
        };
    }

    public event Action<string>? Fault;

    /// <summary>
    /// The terminal window closed and the configured action is to quit. The argument is
    /// null for an ordinary exit, or a message when the terminal died so soon after
    /// starting that it looks like a launch failure rather than the user typing exit.
    /// </summary>
    public event Action<string?>? Closed;

    public IntPtr TerminalHandle => _terminalHwnd;

    public FontResult LastFont { get; private set; }

    private TerminalKind Kind => _cfg.ParsedTerminal;

    private string WindowClass => Kind == TerminalKind.Conhost ? ConhostWindowClass : TerminalWindowClass;

    public void Start()
    {
        if (_disposed)
        {
            return;
        }

        var existing = FindWindowsByClass(WindowClass);
        _lastStartUtc = DateTime.UtcNow;

        Process? launcher;
        try
        {
            launcher = Kind == TerminalKind.Conhost ? StartConhost() : StartWindowsTerminal();
        }
        catch (Exception ex)
        {
            Fault?.Invoke($"could not start the terminal: {ex.Message}");
            return;
        }

        if (launcher is null)
        {
            Fault?.Invoke("the terminal did not start");
            return;
        }

        // For conhost the launcher IS the process tree we own and must clean up. For
        // Windows Terminal the launcher is a throwaway stub that exits immediately after
        // handing off to the monarch, so there is nothing here we may kill.
        _ownedProcess = Kind == TerminalKind.Conhost ? launcher : null;
        if (Kind != TerminalKind.Conhost)
        {
            launcher.Dispose();
        }

        var (hwnd, ownerPid) = LocateWindow(launcher, existing);
        if (hwnd == IntPtr.Zero)
        {
            Fault?.Invoke("timed out waiting for the terminal window");
            KillOwnedProcess();
            return;
        }

        if (Kind == TerminalKind.Conhost)
        {
            // conhost picks up neither the user's profile nor any font fallback, so the
            // face has to be forced onto it explicitly.
            LastFont = ConsoleAppearance.Apply(ownerPid, _cfg.FontFace, _cfg.FontSizePx, _cfg.Utf8Console);
            if (!LastFont.Applied && !string.IsNullOrWhiteSpace(_cfg.FontFace))
            {
                Fault?.Invoke($"Could not set the console font to '{_cfg.FontFace}'. {LastFont.Error}");
            }
        }

        _terminalHwnd = hwnd;
        _chromeTrim = -1;

        string? embedError = Embed(hwnd, ownerPid);
        if (embedError is not null)
        {
            _terminalHwnd = IntPtr.Zero;
            KillOwnedProcess();
            Closed?.Invoke(embedError);
            return;
        }

        _embeddedAtUtc = DateTime.UtcNow;

        _fitTicks = 0;
        _fitTimer.Start();
        _healthTimer.Start();
    }

    private Process? StartConhost()
    {
        string shell = _cfg.ResolveShellPath();
        if (!File.Exists(shell))
        {
            Fault?.Invoke($"shell not found: {shell}");
            return null;
        }

        string conhost = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.System), "conhost.exe");

        return Process.Start(new ProcessStartInfo
        {
            FileName = conhost,
            Arguments = $"\"{shell}\" {_cfg.ShellArgs}".TrimEnd(),
            UseShellExecute = false,
            WorkingDirectory = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        });
    }

    private Process? StartWindowsTerminal()
    {
        string wt = _cfg.ResolveTerminalPath();
        if (!File.Exists(wt))
        {
            Fault?.Invoke($"wt.exe not found at {wt}");
            return null;
        }

        return Process.Start(new ProcessStartInfo
        {
            FileName = wt,
            Arguments = _cfg.TerminalArgs,
            UseShellExecute = false,
            WorkingDirectory = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        });
    }

    public void Restart()
    {
        if (_disposed)
        {
            return;
        }

        _healthTimer.Stop();
        _fitTimer.Stop();
        ReleaseTerminal();
        Start();
    }

    /// <summary>
    /// Sizes the terminal to fill the dock.
    ///
    /// Windows Terminal draws its tab strip inside its own client area, so stripping the
    /// native caption does not remove it. Instead the window is pushed up by exactly the
    /// height of the tab strip and made correspondingly taller: child windows are clipped
    /// to the parent's client area, so the strip falls outside the dock and disappears
    /// while the terminal grid still fills it edge to edge.
    /// </summary>
    public void Fit()
    {
        if (_terminalHwnd == IntPtr.Zero || !Native.IsWindow(_terminalHwnd))
        {
            return;
        }

        var client = _host.ClientSize;
        if (client.Width <= 0 || client.Height <= 0)
        {
            return;
        }

        int trim = ChromeTrim();

        Native.SetWindowPos(
            _terminalHwnd,
            IntPtr.Zero,
            0,
            -trim,
            client.Width,
            client.Height + trim,
            Native.SWP_NOZORDER | Native.SWP_NOACTIVATE | Native.SWP_SHOWWINDOW);
    }

    private int ChromeTrim()
    {
        if (_cfg.ChromeTrimPx >= 0)
        {
            return _cfg.ChromeTrimPx;
        }

        if (Kind == TerminalKind.Conhost)
        {
            return 0;
        }

        if (_chromeTrim >= 0)
        {
            return _chromeTrim;
        }

        int measured = MeasureChrome(_terminalHwnd);

        // A tab strip is tens of pixels tall at any sane DPI. Anything outside that means
        // the measurement caught the window mid-layout, so prefer the computed value.
        if (measured <= 0 || measured > 200)
        {
            // WT's tab strip is 40 DIP tall. Scale it if the drag bar could not be found.
            uint dpi = Native.GetDpiForWindow(_terminalHwnd);
            measured = (int)Math.Round(40.0 * (dpi == 0 ? 96 : dpi) / 96.0) + 1;
        }

        _chromeTrim = measured;
        return _chromeTrim;
    }

    /// <summary>
    /// Distance from the top of the terminal window to the bottom of its drag bar.
    ///
    /// Deliberately not the drag bar's own height: WT's window rect starts one pixel above
    /// the bar, and trimming only the bar's height leaves that pixel row showing along the
    /// top of the dock. Measuring to the bar's bottom edge absorbs the border at any DPI.
    /// </summary>
    private static int MeasureChrome(IntPtr terminal)
    {
        if (!Native.GetWindowRect(terminal, out var windowRect))
        {
            return 0;
        }

        int trim = 0;
        var buffer = new StringBuilder(256);

        Native.EnumWindowsProc callback = (hwnd, _) =>
        {
            buffer.Clear();
            Native.GetClassName(hwnd, buffer, buffer.Capacity);
            if (buffer.ToString() == TerminalDragBarClass && Native.GetWindowRect(hwnd, out var bar))
            {
                trim = bar.Bottom - windowRect.Top;
                return false;
            }

            return true;
        };

        Native.EnumChildWindows(terminal, callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return trim;
    }

    public void FocusTerminal()
    {
        if (_forwardingFocus || _terminalHwnd == IntPtr.Zero || !Native.IsWindow(_terminalHwnd))
        {
            return;
        }

        uint hostThread = Native.GetCurrentThreadId();
        uint childThread = Native.GetWindowThreadProcessId(_terminalHwnd, out _);

        _forwardingFocus = true;
        try
        {
            if (hostThread == childThread)
            {
                Native.SetFocus(_terminalHwnd);
                return;
            }

            if (Native.AttachThreadInput(hostThread, childThread, true))
            {
                Native.SetFocus(_terminalHwnd);
                Native.AttachThreadInput(hostThread, childThread, false);
            }
        }
        finally
        {
            _forwardingFocus = false;
        }
    }

    /// <summary>Null on success, otherwise a message explaining why the window resisted.</summary>
    private string? Embed(IntPtr child, uint ownerPid)
    {
        long originalStyle = Native.GetWindowStyle(child, Native.GWL_STYLE);
        long originalEx = Native.GetWindowStyle(child, Native.GWL_EXSTYLE);

        Native.ShowWindow(child, Native.SW_HIDE);

        long style = Native.GetWindowStyle(child, Native.GWL_STYLE);
        style &= ~(Native.WS_CAPTION | Native.WS_THICKFRAME | Native.WS_SYSMENU
                   | Native.WS_MINIMIZEBOX | Native.WS_MAXIMIZEBOX | Native.WS_BORDER
                   | Native.WS_DLGFRAME | Native.WS_POPUP);
        style |= Native.WS_CHILD | Native.WS_VISIBLE;
        Native.SetWindowStyle(child, Native.GWL_STYLE, style);

        long ex = Native.GetWindowStyle(child, Native.GWL_EXSTYLE);
        ex &= ~(long)(Native.WS_EX_WINDOWEDGE | Native.WS_EX_CLIENTEDGE
                      | Native.WS_EX_APPWINDOW | Native.WS_EX_STATICEDGE
                      | Native.WS_EX_DLGMODALFRAME);
        Native.SetWindowStyle(child, Native.GWL_EXSTYLE, ex);

        Native.SetParent(child, _host.Handle);

        // GetParent is the unambiguous test. SetParent returns null both for failure and
        // for a window that had no parent, which every top-level window did.
        if (Native.GetParent(child) != _host.Handle)
        {
            Native.SetWindowStyle(child, Native.GWL_STYLE, originalStyle);
            Native.SetWindowStyle(child, Native.GWL_EXSTYLE, originalEx);
            Native.SetWindowPos(child, IntPtr.Zero, 0, 0, 0, 0,
                Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOZORDER | Native.SWP_FRAMECHANGED);
            Native.ShowWindow(child, Native.SW_SHOWNORMAL);
            return DescribeEmbedFailure(ownerPid);
        }

        Native.SetWindowPos(child, IntPtr.Zero, 0, 0, 0, 0,
            Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOZORDER | Native.SWP_FRAMECHANGED);
        Fit();
        Native.ShowWindow(child, Native.SW_SHOWNA);
        return null;
    }

    private static string DescribeEmbedFailure(uint ownerPid)
    {
        bool weAreElevated = Elevation.IsCurrentProcessElevated();
        bool? terminalElevated = Elevation.IsProcessElevated((int)ownerPid);

        if (terminalElevated == true && !weAreElevated)
        {
            return "The terminal window belongs to an elevated Windows Terminal, and Windows "
                 + "does not allow an unelevated program to take over an elevated window.\n\n"
                 + "This happens when any elevated terminal is already running, because "
                 + "Windows Terminal creates new windows inside whichever process it already "
                 + "has. Run Docked Console as administrator, or close the elevated terminal "
                 + "windows first.\n\n"
                 + "A terminal window was opened and could not be captured. Close it manually.";
        }

        return "The terminal window could not be captured, so the dock has nothing to show. "
             + "A terminal window may have been left open.";
    }

    private void CheckHealth()
    {
        if (_disposed)
        {
            return;
        }

        // The window handle is the authority for both hosts. Watching a process would be
        // wrong for Windows Terminal, whose window can live inside a process we did not
        // start and must not touch.
        bool gone = _terminalHwnd == IntPtr.Zero || !Native.IsWindow(_terminalHwnd);
        if (!gone)
        {
            return;
        }

        _healthTimer.Stop();
        _fitTimer.Stop();
        bool immediate = DateTime.UtcNow - _embeddedAtUtc < StartupFailureWindow;
        _terminalHwnd = IntPtr.Zero;

        if (_cfg.ParsedOnShellExit == ShellExitAction.Quit)
        {
            KillOwnedProcess();

            // Nobody can type exit that fast. A terminal that vanishes this soon after
            // being embedded failed to launch, and quitting silently would look like the
            // dock itself refusing to start.
            Closed?.Invoke(immediate
                ? "The terminal closed immediately after starting, so the dock has undocked.\n\n"
                  + "Check that the terminal launches on its own, then start the dock again."
                : null);
            return;
        }

        if (DateTime.UtcNow - _lastStartUtc < RapidFailureWindow)
        {
            _rapidFailures++;
        }
        else
        {
            _rapidFailures = 0;
        }

        if (_rapidFailures >= MaxRapidFailures)
        {
            Fault?.Invoke("the terminal keeps closing immediately; stopped relaunching it");
            return;
        }

        KillOwnedProcess();
        Start();
    }

    private (IntPtr Hwnd, uint OwnerPid) LocateWindow(Process launcher, HashSet<IntPtr> existing)
    {
        var deadline = DateTime.UtcNow + LocateTimeout;

        while (DateTime.UtcNow < deadline)
        {
            foreach (var hwnd in FindWindowsByClass(WindowClass))
            {
                if (existing.Contains(hwnd))
                {
                    continue;
                }

                Native.GetWindowThreadProcessId(hwnd, out uint pid);

                if (Kind == TerminalKind.Conhost)
                {
                    // A console window is attributed to the attached shell, which is a
                    // descendant of the conhost we launched rather than conhost itself.
                    if (!IsSelfOrDescendant((int)pid, launcher.Id))
                    {
                        continue;
                    }
                }

                return (hwnd, pid);
            }

            Thread.Sleep(50);
        }

        return (IntPtr.Zero, 0);
    }

    private static HashSet<IntPtr> FindWindowsByClass(string className)
    {
        var found = new HashSet<IntPtr>();
        var buffer = new StringBuilder(256);

        Native.EnumWindowsProc callback = (hwnd, _) =>
        {
            buffer.Clear();
            Native.GetClassName(hwnd, buffer, buffer.Capacity);
            if (buffer.ToString() == className)
            {
                found.Add(hwnd);
            }

            return true;
        };

        Native.EnumWindows(callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return found;
    }

    private static bool IsSelfOrDescendant(int candidate, int rootPid)
    {
        if (candidate == rootPid)
        {
            return true;
        }

        var parents = SnapshotParents();
        int cursor = candidate;

        for (int hop = 0; hop < 16; hop++)
        {
            if (!parents.TryGetValue(cursor, out int parent) || parent == 0 || parent == cursor)
            {
                return false;
            }

            if (parent == rootPid)
            {
                return true;
            }

            cursor = parent;
        }

        return false;
    }

    private static Dictionary<int, int> SnapshotParents()
    {
        var map = new Dictionary<int, int>();
        IntPtr snapshot = Native.CreateToolhelp32Snapshot(Native.TH32CS_SNAPPROCESS, 0);
        if (snapshot == Native.INVALID_HANDLE_VALUE)
        {
            return map;
        }

        try
        {
            var entry = new PROCESSENTRY32W
            {
                dwSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<PROCESSENTRY32W>(),
            };

            if (!Native.Process32FirstW(snapshot, ref entry))
            {
                return map;
            }

            do
            {
                map[(int)entry.th32ProcessID] = (int)entry.th32ParentProcessID;
            }
            while (Native.Process32NextW(snapshot, ref entry));
        }
        finally
        {
            Native.CloseHandle(snapshot);
        }

        return map;
    }

    /// <summary>
    /// Detaches the terminal window and asks it to close. Detaching first matters: a child
    /// window is destroyed with its parent, and letting that happen to a Windows Terminal
    /// window inside a shared process would tear it down without any chance to shut down
    /// its panes cleanly.
    /// </summary>
    private void ReleaseTerminal()
    {
        var hwnd = _terminalHwnd;
        _terminalHwnd = IntPtr.Zero;

        if (hwnd != IntPtr.Zero && Native.IsWindow(hwnd))
        {
            Native.ShowWindow(hwnd, Native.SW_HIDE);
            Native.SetParent(hwnd, IntPtr.Zero);
            Native.PostMessage(hwnd, Native.WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }

        KillOwnedProcess();
    }

    /// <summary>Only ever set for conhost, which we launched and therefore own outright.</summary>
    private void KillOwnedProcess()
    {
        var proc = _ownedProcess;
        _ownedProcess = null;

        if (proc is null)
        {
            return;
        }

        try
        {
            if (!proc.HasExited)
            {
                proc.Kill(entireProcessTree: true);
                proc.WaitForExit(3000);
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"kill failed: {ex.Message}");
        }
        finally
        {
            proc.Dispose();
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _healthTimer.Stop();
        _healthTimer.Dispose();
        _fitTimer.Stop();
        _fitTimer.Dispose();
        ReleaseTerminal();
    }
}
