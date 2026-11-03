#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string_view>
#include "ui/window_corners.h"

namespace ui {

// One shared reading-time policy for automatically dismissed notices and tips.
// Count Unicode characters, excluding layout whitespace; cap before overflow.
[[nodiscard]] inline UINT TransientDisplayDurationMs(std::wstring_view text) noexcept {
    constexpr UINT minimum = 2000;
    constexpr UINT maximum = 10000;
    constexpr UINT perCharacter = 60;
    UINT duration = minimum;
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t ch = text[index];
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') continue;
        if (ch >= 0xD800 && ch <= 0xDBFF && index + 1 < text.size() &&
            text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF) ++index;
        if (duration >= maximum - perCharacter) return maximum;
        duration += perCharacter;
    }
    return duration;
}

inline constexpr UINT_PTR kTransientTooltipSubclassId = 0x7454;

inline LRESULT CALLBACK TransientTooltipProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                             UINT_PTR subclassId, DWORD_PTR pressed) {
    switch (message) {
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetWindowSubclass(window, TransientTooltipProc, subclassId, 1);
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (pressed) {
            SetWindowSubclass(window, TransientTooltipProc, subclassId, 0);
            if (GetCapture() == window) ReleaseCapture();
            SendMessageW(window, TTM_POP, 0, 0);
        }
        return 0;
    case WM_CAPTURECHANGED:
        SetWindowSubclass(window, TransientTooltipProc, subclassId, 0);
        break;
    case WM_SHOWWINDOW:
        if (!wp && pressed) {
            SetWindowSubclass(window, TransientTooltipProc, subclassId, 0);
            if (GetCapture() == window) ReleaseCapture();
        }
        break;
    case WM_NCDESTROY:
        if (GetCapture() == window) ReleaseCapture();
        RemoveWindowSubclass(window, TransientTooltipProc, subclassId);
        break;
    }
    return DefSubclassProc(window, message, wp, lp);
}

[[nodiscard]] inline bool ConfigureTransientTooltip(HWND tooltip, std::wstring_view text) {
    if (!tooltip) return false;
    // Configure on creation/text updates, never on hover; no path lookup or I/O.
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, TransientDisplayDurationMs(text));
    // A new text invalidates a pending click on the previous tip. These subclass
    // helpers also work with the application's classic common-controls version.
    if (GetCapture() == tooltip) ReleaseCapture();
    SetWindowLongPtrW(tooltip, GWL_EXSTYLE, GetWindowLongPtrW(tooltip, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    const HRESULT corners = ApplyAuxiliaryWindowCorners(tooltip);
    (void)corners;
    return SetWindowSubclass(tooltip, TransientTooltipProc, kTransientTooltipSubclassId, 0) != FALSE;
}

} // namespace ui
