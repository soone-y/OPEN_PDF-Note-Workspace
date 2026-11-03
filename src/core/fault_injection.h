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

// Deterministic crash tests own all three named events and the disposable child
// process. No files are written. Disabled unless explicitly enabled and armed;
// missing events are a no-op, and a lost parent cannot block the app indefinitely.
inline void MaybePauseForCrashTest(const wchar_t* point) {
    if (!point || !IsEnabled()) return;
    std::wstring selected, armedName, readyName, releaseName;
    if (!ReadEnvVar(L"PDF_NOTE_SMALL_PAUSE_POINT", &selected) || selected != point ||
        !ReadEnvVar(L"PDF_NOTE_SMALL_PAUSE_ARM_EVENT", &armedName) ||
        !ReadEnvVar(L"PDF_NOTE_SMALL_PAUSE_READY_EVENT", &readyName) ||
        !ReadEnvVar(L"PDF_NOTE_SMALL_PAUSE_RELEASE_EVENT", &releaseName)) return;
    HANDLE armed = OpenEventW(SYNCHRONIZE, FALSE, armedName.c_str());
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, releaseName.c_str());
    if (armed && ready && release && WaitForSingleObject(armed, 0) == WAIT_OBJECT_0 && SetEvent(ready)) {
        (void)WaitForSingleObject(release, 30000);
    }
    if (release) CloseHandle(release);
    if (ready) CloseHandle(ready);
    if (armed) CloseHandle(armed);
}

} // namespace fault_injection
