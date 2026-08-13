#include "ui/menus/main_debug_menu.h"

#include "core/app_core.h"
#include "pdf_view/pdf_view.h"
#include "ui/noop_nav_guard.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {

// DebugResourceMonitorWndProc-local IDs. Keep them unique within this proc.
constexpr UINT kDebugResourceMonitorTimerId = 3701;
constexpr UINT kDebugResourceMonitorRefreshMs = 1000;
constexpr int kDebugResourceMonitorEditId = 3702;
constexpr int kDebugResourceMonitorMinWidth = 420;
constexpr int kDebugResourceMonitorMinHeight = 300;
constexpr int kDebugResourceMonitorPadding = 12;
static constexpr wchar_t kDebugResourceMonitorWndClass[] = L"DebugResourceMonitorWnd";
static constexpr std::array<const wchar_t*, 2> kRelatedProcessImageNames = {
    L"pdf_note_workspace.exe",
    L"readonly_viewer.exe",
};
static HWND g_hDebugResourceMonitorWnd = nullptr;

struct FocusSnapshot {
    HWND focused = nullptr;
    HWND focusedRoot = nullptr;
    HWND caretHwnd = nullptr;
    RECT focusedRect{};
    bool hasFocusedRect = false;
    RECT caretClientRect{};
    RECT caretScreenRect{};
    bool hasCaretRect = false;
    DWORD selStart = 0;
    DWORD selEnd = 0;
    bool hasEditSelection = false;
    int line = -1;
    int column = -1;
    int firstVisibleLine = -1;
    bool pdfFocused = false;
    bool noteFocused = false;
    bool bottomPaneFocused = false;
    bool mainFocused = false;
};

struct DebugResourceMonitorCtx {
    HWND owner = nullptr;
    HWND text = nullptr;
    std::wstring lastText;
    std::wstring pendingText;
    bool hasPendingText = false;
    unsigned long long lastCpu100ns = 0;
    unsigned long long lastCpuTickMs = 0;
    double lastCpuPercent = -1.0;
    bool cpuSampleReady = false;
    unsigned long long lastSuiteCpu100ns = 0;
    unsigned long long lastSuiteCpuTickMs = 0;
    double lastSuiteCpuPercent = -1.0;
    bool suiteCpuSampleReady = false;
    std::wstring lastSuiteSampleKey;
    FocusSnapshot lastAppFocus{};
    bool hasLastAppFocus = false;
};

struct AggregatedProcessLoad {
    unsigned long long cpu100ns = 0;
    unsigned long long uptimeMs = 0;
    unsigned long long workingSetBytes = 0;
    unsigned long long peakWorkingSetBytes = 0;
    unsigned long long privateBytes = 0;
    unsigned long long handles = 0;
    unsigned long long threads = 0;
    unsigned long long gdiObjects = 0;
    unsigned long long userObjects = 0;
    unsigned long long processCount = 0;
    bool hasProcessTimes = false;
    bool hasUptime = false;
    bool hasMemory = false;
    bool hasHandles = false;
    bool hasGuiObjects = false;
    bool hasCompanionProcess = false;
    std::wstring sampleKey;
    std::vector<std::wstring> labels;
};

struct DebugResourceMonitorCreateParams {
    HWND owner = nullptr;
    std::wstring initialText;
};

static unsigned long long FileTimeToUInt64(const FILETIME& ft) {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

static std::wstring FormatMiB(unsigned long long bytes) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1)
        << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << L" MiB";
    return oss.str();
}

static std::wstring FormatMillis(unsigned long long millis) {
    std::wostringstream oss;
    oss << millis << L" ms";
    return oss.str();
}

static std::wstring FormatCount(unsigned long long value) {
    return std::to_wstring(value);
}

static std::wstring FormatPercent(double value) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1) << value << L"%";
    return oss.str();
}

static std::wstring FormatLocalTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wostringstream oss;
    oss << std::setfill(L'0')
        << st.wYear << L"-"
        << std::setw(2) << st.wMonth << L"-"
        << std::setw(2) << st.wDay << L" "
        << std::setw(2) << st.wHour << L":"
        << std::setw(2) << st.wMinute << L":"
        << std::setw(2) << st.wSecond << L"."
        << std::setw(3) << st.wMilliseconds;
    return oss.str();
}

static std::wstring FormatBoolWord(bool english, bool value) {
    if (english) return value ? L"Yes" : L"No";
    return value ? L"あり" : L"なし";
}

static std::wstring FormatWindowHandle(HWND hWnd) {
    if (!hWnd) return L"0x0";
    std::wostringstream oss;
    oss << L"0x" << std::hex << std::uppercase
        << static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hWnd));
    return oss.str();
}

static std::wstring ReadWindowClassName(HWND hWnd) {
    wchar_t cls[128]{};
    if (!hWnd || !GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls)))) return L"";
    return cls;
}

static std::wstring FormatRectValue(const RECT& rc) {
    std::wostringstream oss;
    oss << L"(" << rc.left << L"," << rc.top << L")-("
        << rc.right << L"," << rc.bottom << L") "
        << (rc.right - rc.left) << L"x" << (rc.bottom - rc.top);
    return oss.str();
}

static std::wstring FormatPointValue(const POINT& pt) {
    std::wostringstream oss;
    oss << L"(" << pt.x << L"," << pt.y << L")";
    return oss.str();
}

static bool IsRelatedProcessImageName(const wchar_t* imageName) {
    if (!imageName || !*imageName) return false;
    for (const wchar_t* expected : kRelatedProcessImageNames) {
        if (_wcsicmp(imageName, expected) == 0) return true;
    }
    return false;
}

static std::wstring BuildProcessSampleKey(const std::vector<DWORD>& pids) {
    std::wostringstream oss;
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i != 0) oss << L",";
        oss << pids[i];
    }
    return oss.str();
}

static bool UpdateCpuUsageSample(unsigned long long cpu100ns,
                                 unsigned long long* lastCpu100ns,
                                 unsigned long long* lastCpuTickMs,
                                 double* outPercent,
                                 bool* inOutReady,
                                 const std::wstring* sampleKey = nullptr,
                                 std::wstring* inOutLastSampleKey = nullptr) {
    if (!lastCpu100ns || !lastCpuTickMs || !outPercent || !inOutReady) return false;

    const unsigned long long nowTickMs = GetTickCount64();
    if (sampleKey && inOutLastSampleKey && *inOutLastSampleKey != *sampleKey) {
        *inOutLastSampleKey = *sampleKey;
        *lastCpu100ns = cpu100ns;
        *lastCpuTickMs = nowTickMs;
        *inOutReady = false;
        return false;
    }

    bool updated = false;
    if (*lastCpuTickMs != 0 && nowTickMs > *lastCpuTickMs && cpu100ns >= *lastCpu100ns) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const unsigned long long deltaCpu100ns = cpu100ns - *lastCpu100ns;
        const unsigned long long deltaWallMs = nowTickMs - *lastCpuTickMs;
        const unsigned int processors =
            std::max(1u, static_cast<unsigned int>(si.dwNumberOfProcessors));
        const double deltaCpuMs = static_cast<double>(deltaCpu100ns) / 10000.0;
        const double capacityMs = static_cast<double>(deltaWallMs) * static_cast<double>(processors);
        if (capacityMs > 0.0) {
            *outPercent = std::clamp((deltaCpuMs / capacityMs) * 100.0, 0.0, 999.9);
            *inOutReady = true;
            updated = true;
        }
    }

    *lastCpu100ns = cpu100ns;
    *lastCpuTickMs = nowTickMs;
    return updated;
}

static bool CollectRelatedProcessLoad(AggregatedProcessLoad* out) {
    if (!out) return false;
    *out = AggregatedProcessLoad{};

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    std::vector<DWORD> pids;
    unsigned long long threadCount = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (!IsRelatedProcessImageName(entry.szExeFile)) {
                entry.dwSize = sizeof(entry);
                continue;
            }
            pids.push_back(entry.th32ProcessID);
            threadCount += entry.cntThreads;
            std::wostringstream label;
            label << entry.szExeFile << L" (PID " << entry.th32ProcessID << L")";
            out->labels.push_back(label.str());
            if (entry.th32ProcessID != GetCurrentProcessId()) {
                out->hasCompanionProcess = true;
            }
            entry.dwSize = sizeof(entry);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);

    if (pids.empty()) return false;

    std::sort(pids.begin(), pids.end());
    std::sort(out->labels.begin(), out->labels.end());
    out->processCount = static_cast<unsigned long long>(pids.size());
    out->sampleKey = BuildProcessSampleKey(pids);

    unsigned long long earliestCreate100ns = 0;
    out->threads = threadCount;

    FILETIME nowTime{};
    GetSystemTimeAsFileTime(&nowTime);
    const unsigned long long now100ns = FileTimeToUInt64(nowTime);

    for (DWORD pid : pids) {
        HANDLE process = nullptr;
        bool closeProcess = false;
        if (pid == GetCurrentProcessId()) {
            process = GetCurrentProcess();
        } else {
            process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            closeProcess = (process != nullptr);
        }
        if (!process) continue;

        FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
        if (GetProcessTimes(process, &createTime, &exitTime, &kernelTime, &userTime)) {
            out->hasProcessTimes = true;
            out->cpu100ns += FileTimeToUInt64(kernelTime) + FileTimeToUInt64(userTime);
            const unsigned long long create100ns = FileTimeToUInt64(createTime);
            if (earliestCreate100ns == 0 || create100ns < earliestCreate100ns) {
                earliestCreate100ns = create100ns;
            }
        }

        PROCESS_MEMORY_COUNTERS_EX memCounters{};
        if (GetProcessMemoryInfo(process,
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memCounters),
                                 sizeof(memCounters))) {
            out->hasMemory = true;
            out->workingSetBytes += memCounters.WorkingSetSize;
            out->peakWorkingSetBytes += memCounters.PeakWorkingSetSize;
            out->privateBytes += memCounters.PrivateUsage;
        }

        DWORD handleCount = 0;
        if (GetProcessHandleCount(process, &handleCount)) {
            out->hasHandles = true;
            out->handles += static_cast<unsigned long long>(handleCount);
        }

        out->gdiObjects += static_cast<unsigned long long>(GetGuiResources(process, GR_GDIOBJECTS));
        out->userObjects += static_cast<unsigned long long>(GetGuiResources(process, GR_USEROBJECTS));
        out->hasGuiObjects = true;

        if (closeProcess) CloseHandle(process);
    }

    if (earliestCreate100ns != 0 && now100ns >= earliestCreate100ns) {
        out->uptimeMs = (now100ns - earliestCreate100ns) / 10000ULL;
        out->hasUptime = true;
    }
    return true;
}

static bool TryGetEditCharPos(HWND hWnd, size_t index, POINT* out) {
    if (!out || !hWnd) return false;
    if (ui::IsRichEditWindow(hWnd)) {
        POINTL pt{};
        LRESULT res = SendMessageW(hWnd, EM_POSFROMCHAR,
                                   reinterpret_cast<WPARAM>(&pt),
                                   static_cast<LPARAM>(index));
        if (res == -1) return false;
        out->x = static_cast<int>(pt.x);
        out->y = static_cast<int>(pt.y);
        return true;
    }
    LRESULT pos = SendMessageW(hWnd, EM_POSFROMCHAR, static_cast<WPARAM>(index), 0);
    if (pos == -1) return false;
    out->x = GET_X_LPARAM(pos);
    out->y = GET_Y_LPARAM(pos);
    return true;
}

static std::wstring DescribeFocusArea(bool english, const FocusSnapshot& focus) {
    if (!focus.focused) return english ? L"None" : L"なし";
    if (focus.noteFocused) return english ? L"Note" : L"ノート";
    if (focus.pdfFocused) return english ? L"PDF" : L"PDF";
    if (focus.bottomPaneFocused) return english ? L"Bottom pane" : L"下部ペイン";
    if (focus.mainFocused) return english ? L"Main window" : L"メインウィンドウ";
    if (focus.focused == g_hDebugResourceMonitorWnd ||
        (g_hDebugResourceMonitorWnd && IsChild(g_hDebugResourceMonitorWnd, focus.focused))) {
        return english ? L"Resource monitor" : L"リソースモニター";
    }
    return english ? L"Other" : L"その他";
}

static bool IsDebugResourceMonitorChildWindow(HWND hWnd) {
    return hWnd && g_hDebugResourceMonitorWnd &&
           (hWnd == g_hDebugResourceMonitorWnd || IsChild(g_hDebugResourceMonitorWnd, hWnd));
}

static FocusSnapshot CaptureFocusSnapshot() {
    FocusSnapshot snapshot{};

    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(0, &info)) {
        snapshot.focused = info.hwndFocus;
        snapshot.caretHwnd = info.hwndCaret;
        if (snapshot.caretHwnd) {
            snapshot.caretClientRect = info.rcCaret;
            POINT tl{ info.rcCaret.left, info.rcCaret.top };
            POINT br{ info.rcCaret.right, info.rcCaret.bottom };
            if (ClientToScreen(snapshot.caretHwnd, &tl) &&
                ClientToScreen(snapshot.caretHwnd, &br)) {
                snapshot.caretScreenRect = RECT{ tl.x, tl.y, br.x, br.y };
                snapshot.hasCaretRect = true;
            }
        }
    }
    if (!snapshot.focused) snapshot.focused = GetFocus();
    if (!snapshot.focused || !IsWindow(snapshot.focused)) return snapshot;

    snapshot.focusedRoot = GetAncestor(snapshot.focused, GA_ROOT);
    snapshot.noteFocused = g_hNoteEdit && (snapshot.focused == g_hNoteEdit || IsChild(g_hNoteEdit, snapshot.focused));
    snapshot.pdfFocused = g_hPdfView && (snapshot.focused == g_hPdfView || IsChild(g_hPdfView, snapshot.focused));
    snapshot.bottomPaneFocused =
        (g_hBottomNote && (snapshot.focused == g_hBottomNote || IsChild(g_hBottomNote, snapshot.focused))) ||
        (g_hBottomMath && (snapshot.focused == g_hBottomMath || IsChild(g_hBottomMath, snapshot.focused)));
    snapshot.mainFocused = g_hMainWnd && (snapshot.focused == g_hMainWnd || IsChild(g_hMainWnd, snapshot.focused));
    snapshot.hasFocusedRect = GetWindowRect(snapshot.focused, &snapshot.focusedRect) != FALSE;

    if (ui::IsEditOrRichEditWindow(snapshot.focused)) {
        SendMessageW(snapshot.focused, EM_GETSEL,
                     reinterpret_cast<WPARAM>(&snapshot.selStart),
                     reinterpret_cast<LPARAM>(&snapshot.selEnd));
        snapshot.hasEditSelection = true;
        const DWORD caret = std::max(snapshot.selStart, snapshot.selEnd);
        const LRESULT line = SendMessageW(snapshot.focused, EM_LINEFROMCHAR, static_cast<WPARAM>(caret), 0);
        if (line >= 0) {
            snapshot.line = static_cast<int>(line);
            const LRESULT lineStart = SendMessageW(snapshot.focused, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
            if (lineStart >= 0) {
                snapshot.column = static_cast<int>(caret) - static_cast<int>(lineStart);
            }
        }
        snapshot.firstVisibleLine = static_cast<int>(SendMessageW(snapshot.focused, EM_GETFIRSTVISIBLELINE, 0, 0));
        if (!snapshot.hasCaretRect) {
            POINT pt{};
            if (TryGetEditCharPos(snapshot.focused, static_cast<size_t>(caret), &pt)) {
                POINT screen = pt;
                if (ClientToScreen(snapshot.focused, &screen)) {
                    snapshot.caretScreenRect = RECT{ screen.x, screen.y, screen.x + 1, screen.y + 18 };
                    snapshot.caretClientRect = RECT{ pt.x, pt.y, pt.x + 1, pt.y + 18 };
                    snapshot.caretHwnd = snapshot.focused;
                    snapshot.hasCaretRect = true;
                }
            }
        }
    }
    return snapshot;
}

static FocusSnapshot ResolveMonitorFocusSnapshot(DebugResourceMonitorCtx* ctx) {
    const FocusSnapshot current = CaptureFocusSnapshot();
    if (!current.focused) {
        if (ctx && ctx->hasLastAppFocus) return ctx->lastAppFocus;
        return current;
    }
    if (IsDebugResourceMonitorChildWindow(current.focused)) {
        if (ctx && ctx->hasLastAppFocus) return ctx->lastAppFocus;
        return current;
    }
    if (ctx) {
        ctx->lastAppFocus = current;
        ctx->hasLastAppFocus = true;
    }
    return current;
}

static void AppendFocusSection(std::wostringstream& oss, bool english, const FocusSnapshot& focus) {
    oss << L"\r\n";
    oss << (english ? L"Focus" : L"フォーカス") << L"\r\n";
    oss << (english ? L"Focus area: " : L"フォーカス領域: ")
        << DescribeFocusArea(english, focus) << L"\r\n";

    if (!focus.focused) {
        oss << (english ? L"Focused window: none" : L"対象ウィンドウ: なし") << L"\r\n";
        return;
    }

    oss << (english ? L"Focused window: " : L"対象ウィンドウ: ")
        << FormatWindowHandle(focus.focused)
        << L" class=" << ReadWindowClassName(focus.focused);
    const int ctrlId = GetDlgCtrlID(focus.focused);
    if (ctrlId != 0) {
        oss << L" id=" << ctrlId;
    }
    oss << L"\r\n";
    if (focus.hasFocusedRect) {
        oss << (english ? L"Focused rect: " : L"対象矩形: ")
            << FormatRectValue(focus.focusedRect) << L"\r\n";
    }
    if (focus.focusedRoot && focus.focusedRoot != focus.focused) {
        oss << (english ? L"Focus root: " : L"ルートウィンドウ: ")
            << FormatWindowHandle(focus.focusedRoot)
            << L" class=" << ReadWindowClassName(focus.focusedRoot) << L"\r\n";
    }

    if (focus.hasEditSelection) {
        const unsigned long long selChars = (focus.selEnd >= focus.selStart)
            ? static_cast<unsigned long long>(focus.selEnd - focus.selStart)
            : static_cast<unsigned long long>(focus.selStart - focus.selEnd);
        oss << (english ? L"Focus position: " : L"フォーカス位置: ");
        if (focus.line >= 0 && focus.column >= 0) {
            oss << (english ? L"line " : L"行 ")
                << (focus.line + 1)
                << (english ? L", column " : L"、列 ")
                << (focus.column + 1);
        } else {
            oss << (english ? L"selection" : L"選択位置");
        }
        oss << L"\r\n";
        oss << (english ? L"Selection: " : L"選択範囲: ")
            << focus.selStart << L"-" << focus.selEnd
            << L" (" << selChars << (english ? L" chars)" : L" 文字)") << L"\r\n";
        if (focus.firstVisibleLine >= 0) {
            oss << (english ? L"First visible line: " : L"先頭表示行: ")
                << (focus.firstVisibleLine + 1) << L"\r\n";
        }
    } else if (focus.pdfFocused && g_pdf.pageCount > 0) {
        const int page = std::clamp(PageAtCurrentView(), 0, std::max(0, g_pdf.pageCount - 1));
        oss << (english ? L"Focus position: page " : L"フォーカス位置: ページ ")
            << (page + 1) << L"/" << g_pdf.pageCount
            << (english ? L", scroll=(" : L"、スクロール=(")
            << static_cast<long long>(std::llround(g_pdf.scrollX))
            << L"," << static_cast<long long>(std::llround(g_pdf.scrollY))
            << L")\r\n";
    } else if (focus.hasFocusedRect) {
        POINT center{
            focus.focusedRect.left + (focus.focusedRect.right - focus.focusedRect.left) / 2,
            focus.focusedRect.top + (focus.focusedRect.bottom - focus.focusedRect.top) / 2
        };
        oss << (english ? L"Focus position: window center " : L"フォーカス位置: ウィンドウ中心 ")
            << FormatPointValue(center) << L"\r\n";
    }

    if (focus.caretHwnd) {
        oss << (english ? L"Caret window: " : L"キャレット対象: ")
            << FormatWindowHandle(focus.caretHwnd)
            << L" class=" << ReadWindowClassName(focus.caretHwnd) << L"\r\n";
    }
    if (focus.hasCaretRect) {
        oss << (english ? L"Caret client rect: " : L"キャレット座標(クライアント): ")
            << FormatRectValue(focus.caretClientRect) << L"\r\n";
        oss << (english ? L"Caret screen rect: " : L"キャレット座標(画面): ")
            << FormatRectValue(focus.caretScreenRect) << L"\r\n";
    }
}

static unsigned long long CountCurrentProcessThreads() {
    unsigned long long count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD currentPid = GetCurrentProcessId();
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == currentPid) {
                count = entry.cntThreads;
                break;
            }
            entry.dwSize = sizeof(entry);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return count;
}

static void AppendProcessLoadSection(std::wostringstream& oss, bool english, DebugResourceMonitorCtx* ctx) {
    oss << (english ? L"Main app process load" : L"本体プロセス負荷") << L"\r\n";

    FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
    unsigned long long cpu100ns = 0;
    unsigned long long uptimeMs = 0;
    bool processTimesOk = false;
    if (GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime)) {
        processTimesOk = true;
        cpu100ns = FileTimeToUInt64(kernelTime) + FileTimeToUInt64(userTime);
        FILETIME nowTime{};
        GetSystemTimeAsFileTime(&nowTime);
        const unsigned long long now100ns = FileTimeToUInt64(nowTime);
        const unsigned long long create100ns = FileTimeToUInt64(createTime);
        if (now100ns >= create100ns) {
            uptimeMs = (now100ns - create100ns) / 10000ULL;
        }
    }

    if (ctx && processTimesOk) {
        UpdateCpuUsageSample(cpu100ns,
                             &ctx->lastCpu100ns,
                             &ctx->lastCpuTickMs,
                             &ctx->lastCpuPercent,
                             &ctx->cpuSampleReady);
    }

    if (ctx && ctx->cpuSampleReady) {
        oss << (english ? L"CPU usage: " : L"CPU使用率: ")
            << FormatPercent(ctx->lastCpuPercent) << L"\r\n";
    } else {
        oss << (english ? L"CPU usage: sampling..." : L"CPU使用率: サンプリング中...") << L"\r\n";
    }
    if (processTimesOk) {
        oss << (english ? L"CPU time: " : L"CPU時間: ") << FormatMillis(cpu100ns / 10000ULL) << L"\r\n";
        oss << (english ? L"Uptime: " : L"起動時間: ") << FormatMillis(uptimeMs) << L"\r\n";
    } else {
        oss << (english ? L"CPU time: unavailable" : L"CPU時間: 取得不可") << L"\r\n";
    }

    PROCESS_MEMORY_COUNTERS_EX memCounters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memCounters),
                             sizeof(memCounters))) {
        oss << (english ? L"Working set: " : L"ワーキングセット: ")
            << FormatMiB(memCounters.WorkingSetSize) << L"\r\n";
        oss << (english ? L"Peak working set: " : L"ピークワーキングセット: ")
            << FormatMiB(memCounters.PeakWorkingSetSize) << L"\r\n";
        oss << (english ? L"Private bytes: " : L"プライベートバイト: ")
            << FormatMiB(memCounters.PrivateUsage) << L"\r\n";
    } else {
        oss << (english ? L"Process memory: unavailable" : L"プロセスメモリ: 取得不可") << L"\r\n";
    }

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus)) {
        oss << (english ? L"System memory load: " : L"システムメモリ負荷: ")
            << FormatCount(static_cast<unsigned long long>(memoryStatus.dwMemoryLoad)) << L"%\r\n";
        oss << (english ? L"System memory available: " : L"システム空きメモリ: ")
            << FormatMiB(memoryStatus.ullAvailPhys) << L"\r\n";
    } else {
        oss << (english ? L"System memory: unavailable" : L"システムメモリ: 取得不可") << L"\r\n";
    }

    DWORD handleCount = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handleCount)) {
        oss << (english ? L"Handles: " : L"ハンドル数: ")
            << FormatCount(static_cast<unsigned long long>(handleCount)) << L"\r\n";
    }
    oss << (english ? L"Threads: " : L"スレッド数: ")
        << FormatCount(CountCurrentProcessThreads()) << L"\r\n";
    oss << (english ? L"GDI objects: " : L"GDIオブジェクト: ")
        << FormatCount(static_cast<unsigned long long>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS))) << L"\r\n";
    oss << (english ? L"USER objects: " : L"USERオブジェクト: ")
        << FormatCount(static_cast<unsigned long long>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS))) << L"\r\n";
}

static void AppendRelatedProcessLoadSection(std::wostringstream& oss,
                                            bool english,
                                            DebugResourceMonitorCtx* ctx,
                                            const AggregatedProcessLoad& load) {
    oss << (english ? L"Related app total load" : L"周辺ソフト込み合算負荷") << L"\r\n";
    oss << (english ? L"Processes: " : L"対象プロセス数: ")
        << FormatCount(load.processCount) << L"\r\n";
    if (!load.labels.empty()) {
        oss << (english ? L"Included: " : L"対象: ");
        for (size_t i = 0; i < load.labels.size(); ++i) {
            if (i != 0) oss << L", ";
            oss << load.labels[i];
        }
        oss << L"\r\n";
    }

    if (ctx && load.hasProcessTimes) {
        UpdateCpuUsageSample(load.cpu100ns,
                             &ctx->lastSuiteCpu100ns,
                             &ctx->lastSuiteCpuTickMs,
                             &ctx->lastSuiteCpuPercent,
                             &ctx->suiteCpuSampleReady,
                             &load.sampleKey,
                             &ctx->lastSuiteSampleKey);
    }

    if (ctx && ctx->suiteCpuSampleReady) {
        oss << (english ? L"CPU usage: " : L"CPU使用率: ")
            << FormatPercent(ctx->lastSuiteCpuPercent) << L"\r\n";
    } else {
        oss << (english ? L"CPU usage: sampling..." : L"CPU使用率: サンプリング中...") << L"\r\n";
    }

    if (load.hasProcessTimes) {
        oss << (english ? L"CPU time: " : L"CPU時間: ")
            << FormatMillis(load.cpu100ns / 10000ULL) << L"\r\n";
        if (load.hasUptime) {
            oss << (english ? L"Suite uptime: " : L"合算起動時間: ")
                << FormatMillis(load.uptimeMs) << L"\r\n";
        }
    } else {
        oss << (english ? L"CPU time: unavailable" : L"CPU時間: 取得不可") << L"\r\n";
    }

    if (load.hasMemory) {
        oss << (english ? L"Working set: " : L"ワーキングセット: ")
            << FormatMiB(load.workingSetBytes) << L"\r\n";
        oss << (english ? L"Peak working set: " : L"ピークワーキングセット: ")
            << FormatMiB(load.peakWorkingSetBytes) << L"\r\n";
        oss << (english ? L"Private bytes: " : L"プライベートバイト: ")
            << FormatMiB(load.privateBytes) << L"\r\n";
    } else {
        oss << (english ? L"Process memory: unavailable" : L"プロセスメモリ: 取得不可") << L"\r\n";
    }

    if (load.hasHandles) {
        oss << (english ? L"Handles: " : L"ハンドル数: ")
            << FormatCount(load.handles) << L"\r\n";
    }
    oss << (english ? L"Threads: " : L"スレッド数: ")
        << FormatCount(load.threads) << L"\r\n";
    if (load.hasGuiObjects) {
        oss << (english ? L"GDI objects: " : L"GDIオブジェクト: ")
            << FormatCount(load.gdiObjects) << L"\r\n";
        oss << (english ? L"USER objects: " : L"USERオブジェクト: ")
            << FormatCount(load.userObjects) << L"\r\n";
    }
}

static void AppendAppStateSection(std::wostringstream& oss, bool english) {
    oss << (english ? L"App state" : L"アプリ状態") << L"\r\n";
    oss << (english ? L"Developer mode: " : L"開発者モード: ")
        << (g_config.developerMode ? L"ON" : L"OFF") << L"\r\n";
    oss << (english ? L"Workspace: " : L"ワークスペース: ")
        << (g_workspaceRoot.empty() ? (english ? L"(none)" : L"(なし)") : g_workspaceRoot) << L"\r\n";
    oss << (english ? L"Document kind: " : L"文書種別: ");
    switch (g_pdf.kind) {
    case DocKind::Pdf:
        oss << L"PDF";
        break;
    case DocKind::Image:
        oss << (english ? L"Image" : L"画像");
        break;
    case DocKind::None:
    default:
        oss << (english ? L"None" : L"なし");
        break;
    }
    oss << L"\r\n";
    oss << (english ? L"PDF pages: " : L"PDFページ数: ")
        << FormatCount(static_cast<unsigned long long>(std::max(0, g_pdf.pageCount))) << L"\r\n";
    oss << (english ? L"Rendered page slots: " : L"ページ描画枠: ")
        << FormatCount(static_cast<unsigned long long>(g_pdf.pages.size())) << L"\r\n";
    oss << (english ? L"Annotations: " : L"注釈数: ")
        << FormatCount(static_cast<unsigned long long>(g_annots.size())) << L"\r\n";
    oss << (english ? L"Dirty annotations: " : L"未保存注釈: ")
        << FormatBoolWord(english, g_annotsDirty) << L"\r\n";
}

static std::wstring BuildDebugResourceMonitorTextForContext(DebugResourceMonitorCtx* ctx) {
    const bool english = IsEnglishUi();
    const FocusSnapshot focus = ResolveMonitorFocusSnapshot(ctx);
    std::wostringstream oss;
    oss << (english ? L"Local resource snapshot" : L"ローカルリソーススナップショット") << L"\r\n";
    oss << (english ? L"No data is sent or written." : L"この情報は送信・保存しません。") << L"\r\n\r\n";
    oss << (english ? L"Updated: " : L"更新時刻: ") << FormatLocalTimestamp() << L"\r\n\r\n";
    AppendProcessLoadSection(oss, english, ctx);
    AggregatedProcessLoad relatedLoad{};
    if (CollectRelatedProcessLoad(&relatedLoad) && relatedLoad.hasCompanionProcess) {
        oss << L"\r\n";
        AppendRelatedProcessLoadSection(oss, english, ctx, relatedLoad);
    }
    oss << L"\r\n";
    AppendAppStateSection(oss, english);
    oss << L"\r\n";
    AppendFocusSection(oss, english, focus);
    return oss.str();
}

static void LayoutDebugResourceMonitorWindow(HWND hWnd, DebugResourceMonitorCtx* ctx) {
    if (!ctx || !ctx->text) return;
    RECT rc{};
    if (!GetClientRect(hWnd, &rc)) return;
    const int clientWidth = static_cast<int>(rc.right - rc.left);
    const int clientHeight = static_cast<int>(rc.bottom - rc.top);
    const int width = std::max(1, clientWidth - kDebugResourceMonitorPadding * 2);
    const int height = std::max(1, clientHeight - kDebugResourceMonitorPadding * 2);
    MoveWindow(ctx->text, kDebugResourceMonitorPadding, kDebugResourceMonitorPadding, width, height, TRUE);
}

static bool ShouldDeferMonitorTextUpdate(const DebugResourceMonitorCtx* ctx,
                                         DWORD* outSelStart,
                                         DWORD* outSelEnd,
                                         int* outFirstVisibleLine) {
    if (outSelStart) *outSelStart = 0;
    if (outSelEnd) *outSelEnd = 0;
    if (outFirstVisibleLine) *outFirstVisibleLine = 0;
    if (!ctx || !ctx->text || !IsWindow(ctx->text)) return false;

    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(ctx->text, EM_GETSEL,
                 reinterpret_cast<WPARAM>(&selStart),
                 reinterpret_cast<LPARAM>(&selEnd));
    const int firstVisibleLine = static_cast<int>(SendMessageW(ctx->text, EM_GETFIRSTVISIBLELINE, 0, 0));
    if (outSelStart) *outSelStart = selStart;
    if (outSelEnd) *outSelEnd = selEnd;
    if (outFirstVisibleLine) *outFirstVisibleLine = firstVisibleLine;

    if (GetFocus() != ctx->text) return false;
    if (selStart != selEnd) return true;
    if (GetCapture() == ctx->text) return true;
    if ((GetKeyState(VK_LBUTTON) & 0x8000) != 0) return true;
    return false;
}

static void UpdateDebugResourceMonitorText(DebugResourceMonitorCtx* ctx) {
    if (!ctx || !ctx->text || !IsWindow(ctx->text)) return;
    std::wstring next = BuildDebugResourceMonitorTextForContext(ctx);
    if (next == ctx->lastText && !ctx->hasPendingText) return;

    DWORD selStart = 0;
    DWORD selEnd = 0;
    int firstVisibleLine = 0;
    if (ShouldDeferMonitorTextUpdate(ctx, &selStart, &selEnd, &firstVisibleLine)) {
        ctx->pendingText = std::move(next);
        ctx->hasPendingText = true;
        return;
    }

    if (ctx->hasPendingText) {
        next = std::move(ctx->pendingText);
        ctx->pendingText.clear();
        ctx->hasPendingText = false;
    }
    if (next == ctx->lastText) return;

    SendMessageW(ctx->text, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(ctx->text, next.c_str());

    const DWORD safeSelStart = std::min<DWORD>(selStart, static_cast<DWORD>(next.size()));
    const DWORD safeSelEnd = std::min<DWORD>(selEnd, static_cast<DWORD>(next.size()));
    if (safeSelStart != 0 || safeSelEnd != 0) {
        SendMessageW(ctx->text, EM_SETSEL, safeSelStart, safeSelEnd);
    }
    const int currentFirstVisibleLine = static_cast<int>(SendMessageW(ctx->text, EM_GETFIRSTVISIBLELINE, 0, 0));
    if (firstVisibleLine != currentFirstVisibleLine) {
        SendMessageW(ctx->text, EM_LINESCROLL, 0, firstVisibleLine - currentFirstVisibleLine);
    }
    SendMessageW(ctx->text, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ctx->text, nullptr, TRUE);
    UpdateWindow(ctx->text);
    ctx->lastText = std::move(next);
}

static LRESULT CALLBACK DebugResourceMonitorWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DebugResourceMonitorCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* create = cs ? reinterpret_cast<DebugResourceMonitorCreateParams*>(cs->lpCreateParams) : nullptr;
        auto* newCtx = new DebugResourceMonitorCtx();
        newCtx->owner = create ? create->owner : nullptr;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newCtx));
        ctx = newCtx;
        ctx->text = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            create ? create->initialText.c_str() : L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 1, 1,
            hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDebugResourceMonitorEditId)),
            g_hInst,
            nullptr);
        if (ctx->text) {
            SetUIFont(ctx->text);
        }
        ApplyThemeToDialog(hWnd);
        LayoutDebugResourceMonitorWindow(hWnd, ctx);
        UpdateDebugResourceMonitorText(ctx);
        SetTimer(hWnd, kDebugResourceMonitorTimerId, kDebugResourceMonitorRefreshMs, nullptr);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (mmi) {
            mmi->ptMinTrackSize.x = kDebugResourceMonitorMinWidth;
            mmi->ptMinTrackSize.y = kDebugResourceMonitorMinHeight;
            return 0;
        }
        break;
    }
    case WM_SIZE:
        LayoutDebugResourceMonitorWindow(hWnd, ctx);
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
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_TIMER:
        if (wParam == kDebugResourceMonitorTimerId) {
            if (!ctx || !ctx->owner || !IsWindow(ctx->owner)) {
                DestroyWindow(hWnd);
                return 0;
            }
            UpdateDebugResourceMonitorText(ctx);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, kDebugResourceMonitorTimerId);
        return 0;
    case WM_NCDESTROY:
        if (ctx) {
            delete ctx;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        if (g_hDebugResourceMonitorWnd == hWnd) g_hDebugResourceMonitorWnd = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

std::wstring DebugMenuLabel() {
    return IsEnglishUi() ? L"Debug" : L"デバッグ";
}

std::wstring DebugArchiveLogsMenuLabel() {
    return IsEnglishUi() ? L"Archive Logs as ZIP" : L"ログをZIP保存";
}

std::wstring DebugDeleteLogsMenuLabel() {
    return IsEnglishUi() ? L"Delete Log Files" : L"ログを削除";
}

std::wstring DebugToggleLogsMenuLabel(bool allEnabled, bool anyEnabled) {
    if (IsEnglishUi()) {
        if (anyEnabled && !allEnabled) return L"Debug Logs on Next Start: MIXED";
        return allEnabled ? L"Debug Logs on Next Start: ON" : L"Debug Logs on Next Start: OFF";
    }
    if (anyEnabled && !allEnabled) return L"次回起動時のデバッグログ: 一部ON";
    return allEnabled ? L"次回起動時のデバッグログ: ON" : L"次回起動時のデバッグログ: OFF";
}

std::wstring DebugResourceMonitorMenuLabel() {
    return IsEnglishUi() ? L"Resource Monitor" : L"リソースモニター";
}

std::wstring BuildDebugResourceMonitorText() {
    return BuildDebugResourceMonitorTextForContext(nullptr);
}

void ShowDebugResourceMonitorWindow(HWND owner, const std::wstring& initialText) {
    HWND effectiveOwner = owner && IsWindow(owner) ? owner : g_hMainWnd;
    if (g_hDebugResourceMonitorWnd && IsWindow(g_hDebugResourceMonitorWnd)) {
        auto* ctx = reinterpret_cast<DebugResourceMonitorCtx*>(
            GetWindowLongPtrW(g_hDebugResourceMonitorWnd, GWLP_USERDATA));
        if (ctx && effectiveOwner) ctx->owner = effectiveOwner;
        ShowWindow(g_hDebugResourceMonitorWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hDebugResourceMonitorWnd);
        UpdateDebugResourceMonitorText(ctx);
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DebugResourceMonitorWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush
                                           : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kDebugResourceMonitorWndClass;
    static bool registered = false;
    if (!registered) {
        RegisterClassExW(&wc);
        registered = true;
    }

    DebugResourceMonitorCreateParams create{};
    create.owner = effectiveOwner;
    create.initialText = initialText.empty() ? BuildDebugResourceMonitorText() : initialText;
    g_hDebugResourceMonitorWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kDebugResourceMonitorWndClass,
        DebugResourceMonitorMenuLabel().c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 520,
        effectiveOwner, nullptr, g_hInst, &create);
    if (g_hDebugResourceMonitorWnd) {
        ShowWindow(g_hDebugResourceMonitorWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hDebugResourceMonitorWnd);
    }
}
