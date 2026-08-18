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
#include "tray.h"

#include "diag_log.h"
#include "ids.h"

#include <shellapi.h>

#include <algorithm>
#include <string>

namespace dock {
namespace {

constexpr UINT kIconId = 1;

/// Copies into a fixed-size struct field, truncating rather than overrunning.
/// szInfoTitle is 64 wchars and szInfo is 256; writing past either corrupts the
/// NOTIFYICONDATA and the call fails in a way that looks like a shell bug.
void CopyBounded(wchar_t* destination, size_t capacity, std::wstring_view text)
{
    const size_t count = (std::min)(text.size(), capacity - 1);
    std::copy_n(text.begin(), count, destination);
    destination[count] = L'\0';
}

} // namespace

UINT TaskbarCreatedMessage()
{
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

Tray::~Tray()
{
    Destroy();
}

NOTIFYICONDATAW Tray::BaseData() const
{
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = host_;
    data.uID = kIconId;
    return data;
}

bool Tray::Create(HWND host)
{
    host_ = host;

    // The icon comes from our own resource section, which is already mapped, so
    // unlike ExtractAssociatedIcon it cannot fail for environmental reasons and
    // needs no hand-drawn fallback. Ask for the small-icon metric so the shell
    // gets the right frame out of the multi-size .ico instead of downscaling
    // the 32x32, which is visibly softer at 150%.
    icon_ = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));

    NOTIFYICONDATAW data = BaseData();
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = WM_APP_TRAY;
    data.hIcon = icon_;
    CopyBounded(data.szTip, ARRAYSIZE(data.szTip), L"Docked Console");

    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        log::Write(L"tray: NIM_ADD failed");
        return false;
    }

    // Version 4 changes the callback packing: the cursor position arrives in
    // wParam and the event in LOWORD(lParam), and a right-click or the keyboard
    // menu key both arrive as WM_CONTEXTMENU rather than WM_RBUTTONUP.
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);

    added_ = true;
    return true;
}

void Tray::Readd()
{
    if (!host_) {
        return;
    }
    added_ = false;
    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
    Create(host_);
    log::Write(L"tray: re-added the icon after an explorer restart");
}

void Tray::Destroy()
{
    if (added_) {
        NOTIFYICONDATAW data = BaseData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
    }

    if (icon_) {
        // LoadImageW without LR_SHARED hands back an icon we own.
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

void Tray::Notify(std::wstring_view message)
{
    if (!added_) {
        return;
    }

    NOTIFYICONDATAW data = BaseData();
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    CopyBounded(data.szInfoTitle, ARRAYSIZE(data.szInfoTitle), L"Docked Console");
    CopyBounded(data.szInfo, ARRAYSIZE(data.szInfo), message);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

UINT Tray::OnCallback(WPARAM wparam, LPARAM lparam)
{
    const UINT event = LOWORD(lparam);

    // WM_CONTEXTMENU is what version 4 sends for both a right-click and the
    // keyboard menu key. WM_RBUTTONUP is accepted too, so a failed NIM_SETVERSION
    // does not silently leave the menu unreachable.
    if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
        return ShowMenu();
    }

    // A left click opens the menu as well. There is no primary action to take:
    // the dock is always visible, so there is nothing to toggle.
    if (event == NIN_SELECT || event == WM_LBUTTONUP) {
        return ShowMenu();
    }

    UNREFERENCED_PARAMETER(wparam);
    return 0;
}

UINT Tray::ShowMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return 0;
    }

    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Docked Console");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRestartShell, L"Restart shell");
    AppendMenuW(menu, MF_STRING, kMenuReloadConfig, L"Reload config");
    AppendMenuW(menu, MF_STRING, kMenuEditConfig, L"Edit config");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Undock and exit");

    POINT cursor{};
    GetCursorPos(&cursor);

    menu_up_ = true;

    // KB135788: without SetForegroundWindow first, the menu will not dismiss
    // when the user clicks elsewhere, and without the WM_NULL afterwards it can
    // stay up even then. Both halves are required and neither is superstition.
    SetForegroundWindow(host_);

    UINT align = TPM_LEFTALIGN;
    if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0) {
        align = TPM_RIGHTALIGN;
    }

    const UINT command = static_cast<UINT>(TrackPopupMenuEx(
        menu, align | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
        cursor.x, cursor.y, host_, nullptr));

    PostMessageW(host_, WM_NULL, 0, 0);

    menu_up_ = false;
    DestroyMenu(menu);
    return command;
}

} // namespace dock
