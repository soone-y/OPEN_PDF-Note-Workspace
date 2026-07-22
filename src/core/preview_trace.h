#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "core/app_log.h"

#include <string>

namespace preview_trace {

inline bool IsEnabled() {
    return IsAppLogEnabled(AppLogKind::PreviewTrace);
}

inline std::wstring Bool(bool value) {
    return value ? L"1" : L"0";
}

inline ULONGLONG TickNow() {
    return GetTickCount64();
}

inline std::wstring ElapsedMs(ULONGLONG startTick) {
    const ULONGLONG now = GetTickCount64();
    return std::to_wstring(now >= startTick ? (now - startTick) : 0);
}

inline std::wstring Ptr(const void* value) {
    wchar_t buf[32]{};
    swprintf(buf, 32, L"0x%llX",
             static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(value)));
    return buf;
}

inline std::wstring WindowClass(HWND hwnd) {
    if (!hwnd) return L"(null)";
    wchar_t cls[64]{};
    int len = GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0])));
    if (len <= 0) return L"(unknown)";
    return std::wstring(cls, static_cast<size_t>(len));
}

inline std::wstring Window(HWND hwnd) {
    return Ptr(hwnd) + L":" + WindowClass(hwnd);
}

inline std::wstring Message(UINT msg) {
    switch (msg) {
    case WM_KEYDOWN: return L"WM_KEYDOWN";
    case WM_SYSKEYDOWN: return L"WM_SYSKEYDOWN";
    case WM_COMMAND: return L"WM_COMMAND";
    default: return std::to_wstring(msg);
    }
}

inline void Append(const wchar_t* area, const std::wstring& message) {
    if (!IsEnabled()) return;
    std::wstring line;
    if (area && *area) {
        line = std::wstring(L"[") + area + L"] " + message;
    } else {
        line = message;
    }
    AppendAppLogLine(AppLogKind::PreviewTrace, line);
}

} // namespace preview_trace
