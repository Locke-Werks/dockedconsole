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
#pragma once

#include <windows.h>

#include "config.h"

#include <unordered_map>
#include <vector>

namespace dock {

/// Pushes fullscreen windows out of the reserved strip.
///
/// The AppBar stops MAXIMIZED windows, because the shell subtracts the strip
/// from the work area and a maximizing window asks the shell where to go. A
/// fullscreen window does not ask: it sizes itself to the monitor and paints
/// over everything. The only way to stop that is to notice and move it.
///
/// Everything here runs on the thread that owns the host window. A
/// WINEVENT_OUTOFCONTEXT hook is dispatched on the thread that installed it,
/// from inside GetMessage, so there is nothing to marshal and no lock anywhere
/// in this file. Two consequences follow and are designed for: the callback can
/// arrive inside a nested modal loop such as a tray menu, and if the UI thread
/// stops pumping, events queue up undelivered.
class Enforcer {
public:
    Enforcer() = default;
    ~Enforcer();

    Enforcer(const Enforcer&) = delete;
    Enforcer& operator=(const Enforcer&) = delete;

    void Start(HWND host, const Config& cfg);
    void Stop();

    /// The strip to defend, the monitor it sits on, and which edge it is on.
    ///
    /// The edge is passed in rather than inferred from the two rects, because
    /// inference is ambiguous and wrong in configurations that really occur. A
    /// top-edge strip trimmed by a vertical taskbar has strip.left > monitor.left
    /// and strip.right == monitor.right, which is indistinguishable from a
    /// right-edge dock by geometry alone: reading it that way clamped every
    /// fullscreen window into a sliver the width of the taskbar. A bottom-edge
    /// strip sitting above a bottom taskbar matches no branch at all and silently
    /// disabled the block. The caller already knows the answer from config.
    void SetTarget(HMONITOR monitor, const RECT& monitor_bounds, const RECT& strip,
                   DockEdge edge);

    /// The reparented terminal, so its own location changes are ignored cheaply.
    void SetTerminal(HWND terminal) { terminal_ = terminal; }

    /// Re-asserts topmost. Debounced, so calling it from every notification is
    /// free.
    void ReassertTopmost();

    /// Checks the current foreground window now, without waiting for an event.
    /// Used on ABN_FULLSCREENAPP, which can arrive before the location change.
    void ScanForeground();

private:
    struct Record {
        DWORD pid = 0;
        int attempts = 0;
        ULONGLONG window_start = 0;
        ULONGLONG last_attempt = 0;
        ULONGLONG give_up_until = 0;
        RECT last_target{};
        bool gave_up = false;
    };

    static void CALLBACK EventThunk(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                    LONG object_id, LONG child_id,
                                    DWORD thread_id, DWORD time);

    void OnEvent(DWORD event, HWND hwnd, LONG object_id, LONG child_id);
    void Evaluate(HWND hwnd, bool bypass_cache);
    [[nodiscard]] bool LooksFullscreen(HWND hwnd, LONG_PTR style) const;
    void Clamp(HWND hwnd);
    void Forget(HWND hwnd);

    /// 64-slot direct-mapped "was not fullscreen recently" cache. A dragging
    /// window fires hundreds of location changes a second, and this is what stops
    /// each of them costing a DWM round trip.
    [[nodiscard]] bool RecentlyNotFullscreen(HWND hwnd) const;
    void RememberNotFullscreen(HWND hwnd);

    HWND host_ = nullptr;
    HWND terminal_ = nullptr;
    const Config* cfg_ = nullptr;

    std::vector<HWINEVENTHOOK> hooks_;

    HMONITOR monitor_ = nullptr;
    RECT monitor_bounds_{};
    RECT strip_{};
    RECT clamp_to_{};

    std::unordered_map<HWND, Record> ledger_;

    struct CacheSlot {
        HWND hwnd = nullptr;
        ULONGLONG when = 0;
    };
    CacheSlot cache_[64]{};

    ULONGLONG last_topmost_ = 0;
    int depth_ = 0;
    bool running_ = false;
};

} // namespace dock
