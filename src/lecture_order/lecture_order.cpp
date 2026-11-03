#include "lecture_order/lecture_order.h"
#include "core/app_core.h"
#include "core/localization.h"
#include "workspace/workspace_tree.h"
#include "ui/core/main_window_api.h"
#include "lecture_order/priority_policy.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace {
constexpr int kListId = 6100;
constexpr int kPriorityId = 6101;
constexpr int kSetId = 6102;
constexpr int kClearId = 6103;
constexpr int kCloseId = 6104;
constexpr int kMessageId = 6105;
constexpr wchar_t kClassName[] = L"LectureOrderWindow";
HWND g_window = nullptr;
lecture_priority::Priorities externalPriorities;
std::set<std::wstring> externalPaths;

struct OrderUi {
    HWND list = nullptr;
    HWND edit = nullptr;
    HWND message = nullptr;
    std::vector<std::wstring> names; // Absolute paths; labels are resolved separately.
};

std::vector<std::wstring> CollectManagedLectures() {
    std::vector<std::wstring> names;
    for (const auto& entry : g_tempExternalLectures) names.push_back(entry.path);
    if (g_workspaceRoot.empty()) return names;
    const auto classes = WorkspaceClassesPath(g_workspaceRoot, g_config);
    const auto cacheName = std::filesystem::path(g_config.cacheDir).filename().wstring();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(classes, ec), end; !ec && it != end; it.increment(ec)) {
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(it->path(), isReparse) && isReparse) continue;
        std::error_code typeError;
        if (!it->is_directory(typeError) || typeError) continue;
        const auto name = it->path().filename().wstring();
        if (name == cacheName || name == kLectureSettingsDirName) continue;
        names.push_back(it->path().wstring());
    }
    SortLecturePathsByPriority(names);
    return names;
}

void SelectPriority(OrderUi* ui) {
    const int index = static_cast<int>(SendMessageW(ui->list, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ui->names.size())) {
        SetWindowTextW(ui->edit, L"");
        return;
    }
    const auto priority = LecturePriorityForPath(ui->names[static_cast<size_t>(index)]);
    const std::wstring value = priority ? std::to_wstring(*priority) : L"";
    SetWindowTextW(ui->edit, value.c_str());
}

void RefreshList(OrderUi* ui, bool preserveDraft = false) {
    std::wstring selected;
    const int oldIndex = static_cast<int>(SendMessageW(ui->list, LB_GETCURSEL, 0, 0));
    if (oldIndex >= 0 && oldIndex < static_cast<int>(ui->names.size())) {
        selected = ui->names[static_cast<size_t>(oldIndex)];
    }
    wchar_t draft[16]{};
    if (preserveDraft && !selected.empty()) GetWindowTextW(ui->edit, draft, 16);
    ui->names = CollectManagedLectures();
    SendMessageW(ui->list, LB_RESETCONTENT, 0, 0);
    int newIndex = -1;
    const auto labels = LectureDisplayLabelsForPaths(ui->names);
    for (size_t i = 0; i < ui->names.size(); ++i) {
        const auto priority = LecturePriorityForPath(ui->names[i]);
        const std::wstring rank = priority ? std::to_wstring(*priority) : L"—";
        const std::wstring label = rank + L"    " + labels[i] + (externalPaths.count(lecture_priority::PathKey(ui->names[i]))
            ? localization::Text(L"lecture_order.external_suffix") : L"");
        SendMessageW(ui->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (ui->names[i] == selected) newIndex = static_cast<int>(i);
    }
    if (newIndex < 0 && !ui->names.empty()) newIndex = 0;
    if (newIndex >= 0) SendMessageW(ui->list, LB_SETCURSEL, newIndex, 0);
    SelectPriority(ui);
    if (preserveDraft && newIndex >= 0 && ui->names[static_cast<size_t>(newIndex)] == selected) {
        SetWindowTextW(ui->edit, draft);
    }
}

void SavePriority(OrderUi* ui, bool clear) {
    const int index = static_cast<int>(SendMessageW(ui->list, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ui->names.size())) return;
    int priority = 0;
    if (!clear) {
        wchar_t buffer[16]{};
        GetWindowTextW(ui->edit, buffer, 16);
        const std::wstring value(buffer);
        if (value.empty() || value.size() > 4 ||
            !std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
            SetWindowTextW(ui->message, localization::Text(L"lecture_order.invalid_priority").c_str());
            return;
        }
        priority = std::stoi(value);
    }
    const auto previous = g_config.lecturePriorities;
    const auto previousMode = g_config.lectureSortMode;
    const auto& name = ui->names[static_cast<size_t>(index)];
    std::vector<std::wstring> registered;
    for (const auto& entry : g_tempExternalLectures) registered.push_back(entry.path);
    bool saved = false;
    if (externalPaths.count(lecture_priority::PathKey(name))) {
        auto candidate = LoadSetupTempExternalLecturePriorities();
        if (clear) candidate.erase(lecture_priority::PathKey(name));
        else candidate[lecture_priority::PathKey(name)] = priority;
        // Publish only after the setup-file replacement succeeds. Re-read the
        // latest metadata first so a modeless editor cannot revert new paths.
        saved = PersistSetupTempExternalLectureDirs(registered, nullptr, &candidate);
        if (saved) externalPriorities = std::move(candidate);
    } else {
        const auto key = std::filesystem::path(name).filename().wstring();
        if (clear) g_config.lecturePriorities.erase(key);
        else g_config.lecturePriorities[key] = priority;
        g_config.lectureSortMode = L"manual";
        saved = TryPersistConfig();
    }
    if (!saved) {
        g_config.lecturePriorities = previous;
        g_config.lectureSortMode = previousMode;
        SetWindowTextW(ui->message, localization::Text(L"lecture_order.save_failed").c_str());
        SelectPriority(ui);
        return;
    }
    SetWindowTextW(ui->message, L"");
    {
        struct RestoreSelectionNotifications {
            bool previous = s_ignoreLectureSelChange;
            ~RestoreSelectionNotifications() { s_ignoreLectureSelChange = previous; }
        } restoreSelectionNotifications;
        s_ignoreLectureSelChange = true;
        RefreshRegisteredExternalLectures(registered);
    }
    RefreshList(ui);
}

LRESULT CALLBACK OrderProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui = reinterpret_cast<OrderUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        ui = new OrderUi();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
        auto make = [&](const wchar_t* cls, const std::wstring& text, DWORD style,
                        int x, int y, int width, int height, int id) {
            HWND control = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style,
                                           x, y, width, height, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
            SetUIFont(control);
            return control;
        };
        make(L"STATIC", localization::Text(L"lecture_order.hint"), 0, 16, 14, 420, 34, 0);
        ui->list = make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                        16, 52, 420, 235, kListId);
        make(L"STATIC", localization::Text(L"lecture_order.priority"), 0, 16, 302, 150, 24, 0);
        // Validate on Set so invalid keys receive a silent visual error.
        ui->edit = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                        170, 299, 80, 25, kPriorityId);
        SendMessageW(ui->edit, EM_SETLIMITTEXT, 15, 0);
        make(L"BUTTON", localization::Text(L"lecture_order.set"), BS_PUSHBUTTON,
             260, 299, 82, 26, kSetId);
        make(L"BUTTON", localization::Text(L"lecture_order.clear"), BS_PUSHBUTTON,
             349, 299, 87, 26, kClearId);
        ui->message = make(L"STATIC", L"", 0, 16, 335, 320, 22, kMessageId);
        make(L"BUTTON", localization::Text(L"lecture_order.close"), BS_PUSHBUTTON,
             349, 333, 87, 27, kCloseId);
        RefreshList(ui);
        ApplyThemeToDialog(window);
        return 0;
    }
    case WM_COMMAND:
        if (!ui) break;
        if (LOWORD(wp) == kListId && HIWORD(wp) == LBN_SELCHANGE) SelectPriority(ui);
        if (LOWORD(wp) == kSetId) SavePriority(ui, false);
        if (LOWORD(wp) == kClearId) SavePriority(ui, true);
        if (LOWORD(wp) == kCloseId) DestroyWindow(window);
        return 0;
    case WM_THEMECHANGED:
        ApplyThemeToDialog(window);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return ThemeCtlColorPanel(reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp));
    case WM_DRAWITEM:
        if (DrawThemeButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp))) return TRUE;
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        delete ui;
        if (g_window == window) g_window = nullptr;
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
} // namespace

void RefreshLectureOrderWindow() {
    if (!g_window) return;
    auto* ui = reinterpret_cast<OrderUi*>(GetWindowLongPtrW(g_window, GWLP_USERDATA));
    if (ui) RefreshList(ui, true);
}

void ReloadExternalLecturePriorities() {
    externalPriorities = LoadSetupTempExternalLecturePriorities();
    externalPaths.clear();
    for (const auto& entry : g_tempExternalLectures) externalPaths.insert(lecture_priority::PathKey(entry.path));
}

std::optional<int> LecturePriorityForPath(const std::wstring& path) {
    const auto key = lecture_priority::PathKey(path);
    if (externalPaths.count(key)) {
        const auto it = externalPriorities.find(key);
        return it == externalPriorities.end() ? std::nullopt : std::optional<int>(it->second);
    }
    const auto it = g_config.lecturePriorities.find(std::filesystem::path(path).filename().wstring());
    return it == g_config.lecturePriorities.end() ? std::nullopt : std::optional<int>(it->second);
}

void SortLecturePathsByPriority(std::vector<std::wstring>& paths) {
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
        const auto pa = LecturePriorityForPath(a), pb = LecturePriorityForPath(b);
        if (pa.has_value() != pb.has_value()) return pa.has_value();
        if (pa && *pa != *pb) return *pa < *pb;
        const auto na = std::filesystem::path(a).filename().wstring();
        const auto nb = std::filesystem::path(b).filename().wstring();
        return na == nb ? a < b : na < nb;
    });
}

static void SelectLecturePath(const std::wstring& path) {
    if (path.empty() || !g_window) return;
    auto* ui = reinterpret_cast<OrderUi*>(GetWindowLongPtrW(g_window, GWLP_USERDATA));
    if (!ui) return;
    for (size_t i = 0; i < ui->names.size(); ++i) {
        if (lecture_priority::PathKey(ui->names[i]) != lecture_priority::PathKey(path)) continue;
        SendMessageW(ui->list, LB_SETCURSEL, i, 0);
        SelectPriority(ui);
        SetFocus(ui->edit);
        break;
    }
}

void ShowLectureOrderWindow(HWND parent, const std::wstring& selectedPath) {
    if (g_window) {
        ShowWindow(g_window, SW_SHOW);
        SetForegroundWindow(g_window);
        SelectLecturePath(selectedPath);
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = OrderProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);
    RECT bounds{0, 0, 452, 374};
    AdjustWindowRectEx(&bounds, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE, WS_EX_DLGMODALFRAME);
    g_window = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName,
        localization::Text(L"lecture_order.title").c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, nullptr, g_hInst, nullptr);
    if (g_window) {
        PlaceOwnedPopupAtAppTopLeft(g_window, parent);
        ShowWindow(g_window, SW_SHOW);
        UpdateWindow(g_window);
        SelectLecturePath(selectedPath);
    }
}
