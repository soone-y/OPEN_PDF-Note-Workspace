#pragma once
#include <windows.h>
#include <cstring>

namespace ui {
[[nodiscard]] inline int WindowDpi(HWND window) {
    using Get = UINT (WINAPI*)(HWND);
    static const Get get = [] {
        const FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        Get result = nullptr;
        static_assert(sizeof(result) == sizeof(address));
        std::memcpy(&result, &address, sizeof(result)); return result;
    }();
    const int dpi = get ? static_cast<int>(get(window)) : 96;
    return dpi > 0 ? dpi : 96;
}
} // namespace ui
