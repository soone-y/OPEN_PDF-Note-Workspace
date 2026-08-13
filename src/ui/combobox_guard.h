#pragma once

#include <windows.h>
#include <commctrl.h>

namespace ui {

inline bool IsComboDropped(HWND hWnd) {
    if (!hWnd) return false;
    return SendMessageW(hWnd, CB_GETDROPPEDSTATE, 0, 0) != 0;
}

inline void ForwardComboMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR refData) {
    HWND target = refData ? reinterpret_cast<HWND>(refData) : nullptr;
    if (!target && hWnd) target = GetParent(hWnd);
    if (target) SendMessageW(target, msg, wParam, lParam);
}

inline LRESULT CALLBACK GuardedComboProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR, DWORD_PTR refData) {
    switch (msg) {
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (!IsComboDropped(hWnd)) {
            ForwardComboMessage(hWnd, msg, wParam, lParam, refData);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (!IsComboDropped(hWnd)) {
            switch (wParam) {
            case VK_UP:
            case VK_DOWN:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_HOME:
            case VK_END:
                ForwardComboMessage(hWnd, msg, wParam, lParam, refData);
                return 0;
            default:
                break;
            }
        }
        break;
    default:
        break;
    }

    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

inline void GuardComboAgainstAccidentalChange(HWND hWnd, HWND forwardTarget = nullptr, UINT_PTR subclassId = 1) {
    if (!hWnd) return;
    SetWindowSubclass(hWnd, GuardedComboProc, subclassId, reinterpret_cast<DWORD_PTR>(forwardTarget));
}

} // namespace ui

