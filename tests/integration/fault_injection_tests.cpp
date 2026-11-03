#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <array>

#include "../src/core/fault_injection.h"

struct FaultEnvGuard {
    ~FaultEnvGuard() {
        SetEnvironmentVariableW(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", nullptr);
        SetEnvironmentVariableW(L"PDF_NOTE_SMALL_THROW_POINT", nullptr);
    }
};

static void ClearFaultEnv() {
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", nullptr);
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_THROW_POINT", nullptr);
}

static bool TestDisabledByDefault() {
    FaultEnvGuard guard;
    (void)guard;
    ClearFaultEnv();
    if (fault_injection::IsEnabled()) return false;
    fault_injection::MaybeThrow(L"sample");
    return true;
}

static bool TestEnabledButDifferentPointDoesNotThrow() {
    FaultEnvGuard guard;
    (void)guard;
    ClearFaultEnv();
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", L"1");
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_THROW_POINT", L"other");
    try {
        fault_injection::MaybeThrow(L"sample");
    } catch (...) {
        return false;
    }
    return true;
}

static bool TestMatchingPointThrows() {
    FaultEnvGuard guard;
    (void)guard;
    ClearFaultEnv();
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", L"1");
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_THROW_POINT", L"sample");
    bool threw = false;
    try {
        fault_injection::MaybeThrow(L"sample");
    } catch (const std::runtime_error&) {
        threw = true;
    } catch (...) {
        return false;
    }
    return threw;
}

static bool TestWildcardThrows() {
    FaultEnvGuard guard;
    (void)guard;
    ClearFaultEnv();
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", L"true");
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_THROW_POINT", L"*");
    bool threw = false;
    try {
        fault_injection::MaybeThrow(L"anything");
    } catch (const std::runtime_error&) {
        threw = true;
    } catch (...) {
        return false;
    }
    return threw;
}

static bool TestOnEnablesInjection() {
    FaultEnvGuard guard;
    (void)guard;
    ClearFaultEnv();
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", L"on");
    SetEnvironmentVariableW(L"PDF_NOTE_SMALL_THROW_POINT", L"sample");
    return fault_injection::IsEnabled() && fault_injection::ShouldThrow(L"sample");
}

// The pause helper must be inert in normal execution. Own and restore only
// this process's environment/events, with a bounded, always-released worker.
static bool TestCrashPauseHandshake() {
    const std::array<const wchar_t*, 5> names{{L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", L"PDF_NOTE_SMALL_PAUSE_POINT",
        L"PDF_NOTE_SMALL_PAUSE_ARM_EVENT", L"PDF_NOTE_SMALL_PAUSE_READY_EVENT", L"PDF_NOTE_SMALL_PAUSE_RELEASE_EVENT"}};
    struct Environment {
        const std::array<const wchar_t*, 5>& names;
        std::array<std::wstring, 5> values{};
        std::array<bool, 5> present{};
        explicit Environment(const std::array<const wchar_t*, 5>& input) : names(input) {
            for (size_t i = 0; i < names.size(); ++i) present[i] = fault_injection::ReadEnvVar(names[i], &values[i]);
        }
        ~Environment() {
            for (size_t i = 0; i < names.size(); ++i) SetEnvironmentVariableW(names[i], present[i] ? values[i].c_str() : nullptr);
        }
    } environment(names);
    const auto prefix = L"Local\\PdfNotePauseUnit_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    struct Events {
        HANDLE arm, ready, release;
        ~Events() { if (release) { SetEvent(release); CloseHandle(release); } if (ready) CloseHandle(ready); if (arm) CloseHandle(arm); }
    } events{CreateEventW(nullptr, TRUE, TRUE, (prefix + L"_arm").c_str()),
        CreateEventW(nullptr, TRUE, FALSE, (prefix + L"_ready").c_str()),
        CreateEventW(nullptr, TRUE, TRUE, (prefix + L"_release").c_str())};
    if (!events.arm || !events.ready || !events.release) return false;
    if (!SetEnvironmentVariableW(names[1], L"sample") ||
        !SetEnvironmentVariableW(names[2], (prefix + L"_arm").c_str()) ||
        !SetEnvironmentVariableW(names[3], (prefix + L"_ready").c_str()) ||
        !SetEnvironmentVariableW(names[4], (prefix + L"_release").c_str())) return false;
    const auto inert = [&](const wchar_t* point) {
        fault_injection::MaybePauseForCrashTest(point);
        return WaitForSingleObject(events.ready, 0) == WAIT_TIMEOUT;
    };
    if (!SetEnvironmentVariableW(names[0], nullptr) || !inert(L"sample")) return false;
    if (!SetEnvironmentVariableW(names[0], L"1") || !inert(L"other") || !inert(nullptr)) return false;
    if (!ResetEvent(events.arm) || !inert(L"sample")) return false;
    if (!SetEvent(events.arm) || !SetEnvironmentVariableW(names[3], (prefix + L"_missing").c_str()) || !inert(L"sample")) return false;
    if (!SetEnvironmentVariableW(names[3], (prefix + L"_ready").c_str()) || !ResetEvent(events.release)) return false;
    HANDLE worker = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { fault_injection::MaybePauseForCrashTest(L"sample"); return 0; }, nullptr, 0, nullptr);
    if (!worker) return false;
    const bool reachedAndPaused = WaitForSingleObject(events.ready, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT;
    SetEvent(events.release);
    const bool joined = WaitForSingleObject(worker, 35000) == WAIT_OBJECT_0;
    CloseHandle(worker);
    return reachedAndPaused && joined;
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    int passed = 0;
    int failed = 0;

    auto run = [&](const char* name, bool (*fn)()) {
        const bool ok = fn();
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        if (ok) {
            ++passed;
        } else {
            ++failed;
        }
    };

    run("Fault injection disabled by default", &TestDisabledByDefault);
    run("Fault injection ignores other point", &TestEnabledButDifferentPointDoesNotThrow);
    run("Fault injection throws on matching point", &TestMatchingPointThrows);
    run("Fault injection wildcard throws", &TestWildcardThrows);
    run("Fault injection accepts on", &TestOnEnablesInjection);
    run("Crash pause disabled/wrong/disarmed/missing/ready-release handshake", &TestCrashPauseHandshake);

    ClearFaultEnv();
    std::cout << "Summary: passed=" << passed << " failed=" << failed << "\n";
    return failed == 0 ? 0 : 1;
}
