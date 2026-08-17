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
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DockedConsole;

internal sealed class DockConfig
{
    private const string FileName = "dockedconsole.json";

    /// <summary>left, top, right or bottom.</summary>
    public string Edge { get; set; } = "right";

    /// <summary>
    /// Thickness of the reserved strip in PHYSICAL pixels. The AppBar API works in
    /// physical pixels, so this is not scaled by the monitor's DPI setting.
    /// </summary>
    public int WidthPhysicalPx { get; set; } = 735;

    /// <summary>Device name such as \\.\DISPLAY1. Null means the primary monitor.</summary>
    public string? MonitorDeviceName { get; set; }

    /// <summary>
    /// windowsterminal or conhost. Windows Terminal renders the full Unicode range and
    /// uses your existing profile; conhost cannot render anything outside the BMP.
    /// </summary>
    public string Terminal { get; set; } = "windowsterminal";

    /// <summary>
    /// Command line for wt.exe. "-w new" forces a fresh window rather than a tab in an
    /// existing one. Add -p "Profile Name" to pick a specific profile.
    /// </summary>
    public string TerminalArgs { get; set; } = "-w new";

    /// <summary>
    /// Pixels of terminal chrome hidden above the top of the dock. -1 measures Windows
    /// Terminal's tab strip at runtime, which is the right answer at any DPI.
    /// </summary>
    public int ChromeTrimPx { get; set; } = -1;

    /// <summary>
    /// auto, never or always. Embedding reparents the terminal's window, and UIPI forbids
    /// that across integrity levels, so "auto" elevates only when an elevated Windows
    /// Terminal is running and the window we are handed would therefore be elevated too.
    /// </summary>
    public string Elevation { get; set; } = "auto";

    /// <summary>Full path to a shell, or "auto" to prefer pwsh and fall back to powershell.</summary>
    public string Shell { get; set; } = "auto";

    public string ShellArgs { get; set; } = "-NoLogo";

    /// <summary>
    /// conhost does no font fallback, so this single face must cover every glyph the
    /// shell emits. Empty leaves whatever the console defaults to.
    /// </summary>
    public string FontFace { get; set; } = "3270 Nerd Font Mono";

    /// <summary>Cell height in pixels. Console fonts are sized in pixels, not points.</summary>
    public int FontSizePx { get; set; } = 20;

    public bool Utf8Console { get; set; } = true;

    public string BackgroundColor { get; set; } = "#0C0C0C";

    /// <summary>
    /// quit or relaunch. "quit" makes typing exit in the shell undock and close the whole
    /// thing, the way exit closes an ordinary terminal window. "relaunch" keeps the dock
    /// up and starts a fresh shell in it.
    /// </summary>
    public string OnShellExit { get; set; } = "quit";

    public bool Topmost { get; set; } = true;

    /// <summary>Drop out of the way when a fullscreen app appears, as the taskbar does.</summary>
    public bool YieldToFullscreenApps { get; set; } = true;

    /// <summary>
    /// Beside the executable when a config is already sitting there, which covers portable
    /// use and the development build. Otherwise the per-user data folder, because an
    /// installed copy lives in Program Files and a normal user cannot write to it.
    /// </summary>
    [JsonIgnore]
    public static string Path
    {
        get
        {
            string portable = System.IO.Path.Combine(AppContext.BaseDirectory, FileName);
            return File.Exists(portable) ? portable : PerUserPath;
        }
    }

    [JsonIgnore]
    private static string PerUserPath => System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "DockedConsole",
        FileName);

    [JsonIgnore]
    public DockEdge ParsedEdge => Edge?.Trim().ToLowerInvariant() switch
    {
        "left" => DockEdge.Left,
        "top" => DockEdge.Top,
        "bottom" => DockEdge.Bottom,
        _ => DockEdge.Right,
    };

    [JsonIgnore]
    public ElevationPolicy ParsedElevation => Elevation?.Trim().ToLowerInvariant() switch
    {
        "never" or "none" => ElevationPolicy.Never,
        "always" => ElevationPolicy.Always,
        _ => ElevationPolicy.Auto,
    };

    [JsonIgnore]
    public ShellExitAction ParsedOnShellExit =>
        OnShellExit?.Trim().ToLowerInvariant() is "relaunch" or "restart"
            ? ShellExitAction.Relaunch
            : ShellExitAction.Quit;

    [JsonIgnore]
    public TerminalKind ParsedTerminal =>
        Terminal?.Trim().ToLowerInvariant() is "conhost" or "console"
            ? TerminalKind.Conhost
            : TerminalKind.WindowsTerminal;

    [JsonIgnore]
    public Color ParsedBackground
    {
        get
        {
            try
            {
                return ColorTranslator.FromHtml(BackgroundColor);
            }
            catch (Exception)
            {
                return Color.FromArgb(0x0C, 0x0C, 0x0C);
            }
        }
    }

    /// <summary>Resolves "auto" to pwsh 7 when installed, otherwise Windows PowerShell.</summary>
    public string ResolveShellPath()
    {
        if (!string.Equals(Shell, "auto", StringComparison.OrdinalIgnoreCase)
            && !string.IsNullOrWhiteSpace(Shell))
        {
            return Environment.ExpandEnvironmentVariables(Shell);
        }

        string pwsh = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "PowerShell", "7", "pwsh.exe");

        if (File.Exists(pwsh))
        {
            return pwsh;
        }

        return System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell", "v1.0", "powershell.exe");
    }

    /// <summary>wt.exe is an app execution alias in the per-user WindowsApps folder.</summary>
    public string ResolveTerminalPath()
    {
        return System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Microsoft", "WindowsApps", "wt.exe");
    }

    public Screen ResolveScreen()
    {
        if (!string.IsNullOrWhiteSpace(MonitorDeviceName))
        {
            foreach (var s in Screen.AllScreens)
            {
                if (string.Equals(s.DeviceName, MonitorDeviceName, StringComparison.OrdinalIgnoreCase))
                {
                    return s;
                }
            }
        }

        return Screen.PrimaryScreen ?? Screen.AllScreens[0];
    }

    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public static DockConfig Load()
    {
        try
        {
            if (File.Exists(Path))
            {
                var json = File.ReadAllText(Path);
                var cfg = JsonSerializer.Deserialize<DockConfig>(json, Options);
                if (cfg is not null)
                {
                    cfg.Clamp();
                    return cfg;
                }
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"config load failed, using defaults: {ex.Message}");
        }

        var fresh = new DockConfig();
        fresh.Save();
        return fresh;
    }

    public void Save()
    {
        try
        {
            string target = Path;
            string? dir = System.IO.Path.GetDirectoryName(target);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            File.WriteAllText(target, JsonSerializer.Serialize(this, Options));
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"config save failed: {ex.Message}");
        }
    }

    private void Clamp()
    {
        // A strip wider than the monitor would reserve the entire desktop and leave
        // nowhere to work, which is unrecoverable without editing the file by hand.
        var bounds = ResolveScreen().Bounds;
        int max = ParsedEdge is DockEdge.Left or DockEdge.Right
            ? bounds.Width / 2
            : bounds.Height / 2;

        WidthPhysicalPx = Math.Clamp(WidthPhysicalPx, 80, Math.Max(80, max));
    }
}
