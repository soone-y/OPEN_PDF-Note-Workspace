// Drive only the disposable child process. No physical keyboard/clipboard use.
#include <windows.h>
#include "core/command_ids.h"
#include "core/atomic_write.h"
#include "core/sha256.h"
#include "core/json_string.h"
#include "settings/palette_stock_store.h"
#include "settings/palette_presets_store.h"
#include "settings/palette_active_store.h"
#include "settings/palette_color_math.h"
#include "settings/settings_navigation.h"
#include "palette_presets_test_support.h"
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
static const char* lastCheck = "test startup";
static void Require(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); lastCheck = label; }
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
        std::cerr << "Missing child window: " << std::string(label.begin(), label.end()) << " checks=" << checks
                  << " after=" << lastCheck << '\n';
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
static void CheckSettingsSidebar(HWND settings, int selectedPage) {
    const HWND tree = GetDlgItem(settings, 7100);
    Require(tree && IsWindowVisible(tree) && IsWindowEnabled(tree), "actual settings sidebar is available");
    Require(settings_navigation::GroupCount == 6 && settings_navigation::Count == 16 &&
            Send(tree, TVM_GETCOUNT) == settings_navigation::GroupCount + settings_navigation::Count,
            "six settings groups contain all current pages without an extra assets category");
    auto group = Send(tree, TVM_GETNEXTITEM, TVGN_ROOT);
    DWORD_PTR selectedItem = 0;
    for (int index = 0; index < settings_navigation::GroupCount; ++index) {
        Require(group != 0, "required settings group exists");
        auto leaf = Send(tree, TVM_GETNEXTITEM, TVGN_CHILD, static_cast<LPARAM>(group));
        for (const auto page : settings_navigation::sidebarOrder) {
            if (settings_navigation::entries[page].group != index) continue;
            Require(leaf != 0, "required settings page exists");
            if (page == selectedPage) selectedItem = leaf;
            leaf = Send(tree, TVM_GETNEXTITEM, TVGN_NEXT, static_cast<LPARAM>(leaf));
        }
        Require(leaf == 0, "settings group has no extra page");
        group = Send(tree, TVM_GETNEXTITEM, TVGN_NEXT, static_cast<LPARAM>(group));
    }
    Require(group == 0 && selectedItem != 0 && Send(tree, TVM_GETNEXTITEM, TVGN_CARET) == selectedItem &&
            !WindowText(GetDlgItem(settings, 7103)).empty(),
            "settings entry selects the requested existing page without navigating it in the test");
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
    // Exercise native typing/EN_CHANGE; no cross-process string pointers or
    // synthetic EN_CHANGE notifications and no user's physical keyboard.
    // Queue selection and typing together. Synchronous cross-thread sends can
    // reenter modal teardown before its focus restoration has completed and
    // turn a replacement into an append. Observe the actual resulting text
    // before any save/close operation, without retrying the input itself.
    Require(PostMessageW(editor, EM_SETSEL, 0, -1), "queue owned editor selection");
    if (!*text) Require(PostMessageW(editor, WM_CLEAR, 0, 0), "queue owned editor clearing");
    for (const wchar_t* character = text; *character; ++character)
        Require(PostMessageW(editor, WM_CHAR, *character, 0), "queue owned editor character");
    Require(Until([&] { return WindowText(editor) == text; }), "native typing replaces the owned editor selection");
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
static void SelectAssetRow(HWND list, const std::wstring& label) {
    DWORD owner = 0; GetWindowThreadProcessId(list, &owner);
    Require(owner == childPid && label.size() < 1024, "asset lookup targets only the bounded owned child control");
    // Common-control messages above WM_USER are not pointer-marshalled. Own a
    // tiny buffer in this disposable child, release it on every exit, and use
    // native exact label lookup rather than stale row offsets.
    HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, childPid);
    Require(process != nullptr, "open exact owned child for asset label lookup");
    struct ProcessOwner { HANDLE value; ~ProcessOwner() { CloseHandle(value); } } processOwner{process};
    const size_t textBytes = (label.size() + 1) * sizeof(wchar_t);
    void* memory = VirtualAllocEx(process, nullptr, sizeof(LVFINDINFOW) + textBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Require(memory != nullptr, "allocate owned cross-process list lookup buffer");
    struct BufferOwner { HANDLE process; void* memory; ~BufferOwner() { VirtualFreeEx(process, memory, 0, MEM_RELEASE); } } bufferOwner{process, memory};
    LVFINDINFOW find{}; find.flags = LVFI_STRING;
    find.psz = reinterpret_cast<wchar_t*>(static_cast<unsigned char*>(memory) + sizeof(find));
    SIZE_T written = 0;
    Require(WriteProcessMemory(process, memory, &find, sizeof(find), &written) && written == sizeof(find) &&
        WriteProcessMemory(process, const_cast<wchar_t*>(find.psz), label.c_str(), textBytes, &written) && written == textBytes,
        "copy bounded exact asset label into owned child buffer");
    const auto row = Send(list, LVM_FINDITEMW, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(memory));
    Require(row != static_cast<DWORD_PTR>(-1) && row < Send(list, LVM_GETITEMCOUNT), "expected asset label exists in real inventory");
    Send(list, WM_KEYDOWN, VK_HOME);
    for (DWORD_PTR index = 0; index < row; ++index) Send(list, WM_KEYDOWN, VK_DOWN);
    Require(Send(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) == row, "normal navigation selects the exact asset row");
}
static std::wstring PresetColorText(COLORREF color) {
    wchar_t hex[8]{};
    swprintf(hex, std::size(hex), L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return hex;
}
[[nodiscard]] static bool SavedPaletteTokens(const std::string& bytes, std::vector<std::string>& out) {
    size_t pos = 0;
    const auto skip = [&]() { while (pos < bytes.size() && (bytes[pos] == ' ' || bytes[pos] == '\n' || bytes[pos] == '\r' || bytes[pos] == '\t')) ++pos; };
    const auto take = [&](char ch) { skip(); if (pos == bytes.size() || bytes[pos] != ch) return false; ++pos; return true; };
    std::string key;
    if (!take('{')) return false;
    skip();
    if (!json_string::DecodeToken(bytes, &pos, &key) || key != "colors" || !take(':') || !take('[')) return false;
    std::vector<std::string> colors;
    for (;;) {
        skip();
        std::string hex;
        if (!json_string::DecodeToken(bytes, &pos, &hex) || hex.size() != 7 || hex[0] != '#') return false;
        for (size_t i = 1; i < hex.size(); ++i) {
            char& ch = hex[i];
            if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 'a' + 'A');
            if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) return false;
        }
        colors.push_back(hex);
        skip();
        if (pos < bytes.size() && bytes[pos] == ']') break;
        if (!take(',')) return false;
    }
    if (!take(']') || !take('}')) return false;
    skip();
    if (pos != bytes.size()) return false;
    out = std::move(colors); return true;
}
static void SelectPaletteSet(HWND page, int index) {
    const HWND combo = GetDlgItem(page, 5262);
    Require(Send(combo, CB_SETCURSEL, index) == static_cast<DWORD_PTR>(index), "select a fixed palette preset slot");
    Send(page, WM_COMMAND, MAKEWPARAM(5262, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
}
template<class Completed> static void ConfirmPaletteAction(HWND page, int action, bool yes, Completed completed) {
    Require(PostMessageW(page, WM_COMMAND, action, 0), "post explicit palette action without blocking its modal confirmation");
    const HWND confirm = WaitWindow(L"SilentDialogClass");
    Send(confirm, WM_COMMAND, yes ? 5102 : IDCANCEL);
    Require(Until([&] { return !IsWindow(confirm) && completed(); }), "palette confirmation returns and parent action completes");
}
static void CheckPaletteLayout(HWND page) {
    RECT client{};
    Require(GetClientRect(page, &client), "measure actual palette page");
    std::vector<RECT> bounds;
    // Combo-box rectangle is the closed selector; its drop-down has a separate window.
    for (const int id : {5260, 5261, 5262, 5263, 5264, 5265, 5266, 5267, 5250, 5251, 5252, 5253, 5254, 5255, 5256,
        5270, 5271, 5272, 5273, 5217, 5218, 5280, 5282, 5283}) {
        const HWND control = GetDlgItem(page, id);
        RECT rect{};
        Require(control && GetWindowRect(control, &rect), "palette preset and stock controls all exist");
        MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&rect), 2);
        Require(rect.left >= 0 && rect.right <= client.right && rect.right > rect.left && rect.bottom > rect.top,
            "palette management controls fit the actual page width");
        for (const auto& previous : bounds) {
            RECT overlap{};
            Require(!IntersectRect(&overlap, &rect, &previous), "palette preset and stock controls do not overlap");
        }
        bounds.push_back(rect);
    }
    Send(page, WM_VSCROLL, SB_BOTTOM);
    RECT last{};
    Require(GetWindowRect(GetDlgItem(page, 5256), &last), "measure last stock operation");
    MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&last), 2);
    // On narrow screens the color editor follows stock. This checks content
    // coverage only; physical focus scrolling remains a separate manual check.
    SCROLLINFO scroll{sizeof(scroll), SIF_ALL};
    Require(GetScrollInfo(page, SB_VERT, &scroll) && last.top + scroll.nPos >= 0 &&
        last.bottom + scroll.nPos <= scroll.nMax + 1, "palette scrollbar covers every management action");
    Send(page, WM_VSCROLL, SB_TOP);
}
static void WaitAutoPalette(HWND page, const fs::path& path, std::string& bytes) {
    Require(Until([&] {
        return WindowText(GetDlgItem(page, 5280)).find(L"保存済み") == 0 && ReadFileBytesWin32(path, bytes);
    }), "numeric palette changes auto-save without a save button");
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
int main(int argc, char** argv) {
    PROCESS_INFORMATION process{};
    try {
        // The runner gives an absolute fixture path through this private child environment.
        const DWORD count = GetEnvironmentVariableW(L"WORKSPACE_MEMO_APP_TEST_ROOT", nullptr, 0);
        Require(count > 0, "test root provided");
        std::wstring fixture(count, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"WORKSPACE_MEMO_APP_TEST_ROOT", fixture.data(), count);
        Require(length > 0 && length < count, "test root captured"); fixture.resize(length);
        const fs::path root(fixture);
        const auto executable = root / L"app" / (L"pdf_note_memo_" + root.filename().wstring() + L".exe");
        const auto prefix = (fs::current_path() / L"out" / L"tests").wstring() + L"\\";
        Require(fixture.compare(0, prefix.size(), prefix) == 0, "scope restricted to out/tests");
        if (argc == 3 && std::strcmp(argv[1], "--palette-lock-helper") == 0) {
            const std::string token(argv[2]);
            Require(!token.empty() && token.size() < 64 && token.find_first_not_of("0123456789_") == std::string::npos,
                "bounded owned lock worker identity");
            return palette_test::RunLockWorker(root, std::wstring(token.begin(), token.end())) ? 0 : 1;
        }
        const bool lectureOrderOnly = argc == 2 && std::strcmp(argv[1], "--lecture-order-only") == 0;
        Require(argc == 1 || lectureOrderOnly, "test driver accepts only its explicit modes");
        const auto workspace = root / L"workspace";
        std::wstring error;
        {
            for (const COLORREF rgb : {RGB(255,0,0), RGB(0,255,0), RGB(0,0,255), RGB(255,255,255), RGB(0,0,0), RGB(18,52,86)}) {
                const auto result = palette_color::ToRgb(palette_color::FromRgb(rgb, 2100));
                Require(std::abs(int(GetRValue(result)) - int(GetRValue(rgb))) <= 1 &&
                    std::abs(int(GetGValue(result)) - int(GetGValue(rgb))) <= 1 &&
                    std::abs(int(GetBValue(result)) - int(GetBValue(rgb))) <= 1, "HSV RGB round trip within one channel step");
            }
            Require(palette_color::FromRgb(RGB(40,40,40), 2100).h == 2100 &&
                palette_color::Position(-10, 100, 1000) == 0 && palette_color::Position(200, 100, 1000) == 1000 &&
                palette_color::Position(5, 1, 1000) == 0, "gray hue and degenerate gradient coordinates are stable");
            const auto activeRoot = root / L"active_palette_fixture";
            palette_active::Snapshot state;
            Require(palette_active::Load(activeRoot, state) && !state.exists, "missing active palette is safely editable");
            std::vector<COLORREF> colors(24, RGB(255,255,255));
            Require(palette_active::Save(state, colors), "active palette permits duplicate legacy slots");
            const auto path = palette_stock::FilePath(activeRoot, L"user_palette.json");
            const auto first = state.bytes;
            const auto stale = state;
            colors[0] = RGB(1,2,3);
            Require(palette_active::Save(state, colors), "active palette saves with recovery backup");
            auto staleAttempt = stale;
            Require(!palette_active::Save(staleAttempt, stale.colors) && Equals(path, state.bytes), "stale active palette cannot overwrite newer colors");
            bool backup = false;
            for (const auto& entry : fs::directory_iterator(activeRoot / L"__pdf_note_workspace__" / L"__escape__")) backup |= Equals(entry.path(), first);
            Require(backup, "active auto-save retains previous palette bytes");
            auto parsed = colors;
            for (const auto& bad : {std::string("{}"), std::string("{\"colors\":[]}"),
                std::string("{\"colors\":[\"#12345Z\"]}"), state.bytes + "x", std::string(65537, ' ')})
                Require(!palette_active::Parse(bad, parsed) && parsed == colors, "invalid active palette preserves parsed output");
            Require(palette_active::Parse("{\"colors\":[\"112233\"]}", parsed) && parsed == std::vector<COLORREF>{RGB(17,34,51)}, "active palette accepts legacy hex spelling");
            const auto before = state.bytes;
            HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            Require(lock != INVALID_HANDLE_VALUE, "lock owned active palette replacement");
            colors[0] = RGB(4,5,6);
            const bool saved = palette_active::Save(state, colors);
            CloseHandle(lock);
            Require(!saved && state.bytes == before && Equals(path, before), "active palette write failure preserves file and snapshot");
            Require(atomic_write::AtomicWriteUtf8(path, "broken", path.parent_path(), &error), "prepare corrupt active palette");
            palette_active::Snapshot unreadable;
            Require(!palette_active::Load(activeRoot, unreadable) && !unreadable.ready &&
                !palette_active::Save(unreadable, colors) && Equals(path, "broken"), "corrupt active palette cannot be auto-overwritten");
        }
        {
            std::vector<COLORREF> colors{RGB(1, 2, 3), RGB(171, 205, 239)};
            std::string json;
            Require(palette_stock::Serialize(colors, json), "serialize color stock");
            std::vector<COLORREF> parsed;
            Require(palette_stock::Parse(json, parsed) && parsed == colors, "stock RGB/HEX round trip");
            for (const auto& bad : {std::string("{}"), std::string("{\"colors\":[\"#112233\",]}"),
                std::string("{\"colors\":[\"#112233\",\"#112233\"]}"),
                std::string("{\"colors\":[\"#12345Z\"]}"),
                std::string("{\"colors\":[],\"future\":true}"), json + "x", std::string(65537, ' ')}) {
                Require(!palette_stock::Parse(bad, parsed) && parsed == colors, "invalid stock keeps parsed output unchanged");
            }
            std::vector<COLORREF> capacity;
            for (size_t i = 0; i < palette_stock::kMaxColors; ++i) capacity.push_back(RGB(i, 0, 0));
            Require(palette_stock::Serialize(capacity, json) && palette_stock::Parse(json, parsed) && parsed == capacity,
                    "stock accepts all 128 colors");
            capacity.push_back(RGB(128, 0, 0));
            const auto savedJson = json;
            Require(!palette_stock::Serialize(capacity, json) && json == savedJson, "stock rejects overflow without partial output");
            palette_stock::Snapshot state;
            const auto stockRoot = root / L"stock_store_fixture";
            Require(palette_stock::Load(stockRoot, state) && !state.exists && state.colors.empty(), "missing stock starts empty");
            Require(palette_stock::Save(state, colors), "explicit stock save");
            const auto firstBytes = state.bytes;
            palette_stock::Snapshot stale = state;
            colors.push_back(RGB(19, 87, 155));
            Require(palette_stock::Save(state, colors), "append stock with a recovery backup");
            const auto stockPath = palette_stock::FilePath(stockRoot);
            Require(!palette_stock::Save(stale, {RGB(9, 9, 9)}) && Equals(stockPath, state.bytes), "stale stock cannot overwrite newer colors");
            const auto escape = stockRoot / L"__pdf_note_workspace__" / L"__escape__";
            bool backedUp = false;
            for (const auto& entry : fs::directory_iterator(escape)) backedUp = backedUp || Equals(entry.path(), firstBytes);
            Require(backedUp, "stock update retains the previous file in workspace recovery data");
            HANDLE locked = CreateFileW(stockPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(locked != INVALID_HANDLE_VALUE, "lock owned color stock against replacement");
            const auto beforeFailure = state.bytes;
            const bool replaced = palette_stock::Save(state, {RGB(3, 4, 5)});
            CloseHandle(locked);
            Require(!replaced && state.bytes == beforeFailure && Equals(stockPath, beforeFailure), "failed stock save retains file and snapshot");
            Require(atomic_write::AtomicWriteUtf8(stockPath, "{\"colors\":[\"broken\"]}",
                    stockPath.parent_path(), escape, &error), "prepare corrupt owned stock");
            Require(!palette_stock::Load(stockRoot, state) && state.bytes == beforeFailure &&
                    !palette_stock::Save(state, colors) && Equals(stockPath, "{\"colors\":[\"broken\"]}"),
                    "corrupt stock is never overwritten with an empty default");
        }
        {
            palette_presets::Document doc;
            for (size_t i = 0; i < palette_presets::kUserCount; ++i) {
                palette_presets::Colors colors{};
                colors.fill(RGB(i + 1, 34, 56)); // Repeated colors within a set are intentional and valid.
                doc.users[i] = colors;
            }
            std::string json;
            Require(palette_presets::Serialize(doc, json), "serialize five user palette sets and immutable initial set");
            palette_presets::Document parsed;
            Require(palette_presets::Parse(json, parsed) && parsed.users == doc.users,
                "six palette sets round trip with repeated colors");
            auto alteredInitial = json;
            alteredInitial.replace(alteredInitial.find("#FF8C00"), 7, "#FF8C01");
            auto sixthUser = json;
            sixthUser.insert(sixthUser.rfind("\n  ]"), ", null");
            auto shortSet = json;
            shortSet.erase(shortSet.find(", \"#012238\""), 11);
            auto unknown = json;
            unknown.insert(unknown.rfind('}'), ", \"future\": true");
            auto duplicate = json;
            duplicate.insert(duplicate.rfind('}'), ", \"version\": 1");
            auto unsupported = json;
            unsupported.replace(unsupported.find("\"version\": 1"), 12, "\"version\": 2");
            for (const auto& bad : {alteredInitial, sixthUser, shortSet, unknown, duplicate, unsupported,
                    json + "x", std::string(65537, ' ')}) {
                Require(!palette_presets::Parse(bad, parsed) && parsed.users == doc.users,
                    "invalid palette collection preserves parsed users and immutable initial set");
            }
            auto invalid = doc;
            (*invalid.users[0])[0] = CLR_INVALID;
            const auto beforeSerialize = json;
            Require(!palette_presets::Serialize(invalid, json) && json == beforeSerialize,
                "invalid preset RGB cannot partially serialize");
            palette_presets::Snapshot state;
            const auto presetRoot = root / L"palette_presets_fixture";
            Require(palette_presets::Load(presetRoot, state) && !state.exists &&
                    state.colors.users == palette_presets::Document{}.users,
                "missing palette collection starts with five empty user slots");
            Require(palette_presets::Save(state, doc), "save all five palette user slots in one file");
            auto stale = state;
            const auto firstBytes = state.bytes;
            doc.users[4].reset();
            Require(palette_presets::Save(state, doc), "clear one user slot without changing the initial set");
            const auto path = palette_presets::FilePath(presetRoot);
            Require(!palette_presets::Save(stale, stale.colors) && Equals(path, state.bytes),
                "stale palette collection cannot overwrite newer presets");
            const auto escape = presetRoot / L"__pdf_note_workspace__" / L"__escape__";
            bool backup = false;
            for (const auto& entry : fs::directory_iterator(escape)) backup = backup || Equals(entry.path(), firstBytes);
            Require(backup, "palette preset update keeps previous file in workspace backup");
            const auto beforeBackupFailure = state;
            {
                palette_test::DenyDirectoryWrite deny(escape);
                Require(deny.applied(), "deny writes only to the owned preset backup directory");
                const auto probe = escape / L"denied_palette_backup_probe";
                HANDLE denied = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                const DWORD deniedError = GetLastError();
                if (denied != INVALID_HANDLE_VALUE) CloseHandle(denied);
                const bool saved = palette_presets::Save(state, palette_presets::Document{});
                Require(deny.Restore(), "restore exact fixture backup ACL");
                Require(denied == INVALID_HANDLE_VALUE && deniedError == ERROR_ACCESS_DENIED,
                    "native write probe confirms backup denial is active");
                Require(!saved && state.bytes == beforeBackupFailure.bytes && state.colors.users == beforeBackupFailure.colors.users &&
                    Equals(path, beforeBackupFailure.bytes), "backup creation failure preserves preset file and all five user slots");
            }
            // Use the exact running driver, not a stale executable guessed from PATH.
            std::wstring driver(512, L'\0');
            for (;;) {
                const DWORD size = GetModuleFileNameW(nullptr, driver.data(), static_cast<DWORD>(driver.size()));
                Require(size != 0, "capture current lock test driver");
                if (size < driver.size()) { driver.resize(size); break; }
                Require(driver.size() < 32768, "lock test driver path stays within Win32 limits");
                driver.resize(std::min<size_t>(32768, driver.size() * 2));
            }
            {
                palette_test::OwnedLockWorker worker(root, fs::path(driver));
                Require(worker.ready(), "independent process holds the palette workspace writer lock");
                const auto beforeBusy = state;
                const bool busySave = palette_presets::Save(state, palette_presets::Document{});
                palette_stock::Snapshot stockState;
                Require(palette_stock::Load(presetRoot, stockState), "read stock while preset writer is busy");
                const bool busyStock = palette_stock::Save(stockState, {RGB(1, 2, 3)});
                Require(worker.ReleaseAndJoin(), "release and join the exact palette lock worker");
                Require(!busySave && !busyStock && state.bytes == beforeBusy.bytes && state.colors.users == beforeBusy.colors.users &&
                    Equals(path, beforeBusy.bytes) && !fs::exists(palette_stock::FilePath(presetRoot)),
                    "cross-process lock refuses both palette writers without changing files or snapshots");
                Require(palette_presets::Save(state, doc), "preset saving resumes after the competing process releases its lock");
            }
            const auto beforeFailure = state.bytes;
            HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(locked != INVALID_HANDLE_VALUE, "lock owned palette preset file against replacement");
            const bool saved = palette_presets::Save(state, palette_presets::Document{});
            CloseHandle(locked);
            Require(!saved && state.bytes == beforeFailure && Equals(path, beforeFailure),
                "failed palette preset save retains file and snapshot");
            Require(atomic_write::AtomicWriteUtf8(path, alteredInitial, path.parent_path(), escape, &error),
                "prepare tampered initial set in owned fixture");
            Require(!palette_presets::Load(presetRoot, state) && state.bytes == beforeFailure &&
                    !palette_presets::Save(state, doc) && Equals(path, alteredInitial),
                "tampered initial set is never overwritten with empty user presets");
            Require(palette_stock::FilePath(presetRoot, L"../escape.json").empty(), "palette store rejects arbitrary relative destinations");
        }
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
        std::error_code lectureError;
        fs::create_directories(workspace / L"00_lecture", lectureError);
        Require(!lectureError, "owned unassigned lecture fixture");
        fs::create_directories(workspace / L"02_lecture", lectureError);
        Require(!lectureError, "owned tied lecture fixture");
        Require(atomic_write::AtomicWriteUtf8(workspace / L"workspace.json",
            "{\"classesDir\":\".\",\"startupSelectFirstSession\":false,\"lectureSortMode\":\"manual\",\"lecturePriorities\":[{\"name\":\"01_lecture\",\"priority\":7},{\"name\":\"02_lecture\",\"priority\":7}]}", workspace, &error), "isolated workspace config");
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
        Require(lectureList && sessionList && pdfList && Send(lectureList, LB_GETCOUNT) == 3, "owned workspace navigation controls");
        wchar_t lectureLabel[260]{};
        Send(lectureList, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(lectureLabel));
        Require(std::wstring(lectureLabel).find(L"01_lecture") != std::wstring::npos,
                "priority overrides name order in the actual lecture list");
        Send(lectureList, LB_GETTEXT, 1, reinterpret_cast<LPARAM>(lectureLabel));
        Require(std::wstring(lectureLabel).find(L"02_lecture") != std::wstring::npos,
                "equal priorities use lecture name order");
        Send(lectureList, LB_GETTEXT, 2, reinterpret_cast<LPARAM>(lectureLabel));
        Require(std::wstring(lectureLabel).find(L"00_lecture") != std::wstring::npos,
                "unassigned lectures follow assigned priorities");
        Require(PostMessageW(main, WM_COMMAND, ID_LECTURE_ORDER, 0), "open lecture priority editor");
        HWND orderWindow = WaitWindow(L"LectureOrderWindow");
        Require(GetWindow(orderWindow, GW_OWNER) == main, "priority editor belongs to the isolated app");
        HWND priorityEdit = GetDlgItem(orderWindow, 6101);
        HWND orderList = GetDlgItem(orderWindow, 6100);
        Require(priorityEdit && orderList, "lecture priority editor controls");
        Require(Send(orderList, LB_GETCOUNT) == 3, "lecture priority editor enumerates managed lectures");
        Send(orderList, LB_SETCURSEL, 2);
        Require(Send(orderList, LB_GETCURSEL) == 2, "select unassigned lecture in priority editor");
        Send(orderWindow, WM_COMMAND, MAKEWPARAM(6100, LBN_SELCHANGE), reinterpret_cast<LPARAM>(orderList));
        Send(priorityEdit, WM_CHAR, L'1');
        Send(orderWindow, WM_COMMAND, MAKEWPARAM(6102, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(orderWindow, 6102)));
        wchar_t orderMessage[260]{};
        GetWindowTextW(GetDlgItem(orderWindow, 6105), orderMessage, 260);
        Require(orderMessage[0] == L'\0', "priority editor accepts and saves the entered number");
        Send(lectureList, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(lectureLabel));
        Require(std::wstring(lectureLabel).find(L"00_lecture") != std::wstring::npos,
                "setting priority immediately reorders the main lecture list");
        const auto orderConfigPath = workspace / L"workspace.json";
        std::string orderConfig;
        Require(ReadFileBytesWin32(orderConfigPath, orderConfig) &&
                orderConfig.find("\"name\":\"00_lecture\",\"priority\":1") != std::string::npos,
                "lecture priority is saved to the workspace");
        Send(priorityEdit, WM_CHAR, VK_BACK);
        for (const wchar_t digit : L"10000") {
            if (digit) Send(priorityEdit, WM_CHAR, digit);
        }
        Send(orderWindow, WM_COMMAND, MAKEWPARAM(6102, BN_CLICKED),
             reinterpret_cast<LPARAM>(GetDlgItem(orderWindow, 6102)));
        GetWindowTextW(GetDlgItem(orderWindow, 6105), orderMessage, 260);
        Require(orderMessage[0] && Equals(orderConfigPath, orderConfig),
                "out-of-range priority is rejected without changing saved settings");
        HANDLE lockedOrderConfig = CreateFileW(orderConfigPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedOrderConfig != INVALID_HANDLE_VALUE, "lock owned priority settings for failure test");
        for (int i = 0; i < 5; ++i) Send(priorityEdit, WM_CHAR, VK_BACK);
        Send(priorityEdit, WM_CHAR, L'4');
        Send(orderWindow, WM_COMMAND, MAKEWPARAM(6102, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(orderWindow, 6102)));
        CloseHandle(lockedOrderConfig);
        Require(Equals(orderConfigPath, orderConfig), "failed priority save keeps workspace settings unchanged");
        Send(lectureList, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(lectureLabel));
        Require(std::wstring(lectureLabel).find(L"00_lecture") != std::wstring::npos,
                "failed priority save keeps the previous list order");
        Send(orderWindow, WM_COMMAND, MAKEWPARAM(6103, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(orderWindow, 6103)));
        Send(lectureList, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(lectureLabel));
        Require(std::wstring(lectureLabel).find(L"01_lecture") != std::wstring::npos,
                "clearing priority returns unassigned lectures to the end");
        Send(orderWindow, WM_CLOSE);
        if (lectureOrderOnly) {
            Require(PostMessageW(main, WM_CLOSE, 0, 0), "close lecture order test app");
            Require(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
                    "lecture order test app exits");
            DWORD code = 0;
            Require(GetExitCodeProcess(process.hProcess, &code) && code == 0,
                    "lecture order test app exit code");
            CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
            command = L"\"" + executable.wstring() + L"\"";
            Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, executable.parent_path().c_str(), &startup, &process),
                    "restart lecture order test app");
            childPid = process.dwProcessId;
            main = WaitWindow(L"PdfWorkspaceMainWnd");
            Require(Until([&] { return IsWindowEnabled(main) && IsWindowVisible(main); }),
                    "restarted lecture order app ready");
            lectureList = FindWindowExW(main, nullptr, WC_LISTBOXW, nullptr);
            Require(lectureList && Send(lectureList, LB_GETCOUNT) == 3,
                    "restarted lecture list has all managed lectures");
            Send(lectureList, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(lectureLabel));
            Require(std::wstring(lectureLabel).find(L"01_lecture") != std::wstring::npos,
                    "saved priority order survives restart");
            Send(lectureList, LB_GETTEXT, 2, reinterpret_cast<LPARAM>(lectureLabel));
            Require(std::wstring(lectureLabel).find(L"00_lecture") != std::wstring::npos,
                    "cleared priority remains unassigned after restart");
            Require(PostMessageW(main, WM_CLOSE, 0, 0), "close restarted lecture order app");
            Require(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
                    "restarted lecture order app exits");
            Require(GetExitCodeProcess(process.hProcess, &code) && code == 0,
                    "restarted lecture order app exit code");
            CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
            std::cout << "Lecture order actual application: " << checks << " checks passed.\n";
            return 0;
        }
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
        CheckSettingsSidebar(settings, settings_navigation::Environment);
        Require(PostMessageW(settings, WM_CLOSE, 0, 0) && Until([&] { return !IsWindow(settings); }), "close actual settings");
        // Saving a different settings page must preserve lecture priorities.
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_NOTE, 0), "open note-only settings");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }), "save note-only settings");
        std::string config;
        Require(ReadFileBytesWin32(workspace / L"workspace.json", config) &&
                config.find("\"name\":\"01_lecture\",\"priority\":7") != std::string::npos,
                "note-only save preserves the configured lecture priority");

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
        const bool inlineHexReady = Until([&] { return WindowText(GetDlgItem(palette, 5220)) == L"18" &&
                WindowText(GetDlgItem(palette, 5221)) == L"52" && WindowText(GetDlgItem(palette, 5222)) == L"86" &&
                WindowText(GetDlgItem(palette, 5224)) == L"123456"; });
        if (!inlineHexReady) std::wcerr << L"Inline HEX state: red=" << WindowText(GetDlgItem(palette, 5220))
            << L" hex=" << WindowText(GetDlgItem(palette, 5224)) << L'\n';
        Require(inlineHexReady,
                "HEX typing creates a color inline without rewriting the active field");
        Send(settings, WM_COMMAND, IDOK);
        const bool inlineSaved = Until([&] { return !IsWindow(settings); });
        if (!inlineSaved) {
            std::wcerr << L"Inline Save state: R=" << WindowText(GetDlgItem(palette, 5220))
                << L" G=" << WindowText(GetDlgItem(palette, 5221)) << L" B=" << WindowText(GetDlgItem(palette, 5222))
                << L" HEX=" << WindowText(GetDlgItem(palette, 5224)) << L'\n';
        }
        Require(inlineSaved, "valid inline color saves and closes");

        const auto palettePath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"user_palette.json";
        std::string paletteBytes;
        Require(ReadFileBytesWin32(palettePath, paletteBytes), "capture independently saved palette");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open gradient regression");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        Require(!IsWindowVisible(GetDlgItem(settings, IDOK)) && !IsWindowVisible(GetDlgItem(settings, 7101)) &&
            !IsWindowVisible(GetDlgItem(settings, 7102)) && WindowText(GetDlgItem(settings, IDCANCEL)) == L"閉じる",
            "palette footer contains the current exit action only");
        HWND paletteFooter = FindWindowExW(settings, nullptr, L"STATIC", nullptr);
        while (paletteFooter && WindowText(paletteFooter).find(L"パレットの変更は自動保存されます。") != 0)
            paletteFooter = FindWindowExW(settings, paletteFooter, L"STATIC", nullptr);
        Require(paletteFooter && IsWindowVisible(paletteFooter) &&
            WindowText(paletteFooter).find(L"保存状態") != std::wstring::npos &&
            WindowText(paletteFooter).find(L"破棄") == std::wstring::npos,
            "normal palette footer explains automatic saving without a discard claim");
        Require((GetWindowLongPtrW(palette, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
            "palette background repaint excludes its child controls");
        HWND gradient = GetDlgItem(palette, 5271);
        RECT gradientRect{}; Require(gradient && GetClientRect(gradient, &gradientRect), "saturation brightness gradient exists inline");
        Type(GetDlgItem(palette, 5224), L"#FF0000"); WaitAutoPalette(palette, palettePath, paletteBytes);
        const auto beforeDrag = paletteBytes;
        const auto beforeDragHex = WindowText(GetDlgItem(palette, 5224));
        Send(gradient, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(gradientRect.right / 2, gradientRect.bottom / 2));
        Require(WindowText(GetDlgItem(palette, 5224)) != beforeDragHex && Equals(palettePath, beforeDrag),
            "gradient drag updates numeric preview without writing each sample");
        Send(gradient, WM_KEYDOWN, VK_ESCAPE);
        Require(WindowText(GetDlgItem(palette, 5224)) == beforeDragHex && Equals(palettePath, beforeDrag),
            "gradient Escape restores exact color without saving");
        Send(gradient, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(gradientRect.right / 2, gradientRect.bottom / 2));
        Send(gradient, WM_LBUTTONUP, 0, MAKELPARAM(gradientRect.right / 2, gradientRect.bottom / 2));
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Require(paletteBytes != beforeDrag, "gradient release auto-saves the chosen color");
        const auto beforeArrow = paletteBytes;
        Send(gradient, WM_KEYDOWN, VK_RIGHT); WaitAutoPalette(palette, palettePath, paletteBytes);
        Require(paletteBytes != beforeArrow, "gradient arrows select and save colors");
        Type(GetDlgItem(palette, 5224), L"#A1");
        Send(palette, WM_TIMER, 1);
        Require(Equals(palettePath, paletteBytes), "incomplete numeric input never auto-saves");
        Type(GetDlgItem(palette, 5224), L"#123456"); WaitAutoPalette(palette, palettePath, paletteBytes);
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }) && Equals(palettePath, paletteBytes), "Close keeps automatically saved colors");
        Require(PostMessageW(main, WM_COMMAND, ID_TOOL_COLOR_CUSTOM, 0), "open annotation palette through full settings");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        CheckSettingsSidebar(settings, settings_navigation::Palette);
        Require(palette && !IsWindowEnabled(main),
                "annotation color uses the same full settings dialog and modal owner");
        const std::wstring originalCustomHex = WindowText(GetDlgItem(palette, 5224));
        Send(palette, WM_COMMAND, 5200);
        Send(palette, WM_COMMAND, 5207);
        Require(WindowText(GetDlgItem(palette, 5224)) == originalCustomHex,
                "annotation entry initially selects custom palette slot eight");
        Type(GetDlgItem(palette, 5224), L"#2468AC");
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }) && Equals(palettePath, paletteBytes),
                "annotation palette Close preserves immediately saved colors");

        Require(PostMessageW(main, WM_COMMAND, ID_TOOL_COLOR_CUSTOM, 0), "reopen annotation palette for explicit save");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        Require(palette && WindowText(GetDlgItem(palette, 5224)) == L"#2468AC",
                "automatic custom color persists on reopen");
        Type(GetDlgItem(palette, 5224), L"#13579B");
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Require(IsWindow(settings) && !IsWindowEnabled(main) && ReadFileBytesWin32(palettePath, paletteBytes) &&
                paletteBytes.find("#13579B") != std::string::npos,
                "annotation palette automatic save keeps settings open and writes the custom color");
        Type(GetDlgItem(palette, 5224), L"#AB");
        Require(PostMessageW(palette, WM_COMMAND, 5283, 0), "reload incomplete input asynchronously");
        HWND reloadConfirm = WaitWindow(L"SilentDialogClass");
        Send(reloadConfirm, WM_COMMAND, 5102);
        Require(Until([&] { return !IsWindow(reloadConfirm) && WindowText(GetDlgItem(palette, 5224)) == L"#13579B"; }), "reload discards only incomplete input");
        Require(palette && !IsWindowEnabled(main) && WindowText(GetDlgItem(palette, 5224)) == L"#13579B",
                "reverting the palette reloads the saved color without enabling the modal owner");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }) && Equals(palettePath, paletteBytes),
                "saved annotation palette survives Exit");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open palette preset UI");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        HWND setsList = GetDlgItem(palette, 5262);
        const auto setsPath = palette_presets::FilePath(workspace);
        Require(setsList && Send(setsList, CB_GETCOUNT) == 6 && !IsWindowEnabled(GetDlgItem(palette, 5265)) &&
                !IsWindowEnabled(GetDlgItem(palette, 5266)), "initial palette set is read only alongside five user slots");
        Send(palette, WM_COMMAND, 5265); // Bypass disabled button: backend must still refuse the initial set.
        Require(!fs::exists(setsPath), "initial palette set cannot be overwritten through a direct command");
        Send(setsList, CB_SETCURSEL, 1);
        Send(palette, WM_COMMAND, MAKEWPARAM(5262, CBN_SELCHANGE), reinterpret_cast<LPARAM>(setsList));
        Type(GetDlgItem(palette, 5224), L"#224466");
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Send(palette, WM_COMMAND, 5265);
        std::string setsBytes;
        Require(ReadFileBytesWin32(setsPath, setsBytes) && setsBytes.find("#224466") != std::string::npos &&
                Equals(palettePath, paletteBytes), "saving a palette set is immediate and does not commit the active palette");
        Send(palette, WM_COMMAND, 5207);
        Type(GetDlgItem(palette, 5224), L"#010203");
        Send(palette, WM_COMMAND, 5200);
        Type(GetDlgItem(palette, 5224), L"#6688AA");
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Require(PostMessageW(palette, WM_COMMAND, 5265, 0), "request preset overwrite asynchronously");
        HWND setConfirm = WaitWindow(L"SilentDialogClass");
        Send(setConfirm, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(setConfirm); }) && Equals(setsPath, setsBytes),
                "cancelling preset overwrite preserves saved sets and editor colors");
        Require(WindowText(GetDlgItem(palette, 5224)) == L"#6688AA", "overwrite cancellation retains draft color");
        Require(PostMessageW(palette, WM_COMMAND, 5264, 0), "cancel preset application asynchronously");
        setConfirm = WaitWindow(L"SilentDialogClass");
        Send(setConfirm, WM_COMMAND, IDOK); // Default action is No, not Apply.
        Require(Until([&] { return !IsWindow(setConfirm); }) && WindowText(GetDlgItem(palette, 5224)) == L"#6688AA" &&
                Equals(palettePath, paletteBytes), "default confirmation preserves automatically saved palette edits");
        Require(PostMessageW(palette, WM_COMMAND, 5264, 0), "apply saved palette set asynchronously");
        setConfirm = WaitWindow(L"SilentDialogClass");
        Send(setConfirm, WM_COMMAND, 5102); // Explicit Yes action; Enter defaults to No.
        // Destroying the confirmation HWND precedes returning to the parent
        // palette handler. Observe completed application, not merely closure.
        Require(Until([&] { return !IsWindow(setConfirm) &&
                WindowText(GetDlgItem(palette, 5224)) == L"#224466" &&
                WindowText(GetDlgItem(palette, 5280)).find(L"保存済み") == 0; }),
                "palette preset applies and immediately saves eight slots");
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Send(palette, WM_COMMAND, 5207);
        Require(WindowText(GetDlgItem(palette, 5224)) == L"#13579B", "applying a palette preset also restores the eighth draft slot");
        Send(palette, WM_COMMAND, 5200);
        HANDLE lockedSets = CreateFileW(setsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedSets != INVALID_HANDLE_VALUE, "lock palette collection for UI save failure");
        Send(setsList, CB_SETCURSEL, 2);
        Send(palette, WM_COMMAND, MAKEWPARAM(5262, CBN_SELCHANGE), reinterpret_cast<LPARAM>(setsList));
        Send(palette, WM_COMMAND, 5265);
        CloseHandle(lockedSets);
        Require(Equals(setsPath, setsBytes) && !IsWindowEnabled(GetDlgItem(palette, 5264)),
                "failed UI preset save keeps the original file and empty user slot");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }) && Equals(setsPath, setsBytes) && Equals(palettePath, paletteBytes),
                "palette preset and active application survive Close");
        Require(PostMessageW(main, WM_COMMAND, ID_TOOL_COLOR_CUSTOM, 0), "annotation entry shares palette presets");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        setsList = GetDlgItem(palette, 5262);
        Send(setsList, CB_SETCURSEL, 1);
        Send(palette, WM_COMMAND, MAKEWPARAM(5262, CBN_SELCHANGE), reinterpret_cast<LPARAM>(setsList));
        Require(IsWindowEnabled(GetDlgItem(palette, 5264)), "palette preset is available through the annotation color entry");
        Require(PostMessageW(palette, WM_COMMAND, 5266, 0), "clear saved preset asynchronously");
        setConfirm = WaitWindow(L"SilentDialogClass");
        Send(setConfirm, WM_COMMAND, 5102);
        Require(Until([&] { return !IsWindow(setConfirm) && !IsWindowEnabled(GetDlgItem(palette, 5264)); }) &&
                Equals(palettePath, paletteBytes), "clearing a user preset leaves the active palette unchanged");
        Require(ReadFileBytesWin32(setsPath, setsBytes), "capture cleared palette collection");
        Require(atomic_write::AtomicWriteUtf8(setsPath, "{}", setsPath.parent_path(),
            workspace / L"__pdf_note_workspace__" / L"__escape__", &error), "prepare invalid palette collection for UI reload");
        Send(palette, WM_COMMAND, 5267);
        Require(!IsWindowEnabled(GetDlgItem(palette, 5265)) && Equals(setsPath, "{}"),
                "invalid palette collection disables user writes without replacing the file");
        Send(setsList, CB_SETCURSEL, 0);
        Send(palette, WM_COMMAND, MAKEWPARAM(5262, CBN_SELCHANGE), reinterpret_cast<LPARAM>(setsList));
        Require(IsWindowEnabled(GetDlgItem(palette, 5264)), "initial palette set remains usable when the user file is invalid");
        Require(atomic_write::AtomicWriteUtf8(setsPath, setsBytes, setsPath.parent_path(),
            workspace / L"__pdf_note_workspace__" / L"__escape__", &error), "restore owned palette collection after reload test");
        Send(palette, WM_COMMAND, 5267);
        Send(setsList, CB_SETCURSEL, 5);
        Send(palette, WM_COMMAND, MAKEWPARAM(5262, CBN_SELCHANGE), reinterpret_cast<LPARAM>(setsList));
        Require(IsWindowEnabled(GetDlgItem(palette, 5265)), "explicit preset reload restores fifth user slot access");
        palette_presets::Document expectedUserSets;
        // Save through the real buttons, including every editable slot in every user set.
        for (size_t user = 0; user < palette_presets::kUserCount; ++user) {
            SelectPaletteSet(palette, static_cast<int>(user + 1));
            palette_presets::Colors colors{};
            for (size_t slot = 0; slot < colors.size(); ++slot) {
                colors[slot] = RGB(30 + user * 20 + slot, 20 + slot * 15, 180 - user * 20);
                Send(palette, WM_COMMAND, 5200 + slot);
                Type(GetDlgItem(palette, 5224), PresetColorText(colors[slot]).c_str());
            }
            WaitAutoPalette(palette, palettePath, paletteBytes);
            Send(palette, WM_COMMAND, 5265);
            expectedUserSets.users[user] = colors;
            palette_presets::Snapshot persisted;
            Require(palette_presets::Load(workspace, persisted) && persisted.colors.users == expectedUserSets.users &&
                Equals(palettePath, paletteBytes), "UI saves each user set without altering other sets or rewriting the saved active palette");
        }
        SelectPaletteSet(palette, 2);
        Send(palette, WM_COMMAND, 5200);
        Type(GetDlgItem(palette, 5224), L"#CC8844");
        // The editor still contains user set 5 here. An overwrite intentionally
        // captures all eight draft slots, not just the currently selected color.
        auto overwritten = *expectedUserSets.users[4];
        overwritten[0] = RGB(204, 136, 68);
        expectedUserSets.users[1] = overwritten;
        ConfirmPaletteAction(palette, 5265, true, [&] {
            palette_presets::Snapshot state;
            return palette_presets::Load(workspace, state) && state.colors.users == expectedUserSets.users;
        });
        palette_presets::Snapshot overwrittenState;
        Require(palette_presets::Load(workspace, overwrittenState) && overwrittenState.colors.users == expectedUserSets.users,
            "confirmed UI overwrite replaces all eight colors in only the chosen user set");
        Require(ReadFileBytesWin32(setsPath, setsBytes), "capture five populated user sets");
        for (size_t user = 0; user < palette_presets::kUserCount; ++user) {
            const auto draftBeforeSelection = WindowText(GetDlgItem(palette, 5224));
            SelectPaletteSet(palette, static_cast<int>(user + 1));
            Require(WindowText(GetDlgItem(palette, 5224)) == draftBeforeSelection, "selecting a set previews without changing draft colors");
            Send(palette, WM_COMMAND, 5200);
            ConfirmPaletteAction(palette, 5264, true, [&] {
                return WindowText(GetDlgItem(palette, 5224)) == PresetColorText((*expectedUserSets.users[user])[0]);
            });
            for (size_t slot = 0; slot < palette_presets::kSlotCount; ++slot) {
                Send(palette, WM_COMMAND, 5200 + slot);
                Require(WindowText(GetDlgItem(palette, 5224)) == PresetColorText((*expectedUserSets.users[user])[slot]),
                    "UI applies every color from all five user sets");
            }
            WaitAutoPalette(palette, palettePath, paletteBytes);
            std::vector<COLORREF> activeColors;
            Require(palette_active::Parse(paletteBytes, activeColors) && activeColors.size() == 24 &&
                std::equal(expectedUserSets.users[user]->begin(), expectedUserSets.users[user]->end(), activeColors.begin()) &&
                Equals(setsPath, setsBytes), "applying a set saves all eight active colors without changing stored sets");
        }
        RECT paletteFrame{};
        Require(GetWindowRect(settings, &paletteFrame), "capture palette settings dimensions");
        for (const int width : {900, 640}) {
            Require(SetWindowPos(settings, nullptr, 0, 0, width, 540, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                "resize palette settings through wide and narrow layouts");
            CheckPaletteLayout(palette);
        }
        Require(SetWindowPos(settings, nullptr, 0, 0, paletteFrame.right - paletteFrame.left,
            paletteFrame.bottom - paletteFrame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "restore palette settings dimensions");
        const auto beforeKeyboardSelection = WindowText(GetDlgItem(palette, 5224));
        Send(setsList, CB_SHOWDROPDOWN, TRUE);
        Send(setsList, WM_KEYDOWN, VK_HOME);
        Send(setsList, WM_KEYDOWN, VK_DOWN);
        Send(setsList, WM_KEYDOWN, VK_RETURN);
        Require(Send(setsList, CB_GETCURSEL) == 1 && WindowText(GetDlgItem(palette, 5224)) == beforeKeyboardSelection,
            "keyboard preset selection changes preview only");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "exit preset checks without changing active palette");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open saved color stock UI");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        HWND stockList = GetDlgItem(palette, 5252);
        Require(stockList && Send(stockList, LB_GETCOUNT) == 0, "new workspace has empty color stock");
        Type(GetDlgItem(palette, 5224), L"#556677");
        Send(palette, WM_COMMAND, 5253);
        const auto stockPath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"color_stock.json";
        std::string stockBytes;
        Require(Send(stockList, LB_GETCOUNT) == 1 && ReadFileBytesWin32(stockPath, stockBytes) &&
                stockBytes.find("#556677") != std::string::npos, "stock add saves immediately");
        Send(palette, WM_COMMAND, 5253);
        Require(Send(stockList, LB_GETCOUNT) == 1 && Equals(stockPath, stockBytes), "stock duplicate is selected without a second entry");
        Type(GetDlgItem(palette, 5224), L"#778899");
        Send(palette, WM_COMMAND, 5253);
        Require(Send(stockList, LB_GETCOUNT) == 2, "multiple stock colors coexist with eight palette slots");
        Send(palette, WM_COMMAND, 5202);
        Send(stockList, LB_SETCURSEL, 0);
        Type(GetDlgItem(palette, 5224), L"#A1");
        Send(palette, WM_COMMAND, 5254);
        Require(WindowText(GetDlgItem(palette, 5224)) == L"#A1", "stock loading cannot discard incomplete color input");
        Type(GetDlgItem(palette, 5224), L"#000000");
        Send(palette, WM_COMMAND, 5254);
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Require(WindowText(GetDlgItem(palette, 5224)) == L"#556677" && paletteBytes.find("#556677") != std::string::npos,
                "stock color immediately saves the selected palette slot");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }) && Equals(palettePath, paletteBytes), "stock usage survives Close");

        Require(PostMessageW(main, WM_COMMAND, ID_TOOL_COLOR_CUSTOM, 0), "annotation entry shares saved color stock");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        stockList = GetDlgItem(palette, 5252);
        Require(Send(stockList, LB_GETCOUNT) == 2, "color stock survives Exit and is shared by palette entry points");
        Send(stockList, LB_SETCURSEL, 0);
        Send(palette, WM_COMMAND, 5255);
        Require(Send(stockList, LB_GETCOUNT) == 1 && ReadFileBytesWin32(stockPath, stockBytes) &&
                stockBytes.find("#556677") == std::string::npos && Equals(palettePath, paletteBytes),
                "stock removal leaves palette slots unchanged");
        HANDLE lockedStock = CreateFileW(stockPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedStock != INVALID_HANDLE_VALUE, "lock UI stock against replacement");
        Type(GetDlgItem(palette, 5224), L"#ABCDEF");
        Send(palette, WM_COMMAND, 5253);
        CloseHandle(lockedStock);
        Require(Send(stockList, LB_GETCOUNT) == 1 && Equals(stockPath, stockBytes) &&
                WindowText(GetDlgItem(palette, 5224)) == L"#ABCDEF", "stock write failure retains saved list and editable draft");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "close stock failure editor");
        Require(ReadFileBytesWin32(palettePath, paletteBytes), "capture auto-saved palette independent of stock failure");
        Require(atomic_write::AtomicWriteUtf8(stockPath, "broken", stockPath.parent_path(), &error), "prepare corrupt UI stock fixture");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open palette with corrupt color stock");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        Require(!IsWindowEnabled(GetDlgItem(palette, 5253)) && Equals(stockPath, "broken"),
                "corrupt color stock disables mutation without overwriting the file");
        Require(atomic_write::AtomicWriteUtf8(stockPath, stockBytes, stockPath.parent_path(), &error), "restore owned UI stock fixture");
        Send(palette, WM_COMMAND, 5256);
        Require(IsWindowEnabled(GetDlgItem(palette, 5253)) && Send(GetDlgItem(palette, 5252), LB_GETCOUNT) == 1,
                "explicit stock reload recovers without recreating the dialog");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "close color stock regression");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open palette write failure regression");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        HANDLE lockedPalette = CreateFileW(palettePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lockedPalette != INVALID_HANDLE_VALUE, "lock only owned palette against replacement");
        Type(GetDlgItem(palette, 5224), L"#ABCDEF");
        Send(settings, WM_COMMAND, IDOK);
        const bool paletteHeldOpen = IsWindow(settings);
        CloseHandle(lockedPalette);
        Require(paletteHeldOpen && Equals(palettePath, paletteBytes) &&
                WindowText(GetDlgItem(palette, 5224)) == L"#ABCDEF",
                "palette write failure preserves the file and editable color draft");
        Send(palette, WM_COMMAND, 5282);
        WaitAutoPalette(palette, palettePath, paletteBytes);
        Require(paletteBytes.find("#ABCDEF") != std::string::npos, "failed auto-save retries without losing the edited color");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "close after retrying failed palette save");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "reopen palette after failed save exit");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        Require(WindowText(GetDlgItem(palette, 5224)) == L"#ABCDEF", "retried palette save survives reopening");
        palette_active::Snapshot competing;
        Require(palette_active::Load(workspace, competing), "capture external active-palette update");
        auto competingColors = competing.colors; competingColors[0] = RGB(152,118,84);
        Require(palette_active::Save(competing, competingColors), "save competing palette update outside the dialog");
        Type(GetDlgItem(palette, 5224), L"#654321");
        Send(palette, WM_TIMER, 1);
        Require(Equals(palettePath, competing.bytes) && WindowText(GetDlgItem(palette, 5224)) == L"#654321",
            "stale UI auto-save preserves competing bytes and edited color");
        Require(PostMessageW(palette, WM_COMMAND, 5283, 0), "reload competing palette asynchronously");
        reloadConfirm = WaitWindow(L"SilentDialogClass"); Send(reloadConfirm, WM_COMMAND, 5102);
        Require(Until([&] { return !IsWindow(reloadConfirm) && WindowText(GetDlgItem(palette, 5224)) == L"#987654"; }),
            "explicit reload accepts competing palette without overwriting it");
        WaitAutoPalette(palette, palettePath, paletteBytes);
        const auto relatedConfigPath = workspace / L"workspace.json";
        std::string relatedConfig;
        Require(ReadFileBytesWin32(relatedConfigPath, relatedConfig), "capture related config before partial save failure");
        HANDLE relatedLock = CreateFileW(relatedConfigPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(relatedLock != INVALID_HANDLE_VALUE, "lock related palette config only");
        Type(GetDlgItem(palette, 5224), L"#456789");
        Send(palette, WM_TIMER, 1);
        Require(ReadFileBytesWin32(palettePath, paletteBytes) && paletteBytes.find("#456789") != std::string::npos &&
            Equals(relatedConfigPath, relatedConfig) && WindowText(GetDlgItem(palette, 5280)).find(L"関連設定") != std::wstring::npos,
            "palette file success and related config failure are reported separately");
        CloseHandle(relatedLock);
        const auto alreadySavedPalette = paletteBytes;
        Send(palette, WM_COMMAND, 5282); WaitAutoPalette(palette, palettePath, paletteBytes);
        Require(paletteBytes == alreadySavedPalette, "runtime retry does not rewrite the already saved palette");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "close unchanged palette");
        Require(atomic_write::AtomicWriteUtf8(palettePath, "broken", palettePath.parent_path(), &error), "prepare owned corrupt palette for UI safety");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open corrupt active palette");
        settings = WaitWindow(L"UnifiedSettingsShellClass"); palette = Descendant(settings, L"PaletteSettingsDlgClass");
        Type(GetDlgItem(palette, 5224), L"#050607"); Send(palette, WM_TIMER, 1);
        Require(Equals(palettePath, "broken"), "corrupt UI palette cannot be silently replaced by edited defaults");
        Require(PostMessageW(settings, WM_COMMAND, IDCANCEL, 0), "request Close with unsaved color");
        HWND closeConfirm = WaitWindow(L"SilentDialogClass"); Send(closeConfirm, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(closeConfirm); }) && IsWindow(settings) &&
            WindowText(GetDlgItem(palette, 5224)) == L"#050607", "default Close confirmation retains unsaved edit");
        Require(PostMessageW(settings, WM_COMMAND, IDCANCEL, 0), "explicitly discard unsaved color on Close");
        closeConfirm = WaitWindow(L"SilentDialogClass"); Send(closeConfirm, WM_COMMAND, 5102);
        Require(Until([&] { return !IsWindow(settings); }) && Equals(palettePath, "broken"),
            "explicit discard closes without overwriting corrupt original");
        Require(atomic_write::AtomicWriteUtf8(palettePath, paletteBytes, palettePath.parent_path(), &error), "restore owned palette bytes after corrupt UI test");

        // Hold only the disposable config against replacement, as a real write
        // sharing failure. The editor must stay open and keep the original.
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "open save failure regression");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        Send(settings, WM_APP + 140, settings_navigation::Files);
        CheckSettingsSidebar(settings, settings_navigation::Files);
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
        Require(PostMessageW(settings, WM_COMMAND, IDCANCEL, 0), "request closing after failed save");
        HWND discardSettings = WaitWindow(L"SilentDialogClass");
        Require(GetWindow(discardSettings, GW_OWNER) == settings, "discard choice belongs to the failed settings editor");
        Send(discardSettings, WM_COMMAND, 5103); // Explicit Discard; default remains Continue editing.
        Require(Until([&] { return !IsWindow(settings); }) && Equals(configPath, config),
                "exit after failed save discards only the unsaved draft");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "reopen after cancelled failed save");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        Send(settings, WM_APP + 140, settings_navigation::Files);
        CheckSettingsSidebar(settings, settings_navigation::Files);
        Require(Send(DescendantId(settings, 4214), CB_GETCURSEL) == 0,
                "unsaved layout is not left in runtime for a later auto-save");
        Send(DescendantId(settings, 4214), CB_SETCURSEL, 1);
        Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }) && ReadFileBytesWin32(configPath, config) &&
                config.find("\"sessionFileLayout\": \"session_root\"") != std::string::npos,
                "retry after write failure persists the formerly rejected choice");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "restore the separate-layout fixture");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        Send(settings, WM_APP + 140, settings_navigation::Files);
        CheckSettingsSidebar(settings, settings_navigation::Files);
        Send(DescendantId(settings, 4214), CB_SETCURSEL, 0); Send(settings, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(settings); }) && ReadFileBytesWin32(configPath, config) &&
                config.find("\"sessionFileLayout\": \"separate_directories\"") != std::string::npos,
                "restore the original layout before independent organization checks");

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
        Send(settings, WM_APP + 140, settings_navigation::Files);
        CheckSettingsSidebar(settings, settings_navigation::Files);
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
        const auto clientControlRect = [&](int id) {
            RECT bounds{}; Require(GetWindowRect(GetDlgItem(presets, id), &bounds), "measure preset/restore action geometry");
            MapWindowPoints(nullptr, presets, reinterpret_cast<POINT*>(&bounds), 2);
            return bounds;
        };
        const auto noScroll = [&] {
            SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
            Require(GetScrollInfo(presets, SB_VERT, &info) && info.nPos == 0 && info.nMax < static_cast<int>(info.nPage) &&
                    !(GetWindowLongPtrW(presets, GWL_STYLE) & WS_VSCROLL),
                    "fitting active tab has no scrollbar or scrollable range");
            const auto before = clientControlRect(30);
            Send(presets, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA));
            Send(presets, WM_VSCROLL, SB_BOTTOM);
            const auto after = clientControlRect(30);
            Require(EqualRect(&before, &after), "wheel and scrollbar commands cannot move fitting content");
        };
        noScroll(); // The longer hidden Restore tab must not enlarge Presets.
        Send(presets, WM_COMMAND, 7102);
        SCROLLINFO restoreScroll{}; restoreScroll.cbSize = sizeof(restoreScroll); restoreScroll.fMask = SIF_ALL;
        Require(GetScrollInfo(presets, SB_VERT, &restoreScroll) && restoreScroll.nMax >= static_cast<int>(restoreScroll.nPage),
                "same viewport needs scrolling only for the longer active Restore tab");
        Send(presets, WM_VSCROLL, SB_BOTTOM);
        Send(presets, WM_COMMAND, 7101);
        noScroll(); checkPresetPage(); // Tab change discards the former tab's offset/range.
        // Exercise the application's DPI handler without changing the user's OS
        // scale. Larger margins/actions plus a constrained viewport require scroll.
        Send(presets, WM_DPICHANGED, MAKEWPARAM(192, 192), reinterpret_cast<LPARAM>(&presetBounds));
        Require(SetWindowPos(presets, nullptr, 0, 0, 760, 460, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                "constrain high-DPI preset dialog to a small display");
        SCROLLINFO presetScroll{}; presetScroll.cbSize = sizeof(presetScroll); presetScroll.fMask = SIF_ALL;
        Require(GetScrollInfo(presets, SB_VERT, &presetScroll) && presetScroll.nMax >= static_cast<int>(presetScroll.nPage),
                "long preset/restore content remains accessible by scrolling");
        const auto beforeWheel = clientControlRect(10);
        Send(presets, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA));
        const auto afterWheel = clientControlRect(10);
        Require(afterWheel.top < beforeWheel.top && afterWheel.right - afterWheel.left == beforeWheel.right - beforeWheel.left &&
                afterWheel.bottom - afterWheel.top == beforeWheel.bottom - beforeWheel.top,
                "overflow scroll moves existing controls without resizing/reflowing them");
        Send(presets, WM_VSCROLL, SB_BOTTOM);
        RECT closeBounds{}, presetClient{};
        Require(GetWindowRect(GetDlgItem(presets, 30), &closeBounds) && GetClientRect(presets, &presetClient),
                "measure scrolled exit action");
        MapWindowPoints(nullptr, presets, reinterpret_cast<POINT*>(&closeBounds), 2);
        Require(closeBounds.top >= 0 && closeBounds.bottom <= presetClient.bottom, "scroll reveals the complete exit action");
        Send(presets, WM_COMMAND, 7102);
        Require(GetScrollInfo(presets, SB_VERT, &presetScroll) && presetScroll.nPos == 0 && IsWindowVisible(GetDlgItem(presets, 20)),
                "constrained Restore tab opens at the top, retaining its restore actions");
        Send(presets, WM_VSCROLL, SB_BOTTOM);
        Send(presets, WM_COMMAND, 7101);
        Require(GetScrollInfo(presets, SB_VERT, &presetScroll) && presetScroll.nPos == 0,
                "constrained Presets tab also opens at the top after a scrolled Restore tab");
        Send(presets, WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&presetBounds));
        Require(SetWindowPos(presets, nullptr, 0, 0, presetBounds.right - presetBounds.left,
            900, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "give both tabs sufficient room after constrained layout");
        Send(presets, WM_VSCROLL, SB_TOP);
        Send(presets, WM_COMMAND, 7102);
        Require(!IsWindowVisible(GetDlgItem(presets, 10)) && IsWindowVisible(GetDlgItem(presets, 20)),
                "restore tab shows restore actions without closing the modal dialog");
        checkPresetPage();
        noScroll();
        Send(presets, WM_THEMECHANGED);
        Send(presets, WM_COMMAND, 7101);
        Require(IsWindowVisible(GetDlgItem(presets, 10)) && !IsWindowVisible(GetDlgItem(presets, 20)),
                "preset tab restores save and load actions");
        noScroll();
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
        SelectAssetRow(list, L"テーマフォルダー (__theme__)");
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
        SelectAssetRow(list, L"テーマ: theme_cooldown_test.json");
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
        SelectAssetRow(list, L"テーマ: theme_cooldown_test.json");
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
        SelectAssetRow(list, L"テーマ: theme_cooldown_test.json");
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open repeated missing-theme details");
        missingDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(missingDetails, L"なし", L"前回からの変化: 変化なし");
        Send(missingDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(missingDetails); }), "close repeated missing-theme details");
        Require(atomic_write::AtomicWriteUtf8(newTheme, "{}", themes, &error), "restore only the owned missing theme");
        Require(Until([&] { return IsWindowEnabled(GetDlgItem(assets, 7131)); }, 12000), "recovered-theme check becomes available");
        Send(assets, WM_COMMAND, 7131);
        Require(Send(list, LVM_GETITEMCOUNT) == initialRows + 1, "restored theme does not create a duplicate row");
        SelectAssetRow(list, L"テーマ: theme_cooldown_test.json");
        Require(PostMessageW(assets, WM_COMMAND, 7136, 0), "open recovered theme details");
        HWND recoveredDetails = WaitWindow(L"SilentDialogClass");
        CheckDetails(recoveredDetails, L"あり", L"前回からの変化: なし → あり");
        Send(recoveredDetails, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(recoveredDetails); }), "close recovered theme details");
        SelectAssetRow(list, L"メモ (workspace_memo.txt)");
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
        // App exit retries the dirty memo save and presents its own modal failure.
        // Post the close request so the driver can dismiss that dialog.
        Require(PostMessageW(main, WM_CLOSE, 0, 0), "actual failed-save app exit request");
        notice = WaitWindow(L"SilentDialogClass");
        Send(notice, WM_COMMAND, IDOK);
        Require(Until([&] { return !IsWindow(notice); }), "close actual failed-save exit notice");
        Require(IsWindow(main) && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT &&
                Equals(original, "nested settings shortcut"), "actual save failure blocks app exit and preserves original");
        Require(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_NORMAL), "restore actual fixture attributes");
        Type(editor, L"actual application exit");
        // Commit a complete preset only after the earlier cancellation/failure checks.
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "open palette for persistence across app restart");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        SelectPaletteSet(palette, 5);
        Send(palette, WM_COMMAND, 5200);
        ConfirmPaletteAction(palette, 5264, true, [&] {
            return WindowText(GetDlgItem(palette, 5224)) == PresetColorText((*expectedUserSets.users[4])[0]);
        });
        std::vector<std::string> previousTokens;
        Require(SavedPaletteTokens(paletteBytes, previousTokens), "capture legacy palette slots before committing a preset");
        const auto expectedAppliedPalette = *expectedUserSets.users[4];
        std::vector<std::string> appliedColors;
        Require(ReadFileBytesWin32(palettePath, paletteBytes) && SavedPaletteTokens(paletteBytes, appliedColors),
            "read the palette saved after preset application");
        Require(appliedColors.size() >= expectedAppliedPalette.size(), "committed palette contains all eight editable slots");
        for (size_t slot = 0; slot < expectedAppliedPalette.size(); ++slot) {
            const auto expected = PresetColorText(expectedAppliedPalette[slot]);
            Require(appliedColors[slot] == std::string(expected.begin(), expected.end()),
                "automatic save persists all eight colors of the applied preset");
        }
        Require(previousTokens.size() == appliedColors.size() && std::equal(previousTokens.begin() + 8, previousTokens.end(),
            appliedColors.begin() + 8), "applying a preset preserves every hidden legacy palette slot");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "close committed palette before restart");
        Require(PostMessageW(main, WM_CLOSE, 0, 0), "normal app exit request");
        // Earlier failed theme writes leave a retry copy in memory. Confirm its
        // explicitly announced disposal on exit; the saved memo and palette remain.
        HWND exitConfirm = WaitWindow(L"SilentDialogClass");
        Require(WindowText(exitConfirm) == L"保存できなかった内容" &&
                WindowText(GetDlgItem(exitConfirm, 5101)).find(L"再試行用のコピー") != std::wstring::npos,
            "exit warns about retained failed-write content");
        Send(exitConfirm, WM_COMMAND, 5102);
        Require(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "actual app exit completes");
        Require(Equals(original, "actual application exit"), "normal app exit saves memo without separate close");
        DWORD code = 1; Require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "normal app exit code");
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        process = {};
        command = L"\"" + executable.wstring() + L"\"";
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            executable.parent_path().c_str(), &startup, &process), "restart isolated app with its original valid workspace setup");
        childPid = process.dwProcessId; main = WaitWindow(L"PdfWorkspaceMainWnd");
        Require(Until([&] { return IsWindowEnabled(main) && IsWindowVisible(main); }), "restarted application ready");
        Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_PALETTE, 0), "inspect palette after actual app restart");
        settings = WaitWindow(L"UnifiedSettingsShellClass");
        palette = Descendant(settings, L"PaletteSettingsDlgClass");
        for (size_t slot = 0; slot < expectedAppliedPalette.size(); ++slot) {
            Send(palette, WM_COMMAND, 5200 + slot);
            Require(WindowText(GetDlgItem(palette, 5224)) == PresetColorText(expectedAppliedPalette[slot]),
                "applied preset colors survive actual process restart in all eight slots");
        }
        palette_presets::Snapshot reloadedSets;
        Require(palette_presets::Load(workspace, reloadedSets) && reloadedSets.colors.users == expectedUserSets.users &&
            Equals(setsPath, setsBytes) && Equals(palettePath, paletteBytes), "all five user presets and committed palette survive restart unchanged");
        Send(settings, WM_COMMAND, IDCANCEL);
        Require(Until([&] { return !IsWindow(settings); }), "close restarted palette checks");
        Require(PostMessageW(main, WM_CLOSE, 0, 0) && WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "close exact restarted palette test process");
        Require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "palette restart test app exit code");
        CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
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
