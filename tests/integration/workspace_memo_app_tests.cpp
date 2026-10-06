// Drive only the disposable child process. No physical keyboard/clipboard use.
#include <windows.h>
#include "core/command_ids.h"
#include "core/atomic_write.h"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static int checks = 0;
static DWORD childPid = 0;
static void Require(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); }
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
    const bool appeared = Until([&] { window = Window(name); return window != nullptr; });
    if (!appeared) {
        const std::wstring label(name);
        std::cerr << "Missing child window: " << std::string(label.begin(), label.end()) << " checks=" << checks << '\n';
    }
    Require(appeared, "child window appeared");
    return window;
}
static void Send(HWND window, UINT message, WPARAM wp = 0, LPARAM lp = 0) {
    DWORD_PTR result = 0;
    Require(SendMessageTimeoutW(window, message, wp, lp, SMTO_ABORTIFHUNG, 5000, &result) != 0, "bounded child message");
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
static void CtrlS(HWND editor, const fs::path& path, const std::string& text) {
    const DWORD thread = GetWindowThreadProcessId(editor, nullptr);
    Require(AttachThreadInput(GetCurrentThreadId(), thread, TRUE), "attach isolated child input queue");
    struct Detach { DWORD thread; ~Detach() { AttachThreadInput(GetCurrentThreadId(), thread, FALSE); } } detach{thread};
    BYTE before[256]{}; Require(GetKeyboardState(before), "capture child key state");
    struct Restore { BYTE* keys; ~Restore() { SetKeyboardState(keys); } } restore{before};
    BYTE ctrl[256]{}; std::copy(std::begin(before), std::end(before), std::begin(ctrl));
    ctrl[VK_CONTROL] = 0x80; ctrl[VK_MENU] = 0;
    Require(SetKeyboardState(ctrl) && PostMessageW(editor, WM_KEYDOWN, L'S', 0), "post child Ctrl+S");
    Require(Until([&] { return Equals(path, text); }), "actual application message loop saves memo Ctrl+S");
}
int main() {
    PROCESS_INFORMATION process{};
    try {
        // The runner gives an absolute fixture path through this private child environment.
        const DWORD count = GetEnvironmentVariableW(L"WORKSPACE_MEMO_APP_TEST_ROOT", nullptr, 0);
        Require(count > 0, "test root provided");
        std::wstring fixture(count, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"WORKSPACE_MEMO_APP_TEST_ROOT", fixture.data(), count);
        Require(length > 0 && length < count, "test root captured"); fixture.resize(length);
        const fs::path root(fixture), executable = root / L"app" / L"pdf_note_workspace.exe";
        const auto prefix = (fs::current_path() / L"out" / L"tests").wstring() + L"\\";
        Require(fixture.compare(0, prefix.size(), prefix) == 0, "scope restricted to out/tests");
        const auto workspace = root / L"workspace";
        std::wstring error;
        Require(atomic_write::AtomicWriteUtf8(workspace / L"workspace.json",
            "{\"classesDir\":\".\",\"startupSelectFirstSession\":false}", workspace, &error), "isolated workspace config");
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", workspace.c_str()), "child workspace override");
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_UI_AUTOMATION", nullptr), "disable unrelated automation");
        const std::wstring suffix = L"_memo_test_" + std::to_wstring(GetCurrentProcessId());
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_INSTANCE_SUFFIX", suffix.c_str()), "isolated instance identity");
        std::wstring command = L"\"" + executable.wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               executable.parent_path().c_str(), &startup, &process), "launch disposable actual application");
        childPid = process.dwProcessId;
        HWND main = WaitWindow(L"PdfWorkspaceMainWnd");
        Require(Until([&] { return IsWindowEnabled(main) && IsWindowVisible(main); }), "actual application ready");
        const auto original = workspace / L"__resource__" / L"__memo__" / L"workspace_memo.txt";

        Require(PostMessageW(main, WM_COMMAND, ID_SEARCH, 0), "open actual search");
        HWND search = WaitWindow(L"SearchWnd");
        Send(search, WM_COMMAND, MAKEWPARAM(4143, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(search, 4143)));
        HWND memo = WaitWindow(L"PdfNoteWorkspaceMemoWnd"), editor = GetDlgItem(memo, 1);
        Require(GetWindow(memo, GW_OWNER) == main && editor, "search entry owns memo through main");
        Type(editor, L"search shortcut"); CtrlS(editor, original, "search shortcut");
        Require(PostMessageW(search, WM_CLOSE, 0, 0) && Until([&] { return !IsWindow(search); }), "close actual search");
        Require(IsWindow(memo), "memo survives actual search close");

        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "open actual settings");
        HWND settings = WaitWindow(L"UnifiedSettingsShellClass");
        HWND tab = Descendant(settings, WC_TABCONTROLW);
        Require(tab != nullptr, "actual settings category tab");
        for (int i = 0; i < 5; ++i) Send(tab, WM_KEYDOWN, VK_RIGHT);
        HWND assets = nullptr;
        Require(Until([&] { assets = Descendant(settings, L"SettingsAssetsPageClass"); return assets != nullptr; }), "actual assets page selected");
        Send(assets, WM_COMMAND, MAKEWPARAM(7133, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(assets, 7133)));
        Require(Window(L"PdfNoteWorkspaceMemoWnd") == memo, "assets entry uses same memo");
        Type(editor, L"nested settings shortcut"); CtrlS(editor, original, "nested settings shortcut");
        Require(PostMessageW(settings, WM_CLOSE, 0, 0) && Until([&] { return !IsWindow(settings); }), "close actual settings");
        Require(IsWindow(memo) && IsWindowEnabled(main), "memo survives actual settings close");

        Send(main, WM_COMMAND, ID_WORKSPACE_MEMO);
        Require(Window(L"PdfNoteWorkspaceMemoWnd") == memo, "tools entry uses same memo");
        Require(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_READONLY), "actual readonly failure fixture");
        Type(editor, L"blocked actual exit");
        Require(PostMessageW(memo, WM_CLOSE, 0, 0), "actual memo failure close request");
        HWND notice = WaitWindow(L"SilentDialogClass");
        Send(notice, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(notice) && IsWindowEnabled(memo); }) && IsWindow(memo), "actual save failure keeps memo open");
        // The shared notice service suppresses the same message for 10 seconds.
        // A synchronous close now must return failure, not open a second dialog.
        Send(main, WM_CLOSE);
        Require(IsWindow(main) && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT &&
                Equals(original, "nested settings shortcut"), "actual save failure blocks app exit and preserves original");
        Require(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_NORMAL), "restore actual fixture attributes");
        Type(editor, L"actual application exit");
        Require(PostMessageW(main, WM_CLOSE, 0, 0), "normal app exit request");
        Require(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "actual app exit completes");
        Require(Equals(original, "actual application exit"), "normal app exit saves memo without separate close");
        DWORD code = 1; Require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "normal app exit code");
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        std::cout << "Workspace memo actual application: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        if (process.hProcess) {
            // Only our exact disposable child handle; never another user's app.
            TerminateProcess(process.hProcess, 79); WaitForSingleObject(process.hProcess, 1000);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
