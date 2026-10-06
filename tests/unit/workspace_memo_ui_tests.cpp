// Real Win32 memo controls and message routing; host services are inert stubs.
// The full app build separately verifies search/settings/menu integration.
#include "core/app_core.h"
#include "core/localization.h"
#include "core/atomic_write.h"
#include "workspace/workspace_memo.h"
#include "workspace/workspace_memo_store.h"
#include <iostream>
#include <stdexcept>

HINSTANCE g_hInst = nullptr;
HWND g_hMainWnd = nullptr;
HBRUSH g_hThemeWindowBrush = nullptr;
std::wstring g_workspaceRoot;
static int notices = 0, checks = 0;
void SetUIFont(HWND hwnd) { SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE); }
void ApplyThemeToDialog(HWND) {}
LRESULT ThemeCtlColorPanel(HWND, HDC) { return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH)); }
void ShowSilentMessageDialog(HWND, const std::wstring&, const std::wstring&, SoftNoticeKind, const std::vector<SilentDialogPath>&) { ++notices; }
namespace localization { std::wstring Text(std::wstring_view id) { return std::wstring(id); } }

static void Require(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
static HWND Memo() {
    HWND candidate = nullptr;
    while ((candidate = FindWindowExW(nullptr, candidate, L"PdfNoteWorkspaceMemoWnd", nullptr))) {
        DWORD pid = 0; GetWindowThreadProcessId(candidate, &pid);
        if (pid == GetCurrentProcessId() && GetWindow(candidate, GW_OWNER) == g_hMainWnd) return candidate;
    }
    return nullptr;
}
static std::string Read(const std::filesystem::path& path) {
    std::string value; Require(ReadFileBytesWin32(path, value), "read original"); return value;
}
static void Type(HWND edit, const wchar_t* value) {
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(value));
}
static std::wstring Text(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    Require(GetWindowTextW(edit, text.data(), length + 1) == length, "capture test editor");
    text.resize(static_cast<size_t>(length)); return text;
}
static bool FailCleanup(workspace_memo::testing::FaultPoint point) {
    return point == workspace_memo::testing::FaultPoint::CleanupDelete ||
           point == workspace_memo::testing::FaultPoint::CleanupWrite;
}
static void TestLimits(const std::filesystem::path& fixtures) {
    const auto root = fixtures / L"ui_limits";
    const auto path = WorkspaceMemoPath(root);
    std::wstring error;
    const std::string lf(600000, '\n');
    Require(atomic_write::AtomicWriteUtf8(path, lf, path.parent_path(), &error), "large LF original");
    g_workspaceRoot = root.wstring(); ShowWorkspaceMemoWindow(g_hMainWnd);
    HWND memo = Memo(), edit = GetDlgItem(memo, 1);
    Require(Text(edit).size() == 1200000, "large LF view is complete");
    const std::wstring oversized(workspace_memo::kMaxBytes + 10, L'a');
    Type(edit, oversized.c_str());
    Require(Text(edit).size() == 1200000, "oversize replacement rejected as a whole");
    SendMessageW(memo, WM_CLOSE, 0, 0);
    Require(!IsWindow(memo) && Read(path) == lf, "large LF unchanged close succeeds without rewriting");

    ShowWorkspaceMemoWindow(g_hMainWnd); memo = Memo(); edit = GetDlgItem(memo, 1);
    const std::wstring japanese(workspace_memo::kMaxBytes / 3, L'検');
    Type(edit, japanese.c_str());
    SendMessageW(edit, EM_SETSEL, -1, -1);
    SendMessageW(edit, WM_CHAR, L'a', 0); // exactly 1 MiB in UTF-8
    Require(Text(edit) == japanese + L'a', "Japanese byte boundary accepts last ASCII byte");
    SendMessageW(edit, WM_CHAR, L'b', 0);
    Require(Text(edit) == japanese + L'a', "typing byte overflow rejected");
    SendMessageW(edit, WM_CHAR, 0xD83D, 0);
    SendMessageW(edit, WM_CHAR, 0xDE00, 0);
    Require(Text(edit) == japanese + L'a', "emoji byte overflow rejected without partial surrogate");
    workspace_memo::Document restarted;
    Require(restarted.Load(root) == workspace_memo::Result::Recovered && restarted.text() == japanese + L'a', "last accepted boundary text fully checkpointed");
    SendMessageW(memo, WM_CLOSE, 0, 0);
    Require(!IsWindow(memo) && Read(path).size() == workspace_memo::kMaxBytes, "boundary close saves");

    ShowWorkspaceMemoWindow(g_hMainWnd); memo = Memo(); edit = GetDlgItem(memo, 1);
    Type(edit, L"emoji: "); SendMessageW(edit, EM_SETSEL, -1, -1);
    SendMessageW(edit, WM_CHAR, 0xD83D, 0);
    Require(Text(edit) == L"emoji: ", "half surrogate never inserted");
    SendMessageW(edit, WM_CHAR, 0xDE00, 0);
    Require(Text(edit) == L"emoji: 😀", "surrogate pair inserted atomically");
    const std::wstring beyondNative(workspace_memo::kMaxEditorUnits + 100, L'z');
    Type(edit, beyondNative.c_str());
    Require(Text(edit) == L"emoji: 😀", "native-limit oversize never truncated into control");
    Require(SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(oversized.c_str())) == 0 &&
            Text(edit) == L"emoji: 😀", "programmatic oversize rejected");
    Type(edit, L"undo base"); Type(edit, L"undo changed");
    SendMessageW(edit, WM_UNDO, 0, 0);
    Require(Text(edit) == L"undo base", "memo undo is local");
    SendMessageW(edit, WM_UNDO, 0, 0);
    Require(Text(edit) == L"undo changed", "native memo redo is local");
    SendMessageW(memo, WM_CLOSE, 0, 0);
    Require(Read(path) == "undo changed", "redo content saved");
    ShowWorkspaceMemoWindow(g_hMainWnd); memo = Memo(); edit = GetDlgItem(memo, 1);
    Type(edit, L"LF\nCR\rmixed\r\nending");
    Require(Text(edit) == L"LF\r\nCR\r\nmixed\r\nending", "replacement/paste path normalizes all line endings for EDIT");
    SendMessageW(memo, WM_CLOSE, 0, 0);
    Require(Read(path) == "LF\nCR\nmixed\nending", "edited memo stores canonical LF");
}
static void TestCleanupFailure(const std::filesystem::path& fixtures) {
    const auto root = fixtures / L"ui_cleanup";
    workspace_memo::Document seed;
    Require(seed.Load(root) == workspace_memo::Result::Ok && seed.Save(L"base") == workspace_memo::Result::Ok, "cleanup UI fixture");
    g_workspaceRoot = root.wstring(); ShowWorkspaceMemoWindow(g_hMainWnd);
    HWND memo = Memo(), edit = GetDlgItem(memo, 1);
    Type(edit, L"base");
    const auto path = WorkspaceMemoPath(root);
    std::wstring error;
    Require(atomic_write::AtomicWriteUtf8(path, "external", path.parent_path(), &error), "cleanup UI external edit");
    workspace_memo::testing::SetFaultHook(FailCleanup);
    SendMessageW(memo, WM_CLOSE, 0, 0);
    Require(IsWindow(memo) && Text(edit) == L"base", "failed cleanup cancels close without false adoption");
    Require(!SaveWorkspaceMemoForExit() && !PrepareWorkspaceMemoRootChange(), "failed cleanup blocks exit and switch");
    workspace_memo::testing::SetFaultHook(nullptr);
    SendMessageW(memo, WM_CLOSE, 0, 0);
    Require(!IsWindow(memo) && Read(path) == "external", "cleanup retry closes preserving external original");
    workspace_memo::Document restarted;
    Require(restarted.Load(root) == workspace_memo::Result::Ok && restarted.text() == L"external", "cleanup UI reopen never revives old text");
}
int main() {
    try {
        g_hInst = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls), "initialize controls");
        std::error_code ec;
        const auto current = std::filesystem::current_path(ec);
        Require(!ec, "fixture current directory");
        const auto fixtures = current / L"out" / L"tests" /
            (L"workspace_memo_ui_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(fixtures, ec);
        Require(!ec, "fixture directory");
        g_workspaceRoot = fixtures.wstring();
        g_hMainWnd = CreateWindowW(L"STATIC", L"memo fixture host", WS_OVERLAPPEDWINDOW,
            0, 0, 700, 600, nullptr, nullptr, g_hInst, nullptr);
        HWND search = CreateWindowW(L"STATIC", L"search fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, g_hMainWnd, nullptr, g_hInst, nullptr);
        EnableWindow(g_hMainWnd, FALSE); // settings' nested modal host
        ShowWorkspaceMemoWindow(search);
        HWND memo = Memo(); HWND edit = GetDlgItem(memo, 1);
        Require(memo && edit && GetWindow(memo, GW_OWNER) == g_hMainWnd, "owner is main, not entry dialog");
        DestroyWindow(search);
        EnableWindow(g_hMainWnd, TRUE);
        Require(IsWindow(memo), "closing entry dialog keeps memo alive");
        ShowWorkspaceMemoWindow(g_hMainWnd);
        Require(Memo() == memo, "all entries focus same singleton");
        const auto original = WorkspaceMemoPath(fixtures);
        Type(edit, L"typed search term");
        Require(!PathExistsWin32(original), "typing only checkpoints");
        BYTE keys[256]{}; Require(GetKeyboardState(keys), "keyboard state");
        BYTE savedKeys[256]{}; std::copy(std::begin(keys), std::end(keys), std::begin(savedKeys));
        keys[VK_CONTROL] = 0x80; Require(SetKeyboardState(keys), "set thread-local Ctrl");
        MSG save{}; save.hwnd = edit; save.message = WM_KEYDOWN; save.wParam = L'S';
        Require(HandleWorkspaceMemoMessage(save), "input Ctrl+S consumed before main accelerator");
        Require(SetKeyboardState(savedKeys), "restore keyboard state");
        Require(Read(original) == "typed search term", "Ctrl+S saved current input");
        Type(edit, L"nested loop save");
        Require(SetKeyboardState(keys), "nested thread-local Ctrl");
        SendMessageW(edit, WM_KEYDOWN, L'S', 0);
        SendMessageW(edit, WM_CHAR, 0x13, 0);
        Require(SetKeyboardState(savedKeys), "restore nested keyboard state");
        Require(Read(original) == "nested loop save", "edit subclass saves in nested modal loop without Ctrl+S character");
        Require(notices == 0, "successful save needs no confirmation");
        Type(edit, L"close automatically");
        SendMessageW(memo, WM_CLOSE, 0, 0);
        Require(!IsWindow(memo) && Read(original) == "close automatically", "window close automatically saves");
        ShowWorkspaceMemoWindow(g_hMainWnd);
        memo = Memo(); edit = GetDlgItem(memo, 1);
        Type(edit, L"application exit");
        Require(SaveWorkspaceMemoForExit() && Read(original) == "application exit", "application-exit boundary saves");
        Require(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_READONLY), "readonly fixture");
        Type(edit, L"must retain on failure");
        SendMessageW(memo, WM_CLOSE, 0, 0);
        Require(IsWindow(memo) && Read(original) == "application exit", "save failure cancels close");
        Require(!SaveWorkspaceMemoForExit(), "save failure cancels app exit");
        Require(!PrepareWorkspaceMemoRootChange(), "save failure cancels root change");
        const int before = notices;
        Require(!SaveWorkspaceMemoForExit(false) && notices == before, "OS shutdown never opens modal notice");
        Require(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_NORMAL), "restore readonly fixture");
        Require(PrepareWorkspaceMemoRootChange(), "root-switch save retry");
        ResetWorkspaceMemoForRootChange();
        Require(!IsWindow(memo) && Read(original) == "must retain on failure", "reset only after old-root save");
        const auto next = fixtures / L"next"; std::filesystem::create_directories(next, ec);
        Require(!ec, "next workspace directory");
        g_workspaceRoot = next.wstring(); ShowWorkspaceMemoWindow(g_hMainWnd);
        memo = Memo(); edit = GetDlgItem(memo, 1);
        Require(GetWindowTextLengthW(edit) == 0, "new workspace independent");
        Type(edit, L"new root memo"); SendMessageW(memo, WM_CLOSE, 0, 0);
        Require(Read(WorkspaceMemoPath(next)) == "new root memo" && Read(original) == "must retain on failure", "no old-root content in new original");
        workspace_memo::Document seeded;
        Require(seeded.Load(next) == workspace_memo::Result::Ok &&
                seeded.Checkpoint(L"restored from prior run") == workspace_memo::Result::Ok, "seed interrupted-run recovery");
        ShowWorkspaceMemoWindow(g_hMainWnd); memo = Memo(); edit = GetDlgItem(memo, 1);
        std::wstring restored(64, L'\0');
        const int restoredLength = GetWindowTextW(edit, restored.data(), static_cast<int>(restored.size()));
        restored.resize(static_cast<size_t>(restoredLength));
        Require(restored == L"restored from prior run", "control initialization preserves recovered text");
        Require(Read(WorkspaceMemoPath(next)) == "new root memo", "opening recovery does not update original");
        SendMessageW(memo, WM_CLOSE, 0, 0);
        Require(!IsWindow(memo), "recovered draft closes successfully");
        TestLimits(fixtures);
        TestCleanupFailure(fixtures);
        DestroyWindow(g_hMainWnd); g_hMainWnd = nullptr;
        std::cout << "Workspace memo Win32 UI: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
