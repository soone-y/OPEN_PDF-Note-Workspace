// Drive only the disposable child process. No physical keyboard/clipboard use.
#include <windows.h>
#include "core/command_ids.h"
#include "core/atomic_write.h"
#include "core/sha256.h"
#include "core/json_string.h"
#include "clrop/json.h"
#include <commctrl.h>
#include <sddl.h>
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
static std::wstring AssetCheckTime(HWND assets) {
    const HWND header = FindWindowExW(assets, nullptr, L"STATIC", nullptr);
    Require(header != nullptr, "asset check header exists");
    const auto text = WindowText(header);
    const std::wstring prefix = L"最終確認時刻: ";
    const auto offset = text.find(prefix);
    Require(offset != std::wstring::npos && text.size() >= offset + prefix.size() + 19,
            "header shows a completed asset check time");
    const auto time = text.substr(offset + prefix.size(), 19);
    Require(time[4] == L'-' && time[7] == L'-' && time[10] == L' ' && time[13] == L':' && time[16] == L':',
            "asset check time has a complete local date and time");
    return time;
}
static void CheckAssetButtonLayout(HWND assets) {
    RECT client{}; Require(GetClientRect(assets, &client), "asset client rectangle");
    int previousRight = -1;
    for (const int id : {7136, 7131, 7141, 7139, 7138, 7137}) {
        const HWND button = GetDlgItem(assets, id); RECT rect{};
        Require(button && IsWindowVisible(button) && GetWindowRect(button, &rect), "asset action button is visible");
        MapWindowPoints(nullptr, assets, reinterpret_cast<POINT*>(&rect), 2);
        Require(rect.left > previousRight && rect.right <= client.right && rect.top >= 0 && rect.bottom <= client.bottom,
                "asset action buttons remain inside the resized window without overlap");
        previousRight = rect.right;
    }
}
// Deny reading only in this owned fixture. Preserve/restore the exact original
// ACL, including on test failure, so cleanup never affects another workspace.
class DenyFixtureDirectoryRead {
    std::wstring path_;
    std::vector<unsigned char> security_;
    bool applied_ = false;
public:
    explicit DenyFixtureDirectoryRead(const fs::path& path) : path_(ToExtendedWin32PathIfAbsoluteLocal(path)) {
        DWORD needed = 0;
        GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
        Require(needed > 0, "capture fixture ACL size");
        security_.resize(needed);
        Require(GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, security_.data(), needed, &needed), "capture fixture ACL");
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;OICI;GR;;;WD)(A;OICI;GA;;;WD)",
            SDDL_REVISION_1, &descriptor, nullptr), "prepare fixture read denial");
        applied_ = SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, descriptor) != FALSE;
        LocalFree(descriptor); Require(applied_, "apply fixture read denial");
    }
    bool Restore() {
        if (!applied_) return true;
        if (!SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, security_.data())) return false;
        applied_ = false; return true;
    }
    ~DenyFixtureDirectoryRead() { Restore(); }
};
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
static HMENU CommandMenu(HMENU menu, UINT command) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(menu, i) == command) return menu;
        if (const auto child = GetSubMenu(menu, i)) if (const auto found = CommandMenu(child, command)) return found;
    }
    return nullptr;
}
static void CheckDetails(HWND details, const wchar_t* status, const wchar_t* additional) {
    const HWND message = GetDlgItem(details, 5101);
    const auto length = Send(message, WM_GETTEXTLENGTH);
    Require(length > 0 && length < 65536, "bounded details body");
    std::wstring text(length + 1, L'\0');
    const auto copied = Send(message, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()));
    text.resize(copied);
    const auto state = text.find(L"状況\r\n");
    const auto paths = text.find(L"\r\nパス\r\n");
    const auto information = text.find(L"\r\n追加情報\r\n");
    Require(state == 0 && paths != std::wstring::npos && information != std::wstring::npos && paths < information,
            "details show status, paths, then additional information");
    const auto result = text.find(status), extra = text.find(additional);
    if (result == std::wstring::npos || result >= paths || extra == std::wstring::npos || extra <= information) {
        const auto utf8 = [](const std::wstring& value) {
            const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            std::string bytes(count, '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), bytes.data(), count, nullptr, nullptr);
            return bytes;
        };
        std::cerr << "Details mismatch: expected " << utf8(status) << " / " << utf8(additional) << "\n" << utf8(text) << '\n';
    }
    Require(result != std::wstring::npos && result < paths && extra != std::wstring::npos && extra > information,
            "details retain the result and item-specific information");
    Require(text.find(L"…") != std::wstring::npos, "details abbreviate the long fixture paths");
    Require(GetDlgItem(details, 5200) != nullptr, "details retain explicit full-path copying");
    HWND tooltip = nullptr;
    while ((tooltip = FindWindowExW(nullptr, tooltip, TOOLTIPS_CLASSW, nullptr))) {
        DWORD pid = 0; GetWindowThreadProcessId(tooltip, &pid);
        if (pid == childPid && GetWindow(tooltip, GW_OWNER) == details) break;
    }
    Require(tooltip != nullptr, "details register a full-path hover tooltip");
    Require(Send(tooltip, TTM_GETTOOLCOUNT) == 1, "native tooltip accepts the details hover registration");
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
        // One owned, blank PDF page exercises the real startup PDF reader.
        std::string pdf = "%PDF-1.4\n";
        const char* objects[] = {"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> >>"};
        std::vector<size_t> offsets;
        for (size_t i = 0; i < 3; ++i) { offsets.push_back(pdf.size()); pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
        const auto xref = pdf.size(); pdf += "xref\n0 4\n0000000000 65535 f \n";
        for (const auto offset : offsets) { const auto number = std::to_string(offset); pdf += std::string(10 - number.size(), '0') + number + " 00000 n \n"; }
        pdf += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        const auto pdfPath = workspace / L"01_lecture" / L"01_session" / L"read.pdf";
        Require(atomic_write::AtomicWriteUtf8(pdfPath, pdf, pdfPath.parent_path(), &error), "owned PDF read fixture");
        Require(atomic_write::AtomicWriteUtf8(workspace / L"workspace.json",
            "{\"classesDir\":\".\",\"startupSelectFirstSession\":false,\"scheduleDayMask\":65}", workspace, &error), "isolated workspace config");
        Require(atomic_write::AtomicWriteUtf8(executable.parent_path() / L"pdf_note_workspace_setup.json",
            "{\"workspaceRootMode\":\"absolute\",\"workspaceRoot\":\"" + workspace.generic_u8string() + "\"}", executable.parent_path(), &error), "owned normal startup setup");
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", nullptr), "exercise actual setup reading without a workspace override");
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
        // Use the same native selection notifications as normal navigation;
        // merely finding a PDF on disk is not evidence of reading it.
        HWND lectureList = FindWindowExW(main, nullptr, WC_LISTBOXW, nullptr);
        HWND sessionList = FindWindowExW(main, lectureList, WC_LISTBOXW, nullptr);
        HWND pdfList = FindWindowExW(main, sessionList, WC_LISTBOXW, nullptr);
        Require(lectureList && sessionList && pdfList && Send(lectureList, LB_GETCOUNT) > 0, "owned workspace navigation controls");
        Send(lectureList, LB_SETCURSEL, 0);
        Send(main, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(lectureList));
        Require(Send(sessionList, LB_GETCOUNT) > 0, "owned session was enumerated");
        Send(sessionList, LB_SETCURSEL, 0);
        Send(main, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(sessionList));
        Require(Send(pdfList, LB_GETCOUNT) == 1, "owned PDF was enumerated");
        Send(pdfList, LB_SETCURSEL, 0);
        Send(main, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(pdfList));
        const auto original = workspace / L"__pdf_note_workspace__" / L"__memo__" / L"workspace_memo.txt";

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
        Require(tab && Send(tab, TCM_GETITEMCOUNT) == 5, "assets category removed from settings");
        Require(PostMessageW(settings, WM_CLOSE, 0, 0) && Until([&] { return !IsWindow(settings); }), "close actual settings");
        // A page with no weekday controls must not reset another page's value.
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_NOTE, 0), "open note-only settings");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }), "save note-only settings");
        std::string config;
        Require(ReadFileBytesWin32(workspace / L"workspace.json", config) &&
                config.find("\"scheduleDayMask\": 65") != std::string::npos,
                "note-only save preserves the configured Monday/Sunday mask");

        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open integrated color creation page");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        HWND palette = Descendant(settings, L"PaletteSettingsDlgClass");
        Require(palette && GetDlgItem(palette, 5220), "color editor belongs to settings, not another dialog");
        Type(GetDlgItem(palette, 5220), L"999");
        Send(settings, WM_COMMAND, IDOK);
        Require(IsWindow(settings) && WindowText(GetDlgItem(palette, 5220)) == L"999",
                "invalid RGB blocks save-and-close and preserves the draft");
        Type(GetDlgItem(palette, 5220), L"12");
        Send(GetDlgItem(palette, 5220), EM_SETSEL, 1, 1);
        Send(GetDlgItem(palette, 5220), WM_CHAR, L'0');
        Require(WindowText(GetDlgItem(palette, 5220)) == L"102" &&
                LOWORD(Send(GetDlgItem(palette, 5220), EM_GETSEL)) == 2,
                "live RGB synchronization preserves insertion and caret position");
        Type(GetDlgItem(palette, 5224), L"#A1");
        Send(settings, WM_COMMAND, IDOK);
        Require(IsWindow(settings) && WindowText(GetDlgItem(palette, 5224)) == L"#A1",
                "incomplete HEX is not silently replaced by the last valid color");
        Send(palette, WM_COMMAND, 5201);
        Require(WindowText(GetDlgItem(palette, 5224)) == L"#A1", "slot switch cannot discard invalid color input");
        Type(GetDlgItem(palette, 5224), L"123456");
        Require(WindowText(GetDlgItem(palette, 5220)) == L"18" &&
                WindowText(GetDlgItem(palette, 5224)) == L"123456",
                "HEX typing creates a color inline without rewriting the active field");
        Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }), "valid inline color saves and closes");

        const auto palettePath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"user_palette.json";
        std::string paletteBytes;
        Require(ReadFileBytesWin32(palettePath, paletteBytes), "capture independently saved palette");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open palette write failure regression");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        const std::wstring savedHex = WindowText(GetDlgItem(palette, 5224));
        Type(GetDlgItem(palette, 5224), L"#ABCDEF");
        HANDLE lockedPalette = CreateFileW(palettePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedPalette != INVALID_HANDLE_VALUE, "lock only owned palette against replacement");
        Send(settings, WM_COMMAND, IDOK);
        const bool paletteHeldOpen = IsWindow(settings);
        CloseHandle(lockedPalette);
        Require(paletteHeldOpen && Equals(palettePath, paletteBytes) &&
                WindowText(GetDlgItem(palette, 5224)) == L"#ABCDEF",
                "palette write failure preserves the file and editable color draft");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "exit after failed palette save");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "reopen palette after failed save exit");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        Require(WindowText(GetDlgItem(palette, 5224)) == savedHex, "failed palette save does not replace the saved color");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "close unchanged palette");

        // Hold only the disposable config against replacement, as a real write
        // sharing failure. The editor must stay open and keep the original.
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "open save failure regression");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        const auto configPath = workspace / L"workspace.json";
        HWND pendingLayout = DescendantId(settings, 4214);
        Require(pendingLayout && Send(pendingLayout, CB_GETCURSEL) == 0, "capture original layout before failed save");
        Send(pendingLayout, CB_SETCURSEL, 1);
        Require(ReadFileBytesWin32(configPath, config), "capture config before failed save");
        HANDLE lockedConfig = CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedConfig != INVALID_HANDLE_VALUE, "lock only owned config against replacement");
        Send(settings, WM_COMMAND, IDOK);
        const bool heldOpen = IsWindow(settings);
        CloseHandle(lockedConfig);
        Require(heldOpen && Equals(configPath, config) && Send(pendingLayout, CB_GETCURSEL) == 1,
                "failed save retains editor, pending choice and original config");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "exit after failed save discards only the unsaved draft");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "reopen after cancelled failed save");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        Require(Send(DescendantId(settings, 4214), CB_GETCURSEL) == 0,
                "unsaved layout is not left in runtime for a later auto-save");
        Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }), "retry after write failure succeeds");

        const auto schedulePath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"schedule.json";
        std::string scheduleBytes;
        Require(ReadFileBytesWin32(schedulePath, scheduleBytes), "capture separately persisted timetable");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "open schedule write failure regression");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        HANDLE lockedSchedule = CreateFileW(schedulePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedSchedule != INVALID_HANDLE_VALUE, "lock only owned timetable against replacement");
        Send(settings, WM_COMMAND, IDOK);
        const bool scheduleHeldOpen = IsWindow(settings);
        CloseHandle(lockedSchedule);
        Require(scheduleHeldOpen && Equals(schedulePath, scheduleBytes),
                "timetable save failure propagates instead of closing settings with false success");
        Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }), "retry after timetable write failure succeeds");

        // Both layout directions use real PDF/annotation files and collisions.
        // A separate, unopened PDF also verifies organization does not switch
        // the active document for every member of a batch.
        const auto session = pdfPath.parent_path();
        const auto bundledPdf = session / L"bundle.pdf", linkedNote = session / L"links.md";
        core_hash::Sha256 hash;
        hash.Update(reinterpret_cast<const std::uint8_t*>(pdf.data()), pdf.size());
        std::string digest;
        constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : hash.Finalize()) { digest += hex[byte >> 4]; digest += hex[byte & 15]; }
        const std::string linkedClrop = "{\"version\":1,\"pdf_id\":{\"path\":\"" +
            json_string::Escape(bundledPdf.u8string()) + "\",\"size\":" + std::to_string(pdf.size()) +
            ",\"page_count\":1,\"page_sizes_pt\":[[100,100]],\"sha256\":\"" + digest +
            "\"},\"pages\":[{\"page\":0,\"items\":[{\"type\":\"link-marker\",\"id\":\"organize-link\","
            "\"created\":\"2026-10-09T00:00:00Z\",\"updated\":\"2026-10-09T00:00:00Z\","
            "\"link_id\":\"organize-link\",\"p1\":[16,16],\"width\":6,\"color\":\"#282828\",\"note_path\":\"" +
            json_string::Escape(linkedNote.u8string()) + "\"}]}]}";
        clrop::Document fixtureAnnotations;
        Require(clrop::ParseClropFromJson(linkedClrop, fixtureAnnotations, error) &&
            fixtureAnnotations.pages.size() == 1 && fixtureAnnotations.pages[0].items.size() == 1 &&
            fixtureAnnotations.pages[0].items[0].notePath == linkedNote.wstring(),
            "linked annotation fixture passes the production parser before testing organization");
        const std::string orphanClrop = "{\"version\":1,\"pdf_id\":{\"sha256\":\"\"},\"pages\":[]}";
        Require(atomic_write::AtomicWriteUtf8(bundledPdf, pdf, session, &error) &&
            atomic_write::AtomicWriteUtf8(linkedNote, "linked note contents", session, &error) &&
            atomic_write::AtomicWriteUtf8(session / L"bundle.clrop", linkedClrop, session, &error) &&
            atomic_write::AtomicWriteUtf8(session / L"bundle.annot_history.json", "owned history bytes", session, &error),
            "prepare real linked PDF/note/history bundle");
        Require(atomic_write::AtomicWriteUtf8(session / L"pdf" / L"bundle.pdf", pdf, session, &error) &&
            atomic_write::AtomicWriteUtf8(session / L"pdf" / L"bundle (1).clrop", orphanClrop, session, &error) &&
            atomic_write::AtomicWriteUtf8(session / L"note" / L"links.md", "existing note", session, &error),
            "prepare PDF, annotation-only and note-name conflicts");
        Require(PostMessageW(main, WM_COMMAND, ID_FILE_ORGANIZE_SESSION_FILES, 0), "organize into separate directories");
        const auto movedPdf = session / L"pdf" / L"bundle (2).pdf";
        const auto movedClrop = session / L"pdf" / L"bundle (2).clrop";
        const auto movedNote = session / L"note" / L"links (1).md";
        Require(Until([&] { return fs::exists(movedPdf) && fs::exists(movedClrop) && fs::exists(movedNote) &&
            fs::exists(session / L"pdf" / L"bundle (2).annot_history.json") && !fs::exists(bundledPdf); }),
                "organization moves PDF with a bundle-wide conflict-free basename");
        std::string annotations;
        Require(Equals(movedPdf, pdf) && Equals(session / L"pdf" / L"bundle (2).annot_history.json", "owned history bytes") &&
            ReadFileBytesWin32(movedClrop, annotations) &&
            annotations.find(json_string::Escape(movedNote.u8string())) != std::string::npos &&
            annotations.find("organize-link") != std::string::npos,
            "organization preserves history and rewrites linked note paths");
        Require(Equals(session / L"note" / L"links.md", "existing note") &&
            Equals(session / L"pdf" / L"bundle (1).clrop", orphanClrop), "organization never overwrites conflicts");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "select native flat layout");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        HWND layout = DescendantId(settings, 4214);
        Require(layout && Send(layout, CB_GETCOUNT) == 2, "both layout choices are available");
        Send(layout, CB_SETCURSEL, 1); Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }), "save native flat layout choice");
        Require(PostMessageW(main, WM_COMMAND, ID_FILE_ORGANIZE_SESSION_FILES, 0), "organize into session root");
        const auto flatNote = session / L"links (1).md", flatClrop = session / L"bundle (2).clrop";
        Require(Until([&] { return fs::exists(flatClrop) && fs::exists(flatNote) &&
            fs::exists(session / L"bundle (2).annot_history.json") && !fs::exists(movedPdf); }),
                "flat layout moves both file kinds directly under the session");
        Require(ReadFileBytesWin32(flatClrop, annotations) &&
            annotations.find(json_string::Escape(flatNote.u8string())) != std::string::npos &&
            Equals(session / L"bundle (2).pdf", pdf) &&
            Equals(session / L"bundle (2).annot_history.json", "owned history bytes"),
            "reverse organization preserves the bundle and its note links");
        const auto assetsRequestedAt = GetTickCount64();
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_ASSETS, 0), "open assets through Help command");
        HWND assets = WaitWindow(L"PdfNoteSettingsAssetsWnd");
        HWND list = GetDlgItem(assets, 7130);
        const auto firstAssetCheck = AssetCheckTime(assets);
        Require(Send(reinterpret_cast<HWND>(Send(list, LVM_GETHEADER)), HDM_GETITEMCOUNT) == 8,
                "asset report includes presence, capacity, file-count and delta columns");
        Require(list && Send(list, LVM_GETITEMCOUNT) >= 14 && (GetWindowLongPtrW(list, GWL_STYLE) & LVS_TYPEMASK) == LVS_REPORT,
                "assets uses diagnostic-style report list");
        Require(GetWindow(assets, GW_OWNER) == main && IsWindowEnabled(main), "assets is a modeless main-owned window");
        Require(!GetDlgItem(assets, 7133) && !GetDlgItem(assets, 7134) && !GetDlgItem(assets, 7135),
                "assets no longer offers memo or preset actions");
        CheckAssetButtonLayout(assets);
        RECT assetRect{}; Require(GetWindowRect(assets, &assetRect), "capture asset window dimensions");
        Require(SetWindowPos(assets, nullptr, 0, 0, 840, 570, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                "resize actual asset window to minimum dimensions");
        CheckAssetButtonLayout(assets);
        Require(SetWindowPos(assets, nullptr, 0, 0, assetRect.right - assetRect.left, assetRect.bottom - assetRect.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "restore actual asset window dimensions");
        const auto settingsMenu = CommandMenu(GetMenu(main), ID_SETTINGS_GENERAL);
        Require(settingsMenu && CommandMenu(GetMenu(main), ID_SETTINGS_PRESETS) == settingsMenu,
                "preset entry is directly in Settings, not a nested menu");
        MENUITEMINFOW presetItem{};
        presetItem.cbSize = sizeof(presetItem);
        presetItem.fMask = MIIM_SUBMENU;
        Require(GetMenuItemInfoW(settingsMenu, ID_SETTINGS_PRESETS, FALSE, &presetItem) && !presetItem.hSubMenu,
                "preset entry has no submenu");
        Require(!CommandMenu(GetMenu(main), ID_SETTINGS_PRESET_SAVE) && !CommandMenu(GetMenu(main), ID_SETTINGS_PRESET_LOAD),
                "preset save and load are only offered inside the dialog");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PRESETS, 0), "open current preset entry");
        HWND presets = WaitWindow(L"PdfNoteSettingsPresetDialog");
        Require(Until([&] { return GetDlgItem(presets, 10) && GetDlgItem(presets, 11); }),
                "preset dialog retains save and load actions");
        const auto presetTitle = WindowText(presets);
        if (presetTitle != L"設定プリセットと復元") std::wcerr << L"Actual preset caption: " << presetTitle << L'\n';
        Require(presetTitle == L"設定プリセットと復元", "preset and restore title is explicit");
        Require(!IsWindowEnabled(main) && GetWindow(presets, GW_OWNER) == main,
                "preset and restore dialog is modal and main-owned");
        Require(WindowText(GetDlgItem(presets, 30)) == L"やめる", "preset dialog uses the standard exit action");
        const auto checkPresetPage = [&] {
            RECT client{}; Require(GetClientRect(presets, &client), "read preset client bounds");
            const auto font = Send(GetDlgItem(presets, 30), WM_GETFONT);
            Require(font != 0, "preset actions have the common UI font");
            for (HWND child = GetWindow(presets, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (!IsWindowVisible(child)) continue;
                RECT bounds{}; Require(GetWindowRect(child, &bounds), "read visible preset control bounds");
                MapWindowPoints(nullptr, presets, reinterpret_cast<POINT*>(&bounds), 2);
                Require(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
                        "preset and restore controls stay inside the frame");
                if (!WindowText(child).empty()) Require(Send(child, WM_GETFONT) == font,
                        "preset headings, explanations and actions share the common font");
            }
        };
        checkPresetPage();
        RECT presetBounds{}; Require(GetWindowRect(presets, &presetBounds), "capture responsive preset frame");
        // Exercise the application's DPI handler without changing the user's OS
        // scale. Larger margins/actions plus a constrained viewport require scroll.
        Send(presets, WM_DPICHANGED, MAKEWPARAM(192, 192), reinterpret_cast<LPARAM>(&presetBounds));
        Require(SetWindowPos(presets, nullptr, 0, 0, 760, 460, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                "constrain high-DPI preset dialog to a small display");
        SCROLLINFO presetScroll{}; presetScroll.cbSize = sizeof(presetScroll); presetScroll.fMask = SIF_ALL;
        Require(GetScrollInfo(presets, SB_VERT, &presetScroll) && presetScroll.nMax >= static_cast<int>(presetScroll.nPage),
                "long preset/restore content remains accessible by scrolling");
        Send(presets, WM_VSCROLL, SB_BOTTOM);
        RECT closeBounds{}, presetClient{};
        Require(GetWindowRect(GetDlgItem(presets, 30), &closeBounds) && GetClientRect(presets, &presetClient),
                "measure scrolled exit action");
        MapWindowPoints(nullptr, presets, reinterpret_cast<POINT*>(&closeBounds), 2);
        Require(closeBounds.top >= 0 && closeBounds.bottom <= presetClient.bottom, "scroll reveals the complete exit action");
        Send(presets, WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&presetBounds));
        Require(SetWindowPos(presets, nullptr, 0, 0, presetBounds.right - presetBounds.left,
            presetBounds.bottom - presetBounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "restore preset frame");
        Send(presets, WM_VSCROLL, SB_TOP);
        Send(presets, WM_COMMAND, 7102);
        Require(!IsWindowVisible(GetDlgItem(presets, 10)) && IsWindowVisible(GetDlgItem(presets, 20)),
                "restore tab shows restore actions without closing the modal dialog");
        checkPresetPage();
        Send(presets, WM_THEMECHANGED);
        Send(presets, WM_COMMAND, 7101);
        Require(IsWindowVisible(GetDlgItem(presets, 10)) && !IsWindowVisible(GetDlgItem(presets, 20)),
                "preset tab restores save and load actions");
        // IsDialogMessage maps Escape to this command. It must exit, not select
        // the restore tab (whose former ID collided with IDCANCEL).
        Send(presets, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(presets) && IsWindowEnabled(main); }), "cancel preset dialog without writing");
        const auto help = CommandMenu(GetMenu(main), ID_WRITE_CHECKS);
        Require(help && CommandMenu(GetMenu(main), ID_SETTINGS_ASSETS) == help, "assets and checks share the Help menu");
        Send(main, WM_COMMAND, ID_SETTINGS_ASSETS);
        Require(Window(L"PdfNoteSettingsAssetsWnd") == assets, "assets command reuses the same window");
        const auto initialRows = Send(list, LVM_GETITEMCOUNT);
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open initial presence details");
        HWND initialDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(initialDetails, L"あり", L"前回確認なし");
        Require(WindowText(GetDlgItem(initialDetails, 5101)).find(firstAssetCheck) != std::wstring::npos,
                "initial details show the check time");
        Send(initialDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(initialDetails); }), "close initial presence details");
        Require(!IsWindowEnabled(GetDlgItem(assets, 7131)), "initial presence check starts cooldown");
        Require(WindowText(GetDlgItem(assets, 7131)).find(L"再確認まで") != std::wstring::npos, "presence cooldown is visible");
        const auto themes = workspace / L"__pdf_note_workspace__" / L"__theme__";
        const auto newTheme = themes / L"theme_cooldown_test.json";
        Require(atomic_write::AtomicWriteUtf8(newTheme, "{}", themes, &error), "new asset in owned fixture");
        DenyFixtureDirectoryRead denyThemes(themes);
        Send(assets, WM_COMMAND, 7131); Send(assets, WM_TIMER, 7140);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows, "direct command and timer cannot inspect during cooldown");
        Require(AssetCheckTime(assets) == firstAssetCheck, "blocked refresh preserves the check time");
        Require(PostMessageW(assets, WM_CLOSE, 0, 0) && Until([&] { return !IsWindow(assets); }), "close assets during cooldown");
        Send(main, WM_COMMAND, ID_SETTINGS_ASSETS); assets = WaitWindow(L"PdfNoteSettingsAssetsWnd");
        list = GetDlgItem(assets, 7130);
        Require(!IsWindowEnabled(GetDlgItem(assets, 7131)) && Send(list, LVM_GETITEMCOUNT) == initialRows,
                "reopening retains cooldown and the previous snapshot");
        Require(AssetCheckTime(assets) == firstAssetCheck, "reopening retains the original check time");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7131)); }, 12000), "presence refresh becomes available");
        Require(GetTickCount64() - assetsRequestedAt >= 10000, "presence refresh waits ten seconds");
        Send(assets, WM_COMMAND, 7131);
        Require(!IsWindowEnabled(GetDlgItem(assets, 7131)), "manual refresh starts another cooldown");
        Send(list, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 10; ++i) Send(list, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open presence failure details");
        HWND presenceDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(presenceDetails, L"検査自体の失敗", L"有無を確定できません");
        Require(WindowText(GetDlgItem(presenceDetails, 5101)).find(L"前回からの変化: 比較不可") != std::wstring::npos,
                "inspection errors are not classified as disappearance");
        Send(presenceDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(presenceDetails); }), "close presence failure details");
        Require(denyThemes.Restore(), "restore fixture ACL");
        Send(assets, WM_COMMAND, 7131); Send(assets, WM_TIMER, 7140);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows, "cooldown prevents rescanning after conditions change");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7131)); }, 12000), "next presence refresh becomes available");
        Send(assets, WM_COMMAND, 7131);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows + 1, "allowed refresh discovers the new asset");
        const auto discoveredAt = AssetCheckTime(assets);
        Require(discoveredAt != firstAssetCheck, "completed refresh advances the check time");
        Send(list, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 12; ++i) Send(list, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open newly observed theme details");
        HWND newThemeDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(newThemeDetails, L"あり", L"前回からの変化: 新しく確認");
        Send(newThemeDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(newThemeDetails); }), "close newly observed theme details");
        const HANDLE lockedAsset = CreateFileW(newTheme.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedAsset != INVALID_HANDLE_VALUE, "lock only the owned theme asset");
        Send(assets, WM_COMMAND, 7138);
        HWND preview = GetDlgItem(assets, 7132);
        const auto lockedText = WindowText(preview);
        CloseHandle(lockedAsset);
        Require(lockedText.find(L"内容を読み取れませんでした") != std::wstring::npos && lockedText.find(L"32:") != std::wstring::npos &&
                lockedText.find(L"存在しません") == std::wstring::npos && lockedText.find(L"大きすぎる") == std::wstring::npos,
                "preview preserves sharing error instead of reporting absence or size");
        Require(Equals(newTheme, "{}"), "failed preview preserves the asset");
        Send(assets, WM_COMMAND, 7138);
        Require(WindowText(preview) == L"{}", "preview recovers after the lock is released");
        Require(atomic_write::AtomicWriteUtf8(newTheme, std::string(2 * 1024 * 1024 + 1, 'x'), themes, &error), "owned oversized preview fixture");
        Send(assets, WM_COMMAND, 7138);
        Require(WindowText(preview).find(L"大きすぎる") != std::wstring::npos, "oversized content remains a distinct preview result");
        Require(DeleteFileW(newTheme.c_str()), "remove only the generated preview fixture");
        Send(assets, WM_COMMAND, 7138);
        Require(WindowText(preview).find(L"現在存在しません") != std::wstring::npos, "deleted asset is distinguished from an unreadable one");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7131)); }, 12000), "refresh after deleting the theme becomes available");
        Send(assets, WM_COMMAND, 7131);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows + 1, "deleted theme remains in the report");
        const auto missingAt = AssetCheckTime(assets);
        Send(list, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 12; ++i) Send(list, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open disappeared theme details");
        HWND missingDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(missingDetails, L"なし", L"前回からの変化: あり → なし");
        const auto missingText = WindowText(GetDlgItem(missingDetails, 5101));
        Require(missingText.find(L"前回の状態: あり") != std::wstring::npos &&
                missingText.find(discoveredAt) != std::wstring::npos && missingText.find(missingAt) != std::wstring::npos,
                "disappearance details preserve both states and check times");
        Send(missingDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(missingDetails); }), "close disappeared theme details");
        Send(assets, WM_CLOSE); Require(Until([&] { return !IsWindow(assets); }), "close with a retained missing theme");
        Send(main, WM_COMMAND, ID_SETTINGS_ASSETS); assets = WaitWindow(L"PdfNoteSettingsAssetsWnd");
        list = GetDlgItem(assets, 7130); preview = GetDlgItem(assets, 7132);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows + 1 && AssetCheckTime(assets) == missingAt,
                "reopening preserves the missing row, change and timestamp");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7131)); }, 12000), "repeat missing-theme check becomes available");
        Send(assets, WM_COMMAND, 7131);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows + 1, "repeated missing result retains the known theme");
        Send(list, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 12; ++i) Send(list, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open repeated missing-theme details");
        missingDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(missingDetails, L"なし", L"前回からの変化: 変化なし");
        Send(missingDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(missingDetails); }), "close repeated missing-theme details");
        Require(atomic_write::AtomicWriteUtf8(newTheme, "{}", themes, &error), "restore only the owned missing theme");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7131)); }, 12000), "recovered-theme check becomes available");
        Send(assets, WM_COMMAND, 7131);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows + 1, "restored theme does not create a duplicate row");
        Send(list, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 12; ++i) Send(list, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open recovered theme details");
        HWND recoveredDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(recoveredDetails, L"あり", L"前回からの変化: なし → あり");
        Send(recoveredDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(recoveredDetails); }), "close recovered theme details");
        Send(list, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 7; ++i) Send(list, WM_KEYDOWN, VK_DOWN);
        Send(assets, WM_COMMAND, 7138);
        Require(preview && IsWindowVisible(preview) && (GetWindowLongPtrW(preview, GWL_STYLE) & ES_READONLY) &&
                Send(preview, WM_GETTEXTLENGTH) == 15, "saved memo preview remains visible and read-only");
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open shared path details");
        HWND details = WaitWindow(L"SilentDialogClass");
        CheckDetails(details, L"あり", L"種別: ファイル");
        Send(details, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(details); }), "close path details");
        const auto capacityPresenceTime = AssetCheckTime(assets);
        const auto capacityFile = workspace / L"__pdf_note_workspace__/__tmp__/capacity_fixture.bin";
        Require(atomic_write::AtomicWriteUtf8(capacityFile, std::string(123, 'x'), capacityFile.parent_path(), &error),
                "seed capacity in the owned management directory");
        // Opening the workspace already creates last-open state in __tmp__.
        // Independently sum the fixture metadata rather than assuming it is empty.
        std::uint64_t capacityBytes = 0, capacityFiles = 0;
        for (const auto& file : fs::recursive_directory_iterator(capacityFile.parent_path())) if (file.is_regular_file()) {
            capacityBytes += file.file_size(); ++capacityFiles;
        }
        const auto observedCapacity = [&](std::uint64_t bytes) {
            return std::to_wstring(bytes) + L" バイト / " + std::to_wstring(capacityFiles) + L" ファイル";
        };
        const auto selectTemporary = [&] {
            Send(list, WM_KEYDOWN, VK_HOME);
            const auto index = Send(list, LVM_GETITEMCOUNT) - 4; // Capacity categories are the final five rows.
            for (DWORD_PTR i = 0; i < index; ++i) Send(list, WM_KEYDOWN, VK_DOWN);
        };
        const auto measureCapacity = [&] {
            Require(IsWindowEnabled(GetDlgItem(assets, 7141)), "explicit capacity action is available");
            Send(assets, WM_COMMAND, 7141);
            Require(Until([&] { return WindowText(GetDlgItem(assets, 7141)).find(L"秒") != std::wstring::npos; }),
                    "background capacity scan completes and starts cooldown");
            Require(AssetCheckTime(assets) == capacityPresenceTime, "capacity scan preserves the independent presence timestamp");
        };
        measureCapacity(); selectTemporary();
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open complete capacity details");
        details = WaitWindow(L"SilentDialogClass");
        auto capacityText = WindowText(GetDlgItem(details, 5101));
        Require(capacityText.find(observedCapacity(capacityBytes)) != std::wstring::npos &&
                capacityText.find(L"集計完了") != std::wstring::npos && capacityText.find(L"容量確認時刻:") != std::wstring::npos,
                "capacity details report bytes, count, completion and a distinct timestamp");
        Require(capacityText.find(L"前回容量確認:") == std::wstring::npos, "first capacity measurement has no invented previous result");
        Send(details, WM_COMMAND, IDOK); Require(Until([&] { return !IsWindow(details); }), "close first capacity details");
        Send(assets, WM_CLOSE); Require(Until([&] { return !IsWindow(assets); }), "close measured assets window");
        Send(main, WM_COMMAND, ID_SETTINGS_ASSETS); assets = WaitWindow(L"PdfNoteSettingsAssetsWnd"); list = GetDlgItem(assets, 7130);
        Require(!IsWindowEnabled(GetDlgItem(assets, 7141)), "capacity cooldown survives reopening");
        // Reopening can refresh presence after its separate cooldown expires.
        const auto reopenedPresenceTime = AssetCheckTime(assets);
        selectTemporary(); Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open capacity details asynchronously"); details = WaitWindow(L"SilentDialogClass");
        Require(WindowText(GetDlgItem(details, 5101)).find(observedCapacity(capacityBytes)) != std::wstring::npos,
                "reopening retains the measured capacity snapshot");
        Send(details, WM_COMMAND, IDOK); Require(Until([&] { return !IsWindow(details); }), "close cached capacity details");
        Require(atomic_write::AtomicWriteUtf8(capacityFile, std::string(200, 'y'), capacityFile.parent_path(), &error), "grow owned capacity fixture");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7141)); }, 12000), "capacity recheck becomes available");
        Send(assets, WM_COMMAND, 7141);
        Require(Until([&] { return WindowText(GetDlgItem(assets, 7141)).find(L"秒") != std::wstring::npos; }), "second capacity scan completes");
        Require(AssetCheckTime(assets) == reopenedPresenceTime, "second capacity scan does not rescan presence");
        selectTemporary(); Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open capacity details asynchronously"); details = WaitWindow(L"SilentDialogClass");
        capacityText = WindowText(GetDlgItem(details, 5101));
        Require(capacityText.find(observedCapacity(capacityBytes + 77)) != std::wstring::npos &&
                capacityText.find(L"前回取得分:") != std::wstring::npos && capacityText.find(L"+77 B") != std::wstring::npos,
                "complete snapshots retain previous size and show an accurate delta");
        Send(details, WM_COMMAND, IDOK); Require(Until([&] { return !IsWindow(details); }), "close capacity delta details");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7141)); }, 12000), "partial capacity test becomes available");
        {
            DenyFixtureDirectoryRead denied(capacityFile.parent_path());
            Send(assets, WM_COMMAND, 7141);
            Require(Until([&] { return WindowText(GetDlgItem(assets, 7141)).find(L"秒") != std::wstring::npos; }), "failed subtree scan completes");
            selectTemporary(); Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open capacity details asynchronously"); details = WaitWindow(L"SilentDialogClass");
            capacityText = WindowText(GetDlgItem(details, 5101));
            Require(capacityText.find(L"取得分のみ") != std::wstring::npos && capacityText.find(L"前回容量との差: —") != std::wstring::npos,
                    "unreadable subtree is explicitly partial and never an apparent size decrease");
            Send(details, WM_COMMAND, IDOK); Require(Until([&] { return !IsWindow(details); }), "close partial capacity details");
            Require(denied.Restore(), "restore capacity fixture ACL");
        }
        Require(Equals(capacityFile, std::string(200, 'y')), "capacity inspection preserves fixture contents");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7141)); }, 12000), "close-after-launch check becomes available");
        Send(assets, WM_COMMAND, 7141);
        const auto closingAt = GetTickCount64();
        Send(assets, WM_CLOSE);
        Require(!IsWindow(assets) && GetTickCount64() - closingAt < 1000 && IsWindowEnabled(main),
                "closing after scan launch does not wait for its worker or disable main");
        Send(main, WM_COMMAND, ID_SETTINGS_ASSETS); assets = WaitWindow(L"PdfNoteSettingsAssetsWnd"); list = GetDlgItem(assets, 7130);
        Require(Until([&] { return WindowText(GetDlgItem(assets, 7141)).find(L"秒") != std::wstring::npos; }),
                "reopening safely consumes the completed or canceled worker result");
        selectTemporary(); Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open capacity details asynchronously"); details = WaitWindow(L"SilentDialogClass");
        capacityText = WindowText(GetDlgItem(details, 5101));
        Require(capacityText.find(L"中断") != std::wstring::npos || capacityText.find(L"集計完了") != std::wstring::npos,
                "close-after-launch result reports cancellation or completion without losing its state");
        Send(details, WM_COMMAND, IDOK); Require(Until([&] { return !IsWindow(details); }), "close worker-lifetime details");
        Send(main, WM_COMMAND, ID_WRITE_CHECKS);
        HWND writeChecks = WaitWindow(L"PdfNoteWriteChecksWnd");
        Require(PostMessageW(writeChecks, WM_COMMAND, 4704, 0), "open check details without running a trial");
        details = WaitWindow(L"SilentDialogClass");
        CheckDetails(details, L"成功", L"通常利用で、この対象を読み込んだ");
        Send(details, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(details); }), "close check details");
        HWND checksList = GetDlgItem(writeChecks, 4702);
        Send(checksList, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 2; ++i) Send(checksList, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(writeChecks, WM_COMMAND, 4704, 0), "workspace read evidence without a manual trial");
        details = WaitWindow(L"SilentDialogClass");
        CheckDetails(details, L"成功", L"確認元: 通常利用");
        Send(details, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(details); }), "close normal workspace read details");
        Send(checksList, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 3; ++i) Send(checksList, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(writeChecks, WM_COMMAND, 4704, 0), "open normal-use evidence without a manual trial");
        details = WaitWindow(L"SilentDialogClass");
        CheckDetails(details, L"成功", L"確認元: 通常利用");
        Send(details, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(details); }), "close normal-use check details");
        Send(checksList, WM_KEYDOWN, VK_HOME);
        for (int i = 0; i < 13; ++i) Send(checksList, WM_KEYDOWN, VK_DOWN);
        Require(PostMessageW(writeChecks, WM_COMMAND, 4704, 0), "PDF read evidence without a manual trial");
        details = WaitWindow(L"SilentDialogClass");
        CheckDetails(details, L"成功", L"通常利用で、この対象を読み込んだ");
        Send(details, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(details); }), "close normal PDF read details");
        Require(PostMessageW(writeChecks, WM_CLOSE, 0, 0) && Until([&] { return !IsWindow(writeChecks); }), "close actual write checks");
        Send(main, WM_COMMAND, ID_WORKSPACE_MEMO);
        Require(Window(L"PdfNoteWorkspaceMemoWnd") == memo, "tools entry uses same memo while assets is open");
        Type(editor, L"nested settings shortcut"); CtrlS(editor, original, "nested settings shortcut");
        Require(PostMessageW(assets, WM_CLOSE, 0, 0) && Until([&] { return !IsWindow(assets); }), "close actual assets window");
        Require(IsWindow(memo) && IsWindowEnabled(main), "memo survives actual assets close");

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
        process = {};
        // A fallback startup must preserve the failed setup-read result, even
        // when the application itself starts successfully in its default root.
        Require(atomic_write::AtomicWriteUtf8(executable.parent_path() / L"pdf_note_workspace_setup.json",
            "{ broken fixture setup", executable.parent_path(), &error), "owned corrupt setup fixture");
        command = L"\"" + executable.wstring() + L"\"";
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               executable.parent_path().c_str(), &startup, &process), "launch isolated fallback startup");
        childPid = process.dwProcessId; main = WaitWindow(L"PdfWorkspaceMainWnd");
        Require(Until([&] {
            if (HWND startupNotice = Window(L"SilentDialogClass")) Send(startupNotice, WM_COMMAND, IDOK);
            return IsWindowEnabled(main) && IsWindowVisible(main);
        }), "fallback startup notice acknowledged in owned app");
        Send(main, WM_COMMAND, ID_WRITE_CHECKS); writeChecks = WaitWindow(L"PdfNoteWriteChecksWnd");
        Require(PostMessageW(writeChecks, WM_COMMAND, 4704, 0), "show failed setup read after successful fallback startup");
        details = WaitWindow(L"SilentDialogClass");
        CheckDetails(details, L"失敗", L"確認元: 通常利用");
        Send(details, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(details); }), "close failed setup read details");
        Send(writeChecks, WM_CLOSE); Require(Until([&] { return !IsWindow(writeChecks); }), "close fallback checks");
        Require(PostMessageW(main, WM_CLOSE, 0, 0), "close owned fallback app");
        Require(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "fallback app exit completes");
        Require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "fallback app exit code");
        CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
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
