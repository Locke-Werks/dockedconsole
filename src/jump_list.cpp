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
#include "jump_list.h"

#include "diag_log.h"
#include "win32.h"
#include "wt_settings.h"

#include <objectarray.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>

#include <string>

namespace dock::jump_list {
namespace {

/// Owns one COM reference.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr()
    {
        if (ptr_) {
            ptr_->Release();
        }
    }
    T* operator->() const { return ptr_; }
    T* get() const { return ptr_; }
    T** put() { return &ptr_; }

private:
    T* ptr_ = nullptr;
};

/// Same quoting rule the rest of the dock uses for CommandLineToArgvW: a
/// backslash run before a quote is doubled, and so is one before the closing
/// quote.
std::wstring Quote(const std::wstring& value)
{
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

bool AddLink(IObjectCollection* items, const std::wstring& exe, const ProfileEntry& profile)
{
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.put())))) {
        return false;
    }

    link->SetPath(exe.c_str());
    link->SetArguments((L"--pane-profile " + Quote(profile.name)).c_str());
    link->SetDescription((L"Open a " + profile.name + L" pane in the dock").c_str());
    if (profile.icon.empty()) {
        link->SetIconLocation(exe.c_str(), 0);
    } else {
        link->SetIconLocation(profile.icon.c_str(), 0);
    }

    // A custom-category link shows its System.Title, not its file name.
    ComPtr<IPropertyStore> store;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(store.put())))) {
        return false;
    }
    PROPVARIANT title;
    if (FAILED(InitPropVariantFromString(profile.name.c_str(), &title))) {
        return false;
    }
    const HRESULT set = store->SetValue(PKEY_Title, title);
    PropVariantClear(&title);
    if (FAILED(set) || FAILED(store->Commit())) {
        return false;
    }

    return SUCCEEDED(items->AddObject(link.get()));
}

} // namespace

void ClaimAppId()
{
    const HRESULT hr = SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
    if (FAILED(hr)) {
        log::Writef(L"jumplist: could not claim the app id (0x%08lX)", static_cast<unsigned long>(hr));
    }
}

void Publish()
{
    const std::vector<ProfileEntry> profiles = ListProfiles();
    const std::wstring exe = ExePath();

    ComPtr<ICustomDestinationList> list;
    HRESULT hr = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(list.put()));
    if (FAILED(hr)) {
        log::Writef(L"jumplist: no destination list (0x%08lX)", static_cast<unsigned long>(hr));
        return;
    }
    list->SetAppID(kAppUserModelId);

    UINT max_slots = 0;
    ComPtr<IObjectArray> removed;
    hr = list->BeginList(&max_slots, IID_PPV_ARGS(removed.put()));
    if (FAILED(hr)) {
        log::Writef(L"jumplist: BeginList failed (0x%08lX)", static_cast<unsigned long>(hr));
        return;
    }

    ComPtr<IObjectCollection> items;
    hr = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(items.put()));
    if (FAILED(hr)) {
        list->AbortList();
        return;
    }

    int added = 0;
    for (const ProfileEntry& profile : profiles) {
        if (AddLink(items.get(), exe, profile)) {
            ++added;
        }
    }

    // Tasks rather than a custom category: tasks are never subject to the
    // user removing an item, which a custom category would have to track
    // through the removed array, and they are what Windows Terminal itself uses.
    if (added > 0) {
        ComPtr<IObjectArray> array;
        if (SUCCEEDED(items->QueryInterface(IID_PPV_ARGS(array.put())))) {
            hr = list->AddUserTasks(array.get());
            if (FAILED(hr)) {
                log::Writef(L"jumplist: AddUserTasks failed (0x%08lX)",
                            static_cast<unsigned long>(hr));
            }
        }
    }

    hr = list->CommitList();
    log::Writef(L"jumplist: %d of %zu profiles published (0x%08lX)", added, profiles.size(),
                static_cast<unsigned long>(hr));
}

} // namespace dock::jump_list
