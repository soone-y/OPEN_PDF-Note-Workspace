#pragma once
#include <windows.h>
#include <commctrl.h>

namespace ui {
// UI-thread-owned observer. Failure to observe a text field disables application
// shortcuts in that field; it never disables normal text input or the menus.
class ShortcutInputGuard final {
public:
    ShortcutInputGuard() = default;
    ~ShortcutInputGuard() {
        if (ready_ && IsWindow(window_)) RemoveWindowSubclass(window_, Observe, reinterpret_cast<UINT_PTR>(this));
    }
    ShortcutInputGuard(const ShortcutInputGuard&) = delete;
    ShortcutInputGuard& operator=(const ShortcutInputGuard&) = delete;
    [[nodiscard]] bool Attach(HWND window) noexcept {
        if (window_) return false;
        window_ = window;
        ready_ = window && SetWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this));
        return ready_;
    }
    [[nodiscard]] bool Suppress(HWND focus) const noexcept {
        return focus && focus == window_ && (!ready_ || composing_);
    }
private:
    static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto* guard = reinterpret_cast<ShortcutInputGuard*>(data);
        if (message == WM_IME_STARTCOMPOSITION) guard->composing_ = true;
        if (message == WM_IME_ENDCOMPOSITION || message == WM_KILLFOCUS) guard->composing_ = false;
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, Observe, id);
            guard->window_ = nullptr; guard->ready_ = false; guard->composing_ = false;
        }
        return DefSubclassProc(window, message, wp, lp);
    }
    HWND window_ = nullptr;
    bool ready_ = false;
    bool composing_ = false;
};
} // namespace ui
