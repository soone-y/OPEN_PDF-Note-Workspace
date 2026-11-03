#pragma once

#include "ui/scroll_input_policy.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <climits>
#include <cwchar>
#include <new>

// The host owns only viewport/input state. The caller owns content geometry
// and repositions its controls through the supplied layout callback.
namespace ui::toolbar_scroll {
namespace detail {
inline constexpr UINT_PTR kSubclass = 0x54425343;
inline constexpr const wchar_t* kStateProperty = L"PdfNote.ToolbarScrollState";
struct State {
    void (*layout)(HWND) = nullptr;
    scroll_input::ListWheelInput wheel;
    int contentHeight = 0, pageHeight = 0;
    int key = (std::numeric_limits<int>::min)();
};
inline LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

inline State* GetState(HWND host) {
    return static_cast<State*>(GetPropW(host, kStateProperty));
}
inline int Clamp(int64_t value, int contentHeight, int pageHeight) {
    return static_cast<int>(std::clamp<int64_t>(value, 0, std::max(0, contentHeight - pageHeight)));
}
inline void MoveTo(HWND host, int64_t position) {
    auto* state = GetState(host);
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
    if (!state || !GetScrollInfo(host, SB_VERT, &info)) return;
    const int next = Clamp(position, info.nMax + 1, static_cast<int>(info.nPage));
    if (next == info.nPos) return;
    info.fMask = SIF_POS; info.nPos = next;
    SetScrollInfo(host, SB_VERT, &info, TRUE);
    if (state->layout) state->layout(host);
}
inline void Reveal(HWND host, HWND control) {
    RECT bounds{}, client{};
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_POS;
    if (!GetScrollInfo(host, SB_VERT, &info) || !GetWindowRect(control, &bounds) ||
        !GetClientRect(host, &client)) return;
    MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&bounds), 2);
    if (bounds.top < client.top || bounds.bottom - bounds.top > client.bottom - client.top)
        MoveTo(host, static_cast<int64_t>(info.nPos) + bounds.top - client.top);
    else if (bounds.bottom > client.bottom)
        MoveTo(host, static_cast<int64_t>(info.nPos) + bounds.bottom - client.bottom);
}
inline LRESULT CALLBACK ChildProc(HWND control, UINT message, WPARAM wp, LPARAM lp,
                                 UINT_PTR id, DWORD_PTR data) {
    const HWND host = reinterpret_cast<HWND>(data);
    if (message == WM_SETFOCUS) Reveal(host, control);
    if (message == WM_MOUSEWHEEL) {
        wchar_t name[16]{};
        const bool dropped = GetClassNameW(control, name, 16) && _wcsicmp(name, L"ComboBox") == 0 &&
            SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0);
        // Open dropdowns keep native list scrolling. Closed options scroll the
        // toolbar, before their existing guard could route the wheel to PDF.
        if (!dropped) return SendMessageW(host, message, wp, lp);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control, ChildProc, id);
    return DefSubclassProc(control, message, wp, lp);
}
inline LRESULT CALLBACK HostProc(HWND host, UINT message, WPARAM wp, LPARAM lp,
                                UINT_PTR id, DWORD_PTR data) {
    auto* state = reinterpret_cast<State*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(host, HostProc, id);
        RemovePropW(host, kStateProperty);
        delete state;
        return DefSubclassProc(host, message, wp, lp);
    }
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE) state->wheel.Reset();
    if (message == WM_MOUSEWHEEL || (message == WM_VSCROLL && lp == 0)) {
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
        if (!GetScrollInfo(host, SB_VERT, &info)) return 0;
        int64_t next = info.nPos;
        if (message == WM_MOUSEWHEEL) {
            if (GET_KEYSTATE_WPARAM(wp) & (MK_CONTROL | MK_SHIFT)) return 0;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const uint32_t pixels = lines == WHEEL_PAGESCROLL ? info.nPage
                : static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(lines) * 28, UINT_MAX));
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            next += state->wheel.Take(delta, pixels);
            const int limit = std::max(0, info.nMax + 1 - static_cast<int>(info.nPage));
            if (next < 0 || next > limit || (info.nPos == 0 && delta > 0) ||
                (info.nPos == limit && delta < 0)) state->wheel.Reset();
        } else {
            state->wheel.Reset();
            switch (LOWORD(wp)) {
            case SB_LINEUP: next -= 28; break;
            case SB_LINEDOWN: next += 28; break;
            case SB_PAGEUP: next -= std::max(28, static_cast<int>(info.nPage) - 28); break;
            case SB_PAGEDOWN: next += std::max(28, static_cast<int>(info.nPage) - 28); break;
            case SB_TOP: next = 0; break;
            case SB_BOTTOM: next = info.nMax; break;
            case SB_THUMBPOSITION: case SB_THUMBTRACK: next = info.nTrackPos; break;
            default: return 0;
            }
        }
        MoveTo(host, next);
        return 0;
    }
    return DefSubclassProc(host, message, wp, lp);
}
} // namespace detail

[[nodiscard]] inline bool Install(HWND host, void (*layout)(HWND)) {
    if (!host || !layout) return false;
    if (auto* state = detail::GetState(host)) { state->layout = layout; return true; }
    auto* state = new (std::nothrow) detail::State;
    if (!state) return false;
    state->layout = layout;
    if (!SetPropW(host, detail::kStateProperty, state)) { delete state; return false; }
    if (SetWindowSubclass(host, detail::HostProc, detail::kSubclass, reinterpret_cast<DWORD_PTR>(state))) return true;
    RemovePropW(host, detail::kStateProperty);
    delete state;
    return false;
}
[[nodiscard]] inline bool InstallChild(HWND control, HWND host) {
    return control && detail::GetState(host) && SetWindowSubclass(control, detail::ChildProc,
        detail::kSubclass, reinterpret_cast<DWORD_PTR>(host));
}
[[nodiscard]] inline int Offset(HWND host, int contentHeight, int pageHeight, int key) {
    const auto* state = detail::GetState(host);
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_POS;
    if (!state || state->key != key || !GetScrollInfo(host, SB_VERT, &info)) return 0;
    return detail::Clamp(info.nPos, contentHeight, pageHeight);
}
inline void SetRange(HWND host, int contentHeight, int pageHeight, int key) {
    const int offset = Offset(host, contentHeight, pageHeight, key);
    if (auto* state = detail::GetState(host)) {
        if (state->key != key || state->contentHeight != contentHeight || state->pageHeight != pageHeight)
            state->wheel.Reset();
        state->key = key; state->contentHeight = contentHeight; state->pageHeight = pageHeight;
    }
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMax = std::max(0, contentHeight - 1);
    info.nPage = static_cast<UINT>(std::max(0, pageHeight));
    info.nPos = offset;
    SetScrollInfo(host, SB_VERT, &info, TRUE);
}
} // namespace ui::toolbar_scroll
