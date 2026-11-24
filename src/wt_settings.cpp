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
#include "wt_settings.h"

#include "config.h"
#include "json.h"
#include "win32.h"

namespace dock {
namespace {

bool IsString(const json::Value* value)
{
    return value && value->type == json::Value::Type::String;
}

/// Windows Terminal's own lookup order for a `-p` value: an exact name match
/// first, then a GUID, and the text is only tried as a GUID when it is shaped
/// like one. Hidden profiles are included, as they are there.
const json::Value* FindProfile(const json::Value& list, std::wstring_view selector)
{
    if (selector.empty()) {
        return nullptr;
    }

    for (const json::Value& profile : list.array) {
        const json::Value* name = json::Find(profile, L"name");
        if (IsString(name) && name->string == selector) {
            return &profile;
        }
    }

    if (selector.size() == 38 && selector.front() == L'{') {
        for (const json::Value& profile : list.array) {
            const json::Value* guid = json::Find(profile, L"guid");
            if (IsString(guid) && EqualsNoCase(guid->string, selector)) {
                return &profile;
            }
        }
    }

    return nullptr;
}

std::wstring ProfileLabel(const json::Value& profile)
{
    const json::Value* name = json::Find(profile, L"name");
    if (IsString(name) && !name->string.empty()) {
        return name->string;
    }
    const json::Value* guid = json::Find(profile, L"guid");
    if (IsString(guid)) {
        return guid->string;
    }
    return L"(unnamed)";
}

/// The profile's own setting wins; profiles.defaults is what it inherits; and
/// a profile that says nothing anywhere does not elevate.
int ElevateFlag(const json::Value* profile, const json::Value* defaults)
{
    for (const json::Value* scope : { profile, defaults }) {
        if (!scope) {
            continue;
        }
        const json::Value* elevate = json::Find(*scope, L"elevate");
        if (elevate && elevate->type == json::Value::Type::Bool) {
            return elevate->boolean ? 1 : 0;
        }
    }
    return 0;
}

bool ProfileCheckDisabled()
{
    wchar_t value[8] = {};
    const DWORD length = GetEnvironmentVariableW(L"DOCKEDCONSOLE_NO_PROFILE_CHECK", value,
                                                 ARRAYSIZE(value));
    return length > 0 && length < ARRAYSIZE(value) && value[0] != L'0';
}

} // namespace

std::wstring TerminalSettingsPath()
{
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) {
        return {};
    }

    // Stable first, because the wt.exe alias the dock launches is the stable
    // package's whenever both are installed.
    static constexpr const wchar_t* kRelative[] = {
        L"Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json",
        L"Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json",
        L"Microsoft\\Windows Terminal\\settings.json",
    };

    for (const wchar_t* relative : kRelative) {
        std::wstring path = JoinPath(local, relative);
        if (FileExists(path)) {
            return path;
        }
    }
    return {};
}

ProfileElevation PredictProfileElevation(const Config& cfg, ProfileTarget target)
{
    ProfileElevation out;

    const std::wstring selector = target == ProfileTarget::Column ? cfg.ProfileSelector()
                                                                  : cfg.terminal_profile;
    if (selector.empty()) {
        out.selector = L"the default profile";
    } else if (target == ProfileTarget::Column) {
        out.selector = L"-p " + selector + L" in terminalArgs";
    } else {
        out.selector = L"terminalProfile " + selector;
    }

    out.settings_path = TerminalSettingsPath();

    if (ProfileCheckDisabled()) {
        out.detail = L"skipped because DOCKEDCONSOLE_NO_PROFILE_CHECK is set";
        return out;
    }

    if (out.settings_path.empty()) {
        out.detail = L"no Windows Terminal settings.json was found";
        return out;
    }

    std::wstring text;
    if (!ReadUtf8File(out.settings_path, text)) {
        out.detail = L"could not read " + out.settings_path;
        return out;
    }

    json::Object root;
    std::wstring error;
    json::ParseOptions options;
    options.allow_nesting = true;
    if (!json::ParseObject(text, root, error, options)) {
        out.detail = L"could not parse " + out.settings_path + L": " + error;
        return out;
    }

    const json::Value* profiles = json::Find(root, L"profiles");
    if (!profiles || profiles->type != json::Value::Type::Object) {
        out.detail = L"settings.json has no profiles object";
        return out;
    }

    const json::Value* list = json::Find(*profiles, L"list");
    if (!list || list->type != json::Value::Type::Array) {
        out.detail = L"settings.json has no profiles.list";
        return out;
    }

    const json::Value* defaults = json::Find(*profiles, L"defaults");
    if (defaults && defaults->type != json::Value::Type::Object) {
        defaults = nullptr;
    }

    const json::Value* profile = FindProfile(*list, selector);
    if (!selector.empty() && !profile) {
        // Windows Terminal does not fail on a -p it cannot match. It opens the
        // default profile instead, so that is the one whose elevation matters.
        out.detail = L"no profile is named '" + selector
                     + L"', so Windows Terminal would open the default profile";
    }

    if (!profile) {
        const json::Value* default_guid = json::Find(root, L"defaultProfile");
        if (IsString(default_guid)) {
            profile = FindProfile(*list, default_guid->string);
        }
    }

    if (!profile) {
        // A missing or stale defaultProfile makes Windows Terminal fall back to
        // the first profile in the list, so follow it there too.
        for (const json::Value& candidate : list->array) {
            if (candidate.type == json::Value::Type::Object) {
                profile = &candidate;
                break;
            }
        }
    }

    if (!profile) {
        out.detail = L"settings.json lists no profiles";
        return out;
    }

    out.profile = ProfileLabel(*profile);
    out.elevates = ElevateFlag(profile, defaults);
    return out;
}

std::wstring DescribeProfileElevation(const ProfileElevation& elevation)
{
    if (elevation.elevates < 0) {
        return L"could not tell whether " + elevation.selector + L" elevates: "
               + elevation.detail;
    }

    std::wstring text = L"profile '" + elevation.profile + L"' (" + elevation.selector + L") "
                        + (elevation.elevates == 1 ? L"has \"elevate\": true"
                                                   : L"does not elevate");
    if (!elevation.detail.empty()) {
        text += L"; " + elevation.detail;
    }
    return text;
}

std::vector<ProfileEntry> ListProfiles()
{
    std::vector<ProfileEntry> out;

    const std::wstring path = TerminalSettingsPath();
    std::wstring text;
    if (path.empty() || !ReadUtf8File(path, text)) {
        return out;
    }

    json::Object root;
    std::wstring error;
    json::ParseOptions options;
    options.allow_nesting = true;
    if (!json::ParseObject(text, root, error, options)) {
        return out;
    }

    const json::Value* profiles = json::Find(root, L"profiles");
    if (!profiles || profiles->type != json::Value::Type::Object) {
        return out;
    }
    const json::Value* list = json::Find(*profiles, L"list");
    if (!list || list->type != json::Value::Type::Array) {
        return out;
    }

    for (const json::Value& profile : list->array) {
        if (profile.type != json::Value::Type::Object) {
            continue;
        }
        const json::Value* hidden = json::Find(profile, L"hidden");
        if (hidden && hidden->type == json::Value::Type::Bool && hidden->boolean) {
            continue;
        }
        const json::Value* name = json::Find(profile, L"name");
        if (!IsString(name) || name->string.empty()) {
            continue;
        }

        ProfileEntry entry;
        entry.name = name->string;

        const json::Value* icon = json::Find(profile, L"icon");
        if (IsString(icon) && !icon->string.empty()) {
            wchar_t expanded[MAX_PATH * 2] = {};
            const DWORD length =
                ExpandEnvironmentStringsW(icon->string.c_str(), expanded, ARRAYSIZE(expanded));
            if (length > 0 && length <= ARRAYSIZE(expanded)) {
                const std::wstring candidate = expanded;
                const size_t dot = candidate.rfind(L'.');
                const std::wstring ext = dot == std::wstring::npos ? L"" : candidate.substr(dot);
                if ((EqualsNoCase(ext, L".ico") || EqualsNoCase(ext, L".exe")
                     || EqualsNoCase(ext, L".dll"))
                    && FileExists(candidate)) {
                    entry.icon = candidate;
                }
            }
        }

        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace dock
