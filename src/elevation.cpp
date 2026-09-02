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
#include "elevation.h"

#include "config.h"
#include "win32.h"
#include "wt_settings.h"

#include <shellapi.h>
#include <tlhelp32.h>

namespace dock {
namespace {

bool TokenIsElevated(HANDLE process, bool& elevated)
{
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) {
        return false;
    }

    Handle token(raw_token);
    TOKEN_ELEVATION info{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.Get(), TokenElevation, &info, sizeof(info),
                             &returned)) {
        return false;
    }

    elevated = info.TokenIsElevated != 0;
    return true;
}

/// Quotes one argument for a command line the shell will re-split. Only the
/// cases that can occur here are handled: a value containing spaces or quotes.
std::wstring QuoteArg(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // Backslashes before a quote must be doubled, then the quote escaped.
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

} // namespace

bool IsCurrentProcessElevated()
{
    bool elevated = false;
    return TokenIsElevated(GetCurrentProcess(), elevated) && elevated;
}

int IsProcessElevated(DWORD pid)
{
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        return -1;
    }

    bool elevated = false;
    if (!TokenIsElevated(process.Get(), elevated)) {
        return -1;
    }
    return elevated ? 1 : 0;
}

bool AnyElevatedTerminalRunning(DWORD* pid)
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry)) {
        return false;
    }

    do {
        if (EqualsNoCase(entry.szExeFile, L"WindowsTerminal.exe")
            && IsProcessElevated(entry.th32ProcessID) == 1) {
            if (pid) {
                *pid = entry.th32ProcessID;
            }
            return true;
        }
    } while (Process32NextW(snapshot.Get(), &entry));

    return false;
}

ElevationDecision DecideElevation(const Config& cfg)
{
    ElevationDecision decision;
    if (IsCurrentProcessElevated()) {
        return decision;
    }

    const ElevationPolicy policy = cfg.ParsedElevation();
    if (policy == ElevationPolicy::Always) {
        decision.elevate = true;
        decision.reason = L"\"elevation\" is \"always\"";
        return decision;
    }

    // A running elevated terminal decides it on its own: every new window is
    // born inside whichever monarch already exists. Only with none running does
    // the profile get a say, because that is when wt starts a fresh instance
    // and reads the profile to decide what kind.
    DWORD elevated_pid = 0;
    if (AnyElevatedTerminalRunning(&elevated_pid)) {
        decision.reason = L"an elevated Windows Terminal is already running (pid "
                          + std::to_wstring(elevated_pid) + L")";
    } else {
        const ProfileElevation column = PredictProfileElevation(cfg, ProfileTarget::Column);
        if (column.elevates == 1) {
            decision.reason = DescribeProfileElevation(column);
        } else if (!cfg.terminal_profile.empty()) {
            // A pane profile that elevates cannot join an unelevated window
            // either: Windows Terminal would open it in a new elevated window
            // instead, and every pane request would land there.
            const ProfileElevation pane = PredictProfileElevation(cfg, ProfileTarget::Pane);
            if (pane.elevates == 1) {
                decision.reason = DescribeProfileElevation(pane);
            }
        }
    }

    if (decision.reason.empty()) {
        return decision;
    }

    if (policy == ElevationPolicy::Auto) {
        decision.elevate = true;
    } else {
        decision.blocked = true;
    }
    return decision;
}

bool Relaunch(const std::vector<std::wstring>& args, std::wstring& message)
{
    const std::wstring exe = ExePath();
    if (exe.empty()) {
        message = L"could not determine this executable's path";
        return false;
    }

    std::wstring parameters;
    for (const auto& arg : args) {
        if (EqualsNoCase(arg, kRelaunchFlag)) {
            continue;
        }
        if (!parameters.empty()) {
            parameters.push_back(L' ');
        }
        parameters += QuoteArg(arg);
    }
    if (!parameters.empty()) {
        parameters.push_back(L' ');
    }
    parameters += kRelaunchFlag;

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // SEE_MASK_NOASYNC because this process exits immediately afterwards.
    // Without it the shell may still be servicing the request when we die, and
    // the elevated copy never starts.
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&info)) {
        message = L"relaunched elevated";
        return true;
    }

    if (GetLastError() == ERROR_CANCELLED) {
        message = L"The elevation prompt was declined, so Docked Console did not start.";
        return false;
    }

    message = L"could not relaunch elevated (error "
              + std::to_wstring(GetLastError()) + L")";
    return false;
}

} // namespace dock
