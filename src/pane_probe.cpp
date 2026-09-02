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
#include "pane_probe.h"

// Before UIAutomation.h, which spells its interfaces with the `interface`
// macro that objbase.h defines and a lean-and-mean windows.h does not pull in.
#include <objbase.h>
#include <oleauto.h>

#include <UIAutomation.h>
#include <wrl/client.h>

namespace dock {

int CountTerminalPanes(HWND terminal, DWORD timeout_ms)
{
    using Microsoft::WRL::ComPtr;

    if (!terminal || !IsWindow(terminal)) {
        return -1;
    }

    ComPtr<IUIAutomation2> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation)))
        || !automation) {
        return -1;
    }

    // The default is twenty seconds per call into the provider, and the provider
    // is the terminal's own UI thread. This runs while another program is blocked
    // in SendMessage waiting for our answer, so a hung terminal must not be
    // allowed to hold that program for twenty seconds on our behalf.
    (void)automation->put_ConnectionTimeout(timeout_ms);

    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(terminal, &root)) || !root) {
        return -1;
    }

    VARIANT wanted{};
    wanted.vt = VT_BSTR;
    wanted.bstrVal = SysAllocString(L"TermControl");
    if (!wanted.bstrVal) {
        return -1;
    }

    ComPtr<IUIAutomationCondition> condition;
    const HRESULT made = automation->CreatePropertyCondition(UIA_ClassNamePropertyId, wanted,
                                                             &condition);
    VariantClear(&wanted);
    if (FAILED(made) || !condition) {
        return -1;
    }

    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &found)) || !found) {
        return -1;
    }

    int count = 0;
    if (FAILED(found->get_Length(&count)) || count <= 0) {
        return -1;
    }
    return count;
}

} // namespace dock
