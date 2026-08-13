#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <iostream>
#include <stdexcept>
#include <string>

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

    ClearFaultEnv();
    std::cout << "Summary: passed=" << passed << " failed=" << failed << "\n";
    return failed == 0 ? 0 : 1;
}
