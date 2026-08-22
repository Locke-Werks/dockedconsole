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
#include "enforcer.h"

#include "diag_log.h"
#include "win32.h"

#include <dwmapi.h>

namespace dock {
namespace {

Enforcer* g_instance = nullptr;

/// Two physical pixels. With DWM extended frame bounds the only error left is
/// rounding at fractional scale factors, at most one pixel an edge. A larger
/// tolerance would be actively harmful: the case we care about is a window that
/// is ALMOST fullscreen, and one sitting well short of a 735px strip is already
/// respecting us.
constexpr int kTolerance = 2;

constexpr ULONGLONG kDebounceMs = 60;
constexpr ULONGLONG kCacheTtlMs = 400;
constexpr ULONGLONG kMinClampIntervalMs = 150;
constexpr ULONGLONG kAttemptWindowMs = 3000;
constexpr ULONGLONG kGiveUpMs = 60000;
constexpr ULONGLONG kTopmostDebounceMs = 120;
constexpr int kMaxAttempts = 5;
constexpr ULONGLONG kForever = ~0ULL;

/// Shell surfaces that are legitimately monitor-sized. Clamping these is not a
/// performance question, it is correctness: resizing XamlExplorerHostIslandWindow
/// visibly breaks Task View, Snap Assist and the Windows 11 Alt+Tab.
///
/// Compiled in rather than configurable, because nobody should need to know
/// these names to get working software.
constexpr const wchar_t* kExcludedClasses[] = {
    L"XamlExplorerHostIslandWindow",
    L"MultitaskingViewFrame",
    L"ForegroundStaging",
    L"TaskSwitcherWnd",
    L"TaskSwitcherOverlayWnd",
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
    L"Progman",
    L"WorkerW",
    L"Windows.UI.Core.CoreWindow",
    L"Shell_InputSwitchTopLevelWindow",
    L"Shell_LightDismissOverlay",
    L"EdgeUiInputTopWndClass",
    L"NarratorHelperWindow",
    L"DockedConsole.Host",
};

bool IsExcludedClass(const std::wstring& name)
{
    for (const wchar_t* excluded : kExcludedClasses) {
        if (name == excluded) {
            return true;
        }
    }
    return false;
}

/// Shell surfaces the dock has to get out from in front of.
///
/// The notification area's overflow, the panel behind the chevron, is the one
/// that matters, and it is worth saying why it needs help when nothing else
/// does. It is an ordinary window in the ordinary z-order band: explorer never
/// made it topmost, because it opens ABOVE the taskbar rather than over it, so
/// it had nothing to outrank. A dock standing on that corner of the screen is
/// topmost, and nothing in the ordinary band can be above a topmost window.
/// Raising it is not ours to do, so the only move left is to step down beside
/// it and go back up when it closes.
///
/// The visible half of this is a flyout that never appears. The half that bites
/// is that the icons in it stop being clickable, because the clicks land on us,
/// and an app whose only interface is a tray icon is then unreachable.
///
/// Compiled in for the same reason as kExcludedClasses: nobody should need to
/// know these names to get working software.
constexpr const wchar_t* kFlyoutClasses[] = {
    L"TopLevelWindowForOverflowXamlIsland", // Windows 11
    L"NotifyIconOverflowWindow",            // Windows 10
};

bool IsFlyoutClass(const std::wstring& name)
{
    for (const wchar_t* flyout : kFlyoutClasses) {
        if (name == flyout) {
            return true;
        }
    }
    return false;
}

/// The visible frame, which is not what GetWindowRect returns.
///
/// Since Windows 10 a resizable window's rect includes an invisible border of
/// SM_CXSIZEFRAME + SM_CXPADDEDBORDER per side, about 9 physical pixels at 125%
/// scaling. Measured on this project's dev machine, an ordinary MAXIMIZED
/// Explorer window reports GetWindowRect (-9,-9)-(2761,1981) on a 2752x2032
/// monitor: it overhangs the display on every edge. A "does this cover the
/// monitor" test built on GetWindowRect would therefore classify every maximized
/// window as fullscreen and move it, breaking the one behaviour that already
/// worked. DWM's extended frame bounds report (0,0)-(2752,1972) for the same
/// window, which is the truth.
bool VisibleFrame(HWND hwnd, LONG_PTR style, RECT& out)
{
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out,
                                        sizeof(out)))
        && !IsEmptyRect(out)) {
        return true;
    }

    // DWM has not composed the window yet, which is exactly the EVENT_OBJECT_SHOW
    // case. Fall back and deflate the border arithmetically.
    if (!GetWindowRect(hwnd, &out)) {
        return false;
    }

    if (style & WS_THICKFRAME) {
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) {
            dpi = 96;
        }
        const int bx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                       + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int by = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                       + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        out.left += bx;
        out.right -= bx;
        out.bottom -= by; // the top edge has no invisible border
    }
    return true;
}

} // namespace

Enforcer::~Enforcer()
{
    Stop();
}

void Enforcer::Start(HWND host, const Config& cfg)
{
    if (running_) {
        return;
    }

    host_ = host;
    cfg_ = &cfg;
    clamping_ = cfg.block_fullscreen;

    // Topmost earns hooks of its own: a dock that is not topmost is not in front
    // of anything and has nothing to yield.
    if (!clamping_ && !cfg.topmost) {
        log::Write(L"enforce: blockFullscreen and topmost are both off; not hooking");
        return;
    }

    // Published only once we are actually going to install hooks, so the
    // callback thunk can never see a half-configured instance.
    g_instance = this;

    // Tight ranges rather than EVENT_MIN..EVENT_MAX. The real cost of this layer
    // is the kernel marshalling events into our queue, not our filter chain, so
    // not subscribing beats filtering. A wide range would sign us up for every
    // name change, value change and caret move on the desktop.
    struct Range {
        DWORD min;
        DWORD max;
    };

    // Appearing, disappearing and taking focus. Both jobs need these, and for
    // the flyout yield they are the whole story: a flyout opens, is shown, and
    // is hidden again without ever moving.
    static constexpr Range kAlways[] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };

    // Motion, which only the clamp cares about. LOCATIONCHANGE is the most
    // expensive subscription on the desktop by a wide margin, a dragged window
    // firing it hundreds of times a second, so it is not taken out unless
    // something is going to read it.
    static constexpr Range kWhenClamping[] = {
        {EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
    };

    auto install = [this](const Range& range) {
        // WINEVENT_SKIPOWNPROCESS filters our own windows in the OS, before the
        // event is ever queued: the cheapest rejection available.
        HWINEVENTHOOK hook = SetWinEventHook(
            range.min, range.max, nullptr, EventThunk, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hook) {
            hooks_.push_back(hook);
        }
    };

    for (const Range& range : kAlways) {
        install(range);
    }
    if (clamping_) {
        for (const Range& range : kWhenClamping) {
            install(range);
        }
    }

    running_ = !hooks_.empty();
    log::Writef(L"enforce: %zu hook(s) installed; fullscreen clamp %s, flyout yield %s",
                hooks_.size(), clamping_ ? L"on" : L"off",
                cfg.topmost ? L"on" : L"off");
}

void Enforcer::Stop()
{
    // Before the hooks go. Once they are gone the hide event that would have
    // restored us is never delivered, and the dock stays out of the topmost band
    // with nothing left running to notice.
    Unyield();

    for (HWINEVENTHOOK hook : hooks_) {
        UnhookWinEvent(hook);
    }
    hooks_.clear();
    ledger_.clear();
    running_ = false;
    clamping_ = false;
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

void Enforcer::SetTarget(HMONITOR monitor, const RECT& monitor_bounds, const RECT& strip,
                         DockEdge edge)
{
    monitor_ = monitor;
    monitor_bounds_ = monitor_bounds;
    strip_ = strip;

    // The clamp target is the monitor minus the strip, NOT the work area.
    // Containing a fullscreen app out of the taskbar's space as well would be
    // more intrusive than asked for, and it would break the reasonable
    // expectation that a fullscreen app covers the taskbar.
    //
    // The edge comes from config. See the header for why inferring it from the
    // rects is not merely inelegant but actively wrong.
    clamp_to_ = monitor_bounds;
    switch (edge) {
    case DockEdge::Left:   clamp_to_.left = strip.right;   break;
    case DockEdge::Right:  clamp_to_.right = strip.left;   break;
    case DockEdge::Top:    clamp_to_.top = strip.bottom;   break;
    case DockEdge::Bottom: clamp_to_.bottom = strip.top;   break;
    }

    // A strip that swallowed the monitor would leave nothing to clamp into.
    if (IsEmptyRect(clamp_to_)) {
        log::Writef(L"enforce: strip (%d,%d)-(%d,%d) leaves no room on monitor "
                    L"(%d,%d)-(%d,%d); fullscreen block idle",
                    strip.left, strip.top, strip.right, strip.bottom,
                    monitor_bounds.left, monitor_bounds.top,
                    monitor_bounds.right, monitor_bounds.bottom);
        clamp_to_ = RECT{0, 0, 0, 0};
    }
}

void CALLBACK Enforcer::EventThunk(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                   LONG object_id, LONG child_id, DWORD, DWORD)
{
    // g_instance is written and cleared on the UI thread, and callbacks only
    // arrive on that same thread, so this needs no synchronisation.
    if (g_instance) {
        g_instance->OnEvent(event, hwnd, object_id, child_id);
    }
}

bool Enforcer::IsOurs(HWND hwnd) const
{
    // A linear scan over at most six handles, on the hottest path in the
    // program. A set would cost a hash where this costs a register compare.
    for (HWND own : owned_) {
        if (own == hwnd) {
            return true;
        }
    }
    return false;
}

void Enforcer::OnEvent(DWORD event, HWND hwnd, LONG object_id, LONG child_id)
{
    // ---- tier 0: register compares, run on every single event ----
    if (object_id != OBJID_WINDOW || child_id != CHILDID_SELF || !hwnd) {
        return; // caret, cursor, scrollbars, per-element changes
    }
    if (hwnd == host_ || IsOurs(hwnd)) {
        return;
    }
    if (depth_ > 0) {
        return; // re-entered through our own SetWindowPos
    }

    switch (event) {
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_SYSTEM_MINIMIZESTART:
        if (hwnd == yielded_to_) {
            Unyield();
        }
        Forget(hwnd);
        return;

    case EVENT_SYSTEM_FOREGROUND:
        // Ahead of ReassertTopmost, and returning rather than falling through.
        // A flyout that takes focus arrives here, and re-asserting topmost after
        // stepping behind it would climb back over it in the same message.
        if (HandleFlyout(hwnd)) {
            return;
        }
        ReassertTopmost();
        Evaluate(hwnd, /*bypass_cache=*/true);
        return;

    case EVENT_SYSTEM_MOVESIZEEND:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_UNCLOAKED:
        // Rare transitions we must never miss, so they skip the caches.
        if (HandleFlyout(hwnd)) {
            return;
        }
        Evaluate(hwnd, /*bypass_cache=*/true);
        return;

    case EVENT_OBJECT_LOCATIONCHANGE:
        Evaluate(hwnd, /*bypass_cache=*/false);
        return;

    default:
        return;
    }
}

bool Enforcer::RecentlyNotFullscreen(HWND hwnd) const
{
    const size_t slot = (reinterpret_cast<uintptr_t>(hwnd) >> 4) & 63;
    return cache_[slot].hwnd == hwnd
           && (GetTickCount64() - cache_[slot].when) < kCacheTtlMs;
}

void Enforcer::RememberNotFullscreen(HWND hwnd)
{
    const size_t slot = (reinterpret_cast<uintptr_t>(hwnd) >> 4) & 63;
    cache_[slot].hwnd = hwnd;
    cache_[slot].when = GetTickCount64();
}

void Enforcer::Evaluate(HWND hwnd, bool bypass_cache)
{
    if (!running_ || !clamping_ || !monitor_ || IsEmptyRect(clamp_to_)) {
        return;
    }

    const ULONGLONG now = GetTickCount64();

    // ---- tier 1: cached rejections, no syscalls ----
    if (!bypass_cache && RecentlyNotFullscreen(hwnd)) {
        return;
    }

    if (auto it = ledger_.find(hwnd); it != ledger_.end()) {
        const Record& record = it->second;
        if (record.gave_up && now < record.give_up_until) {
            return;
        }
        if (!bypass_cache && (now - record.last_attempt) < kDebounceMs) {
            return;
        }
    }

    // ---- tier 2: cheap user32, no cross-process work ----
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        // Every child window, including our reparented terminal and every UWP
        // CoreWindow hosted inside an ApplicationFrameWindow. GA_ROOT and not
        // GA_ROOTOWNER: the latter maps an owned popup to its owner and we would
        // move the wrong window.
        return;
    }

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & WS_VISIBLE) || (style & WS_MINIMIZE) || (style & WS_CHILD)) {
        return;
    }

    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) {
        // Tooltips, IME candidates, floating palettes. Never a fullscreen app.
        return;
    }

    // ---- tier 3: class ----
    if (IsExcludedClass(ClassNameOf(hwnd))) {
        return;
    }

    // ---- tier 4: cloaked (other virtual desktop, suspended UWP) ----
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0) {
        return;
    }

    // ---- tier 5: monitor and geometry ----
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != monitor_) {
        return; // other monitors are not our business
    }

    if (!LooksFullscreen(hwnd, style)) {
        RememberNotFullscreen(hwnd);
        if (auto it = ledger_.find(hwnd); it != ledger_.end()) {
            // Only a window that has stayed out of fullscreen for longer than
            // the whole attempt window earns a fresh allowance.
            //
            // Resetting unconditionally here reads as obviously correct and is
            // exactly wrong: a SUCCESSFUL clamp is itself what makes the window
            // stop being fullscreen, so the echo from our own SetWindowPos would
            // clear the counter every single time. The give-up backstop could
            // then never reach maxAttempts, and an app that re-asserts its rect
            // would be fought forever, which is the one outcome this whole
            // design is meant to avoid.
            if ((now - it->second.last_attempt) > kAttemptWindowMs) {
                it->second.attempts = 0;
                it->second.window_start = 0;
                it->second.gave_up = false;
            }
        }
        return;
    }

    // ---- tier 6: process exclusions, the only step that opens a handle ----
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (cfg_ && cfg_->IsExcluded(ProcessImageName(pid))) {
        return;
    }

    Clamp(hwnd);
}

bool Enforcer::LooksFullscreen(HWND hwnd, LONG_PTR style) const
{
    RECT frame{};
    if (!VisibleFrame(hwnd, style, frame)) {
        return false;
    }
    return Covers(frame, monitor_bounds_, kTolerance);
}

void Enforcer::Clamp(HWND hwnd)
{
    const ULONGLONG now = GetTickCount64();
    Record& record = ledger_[hwnd];

    if (record.window_start == 0 || (now - record.window_start) > kAttemptWindowMs) {
        record.window_start = now;
        record.attempts = 0;
    }

    if ((now - record.last_attempt) < kMinClampIntervalMs) {
        return; // below this it is a fight, not a correction
    }

    if (record.attempts >= kMaxAttempts) {
        record.gave_up = true;
        record.give_up_until = now + kGiveUpMs;
        log::Writef(L"enforce: hwnd=0x%p re-asserts its own rect; standing down for 60s "
                    L"(this is what DXGI exclusive fullscreen looks like)", hwnd);
        return;
    }

    GetWindowThreadProcessId(hwnd, &record.pid);

    ++depth_; // our own SetWindowPos echoes back as EVENT_OBJECT_LOCATIONCHANGE
    SetLastError(0);
    const BOOL ok = SetWindowPos(
        hwnd, nullptr, clamp_to_.left, clamp_to_.top,
        Width(clamp_to_), Height(clamp_to_),
        // NOSENDCHANGING because WM_WINDOWPOSCHANGING is exactly where an app
        // rewrites the rect back to the monitor bounds; measured on the Windows
        // 11 taskbar, and game engines do the same. ASYNCWINDOWPOS because a
        // fullscreen app stalled in a shader compile must not hang our message
        // loop, and therefore the user's terminal.
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
            | SWP_ASYNCWINDOWPOS);
    const DWORD error = GetLastError();
    --depth_;

    record.last_attempt = now;
    record.last_target = clamp_to_;
    ++record.attempts;

    if (!ok && error == ERROR_ACCESS_DENIED) {
        // UIPI: the target runs at a higher integrity level than we do. Retrying
        // would burn CPU to no effect.
        record.gave_up = true;
        record.give_up_until = kForever;
        log::Writef(L"enforce: clamp denied by UIPI for hwnd=0x%p pid=%lu", hwnd, record.pid);
        return;
    }

    if (record.attempts == 1) {
        log::Writef(L"enforce: clamped hwnd=0x%p pid=%lu class=%s to (%d,%d)-(%d,%d)",
                    hwnd, record.pid, ClassNameOf(hwnd).c_str(),
                    clamp_to_.left, clamp_to_.top, clamp_to_.right, clamp_to_.bottom);
    }
}

void Enforcer::Forget(HWND hwnd)
{
    ledger_.erase(hwnd);

    const size_t slot = (reinterpret_cast<uintptr_t>(hwnd) >> 4) & 63;
    if (cache_[slot].hwnd == hwnd) {
        cache_[slot].hwnd = nullptr;
    }
}

void Enforcer::ScanForeground()
{
    if (HWND foreground = GetForegroundWindow()) {
        Evaluate(foreground, /*bypass_cache=*/true);
    }
}

bool Enforcer::HandleFlyout(HWND hwnd)
{
    // Register compares before the syscall. This runs on every foreground change
    // and every window that appears, and a dock that is not topmost is not in
    // front of the flyout to begin with, so there is nothing to identify.
    if (!host_ || !cfg_ || !cfg_->topmost) {
        return false;
    }

    if (!IsFlyoutClass(ClassNameOf(hwnd))) {
        return false;
    }

    RECT flyout{};
    if (!GetWindowRect(hwnd, &flyout) || !Intersects(flyout, strip_)) {
        // The overflow opens at the notification area, so a left-edge or top-edge
        // dock is nowhere near it and has no reason to give up the band. The rect
        // is read rather than assumed: the flyout is positioned before it is
        // shown, so it is already final by the time this runs.
        return true;
    }

    YieldTo(hwnd);
    return true;
}

void Enforcer::YieldTo(HWND flyout)
{
    if (yielded_to_ == flyout) {
        return; // already sitting behind this one
    }

    yielded_to_ = flyout;

    // Ordering ourselves after a non-topmost window is documented to take the
    // topmost style off, and that is the point rather than a side effect: while
    // we hold WS_EX_TOPMOST there is no z-order that puts us below the flyout.
    // Naming the flyout instead of passing HWND_NOTOPMOST also pins the result,
    // rather than leaving it to whatever the ordinary band happens to look like.
    SetWindowPos(host_, flyout, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    log::Writef(L"z-order: stepped behind %s until it closes",
                ClassNameOf(flyout).c_str());
}

void Enforcer::Unyield()
{
    if (!yielded_to_) {
        return;
    }

    yielded_to_ = nullptr;

    // Clearing the debounce first. This is a one-shot recovery, not one of the
    // routine re-asserts the debounce exists to thin out, and losing it would
    // leave the dock beneath every window it just stepped behind.
    last_topmost_ = 0;
    ReassertTopmost();
}

void Enforcer::CheckYield()
{
    if (!yielded_to_) {
        return;
    }

    if (!IsWindow(yielded_to_) || !IsWindowVisible(yielded_to_)) {
        log::Write(L"z-order: flyout closed without a hide event; coming back up");
        Unyield();
    }
}

void Enforcer::ReassertTopmost()
{
    if (!host_ || !cfg_ || !cfg_->topmost) {
        return;
    }

    if (yielded_to_) {
        return; // a shell flyout is open over the strip and outranks us
    }

    const ULONGLONG now = GetTickCount64();
    if ((now - last_topmost_) < kTopmostDebounceMs) {
        return;
    }
    last_topmost_ = now;

    // Within the topmost band, z-order is most-recently-raised wins, so calling
    // this on a window that is already topmost is not a no-op. We do not outrank
    // a fullscreen app; we out-last it, because our triggers are downstream
    // notifications of its own. SWP_NOACTIVATE so this never steals focus from
    // whatever the user is typing into.
    SetWindowPos(host_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

} // namespace dock
