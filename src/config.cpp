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
#include "config.h"

#include "json.h"
#include "monitors.h"
#include "win32.h"

#include <algorithm>
#include <vector>

namespace dock {
namespace {

constexpr wchar_t kFileName[] = L"dockedconsole.json";

std::wstring PerUserPath()
{
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData);
    if (dir.empty()) {
        return {};
    }
    return JoinPath(JoinPath(std::move(dir), L"DockedConsole"), kFileName);
}

/// Reads a UTF-8 file into UTF-16. Empty on any failure, which the caller treats
/// as unreadable rather than as an empty config.
bool ReadTextFile(const std::wstring& path, std::wstring& out)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart > (8 << 20)) {
        return false;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!bytes.empty()
        && !ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                     &read, nullptr)) {
        return false;
    }
    bytes.resize(read);

    // Strip a UTF-8 BOM. System.Text.Json consumed it silently, so any file that
    // has been through Notepad carries one and would otherwise fail on the
    // invisible U+FEFF before the first key.
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    out = Widen(bytes);
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos && !EnsureDirectory(path.substr(0, slash))) {
        return false;
    }

    // Write a temp file and rename over the target, so an interrupted save
    // cannot leave a half-written config behind.
    const std::wstring temp = path + L".tmp";
    {
        Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            return false;
        }

        // UTF-8, no BOM, matching what System.Text.Json wrote so an existing
        // file stays byte-comparable.
        const std::string utf8 = Narrow(text);
        DWORD written = 0;
        if (!utf8.empty()
            && !WriteFile(file.Get(), utf8.data(), static_cast<DWORD>(utf8.size()),
                          &written, nullptr)) {
            return false;
        }
    }

    return MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void ReadString(const json::Object& object, std::wstring_view key, std::wstring& out)
{
    const json::Value* value = json::Find(object, key);
    if (!value) {
        return;
    }
    if (value->type == json::Value::Type::String) {
        out = value->string;
    } else if (value->type == json::Value::Type::Null) {
        out.clear();
    }
}

void ReadInt(const json::Object& object, std::wstring_view key, int& out)
{
    const json::Value* value = json::Find(object, key);
    if (value && value->type == json::Value::Type::Number) {
        out = static_cast<int>(value->number);
    }
}

void ReadBool(const json::Object& object, std::wstring_view key, bool& out)
{
    const json::Value* value = json::Find(object, key);
    if (value && value->type == json::Value::Type::Bool) {
        out = value->boolean;
    }
}

void ReadStringArray(const json::Object& object, std::wstring_view key,
                     std::vector<std::wstring>& out)
{
    const json::Value* value = json::Find(object, key);
    if (value && value->type == json::Value::Type::Array) {
        out = value->array;
    }
}

int ParseHexPair(wchar_t high, wchar_t low, bool& ok)
{
    const auto digit = [&ok](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        ok = false;
        return 0;
    };
    return digit(high) * 16 + digit(low);
}

} // namespace

DockEdge Config::ParsedEdge() const
{
    const std::wstring value = ToLower(std::wstring(Trim(edge)));
    if (value == L"left")   return DockEdge::Left;
    if (value == L"top")    return DockEdge::Top;
    if (value == L"bottom") return DockEdge::Bottom;
    return DockEdge::Right;
}

ElevationPolicy Config::ParsedElevation() const
{
    const std::wstring value = ToLower(std::wstring(Trim(elevation)));
    if (value == L"never" || value == L"none") return ElevationPolicy::Never;
    if (value == L"always")                    return ElevationPolicy::Always;
    return ElevationPolicy::Auto;
}

ShellExitAction Config::ParsedOnShellExit() const
{
    const std::wstring value = ToLower(std::wstring(Trim(on_shell_exit)));
    if (value == L"relaunch" || value == L"restart") return ShellExitAction::Relaunch;
    return ShellExitAction::Quit;
}

COLORREF Config::Background() const
{
    constexpr COLORREF fallback = RGB(0x0C, 0x0C, 0x0C);

    std::wstring_view text = Trim(background_color);
    if (!text.empty() && text.front() == L'#') {
        text.remove_prefix(1);
    }
    if (text.size() != 6) {
        return fallback;
    }

    bool ok = true;
    const int r = ParseHexPair(text[0], text[1], ok);
    const int g = ParseHexPair(text[2], text[3], ok);
    const int b = ParseHexPair(text[4], text[5], ok);
    if (!ok) {
        return fallback;
    }

    // RGB() and not a hand-assembled integer: COLORREF is 0x00BBGGRR, the
    // reverse of the HTML byte order, and #0C0C0C is grey either way round so
    // the mistake would not surface until somebody picked a real colour.
    return RGB(r, g, b);
}

std::wstring Config::ResolveTerminalPath() const
{
    std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) {
        return {};
    }
    return JoinPath(JoinPath(JoinPath(std::move(local), L"Microsoft"), L"WindowsApps"),
                    L"wt.exe");
}

bool Config::IsExcluded(std::wstring_view image) const
{
    return std::any_of(fullscreen_exclusions.begin(), fullscreen_exclusions.end(),
                       [image](const std::wstring& entry) {
                           return EqualsNoCase(entry, image);
                       });
}

void Config::Clamp()
{
    const MonitorInfo monitor = ResolveMonitor(monitor_device_name);
    const DockEdge which = ParsedEdge();

    const int span = (which == DockEdge::Left || which == DockEdge::Right)
                         ? Width(monitor.bounds)
                         : Height(monitor.bounds);

    const int maximum = (std::max)(80, span / 2);
    width_physical_px = (std::min)((std::max)(width_physical_px, 80), maximum);
}

std::wstring Config::FilePath()
{
    const std::wstring portable = JoinPath(ExeDir(), kFileName);
    if (FileExists(portable)) {
        return portable;
    }
    return PerUserPath();
}

Config Config::Load(ConfigStatus& status, std::wstring& detail)
{
    Config config;
    detail.clear();

    const std::wstring path = FilePath();
    if (path.empty()) {
        status = ConfigStatus::Unreadable;
        detail = L"could not work out where the config should live";
        return config;
    }

    if (!FileExists(path)) {
        status = ConfigStatus::Missing;
        config.Clamp();
        return config;
    }

    std::wstring text;
    if (!ReadTextFile(path, text)) {
        status = ConfigStatus::Unreadable;
        detail = L"could not read " + path;
        return config;
    }

    json::Object object;
    std::wstring error;
    if (!json::ParseObject(text, object, error)) {
        status = ConfigStatus::Unreadable;
        detail = error;
        return config;
    }

    ReadString(object, L"edge", config.edge);
    ReadInt(object, L"widthPhysicalPx", config.width_physical_px);
    ReadString(object, L"monitorDeviceName", config.monitor_device_name);
    ReadString(object, L"terminalArgs", config.terminal_args);
    ReadInt(object, L"chromeTrimPx", config.chrome_trim_px);
    ReadString(object, L"elevation", config.elevation);
    ReadString(object, L"onShellExit", config.on_shell_exit);
    ReadString(object, L"backgroundColor", config.background_color);
    ReadBool(object, L"topmost", config.topmost);
    ReadBool(object, L"blockFullscreen", config.block_fullscreen);
    ReadBool(object, L"pushTaskbar", config.push_taskbar);
    ReadStringArray(object, L"fullscreenExclusions", config.fullscreen_exclusions);

    // yieldToFullscreenApps is deliberately NOT migrated onto blockFullscreen.
    //
    // It is tempting, because the two keys are about the same situation, but it
    // would be wrong in both directions. `true` was the old default, written
    // into every config the C# version ever created, so it records no decision
    // anybody made; honouring it would ship the screen block switched off for
    // every existing user. `false` meant "do not drop to the bottom of the
    // z-order", which if anything argues for blocking rather than against it.
    //
    // So the key is reported and dropped, and blockFullscreen takes its default.
    // Someone who genuinely wants the old yielding behaviour sets
    // blockFullscreen to false and gets it.
    if (json::Find(object, L"yieldToFullscreenApps")) {
        detail = L"yieldToFullscreenApps is obsolete and was ignored; "
                 L"set blockFullscreen to false for the old behaviour";
    }

    config.Clamp();
    status = ConfigStatus::Loaded;
    return config;
}

bool Config::Save() const
{
    std::wstring text;
    text.reserve(1024);

    const auto line = [&text](std::wstring_view key, const std::wstring& value,
                              bool last = false) {
        text += L"  " + json::Quote(key) + L": " + value;
        text += last ? L"\n" : L",\n";
    };

    text += L"{\n";
    line(L"edge", json::Quote(edge));
    line(L"widthPhysicalPx", std::to_wstring(width_physical_px));
    line(L"monitorDeviceName",
         monitor_device_name.empty() ? L"null" : json::Quote(monitor_device_name));
    line(L"terminalArgs", json::Quote(terminal_args));
    line(L"chromeTrimPx", std::to_wstring(chrome_trim_px));
    line(L"elevation", json::Quote(elevation));
    line(L"onShellExit", json::Quote(on_shell_exit));
    line(L"backgroundColor", json::Quote(background_color));
    line(L"topmost", topmost ? L"true" : L"false");
    line(L"blockFullscreen", block_fullscreen ? L"true" : L"false");
    line(L"pushTaskbar", push_taskbar ? L"true" : L"false");

    std::wstring exclusions = L"[";
    for (size_t i = 0; i < fullscreen_exclusions.size(); ++i) {
        exclusions += (i == 0) ? L"\n    " : L",\n    ";
        exclusions += json::Quote(fullscreen_exclusions[i]);
    }
    exclusions += fullscreen_exclusions.empty() ? L"]" : L"\n  ]";
    line(L"fullscreenExclusions", exclusions, /*last=*/true);

    text += L"}\n";

    return WriteTextFile(FilePath(), text);
}

} // namespace dock
