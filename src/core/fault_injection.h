#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <stdexcept>
#include <string>

namespace fault_injection {

inline bool ReadEnvVar(const wchar_t* name, std::wstring* out) {
    if (!name || !out) return false;
    out->clear();
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (len == 0) return false;
    out->resize(static_cast<size_t>(len));
    DWORD got = GetEnvironmentVariableW(name, out->data(), len);
    if (got == 0) {
        out->clear();
        return false;
    }
    if (!out->empty() && out->back() == L'\0') out->pop_back();
    return true;
}

inline bool IsEnabled() {
    std::wstring value;
    if (!ReadEnvVar(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", &value)) return false;
    return value == L"1" || value == L"true" || value == L"TRUE" || value == L"on";
}

inline bool ShouldThrow(const wchar_t* point) {
    if (!point || !*point || !IsEnabled()) return false;
    std::wstring configured;
    if (!ReadEnvVar(L"PDF_NOTE_SMALL_THROW_POINT", &configured)) return false;
    return configured == point || configured == L"*";
}

inline void MaybeThrow(const wchar_t* point) {
    if (!ShouldThrow(point)) return;
    std::wstring msg = L"Injected exception at ";
    msg += point;
    throw std::runtime_error(std::string(msg.begin(), msg.end()));
}

} // namespace fault_injection
