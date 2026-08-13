#pragma once

#include <windows.h>
#include <imm.h>
#include <cwchar>
#include <string>

namespace ui {

inline bool IsRichEditWindow(HWND hWnd) {
    if (!hWnd) return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(hWnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0])))) return false;

    for (wchar_t& ch : cls) {
        if (ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
        }
    }
    return std::wcsstr(cls, L"RICHEDIT") != nullptr;
}

inline bool IsEditOrRichEditWindow(HWND hWnd) {
    if (!hWnd) return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(hWnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0])))) return false;

    for (wchar_t& ch : cls) {
        if (ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
        }
    }

    if (std::wcscmp(cls, L"EDIT") == 0) return true;
    return std::wcsstr(cls, L"RICHEDIT") != nullptr;
}

inline size_t GetEditTextLengthForNavigation(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return 0;
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) return 0;
    if (!IsRichEditWindow(hWnd)) return static_cast<size_t>(len);

    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hWnd, text.data(), len + 1);
    if (copied <= 0) return 0;
    text.resize(static_cast<size_t>(copied));

    size_t visibleLen = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
            ++i;
        }
        ++visibleLen;
    }
    return visibleLen;
}

inline bool IsImeComposingOnWindow(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return false;
    HIMC himc = ImmGetContext(hWnd);
    if (!himc) return false;
    const LONG compBytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(hWnd, himc);
    return compBytes > 0;
}

inline bool ConsumeNoOpEdgeNavKeyForMultilineEdit(const MSG& msg, HWND exclude = nullptr) {
    if (msg.message != WM_KEYDOWN) return false;
    if (msg.wParam != VK_UP && msg.wParam != VK_DOWN && msg.wParam != VK_PRIOR && msg.wParam != VK_NEXT &&
        msg.wParam != VK_LEFT && msg.wParam != VK_RIGHT) {
        return false;
    }

    const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (altDown) return false;
    const bool requirePlain = (msg.wParam == VK_UP || msg.wParam == VK_DOWN || msg.wParam == VK_PRIOR || msg.wParam == VK_NEXT);
    if (requirePlain && (shiftDown || ctrlDown)) return false;

    HWND focused = GetFocus();
    if (!focused || !IsWindow(focused)) return false;
    if (exclude && focused == exclude) return false;

    if (IsImeComposingOnWindow(focused)) return false;
    if (!IsEditOrRichEditWindow(focused)) return false;
    if (HWND parent = GetParent(focused)) {
        wchar_t parentCls[64]{};
        if (GetClassNameW(parent, parentCls, static_cast<int>(sizeof(parentCls) / sizeof(parentCls[0])))) {
            for (wchar_t& ch : parentCls) {
                if (ch >= L'a' && ch <= L'z') {
                    ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
                }
            }
            if (std::wcsstr(parentCls, L"COMBOBOX") != nullptr) {
                return false;
            }
        }
    }

    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(focused, EM_GETSEL,
                 reinterpret_cast<WPARAM>(&selStart),
                 reinterpret_cast<LPARAM>(&selEnd));
    if (selStart != selEnd) return false;

    if (msg.wParam == VK_LEFT && selStart == 0) {
        return true;
    }
    if (msg.wParam == VK_RIGHT) {
        const size_t len = GetEditTextLengthForNavigation(focused);
        if (static_cast<size_t>(selEnd) >= len) {
            return true;
        }
    }

    int lineCount = static_cast<int>(SendMessageW(focused, EM_GETLINECOUNT, 0, 0));
    if (lineCount <= 0) lineCount = 1;
    int caretLine = static_cast<int>(SendMessageW(focused, EM_LINEFROMCHAR, selStart, 0));
    if (caretLine < 0) caretLine = 0;

    if ((msg.wParam == VK_UP || msg.wParam == VK_PRIOR) && caretLine <= 0) {
        return true;
    }
    if ((msg.wParam == VK_DOWN || msg.wParam == VK_NEXT) && caretLine >= lineCount - 1) {
        return true;
    }
    return false;
}

} // namespace ui
