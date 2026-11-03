#pragma once
// Bounded native controls for owned disposable children. This workflow driver
// operates only on controls belonging to its exact child process.
#include <windows.h>
#include "core/atomic_write.h"
#include <commctrl.h>
#include <cstring>
#include <vector>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static int checks = 0;
static DWORD childPid = 0;
static void Require(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); }
static void CheckWindowCorners(HWND window, const wchar_t* className) {
    using GetAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static const auto getAttribute = []() -> GetAttribute {
        const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const FARPROC address = module ? GetProcAddress(module, "DwmGetWindowAttribute") : nullptr;
        GetAttribute query = nullptr;
        static_assert(sizeof(query) == sizeof(address));
        std::memcpy(&query, &address, sizeof(query));
        return query;
    }();
    Require(getAttribute != nullptr, "DWM frame query is available");
    DWORD preference = 0;
    const HRESULT result = getAttribute(window, 33, &preference, sizeof(preference));
    if (result == E_INVALIDARG) {
        // Older supported Windows has no corner preference. Do not claim the
        // appearance assertion ran when the OS cannot observe that attribute.
        static bool reported = false;
        if (!reported) std::cout << "[SKIP] Window corner preference unavailable on this Windows\n";
        reported = true;
        return;
    }
    Require(SUCCEEDED(result), "read actual child application's frame preference");
    const bool expected = wcscmp(className, L"PdfWorkspaceMainWnd") == 0 ? preference == 0 : preference == 1;
    if (!expected) std::wcerr << L"Corner preference mismatch: " << className << L" / " << preference << L'\n';
    Require(expected,
            "main retains its default frame; auxiliary windows forbid rounding");
}
static HWND Window(const wchar_t* name) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, name, nullptr))) {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == childPid) return window;
    }
    return nullptr;
}
template<class Predicate> static bool Until(Predicate predicate, DWORD timeout = 10000) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(20); } while (GetTickCount64() < deadline);
    return false;
}
static HWND WaitWindow(const wchar_t* name) {
    HWND window = nullptr;
    // FindWindow can see an HWND while WM_CREATE is still initializing it.
    // Wait for auxiliary windows' show boundary before checking their frame.
    // Main may stay hidden until the startup notice is acknowledged below.
    const bool appeared = Until([&] {
        window = Window(name);
        return window && (wcscmp(name, L"PdfWorkspaceMainWnd") == 0 || IsWindowVisible(window));
    });
    if (!appeared) {
        const std::wstring label(name);
        std::cerr << "Missing child window: " << std::string(label.begin(), label.end()) << " checks=" << checks << '\n';
    }
    Require(appeared, "child window appeared");
    CheckWindowCorners(window, name);
    return window;
}
static DWORD_PTR Send(HWND window, UINT message, WPARAM wp = 0, LPARAM lp = 0) {
    DWORD_PTR result = 0;
    const auto sent = SendMessageTimeoutW(window, message, wp, lp, SMTO_ABORTIFHUNG, 5000, &result);
    if (!sent) {
        const DWORD error = GetLastError(); char className[128]{};
        GetClassNameA(window, className, 128);
        std::cerr << "Child message failed: " << className << " / message=" << message << " / wp=" << wp << " / error=" << error << '\n';
    }
    Require(sent != 0, "bounded child message");
    return result;
}
static std::wstring WindowText(HWND window) {
    const auto length = Send(window, WM_GETTEXTLENGTH);
    Require(length < 65536, "bounded child control text");
    std::wstring text(length + 1, L'\0');
    text.resize(Send(window, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data())));
    return text;
}
static void Type(HWND editor, const wchar_t* text) {
    Send(editor, EM_SETSEL, 0, -1);
    // Exercise native typing/EN_CHANGE; no cross-process string pointers or
    // synthetic EN_CHANGE notifications and no user's physical keyboard.
    if (!*text) Send(editor, WM_CLEAR);
    for (const wchar_t* character = text; *character; ++character) Send(editor, WM_CHAR, *character);
}
static bool Equals(const fs::path& path, const std::string& expected) {
    std::string bytes; return ReadFileBytesWin32(path, bytes) && bytes == expected;
}
static HWND Descendant(HWND parent, const wchar_t* name) {
    struct Search { const wchar_t* name; HWND found = nullptr; } search{name};
    EnumChildWindows(parent, [](HWND child, LPARAM argument) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(argument);
        wchar_t name[128]{};
        if (GetClassNameW(child, name, 128) && wcscmp(name, search.name) == 0) { search.found = child; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
static HWND DescendantId(HWND parent, int id) {
    struct Search { int id; HWND found = nullptr; } search{id};
    EnumChildWindows(parent, [](HWND child, LPARAM argument) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(argument);
        if (GetDlgCtrlID(child) == search.id) { search.found = child; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
