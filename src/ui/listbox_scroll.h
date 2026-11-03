#pragma once

#include "ui/scroll_input_win32.h"
#include <algorithm>
#include <commctrl.h>
#include <new>
#include <windowsx.h>

namespace ui {
namespace listbox_scroll_detail {
inline constexpr UINT_PTR kSubclass = 0x4c425743;
inline constexpr const wchar_t* kStateProperty = L"PdfNote.ListBoxWheelState";
struct State {
    scroll_input::ListWheelInput wheel;
    bool applyingWheel = false;
};

// UI-thread owned. This changes only the viewport: native selection, focus,
// keyboard navigation, row painting and parent notifications stay native.
inline LRESULT CALLBACK Proc(HWND list, UINT message, WPARAM wParam, LPARAM lParam,
                             UINT_PTR, DWORD_PTR data) {
    auto* state = reinterpret_cast<State*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(list, Proc, kSubclass);
        RemovePropW(list, kStateProperty);
        delete state;
        return DefSubclassProc(list, message, wParam, lParam);
    }
    if (message == WM_MOUSEWHEEL && !(GET_KEYSTATE_WPARAM(wParam) & (MK_CONTROL | MK_SHIFT))) {
        UINT rows = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &rows, 0);
        if (rows == WHEEL_PAGESCROLL) {
            RECT client{};
            GetClientRect(list, &client);
            const auto height = SendMessageW(list, LB_GETITEMHEIGHT, 0, 0);
            rows = static_cast<UINT>(std::max<LRESULT>(1, (client.bottom - client.top) / std::max<LRESULT>(1, height) - 1));
        }
        const int move = state->wheel.Take(GET_WHEEL_DELTA_WPARAM(wParam), rows);
        const auto top = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        const auto count = SendMessageW(list, LB_GETCOUNT, 0, 0);
        if (top == LB_ERR || count <= 0) { state->wheel.Reset(); return 0; }
        SCROLLINFO range{};
        range.cbSize = sizeof(range);
        range.fMask = SIF_RANGE | SIF_PAGE;
        if (GetScrollInfo(list, SB_VERT, &range)) {
            const auto last = std::max<int64_t>(0, static_cast<int64_t>(range.nMax) - std::max<UINT>(1, range.nPage) + 1);
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if ((top == 0 && delta > 0) || (top >= last && delta < 0)) {
                state->wheel.Reset();
                return 0;
            }
        }
        if (move) {
            const auto requested = static_cast<int64_t>(top) + move;
            const auto target = std::clamp<int64_t>(requested, 0, count - 1);
            state->applyingWheel = true;
            SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(target), 0);
            state->applyingWheel = false;
            const auto actual = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
            if (actual != requested) state->wheel.Reset(); // no buffered distance at an edge
            // LISTBOX can draw directly during LB_SETTOPINDEX. Complete any
            // pending paint before the next packet without forcing a full erase.
            if (actual != top) UpdateWindow(list);
        }
        return 0;
    }
    if (!state->applyingWheel && (message == LB_RESETCONTENT || message == LB_SETTOPINDEX ||
        message == LB_SETCURSEL || message == WM_VSCROLL || message == WM_KEYDOWN ||
        message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_MOUSEHWHEEL))
        state->wheel.Reset();
    return DefSubclassProc(list, message, wParam, lParam);
}
} // namespace listbox_scroll_detail

[[nodiscard]] inline bool InstallListBoxScroll(HWND list) {
    using namespace listbox_scroll_detail;
    if (!list) return false;
    if (GetPropW(list, kStateProperty)) return true;
    auto* state = new (std::nothrow) State;
    if (!state) return false;
    if (!SetPropW(list, kStateProperty, reinterpret_cast<HANDLE>(state))) { delete state; return false; }
    if (SetWindowSubclass(list, Proc, kSubclass, reinterpret_cast<DWORD_PTR>(state))) return true;
    RemovePropW(list, kStateProperty);
    delete state;
    return false;
}
} // namespace ui
