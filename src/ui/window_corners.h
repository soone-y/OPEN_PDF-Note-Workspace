#pragma once

#include "core/constants.h"

#include <cwchar>

namespace ui {

// Independent of control theming: auxiliary frames include native dialogs and
// windows which do not call ApplyThemeToDialog. Older Windows may reject this
// optional DWM attribute; keep their native frame and the real operation intact.
[[nodiscard]] inline HRESULT ApplyAuxiliaryWindowCorners(HWND window) noexcept {
    const DWORD savedError = GetLastError();
    const HRESULT result = [window]() noexcept -> HRESULT {
        if (!window || !IsWindow(window)) return E_HANDLE;
        if (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) return S_FALSE;
        wchar_t className[128]{};
        if (!GetClassNameW(window, className, 128)) return E_HANDLE;
        if (std::wcscmp(className, kMainClass) == 0) return S_FALSE;
        using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        static const auto setAttribute = []() noexcept -> SetAttribute {
            const HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return module ? reinterpret_cast<SetAttribute>(GetProcAddress(module, "DwmSetWindowAttribute")) : nullptr;
        }();
        if (!setAttribute) return E_NOTIMPL;
        constexpr DWORD windowCornerPreference = 33;
        constexpr DWORD doNotRound = 1;
        return setAttribute(window, windowCornerPreference, &doNotRound, sizeof(doNotRound));
    }();
    SetLastError(savedError);
    return result;
}

// UI-thread-owned hook, scoped to wWinMain including early startup dialogs.
// It only changes this thread's auxiliary frames, never blocks creation or
// alters messages/focus, and is uninstalled on every return/exception path.
class ScopedAuxiliaryWindowCorners final {
public:
    ScopedAuxiliaryWindowCorners() noexcept {
        const DWORD savedError = GetLastError();
        hook_ = SetWindowsHookExW(WH_CALLWNDPROC, Observe, nullptr, GetCurrentThreadId());
        SetLastError(savedError);
    }
    ~ScopedAuxiliaryWindowCorners() {
        const DWORD savedError = GetLastError();
        if (hook_) UnhookWindowsHookEx(hook_);
        SetLastError(savedError);
    }
    ScopedAuxiliaryWindowCorners(const ScopedAuxiliaryWindowCorners&) = delete;
    ScopedAuxiliaryWindowCorners& operator=(const ScopedAuxiliaryWindowCorners&) = delete;
    [[nodiscard]] bool installed() const noexcept { return hook_ != nullptr; }
private:
    static LRESULT CALLBACK Observe(int code, WPARAM wp, LPARAM lp) noexcept {
        if (code == HC_ACTION && lp) {
            const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lp);
            if (message.message == WM_CREATE || message.message == WM_INITDIALOG ||
                (message.message == WM_SHOWWINDOW && message.wParam) || message.message == WM_THEMECHANGED) {
                const HRESULT result = ApplyAuxiliaryWindowCorners(message.hwnd);
                (void)result; // Cosmetic fallback only; no silent operation failure.
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    HHOOK hook_ = nullptr;
};

} // namespace ui
