// Actual browser controls/message loop; host services are inert test stubs.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <optional>
#include "core/path_safety.h"
#include "core/localization.h"
#include "core/ui_notify.h"
#include "core/ui_prompts.h"
#include "ui/noop_nav_guard.h"

HINSTANCE g_hInst = GetModuleHandleW(nullptr);
HBRUSH g_hThemeWindowBrush = nullptr;
struct BrowserConfig { bool useNativeFileDialogs = false; } g_config;
std::wstring g_workspaceRoot;
static int notices = 0;
static int configSaves = 0;
static std::wstring copiedPath;
static bool CopyPathToClipboard(HWND, const std::wstring& path) { copiedPath = path; return true; }
void ShowSoftNotice(HWND, const std::wstring&, SoftNoticeKind) { ++notices; }
void SaveWorkspaceConfig(const std::wstring&, const BrowserConfig&) { ++configSaves; }
void ApplyThemeToDialog(HWND) {}
LRESULT ThemeCtlColorPanel(HWND, HDC) { return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW)); }
void PlaceOwnedPopupAtAppTopLeft(HWND, HWND) {}
bool ShouldSkipImeMessageInLoop(const MSG&) { return false; }
HWND MainDialogOwner(HWND owner) { return owner; }
std::wstring ToLowerAscii(std::wstring text) {
    for (auto& ch : text) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    return text;
}
std::wstring TrimWhitespace(const std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}
std::filesystem::path ResolveDialogInitialFolder(const std::filesystem::path& path) { return path; }
namespace localization {
std::wstring Text(std::wstring_view id) { return std::wstring(id); }
std::wstring Format(std::wstring_view id, const std::vector<std::pair<std::wstring_view, std::wstring>>&) {
    return std::wstring(id);
}
}
#include "ui/lists/main_local_path_browser.cppinc"

static int scenario = 0;
static bool operated = false;
static std::filesystem::path destination;
// Thread timer; the returned ID is owned/killed by each individual prompt test.
static constexpr UINT_PTR kBrowserAutomationTimer = 4901;
static std::wstring ReadText(HWND control) {
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const int copied = GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(static_cast<size_t>(copied));
    return value;
}
static void SelectEntry(LocalPathBrowserState* state, const std::filesystem::path& path, bool open,
                        bool suggestion = false) {
    HWND list = suggestion ? state->suggestionList : state->list;
    const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    for (int item = 0; item < count; ++item) {
        const LRESULT data = SendMessageW(list, LB_GETITEMDATA, item, 0);
        assert(data >= 0 && static_cast<size_t>(data) < state->entries.size());
        if (state->entries[static_cast<size_t>(data)].path == path) {
            if (state->allowMultiple) SendMessageW(list, LB_SETSEL, TRUE, item);
            else SendMessageW(list, LB_SETCURSEL, item, 0);
            SendMessageW(state->hwnd, WM_COMMAND, MAKEWPARAM(suggestion ? 104 : 102, LBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(list));
            if (open) SendMessageW(state->hwnd, WM_COMMAND, MAKEWPARAM(suggestion ? 104 : 102, LBN_DBLCLK),
                                   reinterpret_cast<LPARAM>(list));
            return;
        }
    }
    assert(false && "required entry missing");
}

static void CheckBrowserLists(LocalPathBrowserState* state) {
    assert(IsWindow(state->pathTooltip));
    assert(state->pathTooltipText == (state->showDriveRoots ? L"" : state->currentDir.wstring()));
    if (!state->showDriveRoots) {
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = state->hwnd;
        tool.uId = reinterpret_cast<UINT_PTR>(state->edit);
        std::wstring tooltipText(std::max<size_t>(80, state->currentDir.wstring().size() + 1), L'\0');
        tool.lpszText = tooltipText.data();
        const LRESULT foundTool = SendMessageW(state->pathTooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool));
        if (!foundTool) std::cerr << "Tooltip count: " << SendMessageW(state->pathTooltip, TTM_GETTOOLCOUNT, 0, 0) << "\n";
        assert(foundTool);
        tool.lpszText = tooltipText.data();
        SendMessageW(state->pathTooltip, TTM_GETTEXTW, tooltipText.size(), reinterpret_cast<LPARAM>(&tool));
        if (std::wstring(tooltipText.c_str()) != state->currentDir.wstring())
            std::cerr << "Tooltip length: " << wcslen(tooltipText.c_str()) << " expected: " << state->currentDir.wstring().size() << "\n";
        assert(std::wstring(tooltipText.c_str()) == state->currentDir.wstring());
        SendMessageW(state->hwnd, WM_COMMAND, 206, 0);
        assert(copiedPath == state->currentDir.wstring());
    }
    assert(static_cast<size_t>(SendMessageW(state->list, LB_GETCOUNT, 0, 0)) == state->entries.size());
    assert(static_cast<size_t>(SendMessageW(state->suggestionList, LB_GETCOUNT, 0, 0)) == state->suggestedEntries.size());
    const bool suggestions = !state->suggestedEntries.empty();
    for (HWND control : {state->suggestionLabel, state->suggestionList, state->listDivider, state->allEntriesLabel}) {
        assert(IsWindowVisible(control) == suggestions);
    }
    bool sawFile = false;
    for (size_t index = 0; index < state->entries.size(); ++index) {
        const auto& entry = state->entries[index];
        assert(SendMessageW(state->list, LB_GETITEMDATA, index, 0) == static_cast<LRESULT>(index));
        if (IsBrowserDirectoryEntry(entry)) assert(!sawFile);
        else sawFile = true;
        if (index > 0 && entry.kind == state->entries[index - 1].kind) {
            assert(ToLowerAscii(state->entries[index - 1].path.filename().wstring()) <=
                   ToLowerAscii(entry.path.filename().wstring()));
        }
    }
    for (size_t item = 0; item < state->suggestedEntries.size(); ++item) {
        const size_t index = state->suggestedEntries[item];
        assert(SendMessageW(state->suggestionList, LB_GETITEMDATA, item, 0) == static_cast<LRESULT>(index));
        const auto& entry = state->entries[index];
        assert(entry.highlighted || LocalBrowserDirectoryRelevance(entry) > 0);
        // The same path is still present in the normal list at its name-order position.
        assert(state->entries[static_cast<size_t>(SendMessageW(state->list, LB_GETITEMDATA, index, 0))].path == entry.path);
    }
    if (suggestions) {
        RECT top{}, divider{}, bottom{};
        GetWindowRect(state->suggestionList, &top);
        GetWindowRect(state->listDivider, &divider);
        GetWindowRect(state->list, &bottom);
        assert(top.bottom < divider.top && divider.bottom < bottom.top);
        assert(GetNextDlgTabItem(state->hwnd, state->suggestionList, FALSE) == state->list);
    }
    if (!state->showDriveRoots) {
        const std::wstring fullPath = state->currentDir.wstring();
        const std::wstring display = ReadText(state->edit);
        assert(!display.empty());
        // For a long path, the visible suffix and each click target still refer
        // to the original directory; omitted components have no hit target.
        if (display != fullPath) {
            assert(display.find(L"…") != std::wstring::npos || display.find(L"path_hover") != std::wstring::npos);
        }
        for (const auto& hit : state->pathHits) {
            assert(fullPath.rfind(hit.path.wstring(), 0) == 0);
            assert(hit.rect.left < hit.rect.right);
        }
    }
}

static void CALLBACK Operate(HWND, UINT, UINT_PTR, DWORD) {
    HWND window = FindWindowW(L"LocalPathBrowserDlg", nullptr);
    if (!window || operated) return;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) return;
    operated = true;
    auto* state = reinterpret_cast<LocalPathBrowserState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    assert(state);
    CheckBrowserLists(state);
    const auto suggestedFolder = state->currentDir / L"授業_候補";
    if (scenario >= 11) {
        if (scenario == 17) {
            assert(ReadText(state->edit).find(L"…") != std::wstring::npos);
            assert(ReadText(state->edit).size() >= state->currentDir.filename().wstring().size());
            assert(!state->pathHits.empty() && state->pathHits.back().path == state->currentDir);
            const auto rootPath = state->currentDir.root_path();
            const auto hit = state->pathHits.front();
            assert(hit.path == rootPath);
            const POINT point{(hit.rect.left + hit.rect.right) / 2, (hit.rect.top + hit.rect.bottom) / 2};
            SendMessageW(state->edit, WM_LBUTTONDOWN, 0, MAKELPARAM(point.x, point.y));
            assert(state->currentDir == rootPath && state->pathTooltipText == rootPath.wstring());
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
            return;
        }
        if (scenario == 11) {
            assert(state->saveMode && state->suggestedEntries.size() >= 4);
            SetWindowTextW(state->fileNameEdit, L"上段の出力");
            SelectEntry(state, suggestedFolder, true, true);
            assert(state->currentDir == suggestedFolder && ReadText(state->fileNameEdit) == L"上段の出力");
            CheckBrowserLists(state); // No suggestions in the destination: normal list expands.
            assert(state->suggestedEntries.empty() && !IsWindowVisible(state->suggestionList));
            assert(GetFocus() != state->suggestionList);
            SendMessageW(window, WM_COMMAND, IDOK, 0);
        } else if (scenario == 12) {
            assert(state->allowMultiple && state->requireDirectory);
            SelectEntry(state, suggestedFolder, false, true);
            SelectEntry(state, suggestedFolder, false);
            SelectEntry(state, state->currentDir / L"A_ordinary", false);
            assert(SelectedLocalBrowserEntries(state).size() == 2);
            SendMessageW(window, WM_COMMAND, IDOK, 0);
        } else if (scenario == 13 || scenario == 14) {
            if (scenario == 14) {
                assert(ReadText(GetDlgItem(window, IDOK)) == L"common.close");
                assert(!GetDlgItem(window, IDCANCEL));
            }
            SelectEntry(state, suggestedFolder, true, true);
            CheckBrowserLists(state);
            assert(state->suggestedEntries.empty());
            if (scenario == 13) SelectEntry(state, suggestedFolder / L"existing.pdf", true);
            else SendMessageW(window, WM_COMMAND, IDOK, 0);
        } else if (scenario == 15) {
            assert(!state->suggestedEntries.empty() && state->entries[state->suggestedEntries.front()].highlighted);
            assert(SelectedLocalBrowserEntries(state).size() == 1);
            SelectEntry(state, state->highlightPath, true, true);
            SendMessageW(window, WM_COMMAND, IDOK, 0);
        } else if (scenario == 16) {
            SelectEntry(state, suggestedFolder, false, true);
            SelectEntry(state, state->currentDir / L"A_ordinary", false);
            assert(SendMessageW(state->suggestionList, LB_GETCURSEL, 0, 0) == LB_ERR);
            SelectEntry(state, suggestedFolder, false, true);
            assert(SendMessageW(state->list, LB_GETCURSEL, 0, 0) == LB_ERR);
            assert(SelectedLocalBrowserEntries(state).size() == 1);
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
        }
        return;
    }
    if (scenario == 8 || scenario == 9 || scenario == 10) {
        assert(!state->saveMode && !state->fileNameEdit);
        HWND confirm = GetDlgItem(window, IDOK);
        const std::wstring label = ReadText(confirm);
        assert(label ==
               (scenario == 8 ? L"取り込む" : scenario == 9 ? L"Select destination" : L"Convert"));
        RECT confirmRect{}, cancelRect{};
        GetWindowRect(confirm, &confirmRect);
        GetWindowRect(GetDlgItem(window, IDCANCEL), &cancelRect);
        assert(confirmRect.right < cancelRect.left);
        HDC dc = GetDC(confirm);
        assert(dc);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(confirm, WM_GETFONT, 0, 0));
        const HGDIOBJ previous = SelectObject(dc, font ? font : GetStockObject(SYSTEM_FONT));
        SIZE labelSize{};
        assert(GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &labelSize));
        assert(labelSize.cx + 16 <= confirmRect.right - confirmRect.left);
        SelectObject(dc, previous);
        ReleaseDC(confirm, dc);
        SelectEntry(state, scenario == 9 ? destination : state->currentDir / L"existing.pdf", scenario != 10);
        if (scenario != 8) SendMessageW(window, WM_COMMAND, IDOK, 0);
        return;
    }
    assert(state->saveMode && state->fileNameEdit);
    assert(ReadText(state->fileNameEdit) == L"default.pdf");
    if (scenario == 1 || scenario == 4) {
        SetWindowTextW(state->fileNameEdit, L"選んだ 出力");
        SelectEntry(state, destination, true);
        assert(state->currentDir == destination);
        assert(ReadText(state->fileNameEdit) == L"選んだ 出力");
        if (scenario == 4) {
            SendMessageW(GetDlgItem(window, 205), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(window, WM_COMMAND, 204, 0);
        } else SendMessageW(window, WM_COMMAND, IDOK, 0);
    } else if (scenario == 2) {
        SelectEntry(state, state->currentDir / L"existing.pdf", false);
        assert(ReadText(state->fileNameEdit) == L"existing.pdf");
        assert(!state->accepted);
        SendMessageW(window, WM_COMMAND, IDOK, 0);
    } else if (scenario == 3) {
        for (const auto* name : {L"", L"..", L"../escaped.pdf", L"C:relative.pdf", L"bad?.pdf"}) {
            SetWindowTextW(state->fileNameEdit, name);
            const int before = notices;
            SendMessageW(window, WM_COMMAND, IDOK, 0);
            assert(IsWindow(window) && !state->accepted && notices == before + 1);
        }
        SendMessageW(window, WM_COMMAND, 202, 0); // drive list is not a destination
        CheckBrowserLists(state);
        assert(state->suggestedEntries.empty());
        SetWindowTextW(state->fileNameEdit, L"valid.pdf");
        SendMessageW(window, WM_COMMAND, IDOK, 0);
        assert(IsWindow(window) && !state->accepted);
        SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    } else if (scenario == 5) {
        SendMessageW(window, WM_CLOSE, 0, 0);
    } else if (scenario == 6) {
        SelectEntry(state, destination, true);
        SetWindowTextW(state->fileNameEdit, L"handoff.pdf");
        SendMessageW(window, WM_COMMAND, 204, 0);
    } else if (scenario == 7) {
        std::wstring longName(280, L'a');
        SetWindowTextW(state->fileNameEdit, longName.c_str());
        assert(ReadText(state->fileNameEdit).size() == 280);
        // Name length is retained without truncation; no write is attempted.
        SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    }
}
static SavePathPromptSelection Run(int id, HWND owner, const std::filesystem::path& root) {
    scenario = id; operated = false;
    const UINT_PTR timer = SetTimer(nullptr, kBrowserAutomationTimer, 10, Operate); assert(timer);
    const auto result = PromptSavePath(owner, L"Browser save test", root, L"default.pdf", L"pdf");
    KillTimer(nullptr, timer);
    assert(operated && IsWindowEnabled(owner));
    assert(!FindWindowW(L"LocalPathBrowserDlg", nullptr));
    return result;
}
int wmain() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    assert(InitCommonControlsEx(&controls));
    assert(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    std::error_code ec;
    const auto root = std::filesystem::current_path(ec) / (L"browser_fixture_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    assert(!ec && !std::filesystem::exists(root, ec) && !ec);
    destination = root / L"日本語 destination";
    assert(std::filesystem::create_directories(destination, ec) && !ec);
    for (const auto* name : {L"A_ordinary", L"Course", L"Lecture", L"授業_候補", L"課題"}) {
        assert(std::filesystem::create_directory(root / name, ec) && !ec);
    }
    { std::ofstream file(root / L"existing.pdf", std::ios::binary); file << "original bytes"; }
    { std::ofstream file(root / L"授業.txt", std::ios::binary); file << "not a directory suggestion"; }
    { std::ofstream file(root / L"授業_候補/existing.pdf", std::ios::binary); file << "suggested folder source"; }
    HWND owner = CreateWindowW(L"STATIC", L"Browser test owner", WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, g_hInst, nullptr);
    assert(owner);
    auto result = Run(1, owner, root);
    assert(result.action == SavePathPromptResult::DirectInput && result.directory == destination &&
           result.fileName == L"選んだ 出力.pdf");
    assert(!std::filesystem::exists(destination / result.fileName, ec) && !ec);
    result = Run(2, owner, root);
    assert(result.action == SavePathPromptResult::DirectInput && result.fileName == L"existing.pdf");
    assert(Run(3, owner, root).action == SavePathPromptResult::Cancel);
    assert(Run(5, owner, root).action == SavePathPromptResult::Cancel);
    assert(Run(7, owner, root).action == SavePathPromptResult::Cancel);
    result = Run(6, owner, root);
    assert(result.action == SavePathPromptResult::OpenSystemDialog && result.directory == destination &&
           result.fileName == L"handoff.pdf" && !g_config.useNativeFileDialogs && configSaves == 0);
    g_workspaceRoot = root.wstring();
    result = Run(4, owner, root);
    assert(result.action == SavePathPromptResult::OpenSystemDialog && result.directory == destination &&
           result.fileName == L"選んだ 出力" && g_config.useNativeFileDialogs && configSaves == 1);
    result = PromptSavePath(owner, L"Native direct", root, L"direct.pdf", L"pdf");
    assert(result.action == SavePathPromptResult::OpenSystemDialog && result.directory == root &&
           result.fileName == L"direct.pdf" && !FindWindowW(L"LocalPathBrowserDlg", nullptr));
    std::ifstream original(root / L"existing.pdf", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(original)), {});
    assert(bytes == "original bytes" && std::filesystem::is_empty(destination, ec) && !ec);
    g_config.useNativeFileDialogs = false;
    result = Run(11, owner, root);
    assert(result.action == SavePathPromptResult::DirectInput && result.directory == root / L"授業_候補" &&
           result.fileName == L"上段の出力.pdf");
    assert(!std::filesystem::exists(result.directory / result.fileName, ec) && !ec);
    assert(Run(16, owner, root).action == SavePathPromptResult::Cancel);
    const auto longFolder = root / std::wstring(70, L'階') / std::wstring(70, L'層') / L"末尾の保存先";
    assert(std::filesystem::create_directories(longFolder, ec) && !ec);
    assert(Run(17, owner, longFolder).action == SavePathPromptResult::Cancel);
    for (const int id : {8, 9, 10}) {
        scenario = id; operated = false;
        const UINT_PTR timer = SetTimer(nullptr, kBrowserAutomationTimer, 10, Operate); assert(timer);
        if (id == 10) {
            const auto picked = PromptExistingLocalFilesAppFirst(owner, root, L"Existing files test", true, L"Convert");
            assert(picked.size() == 1 && picked.front() == (root / L"existing.pdf").wstring());
        } else {
            const auto picked = PromptExistingLocalPath(owner, root, L"Existing path test", id == 9, {},
                                                        id == 8 ? L"取り込む" : L"Select destination");
            assert(picked && *picked == (id == 9 ? destination : root / L"existing.pdf").wstring());
        }
        KillTimer(nullptr, timer);
        assert(operated && IsWindowEnabled(owner));
    }
    for (const int id : {12, 13, 14, 15}) {
        scenario = id; operated = false;
        const UINT_PTR timer = SetTimer(nullptr, kBrowserAutomationTimer, 10, Operate); assert(timer);
        if (id == 12) {
            const auto picked = PromptExistingLocalFolders(owner, root, L"Folder suggestions test", true, {});
            assert(picked.size() == 2 && picked[0] == (root / L"授業_候補").wstring() &&
                   picked[1] == (root / L"A_ordinary").wstring());
        } else if (id == 13) {
            const auto picked = PromptExistingLocalPath(owner, root, L"File suggestions test", false, {}, {});
            assert(picked && *picked == (root / L"授業_候補" / L"existing.pdf").wstring());
        } else if (id == 14) {
            assert(PromptLocalPathBrowser(owner, root, L"Browse suggestions test", false, false, true, {}).empty());
        } else {
            const auto picked = PromptExistingLocalPath(owner, root, L"Current workspace suggestions test", true,
                                                        root / L"A_ordinary", {});
            assert(picked && *picked == (root / L"A_ordinary").wstring());
        }
        KillTimer(nullptr, timer);
        assert(operated && IsWindowEnabled(owner));
    }
    DestroyWindow(owner); CoUninitialize();
    // Fixtures are retained inside the test output directory for inspection.
    std::cout << "Local browser tests passed (suggestions/all entries, duplicate selection, navigation, save names, cancellation, native handoff, no writes).\n";
    return 0;
}
