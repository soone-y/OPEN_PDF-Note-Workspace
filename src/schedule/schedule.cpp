// file: schedule.cpp
#include "schedule/schedule.h"

#include "core/app_core.h"
#include "ui/combobox_guard.h"

#include <algorithm>
#include <filesystem>
#include <vector>
#include <commctrl.h>

namespace {
constexpr int kScheduleMaxDays = 7;
constexpr int kScheduleMaxPeriods = 13;
constexpr int kDefaultScheduleMask = 0x1F;
constexpr int kScheduleCellBase = 6100;
constexpr int kMargin = 12;
constexpr int kHeaderHeight = 20;
constexpr int kRowHeaderWidth = 48;
constexpr int kCellWidth = 130;
constexpr int kCellHeight = 24;
constexpr int kGap = 6;
constexpr int kComboDropHeight = 200;
static constexpr wchar_t kScheduleWndClass[] = L"LectureScheduleWnd";
static HWND g_hScheduleWnd = nullptr;

struct ScheduleCtx {
    int columns = 5;
    int periods = 6;
    std::vector<int> dayIndices;
    std::vector<int> cellMap;
    std::vector<HWND> dayLabels;
    std::vector<HWND> periodLabels;
    std::vector<HWND> cells;
    std::vector<std::wstring> lectureNames;
};

static void EnsureScheduleConfig() {
    g_config.scheduleDayMask &= 0x7F;
    if (g_config.scheduleDayMask == 0) g_config.scheduleDayMask = kDefaultScheduleMask;
    g_config.schedulePeriods = std::clamp(g_config.schedulePeriods, 1, kScheduleMaxPeriods);
    size_t cellTotal = static_cast<size_t>(kScheduleMaxDays * g_config.schedulePeriods);
    if (g_config.scheduleCells.size() < cellTotal) {
        g_config.scheduleCells.resize(cellTotal);
    } else if (g_config.scheduleCells.size() > cellTotal) {
        g_config.scheduleCells.resize(cellTotal);
    }
    size_t timeTotal = static_cast<size_t>(kScheduleMaxDays * kScheduleMaxPeriods);
    if (g_config.scheduleStartTimes.size() < timeTotal) {
        g_config.scheduleStartTimes.resize(timeTotal);
    } else if (g_config.scheduleStartTimes.size() > timeTotal) {
        g_config.scheduleStartTimes.resize(timeTotal);
    }
}

static std::vector<int> BuildEnabledDayIndices(int mask) {
    std::vector<int> indices;
    for (int i = 0; i < kScheduleMaxDays; ++i) {
        if (mask & (1 << i)) indices.push_back(i);
    }
    if (indices.empty()) indices.push_back(0);
    return indices;
}

static std::vector<std::wstring> BuildDayLabels(const std::vector<int>& dayIndices) {
    std::vector<std::wstring> labels;
    const std::vector<std::wstring> ja = { L"月", L"火", L"水", L"木", L"金", L"土", L"日" };
    const std::vector<std::wstring> en = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
    const auto& base = IsEnglishUi() ? en : ja;
    for (int idx : dayIndices) {
        if (idx >= 0 && idx < static_cast<int>(base.size())) {
            labels.push_back(base[static_cast<size_t>(idx)]);
        }
    }
    return labels;
}

static std::wstring BuildPeriodLabel(int index) {
    int n = index + 1;
    if (IsEnglishUi()) {
        return L"P" + std::to_wstring(n);
    }
    return std::to_wstring(n) + L"限";
}

static std::vector<std::wstring> CollectLectureNames() {
    std::vector<std::wstring> names;
    if (!g_lectures.empty()) {
        names.reserve(g_lectures.size());
        for (const auto& path : g_lectures) {
            if (path.empty()) continue;
            std::wstring name = std::filesystem::path(path).filename().wstring();
            if (!name.empty()) names.push_back(std::move(name));
        }
        if (!names.empty()) return names;
    }
    if (g_workspaceRoot.empty()) return names;
    std::error_code ec;
    auto classesPath = WorkspaceClassesPath(g_workspaceRoot, g_config);
    if (!std::filesystem::exists(classesPath, ec)) return names;
    auto cacheName = std::filesystem::path(g_config.cacheDir).filename().wstring();
    for (const auto& entry : std::filesystem::directory_iterator(classesPath, ec)) {
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(entry.path(), isReparse) && isReparse) continue;
        std::error_code stEc;
        if (!entry.is_directory(stEc) || stEc) continue;
        auto name = entry.path().filename().wstring();
        if (!cacheName.empty() && name == cacheName) continue;
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

static void LayoutSchedule(HWND hWnd, ScheduleCtx* ctx) {
    if (!ctx) return;
    auto dayLabels = BuildDayLabels(ctx->dayIndices);
    int columns = static_cast<int>(dayLabels.size());
    int startX = kMargin + kRowHeaderWidth + kGap;
    int startY = kMargin + kHeaderHeight + kGap;

    for (int col = 0; col < columns; ++col) {
        int x = startX + col * (kCellWidth + kGap);
        if (col < static_cast<int>(ctx->dayLabels.size())) {
            SetWindowPos(ctx->dayLabels[col], nullptr, x, kMargin, kCellWidth, kHeaderHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowTextW(ctx->dayLabels[col], dayLabels[static_cast<size_t>(col)].c_str());
        }
    }

    for (int p = 0; p < ctx->periods; ++p) {
        int y = startY + p * (kCellHeight + kGap);
        if (p < static_cast<int>(ctx->periodLabels.size())) {
            auto label = BuildPeriodLabel(p);
            SetWindowPos(ctx->periodLabels[p], nullptr, kMargin, y, kRowHeaderWidth, kCellHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowTextW(ctx->periodLabels[p], label.c_str());
        }
    }

    for (int p = 0; p < ctx->periods; ++p) {
        for (int col = 0; col < columns; ++col) {
            size_t index = static_cast<size_t>(p * columns + col);
            if (index >= ctx->cells.size()) continue;
            int x = startX + col * (kCellWidth + kGap);
            int y = startY + p * (kCellHeight + kGap);
            SetWindowPos(ctx->cells[index], nullptr, x, y, kCellWidth, kCellHeight + kComboDropHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

static void FillScheduleCombo(HWND combo,
                              const std::vector<std::wstring>& lectureNames,
                              const std::wstring& current) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"-"));
    for (const auto& name : lectureNames) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    int sel = 0;
    if (!current.empty()) {
        for (size_t i = 0; i < lectureNames.size(); ++i) {
            if (lectureNames[i] == current) {
                sel = static_cast<int>(i + 1);
                break;
            }
        }
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
    SendMessageW(combo, CB_SETDROPPEDWIDTH, kCellWidth * 2, 0);
}

static void InitScheduleUi(HWND hWnd, ScheduleCtx* ctx) {
    if (!ctx) return;
    ctx->lectureNames = CollectLectureNames();
    ctx->dayIndices = BuildEnabledDayIndices(g_config.scheduleDayMask);
    ctx->columns = static_cast<int>(ctx->dayIndices.size());
    ctx->dayLabels.resize(static_cast<size_t>(ctx->columns));
    ctx->periodLabels.resize(static_cast<size_t>(ctx->periods));
    ctx->cells.resize(static_cast<size_t>(ctx->columns * ctx->periods));
    ctx->cellMap.resize(ctx->cells.size());

    for (int col = 0; col < ctx->columns; ++col) {
        ctx->dayLabels[col] = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        SetUIFont(ctx->dayLabels[col]);
    }

    for (int p = 0; p < ctx->periods; ++p) {
        ctx->periodLabels[p] = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        SetUIFont(ctx->periodLabels[p]);
    }

    for (int p = 0; p < ctx->periods; ++p) {
        for (int col = 0; col < ctx->columns; ++col) {
            size_t index = static_cast<size_t>(p * ctx->columns + col);
            int controlId = kScheduleCellBase + static_cast<int>(index);
            HWND combo = CreateWindowExW(
                0, WC_COMBOBOXW, L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                0, 0, 0, 0, hWnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                g_hInst, nullptr);
            ctx->cells[index] = combo;
            SetUIFont(combo);
            ui::GuardComboAgainstAccidentalChange(combo);
            int dayIndex = ctx->dayIndices[static_cast<size_t>(col)];
            size_t scheduleIndex = static_cast<size_t>(p * kScheduleMaxDays + dayIndex);
            ctx->cellMap[index] = static_cast<int>(scheduleIndex);
            const std::wstring& current = (scheduleIndex < g_config.scheduleCells.size())
                                          ? g_config.scheduleCells[scheduleIndex]
                                          : L"";
            FillScheduleCombo(combo, ctx->lectureNames, current);
        }
    }
    LayoutSchedule(hWnd, ctx);
}

static void RefreshScheduleLectureNames(ScheduleCtx* ctx) {
    if (!ctx) return;
    auto names = CollectLectureNames();
    if (names == ctx->lectureNames) return;
    ctx->lectureNames = std::move(names);
    for (size_t i = 0; i < ctx->cells.size(); ++i) {
        size_t scheduleIndex = static_cast<size_t>(ctx->cellMap[i]);
        const std::wstring& current = (scheduleIndex < g_config.scheduleCells.size())
                                      ? g_config.scheduleCells[scheduleIndex]
                                      : L"";
        FillScheduleCombo(ctx->cells[i], ctx->lectureNames, current);
    }
}

static LRESULT CALLBACK ScheduleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<ScheduleCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        EnsureScheduleConfig();
        auto* newCtx = new ScheduleCtx();
        newCtx->periods = std::clamp(g_config.schedulePeriods, 1, kScheduleMaxPeriods);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newCtx));
        InitScheduleUi(hWnd, newCtx);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_SIZE:
        if (ctx) {
            LayoutSchedule(hWnd, ctx);
        }
        return 0;
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (!ctx) break;
        if (id >= kScheduleCellBase &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            size_t index = static_cast<size_t>(id - kScheduleCellBase);
            if (index >= ctx->cellMap.size()) return 0;
            HWND combo = reinterpret_cast<HWND>(lParam);
            int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
            std::wstring value;
            if (sel > 0) {
                size_t nameIndex = static_cast<size_t>(sel - 1);
                if (nameIndex < ctx->lectureNames.size()) {
                    value = ctx->lectureNames[nameIndex];
                }
            }
            size_t scheduleIndex = static_cast<size_t>(ctx->cellMap[index]);
            if (scheduleIndex < g_config.scheduleCells.size()) {
                g_config.scheduleCells[scheduleIndex] = std::move(value);
                PersistConfig();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_NCDESTROY: {
        if (ctx) {
            delete ctx;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        if (g_hScheduleWnd == hWnd) g_hScheduleWnd = nullptr;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void CalcScheduleWindowSize(int columns, int periods, int& outW, int& outH) {
    int clientW = kMargin * 2 + kRowHeaderWidth + kGap + columns * kCellWidth + (columns - 1) * kGap;
    int clientH = kMargin * 2 + kHeaderHeight + kGap + periods * kCellHeight + (periods - 1) * kGap;
    RECT rc{ 0, 0, clientW, clientH };
    DWORD style = WS_CAPTION | WS_POPUPWINDOW;
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    outW = rc.right - rc.left;
    outH = rc.bottom - rc.top;
}

} // namespace

void RefreshScheduleWindowLectureNames() {
    if (!g_hScheduleWnd) return;
    auto* ctx = reinterpret_cast<ScheduleCtx*>(GetWindowLongPtrW(g_hScheduleWnd, GWLP_USERDATA));
    RefreshScheduleLectureNames(ctx);
}

void ShowScheduleWindow(HWND parent) {
    if (g_hScheduleWnd) {
        ShowWindow(g_hScheduleWnd, SW_SHOW);
        SetForegroundWindow(g_hScheduleWnd);
        return;
    }
    EnsureScheduleConfig();
    WNDCLASSW wc{};
    wc.lpfnWndProc = ScheduleWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush
                                           : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kScheduleWndClass;
    RegisterClassW(&wc);

    int periods = std::clamp(g_config.schedulePeriods, 1, kScheduleMaxPeriods);
    int columns = static_cast<int>(BuildEnabledDayIndices(g_config.scheduleDayMask).size());
    int width = 0;
    int height = 0;
    CalcScheduleWindowSize(columns, periods, width, height);

    g_hScheduleWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        GetUiText().menuLectureSchedule.c_str(),
        WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        parent, nullptr, g_hInst, nullptr);
    if (g_hScheduleWnd) {
        ShowWindow(g_hScheduleWnd, SW_SHOW);
        UpdateWindow(g_hScheduleWnd);
    }
}


