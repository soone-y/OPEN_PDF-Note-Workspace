// file: main.cpp
#include "core/app_core.h"
#include "core/fault_injection.h"
#include "core/preview_trace.h"
#include "core/font_list.h"
#include "core/private_fonts.h"
#include "help/help.h"
#include "pdf_view/pdf_view.h"
#include "note_view/note_view.h"
#include "note/note_identity_store.h"
#include "file_output/file_output.h"
#include "search/search.h"
#include "schedule/schedule.h"
#include "settings/settings.h"
#include "resources/app_resource.h"
#include "ui/toolbar.h"
#include "ui/combobox_guard.h"
#include "ui/noop_nav_guard.h"
#include "clrop/bridge.h"
#include "ui/splitter.h"
#include "core/atomic_write.h"
#include "workspace/workspace_config_io.h"
#include "ui/core/main_window_api.h"
#include "workspace/workspace_actions.h"
#include "ui/dialogs/dialogs.h"
#include "workspace/workspace_tree.h"
#include "workspace/file_ops.h"
#include "workspace/workspace_write_lock.h"
#include "app/main_close_policy.h"
#include "ui/menus/main_debug_menu.h"
#include "app/main_escape_backup.h"

bool HandleAnnotToolShortcutInLoop(HWND owner, const MSG& msg);
bool HandleFixedAnnotToolNavigationShortcutInLoop(HWND owner, const MSG& msg);
bool HandleAnnotColorCycleShortcutInLoop(HWND owner, const MSG& msg);
bool HandleMainPdfZoomShortcutInLoop(HWND owner, const MSG& msg);
#include "ui/menus/main_menu_owner_draw.h"
#include "ui/menus/main_menu_snapshot.h"
#include "ui/menus/main_status_display.h"
#include "workspace/main_workspace_logs.h"
#include "ui/menus/menu_build.h"
#include "app/startup_instance.h"
#include "ui/core/wheel_routing.h"
#include "ui/dialogs/annot_math_panel.h"
#include "note_view/bottom_math_panel.h"
#include "ui/dialogs/export_dialog.h"
#include "office/docx_space_protection.h"
#include "fpdf_edit.h"
#include "fpdf_save.h"
#include <richedit.h>
#include <commctrl.h>
#include <shlobj.h>
#include <imm.h>
#include <climits>
#include <chrono>
#include <iterator>
#include <regex>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

void LayoutChildren(HWND hWnd);
static void LayoutChildrenLive(HWND hWnd);
static void LayoutChildrenImpl(HWND hWnd, ui::LayoutPass pass);
void SyncBottomPaneAfterNoteLoad(HWND hWnd);
void AutoOpenSingleSessionFiles(HWND hWnd);
static bool ActivateExistingInstance();
static HANDLE g_hSingleInstanceMutex = nullptr;
HANDLE g_hSingleInstanceReadyEvent = nullptr;
static constexpr DWORD kStartupWindowTimeoutMs = 15000;
static HANDLE g_hStartupWatchdogThread = nullptr;
static DWORD WINAPI StartupWatchdogThread(LPVOID);
static void AppendStartupWatchdogLog(const char* line);
static HANDLE g_hStartupAbortAckEvent = nullptr;
static DWORD g_mainThreadId = 0;
// MainWndProc WM_APP message; keep value in the central registry in core/app_core.h.
// Main thread WM_APP message; keep value in the central registry in core/app_core.h.
static HANDLE g_hSingleInstanceShutdownRequestEvent = nullptr;
static HANDLE g_hSingleInstanceShutdownStopEvent = nullptr;
static HANDLE g_hSingleInstanceShutdownThread = nullptr;
static DWORD WINAPI SingleInstanceShutdownRequestThread(LPVOID);
// MainWndProc timer ID; keep unique with the MainWndProc timer registry in core/app_core.h.
static constexpr UINT_PTR kExitRetryTimerId = 0x5E11;
static bool s_exitInProgress = false;
static bool s_exitPending = false;
static constexpr int kManagedAbnormalExitCode = 3;
struct ManagedAbnormalExitState {
    bool requested = false;
    bool completed = false;
    bool reportPending = false;
    int exitCode = 0;
    std::wstring triggerTitle;
    std::wstring triggerMessage;
    std::wstring reportText;
};
static ManagedAbnormalExitState s_managedAbnormalExit;
static bool s_inSizeMove = false;
bool s_ignoreLectureSelChange = false;
static bool s_skipAutoChildFocusOnce = false;
enum class ListClickKind {
    Pdf,
    Note
};

struct ListClickState {
    ListClickKind kind = ListClickKind::Pdf;
    int preSel = -1;
    int hitIndex = -1;
    bool tracking = false;
    bool suppressNextScroll = false;
    int pendingSel = -1;
    std::wstring openPath; // snapshot at WM_LBUTTONDOWN to avoid "newly opened file" false positives
};

static ListClickState s_pdfListClick{ListClickKind::Pdf};
static ListClickState s_noteListClick{ListClickKind::Note};
std::unordered_set<std::wstring> s_searchTempPdfKeys;
std::unordered_set<std::wstring> s_searchTempNoteKeys;
struct DirectoryListDblClickState {
    bool pending = false;
    bool cancelled = false;
    int hitIndex = -1;
    POINT start{};
};

static DirectoryListDblClickState s_lectureListDblClick;
static DirectoryListDblClickState s_sessionListDblClick;
struct SessionOrganizeStats;
static int  CalcListItemHeight();
static void OrganizeCurrentSessionFiles(HWND hWnd);
static SessionOrganizeStats NormalizeCacheSession(const std::filesystem::path& sessionRoot);
static void OnLectureSelChange(HWND hWnd);
static void OnSessionSelChange(HWND hWnd);
static void OnPdfSelChange(HWND hWnd);
static void OnNoteSelChange(HWND hWnd);
void ReloadSessionsAndSelect(const std::wstring& lecturePath,
                                    const std::wstring& desiredName,
                                    bool reopenFiles);
void RefreshStatusDisplay(HWND hWnd);
int CurrentLectureIndex();
int CurrentSessionIndex();
int CurrentPdfIndex();
int CurrentNoteIndex();
static bool HandleAppExit(HWND hWnd);
static bool HandleManagedAbnormalExit(HWND hWnd);
static void FinalizeAppExitState(HWND hWnd, const std::wstring& previewTempPath);
static void ClearManagedAbnormalExitRequest();
static std::wstring BuildManagedAbnormalExitReportText(bool noteHadDirty,
                                                       bool noteStageOk,
                                                       bool annotHadDirty,
                                                       bool annotStageOk);
static bool PrepareWorkspaceRestart(HWND hWnd);
bool SaveStartupLastOpenTarget();
static bool SaveSessionLastOpenMap();
void LoadSessionLastOpenMap();
static bool RestoreStartupLastOpenSelection(HWND hWnd);
std::optional<std::wstring> PickFileUnder(HWND owner,
                                                 const std::filesystem::path& initial,
                                                 const std::wstring& title);
std::vector<std::wstring> PickFilesUnder(HWND owner,
                                                const std::filesystem::path& initial,
                                                const std::wstring& title);
std::vector<std::wstring> PickOfficeFilesUnder(HWND owner,
                                                      const std::filesystem::path& initial,
                                                      const std::wstring& title);
static std::filesystem::path DialogContextInitialFolder();
std::filesystem::path DialogWorkspaceInitialFolder();
namespace {
}
void LoadLectures();
static void SetBottomPanePinMode(BottomPanePin pin, BottomNoteMode noteMode);
static void ReloadWorkspaceFromRoot(HWND hWnd, const std::wstring& root, bool promptClassdir, bool persistConfig);
static void ScrollPdfToFileStart();
static void ScrollNoteToFileStart();
static std::wstring SessionKeyFromPath(const std::wstring& sessionPath);
static bool SetListboxSelAndNotify(HWND list, int newSel);
static bool IsPotentialNetworkPath(const std::filesystem::path& path);
static bool OpenNoteIfDifferent(HWND hWnd, const std::wstring& path);
bool OpenPdfIfDifferent(HWND hWnd, const std::wstring& path);
void LoadSessions(const std::wstring& lecturePath);
void LoadFiles(const SessionEntry& session, std::wstring& preferredPdf, std::wstring& preferredNote);
bool ValidateCreateFileSystemName(HWND owner,
                                  const std::wstring& name,
                                  const std::wstring& title);
bool IsWorkspaceReservedImportDirectoryName(const std::filesystem::path& path);
void EnterNoteNormalMode(HWND hWnd);
void CancelPendingLinkMode(HWND owner);
static void PreparePendingLinkForPdfSwitch(HWND owner);
static void FinalizePendingLinkModeIfReady(HWND owner);
static bool EnsureListboxSelection(HWND list);
static bool ShowDirectoryListContextMenu(HWND list, LPARAM lParam);
static bool ShowDirectoryHierarchyPopup(HWND owner,
                                        const std::filesystem::path& root,
                                        const std::wstring& label,
                                        POINT screenPt);
static bool IsOfficeFileListPath(const std::wstring& path);
static bool CanOpenPdfListPathInApp(const std::wstring& path);
static std::wstring FirstOpenablePdfListPath();
static void RestorePdfListSelectionToCurrent();
static bool PromptOfficeFileListAction(HWND owner, const std::wstring& officePath);
static LRESULT CALLBACK PdfNoteListProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK DirectoryListProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR, DWORD_PTR);
static bool IsExplicitImeTargetWindow(HWND hWnd);
void EnforceImePolicyForWindow(HWND hWnd);
bool ShouldSkipImeMessageInLoop(const MSG& msg);
static bool DrawLeftPaneListItem(const DRAWITEMSTRUCT* dis);
static std::wstring NormalizePathKeyForList(const std::wstring& path);

namespace {
}

static bool PromptStayOrOpenDiffManager(HWND hWnd,
                                        const std::wstring& blockedAction,
                                        const std::wstring& failureDetail) {
    // A blocked switch can be the last action before the user restarts the app.
    // Keep the PDF restore point current even though the document itself stays open.
    SaveLastPdfViewInfo();
    if (!hWnd) return false;
    if (!file_output::HasAnyStagedDiffs()) {
        std::wstring msg = blockedAction;
        msg += L"を中止しました。\n現在のファイルはそのままです。";
        if (!failureDetail.empty()) msg += L"\n\n" + failureDetail;
        msg += L"\n\n差分管理で扱える未統合差分はまだありません。";
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Warning);
        return false;
    }

    SilentDialogOptions dialog;
    dialog.title = IsEnglishUi() ? L"Switch canceled" : L"切替を中止しました";
    dialog.message = blockedAction +
        (IsEnglishUi()
             ? L" was canceled because saving or integrating staged changes did not complete.\n"
               L"The current file stays open.\n\n"
               L"You can resolve staged diffs safely in Diff Manager."
             : L" の前に、保存または未統合差分の処理を完了できなかったため中止しました。\n"
               L"現在のファイルはそのままです。\n\n"
               L"未統合差分は「操作」→「差分管理」から安全に整理できます。");
    if (!failureDetail.empty()) {
        dialog.message += L"\n\n";
        dialog.message += failureDetail;
    }
    const std::wstring stagedLocations = file_output::FormatStagedDiffLocationSummary();
    if (!stagedLocations.empty()) {
        dialog.message += L"\n\n";
        dialog.message += stagedLocations;
    }
    dialog.kind = SoftNoticeKind::Warning;
    dialog.buttons = SilentDialogButtons::YesNo;
    dialog.defaultResult = SilentDialogResult::Yes;
    dialog.escapeResult = SilentDialogResult::No;
    dialog.yesLabel = IsEnglishUi() ? L"Open Diff Manager" : L"差分管理を開く";
    dialog.noLabel = IsEnglishUi() ? L"Stay Here" : L"ここに留まる";
    if (ShowSilentDialog(hWnd, dialog) == SilentDialogResult::Yes) {
        ShowStageManagerDialog(hWnd);
    }
    return false;
}

std::unordered_map<std::wstring, std::wstring> g_lastNoteBySession;
std::unordered_map<std::wstring, std::wstring> g_lastPdfBySession;
std::wstring g_lastStartupLecturePathSaved;
std::wstring g_lastStartupSessionPathSaved;
const wchar_t kLectureSettingsDirName[] = L"__resource__";
static constexpr wchar_t kLectureLastOpenFileName[] = L"lecture_last_open.txt";
static bool g_scheduleSortTimeSet = false;
int g_scheduleSortDayIndex = 0;
int g_scheduleSortMinutes = 0;
static HBRUSH s_hShortcutTagBrush = nullptr;
static COLORREF s_shortcutTagBrushColor = CLR_INVALID;
static bool s_normalizingShortcutBody = false;


struct StartupLastOpenTarget {
    std::wstring lecturePath;
    std::wstring sessionPath;
};

std::vector<TempExternalLecture> g_tempExternalLectures;

struct MenuItemData {
    std::wstring text;
    bool separator = false;
    bool topLevel = false;
    bool rightJustify = false;
};

static std::vector<std::unique_ptr<MenuItemData>> g_menuItemData;

struct StatusDisplayStateSnapshot {
    std::wstring text;

    bool operator==(const StatusDisplayStateSnapshot& other) const {
        return text == other.text;
    }
};

static std::optional<MainMenuStateSnapshot> s_lastMainMenuStateSnapshot;
static std::optional<StatusDisplayStateSnapshot> s_lastStatusDisplayStateSnapshot;
static int s_deferredMainWindowUiRefreshDepth = 0;
static bool s_deferredMainWindowUiRefreshPending = false;
static HWND s_deferredMainWindowUiRefreshOwner = nullptr;


OfficeConversionProgressState s_officeConversionProgress;

HACCEL BuildAccelerators();
bool HandleMainPdfZoomShortcutInLoop(HWND owner, const MSG& msg);
bool HandleFixedAnnotToolNavigationShortcutInLoop(HWND owner, const MSG& msg);
bool HandleAnnotColorCycleShortcutInLoop(HWND owner, const MSG& msg);
void RefreshMainMenuBar(HWND hWnd);
void RefreshMainWindowUiState(HWND hWnd);
static bool HasExportablePdf();
static bool HasExportableNote();
static std::wstring BuildSaveStateStatusText();
static std::wstring BuildOfficeConversionProgressStatusText();
void BeginOfficeConversionProgress(HWND owner, size_t total,
                                          const std::filesystem::path& source);
void UpdateOfficeConversionProgress(HWND owner, size_t current, size_t total,
                                           const std::filesystem::path& source);
void PulseOfficeConversionProgress(HWND owner);
bool IsOfficeConversionCancelRequested();
void EndOfficeConversionProgress(HWND owner);
static void DisableIntegratedPdfPreview(HWND owner, bool restoreOriginalPdf);
static bool CanStartExportCommand(HWND hWnd, UINT id);
HWND MainDialogOwner(HWND owner);
static void ShowMainSoftNotice(HWND owner, const std::wstring& text,
                               SoftNoticeKind kind = SoftNoticeKind::Info);
void ShowMainMessageDialog(HWND owner, const std::wstring& title,
                                  const std::wstring& message, SoftNoticeKind kind);
void AppendMainOperationExceptionLog(const char* area, const char* detail);
void ReportMainOperationException(HWND owner, const wchar_t* operation);
void AppendUiAutomationTrace(const std::wstring& line);
static void WriteUiAutomationResult(bool ok, const std::wstring& detail);
static void RestorePreviousNoteAfterException(HWND owner, const std::wstring& previousNotePath);
static void RestorePreviousPdfAfterException(HWND owner, const std::wstring& previousPdfPath);
static bool RunUiAutomationScenarios(HWND owner, std::wstring* outError);
static bool RunUiAutomationMathRenderScenario(HWND owner, std::wstring* outError);
static void RestoreLectureSessionStateAfterException(HWND owner,
                                                     const std::wstring& previousLecturePath,
                                                     const std::wstring& previousSessionPath,
                                                     const std::wstring& previousPdfPath,
                                                     const std::wstring& previousNotePath);
bool ConfirmMainYesNo(HWND owner, const std::wstring& title, const std::wstring& message,
                             SoftNoticeKind kind, SilentDialogResult defaultResult,
                             SilentDialogResult escapeResult);
static void ArchiveWorkspaceLogFiles(HWND owner);
static void DeleteWorkspaceLogFiles(HWND owner);
static void ToggleAllDebugLogs(HWND owner);

struct ScopedDeferredMainWindowUiRefresh {
    explicit ScopedDeferredMainWindowUiRefresh(HWND owner) {
        HWND resolvedOwner = g_hMainWnd ? g_hMainWnd : owner;
        if (resolvedOwner) {
            s_deferredMainWindowUiRefreshOwner = resolvedOwner;
        }
        ++s_deferredMainWindowUiRefreshDepth;
    }

    ~ScopedDeferredMainWindowUiRefresh() {
        if (s_deferredMainWindowUiRefreshDepth <= 0) return;
        --s_deferredMainWindowUiRefreshDepth;
        if (s_deferredMainWindowUiRefreshDepth != 0 || !s_deferredMainWindowUiRefreshPending) return;
        HWND owner = s_deferredMainWindowUiRefreshOwner;
        s_deferredMainWindowUiRefreshPending = false;
        s_deferredMainWindowUiRefreshOwner = nullptr;
        if (owner) {
            RefreshMainWindowUiState(owner);
        }
    }
};

struct ScopedDeferredLeftPaneSelection {
    ScopedDeferredLeftPaneSelection() {
        BeginDeferredLeftPaneSelection();
    }

    ~ScopedDeferredLeftPaneSelection() {
        EndDeferredLeftPaneSelection();
    }
};
std::filesystem::path CanonicalOrSelf(const std::filesystem::path& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    return ec ? p : c;
}

// MainWndProc WM_APP message; keep value in the central registry in core/app_core.h.
static constexpr UINT kMsgListMaybeScroll = WM_APP + 120;
static constexpr UINT_PTR kListReselectTimerId = 0x12001;
static constexpr bool kEnableSwitchTimingLogs = true;

static void EmitSwitchTimingLine(const std::wstring& line) {
    std::wstring out = line + L"\n";
    OutputDebugStringW(out.c_str());
    if (!kEnableSwitchTimingLogs) return;
    AppendAppLogLine(AppLogKind::SwitchTiming, line);
}

static std::wstring FormatTimingMs(double ms) {
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%.2f", ms);
    return std::wstring(buf);
}

static std::wstring ShortPathForTiming(const std::wstring& path) {
    if (path.empty()) return L"(empty)";
    try {
        std::filesystem::path p(path);
        if (!p.filename().empty()) return p.filename().wstring();
    } catch (...) {
    }
    return path;
}

class SwitchTimingScope {
public:
    SwitchTimingScope(const wchar_t* op, const std::wstring& targetPath)
        : m_op(op ? op : L"switch"),
          m_target(ShortPathForTiming(targetPath)),
          m_outcome(L"unfinished") {
        if (!kEnableSwitchTimingLogs) return;
        if (!QueryPerformanceFrequency(&m_freq)) return;
        QueryPerformanceCounter(&m_start);
        m_last = m_start;
        m_enabled = true;
        LogLine(L"begin", 0.0, 0.0);
    }

    void Mark(const wchar_t* step) {
        if (!m_enabled) return;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double lapMs = ElapsedMs(m_last, now);
        const double totalMs = ElapsedMs(m_start, now);
        LogLine(step ? step : L"step", lapMs, totalMs);
        m_last = now;
    }

    void SetOutcome(const wchar_t* outcome) {
        m_outcome = outcome ? outcome : L"unknown";
    }

    ~SwitchTimingScope() {
        if (!m_enabled) return;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double totalMs = ElapsedMs(m_start, now);
        std::wstring line = L"[switch_timing] op=" + m_op +
                            L" target=\"" + m_target + L"\"" +
                            L" result=" + m_outcome +
                            L" total_ms=" + FormatTimingMs(totalMs);
        EmitSwitchTimingLine(line);
    }

private:
    double ElapsedMs(const LARGE_INTEGER& from, const LARGE_INTEGER& to) const {
        const LONGLONG delta = to.QuadPart - from.QuadPart;
        return (1000.0 * static_cast<double>(delta)) / static_cast<double>(m_freq.QuadPart);
    }

    void LogLine(const wchar_t* step, double lapMs, double totalMs) const {
        std::wstring line = L"[switch_timing] op=" + m_op +
                            L" target=\"" + m_target + L"\"" +
                            L" step=" + (step ? step : L"step") +
                            L" lap_ms=" + FormatTimingMs(lapMs) +
                            L" total_ms=" + FormatTimingMs(totalMs);
        EmitSwitchTimingLine(line);
    }

    bool m_enabled = false;
    LARGE_INTEGER m_freq{};
    LARGE_INTEGER m_start{};
    LARGE_INTEGER m_last{};
    std::wstring m_op;
    std::wstring m_target;
    std::wstring m_outcome;
};

class SwitchBackdropGuard {
public:
    explicit SwitchBackdropGuard(HWND owner) : m_owner(owner) {}

    void AddWindow(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;
        for (const auto& e : m_entries) {
            if (e.hwnd == hwnd) return;
        }
        Entry e{};
        e.hwnd = hwnd;
        e.wasVisible = (IsWindowVisible(hwnd) != FALSE);
        m_entries.push_back(e);
    }

    void Activate() {
        if (m_active) return;
        bool anyHidden = false;
        for (const auto& e : m_entries) {
            if (!e.wasVisible) continue;
            ShowWindow(e.hwnd, SW_HIDE);
            anyHidden = true;
        }
        m_active = anyHidden;
        if (m_active) {
            RedrawWindow(ResolveOwner(), nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            PaintBackdropRects();
        }
    }

    ~SwitchBackdropGuard() {
        if (!m_active) return;
        for (const auto& e : m_entries) {
            if (!e.wasVisible) continue;
            ShowWindow(e.hwnd, SW_SHOWNA);
        }
        RedrawWindow(ResolveOwner(), nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

private:
    struct Entry {
        HWND hwnd = nullptr;
        bool wasVisible = false;
    };

    HWND ResolveOwner() const {
        if (m_owner && IsWindow(m_owner)) return m_owner;
        if (g_hMainWnd && IsWindow(g_hMainWnd)) return g_hMainWnd;
        if (!m_entries.empty()) {
            HWND root = GetAncestor(m_entries.front().hwnd, GA_ROOT);
            if (root && IsWindow(root)) return root;
        }
        return nullptr;
    }

    static COLORREF BackdropColorForWindow(HWND hwnd) {
        if (hwnd == g_hPdfView) {
            return g_theme.pdfBg;
        }
        if (hwnd == g_hNoteEdit || hwnd == g_hNoteRender) {
            return g_noteBgColor;
        }
        return g_theme.panelBg;
    }

    void PaintBackdropRects() const {
        HWND owner = ResolveOwner();
        if (!owner || !IsWindow(owner)) return;
        HDC hdc = GetDC(owner);
        if (!hdc) return;
        RECT ownerClient{};
        GetClientRect(owner, &ownerClient);
        for (const auto& e : m_entries) {
            if (!e.wasVisible || !e.hwnd || !IsWindow(e.hwnd)) continue;
            RECT rc{};
            if (!GetWindowRect(e.hwnd, &rc)) continue;
            MapWindowPoints(HWND_DESKTOP, owner, reinterpret_cast<LPPOINT>(&rc), 2);
            RECT clipped{};
            if (!IntersectRect(&clipped, &rc, &ownerClient)) continue;
            HBRUSH br = CreateSolidBrush(BackdropColorForWindow(e.hwnd));
            if (!br) continue;
            FillRect(hdc, &clipped, br);
            DeleteObject(br);
        }
        ReleaseDC(owner, hdc);
    }

    HWND m_owner = nullptr;
    std::vector<Entry> m_entries;
    bool m_active = false;
};

std::wstring ToLowerAscii(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

#include "workspace/main_workspace_logs.cppinc"

static std::wstring NormalizeStemLower(const std::filesystem::path& p) {
    return ToLowerAscii(p.stem().wstring());
}

static bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

static std::wstring StripSuffix(std::wstring s, const std::wstring& suffix) {
    if (EndsWith(s, suffix)) s.resize(s.size() - suffix.size());
    return s;
}

static int CommonPrefixLen(const std::wstring& a, const std::wstring& b) {
    int n = static_cast<int>(std::min(a.size(), b.size()));
    int i = 0;
    for (; i < n; ++i) {
        if (a[static_cast<size_t>(i)] != b[static_cast<size_t>(i)]) break;
    }
    return i;
}

static int MatchScore(const std::wstring& aStem, const std::wstring& bStem) {
    if (aStem.empty() || bStem.empty()) return 0;
    if (aStem == bStem) return 100000 + static_cast<int>(aStem.size());
    // Prefer strong inclusion relationships (common in "xxx_ノート" patterns).
    if (aStem.find(bStem) != std::wstring::npos || bStem.find(aStem) != std::wstring::npos) {
        return 50000 + static_cast<int>(std::min(aStem.size(), bStem.size()));
    }
    return CommonPrefixLen(aStem, bStem);
}

std::optional<std::filesystem::file_time_type> TryLastWriteTime(const std::filesystem::path& p) {
    std::error_code ect;
    auto t = std::filesystem::last_write_time(p, ect);
    if (ect) return std::nullopt;
    return t;
}

std::wstring BestMatchByStem(const std::filesystem::path& anchor,
                                    const std::vector<FileEntry>& candidates) {
    auto aStem = NormalizeStemLower(anchor);
    if (aStem.empty() || candidates.empty()) return L"";

    // Heuristics: allow typical suffix variations for notes.
    std::vector<std::wstring> stemVariants;
    stemVariants.reserve(8);
    stemVariants.push_back(aStem);
    stemVariants.push_back(StripSuffix(aStem, L"_ノート"));
    stemVariants.push_back(StripSuffix(aStem, L"ノート"));
    stemVariants.push_back(StripSuffix(aStem, L"_note"));
    stemVariants.push_back(StripSuffix(aStem, L"-note"));
    // Remove duplicates / empties cheaply.
    stemVariants.erase(std::remove_if(stemVariants.begin(), stemVariants.end(),
                                      [](const std::wstring& s) { return s.empty(); }),
                       stemVariants.end());
    std::sort(stemVariants.begin(), stemVariants.end());
    stemVariants.erase(std::unique(stemVariants.begin(), stemVariants.end()), stemVariants.end());

    int best = -1;
    std::optional<std::filesystem::file_time_type> bestTime;
    std::wstring bestPath;
    for (const auto& f : candidates) {
        std::filesystem::path p(f.path);
        auto bStem = NormalizeStemLower(p);
        int score = 0;
        for (const auto& v : stemVariants) {
            score = std::max(score, MatchScore(v, bStem));
        }
        if (score <= 0) continue;

        auto t = TryLastWriteTime(p);
        bool take = false;
        if (best < 0 || score > best) {
            take = true;
        } else if (score == best) {
            // Tie-break by newest mtime if available.
            if (t && (!bestTime || *t > *bestTime)) take = true;
        }
        if (take) {
            best = score;
            bestTime = t;
            bestPath = f.path;
        }
    }
    return bestPath;
}

#include "features/external_files/main_temp_external.cppinc"

static std::wstring SessionKeyFromPath(const std::wstring& sessionPath);

static bool HasClropFilesUnderPath(const std::filesystem::path& root) {
    if (root.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) return false;
    if (!std::filesystem::is_directory(root, ec) || ec) return false;

    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(it->path(), isReparse) && isReparse) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code stEc;
        if (!it->is_regular_file(stEc) || stEc) continue;
        std::wstring ext = it->path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".clrop") return true;
    }
    return false;
}

static bool ConfirmRecommendedPdfExportBeforeRemovingTempLecture(HWND owner,
                                                                 const std::filesystem::path& lectureRoot) {
    if (!HasClropFilesUnderPath(lectureRoot)) return true;

    std::wstring msg = IsEnglishUi()
        ? (std::wstring(
            L"This temporary external lecture contains .clrop annotation files.\n\n"
            L"Removing the path will keep those files on disk, but the annotations will not be embedded into the PDF itself.\n"
            L"It is recommended to export annotated PDFs before removing this path.\n\n"
            L"Continue removing the path now?"))
        : (std::wstring(g_config.studentMode
                            ? L"この一時外部授業には .clrop 注釈ファイルがあります。\n\n"
                            : L"この一時外部上位項目には .clrop 注釈ファイルがあります。\n\n") +
           L"パスを削除しても .clrop 自体は残りますが、注釈は PDF 本体へ統合されません。\n"
           L"削除前に、注釈入り PDF を別名で書き出しておくことをおすすめします。\n\n"
           L"このままパスの削除を続けますか？");
    return ConfirmMainYesNo(owner, GetUiText().menuRemoveTempExternalLecture, msg,
                            SoftNoticeKind::Info, SilentDialogResult::No,
                            SilentDialogResult::No);
}

static std::wstring FilenameOrPath(const std::filesystem::path& path) {
    if (!path.filename().empty()) return path.filename().wstring();
    return path.wstring();
}

static std::wstring RelativeOrPathForDisplay(const std::filesystem::path& path,
                                             const std::filesystem::path& root) {
    std::error_code ec;
    if (!root.empty()) {
        auto rel = std::filesystem::relative(path, root, ec);
        if (!ec && !rel.empty()) {
            return rel.wstring();
        }
    }
    return FilenameOrPath(path);
}

std::wstring DirectFilesSessionLabel() {
    return IsEnglishUi() ? L"(Direct files)" : L"直下ファイル";
}

static bool IsDirectFilesSessionPath(const std::wstring& sessionPath,
                                     const std::wstring& lecturePath) {
    if (sessionPath.empty() || lecturePath.empty()) return false;
    return SessionKeyFromPath(sessionPath) == SessionKeyFromPath(lecturePath);
}

std::wstring LectureDisplayLabelForPath(const std::wstring& lecturePath,
                                               const std::vector<std::wstring>& lecturePaths) {
    std::wstring base = TempExternalLectureLabel(lecturePath);
    if (!IsTempExternalLecturePath(lecturePath)) {
        std::filesystem::path path(lecturePath);
        base = FilenameOrPath(path);
    }

    size_t duplicates = 0;
    std::wstring baseKey = ToLowerAscii(base);
    for (const auto& item : lecturePaths) {
        std::wstring other = TempExternalLectureLabel(item);
        if (!IsTempExternalLecturePath(item)) {
            other = FilenameOrPath(std::filesystem::path(item));
        }
        if (ToLowerAscii(other) == baseKey) {
            ++duplicates;
        }
    }
    if (duplicates <= 1) return base;

    std::filesystem::path path(lecturePath);
    std::wstring parent = path.parent_path().wstring();
    if (parent.empty()) return base;
    return base + L" [" + parent + L"]";
}

std::wstring SessionDisplayLabelForEntry(const SessionEntry& session,
                                                const std::vector<SessionEntry>& sessions,
                                                const std::wstring& lecturePath) {
    if (IsDirectFilesSessionPath(session.path, lecturePath)) {
        return DirectFilesSessionLabel();
    }

    std::wstring base = session.displayName.empty()
        ? FilenameOrPath(std::filesystem::path(session.path))
        : session.displayName;
    size_t duplicates = 0;
    std::wstring baseKey = ToLowerAscii(base);
    for (const auto& item : sessions) {
        std::wstring other = item.displayName.empty()
            ? FilenameOrPath(std::filesystem::path(item.path))
            : item.displayName;
        if (ToLowerAscii(other) == baseKey) {
            ++duplicates;
        }
    }
    if (duplicates <= 1) return base;

    std::filesystem::path path(session.path);
    std::wstring parent = path.parent_path().wstring();
    if (parent.empty()) return base;
    return base + L" [" + parent + L"]";
}


static std::wstring SessionKeyFromPath(const std::wstring& sessionPath) {
    if (sessionPath.empty()) return L"";
    return CanonicalOrSelf(std::filesystem::path(sessionPath)).wstring();
}

static int FindLectureIndexByPath(const std::wstring& lecturePath) {
    if (lecturePath.empty()) return -1;
    std::wstring key = NormalizePathKeyForList(lecturePath);
    for (size_t i = 0; i < g_lectures.size(); ++i) {
        if (NormalizePathKeyForList(g_lectures[i]) == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static int FindSessionIndexByPath(const std::wstring& sessionPath) {
    if (sessionPath.empty()) return -1;
    std::wstring key = SessionKeyFromPath(sessionPath);
    for (size_t i = 0; i < g_sessions.size(); ++i) {
        if (SessionKeyFromPath(g_sessions[i].path) == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static std::wstring SessionKey(const SessionEntry& s) {
    return SessionKeyFromPath(s.path);
}

std::wstring NowTimestampString() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}


enum class SessionAutoOpenMode { Off, Edit, View };

static COLORREF BlendColor(COLORREF a, COLORREF b, double t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = static_cast<int>(std::lround(ar + (br - ar) * t));
    int g = static_cast<int>(std::lround(ag + (bg - ag) * t));
    int b2 = static_cast<int>(std::lround(ab + (bb - ab) * t));
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b2, 0, 255));
}

static MenuItemData* RegisterMenuItemData(const std::wstring& text, bool separator, bool topLevel, bool rightJustify) {
    auto data = std::make_unique<MenuItemData>();
    data->text = text;
    data->separator = separator;
    data->topLevel = topLevel;
    data->rightJustify = rightJustify;
    MenuItemData* raw = data.get();
    g_menuItemData.push_back(std::move(data));
    return raw;
}

static void ReleaseMenuItemData(MenuItemData* data) {
    if (!data) return;
    auto it = std::find_if(g_menuItemData.begin(), g_menuItemData.end(),
                           [&](const std::unique_ptr<MenuItemData>& p) { return p.get() == data; });
    if (it != g_menuItemData.end()) {
        g_menuItemData.erase(it);
    }
}

static std::wstring GetMenuItemTextByPos(HMENU menu, int pos) {
    int len = GetMenuStringW(menu, static_cast<UINT>(pos), nullptr, 0, MF_BYPOSITION);
    if (len <= 0) return L"";
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetMenuStringW(menu, static_cast<UINT>(pos), text.data(), len + 1, MF_BYPOSITION);
    if (copied < 0) copied = 0;
    text.resize(static_cast<size_t>(copied));
    return text;
}

static void ApplyMenuTheme(HMENU menu) {
    if (!menu) return;
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = g_hThemeMenuBrush ? g_hThemeMenuBrush : GetSysColorBrush(COLOR_MENU);
    SetMenuInfo(menu, &mi);
}

void ApplyMenuOwnerDraw(HMENU menu, bool topLevel) {
    if (!menu) return;
    ApplyMenuTheme(menu);
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_STATE;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
        bool separator = (mii.fType & MFT_SEPARATOR) != 0;
        std::wstring text = separator ? L"" : GetMenuItemTextByPos(menu, i);
        bool rightJustify = (mii.fType & MFT_RIGHTJUSTIFY) != 0;
        MenuItemData* data = RegisterMenuItemData(text, separator, topLevel, rightJustify);
        mii.fMask = MIIM_FTYPE | MIIM_DATA;
        mii.fType |= MFT_OWNERDRAW;
        mii.dwItemData = reinterpret_cast<ULONG_PTR>(data);
        SetMenuItemInfoW(menu, i, TRUE, &mii);
        if (mii.hSubMenu) {
            ApplyMenuOwnerDraw(mii.hSubMenu, false);
        }
    }
}

static void UpdateMenuItemText(HMENU menu, UINT id, const std::wstring& text) {
    if (!menu) return;
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_DATA;
    if (!GetMenuItemInfoW(menu, id, FALSE, &mii)) return;
    auto* data = reinterpret_cast<MenuItemData*>(mii.dwItemData);
    if (data) data->text = text;

    // Keep the underlying menu string in sync so DrawMenuBar can remeasure
    // right-justified owner-drawn items when the status text length changes.
    MENUITEMINFOW textInfo{};
    textInfo.cbSize = sizeof(textInfo);
    textInfo.fMask = MIIM_STRING;
    textInfo.dwTypeData = const_cast<LPWSTR>(text.c_str());
    textInfo.cch = static_cast<UINT>(text.size());
    SetMenuItemInfoW(menu, id, FALSE, &textInfo);
}

static bool MeasureThemedMenuItem(MEASUREITEMSTRUCT* mi) {
    if (!mi || mi->CtlType != ODT_MENU) return false;
    auto* data = reinterpret_cast<MenuItemData*>(mi->itemData);
    if (!data) return false;
    if (data->separator) {
        mi->itemHeight = 6;
        mi->itemWidth = 1;
        return true;
    }
    auto menuBarPadX = []() -> int {
        // Approximate native menu-bar padding; keep it compact.
        int edge = GetSystemMetrics(SM_CXEDGE);
        int pad = 8 + std::max(0, edge);
        return std::clamp(pad, 8, 14);
    };
    auto menuBarPadY = []() -> int {
        int edge = GetSystemMetrics(SM_CYEDGE);
        int pad = 4 + std::max(0, edge);
        return std::clamp(pad, 4, 10);
    };
    HDC hdc = GetDC(nullptr);
    if (!hdc) return false;
    HFONT old = g_hUIFont ? static_cast<HFONT>(SelectObject(hdc, g_hUIFont)) : nullptr;
    SIZE sz{};
    if (!data->text.empty()) {
        // Keep measurement consistent with DrawTextW (notably '&' prefix handling).
        RECT calc{ 0, 0, 0, 0 };
        const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_CALCRECT;
        DrawTextW(hdc, data->text.c_str(), -1, &calc, flags);
        sz.cx = std::max<LONG>(0, calc.right - calc.left);
        sz.cy = std::max<LONG>(0, calc.bottom - calc.top);
    }
    if (old) SelectObject(hdc, old);
    ReleaseDC(nullptr, hdc);

    int padX = data->topLevel ? menuBarPadX() : 28;
    int padY = data->topLevel ? menuBarPadY() : 4;
    UINT height = static_cast<UINT>(sz.cy + padY * 2);
    if (data->topLevel) {
        int sysH = GetSystemMetrics(SM_CYMENUSIZE);
        if (sysH > 0) height = std::max<UINT>(height, static_cast<UINT>(sysH));
    }
    mi->itemHeight = height;
    mi->itemWidth = static_cast<UINT>(sz.cx + padX * 2);
    return true;
}

static bool DrawThemedMenuItem(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_MENU) return false;
    auto* data = reinterpret_cast<MenuItemData*>(dis->itemData);
    if (!data) return false;
    RECT rc = dis->rcItem;
    HDC hdc = dis->hDC;
    if (data->separator) {
        int y = (rc.top + rc.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, g_theme.splitterLine);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, rc.left + 6, y, nullptr);
        LineTo(hdc, rc.right - 6, y);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        return true;
    }

    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF back = selected ? g_theme.menuSelBg : g_theme.menuBg;
    COLORREF text = selected ? g_theme.menuSelText : g_theme.menuText;
    if (disabled) {
        text = BlendColor(text, back, 0.5);
    }
    HBRUSH br = CreateSolidBrush(back);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);
    HFONT old = g_hUIFont ? static_cast<HFONT>(SelectObject(hdc, g_hUIFont)) : nullptr;

    RECT textRc = rc;
    auto menuBarPadX = []() -> int {
        int edge = GetSystemMetrics(SM_CXEDGE);
        int pad = 8 + std::max(0, edge);
        return std::clamp(pad, 8, 14);
    };
    if (!data->topLevel) {
        textRc.left += 20;
        if (dis->itemState & ODS_CHECKED) {
            RECT box{ rc.left + 6, rc.top + (rc.bottom - rc.top - 10) / 2,
                      rc.left + 16, rc.top + (rc.bottom - rc.top - 10) / 2 + 10 };
            HBRUSH boxFrame = CreateSolidBrush(g_theme.menuText);
            FrameRect(hdc, &box, boxFrame);
            DeleteObject(boxFrame);
            HPEN mark = CreatePen(PS_SOLID, 2, text);
            HGDIOBJ oldPen = SelectObject(hdc, mark);
            MoveToEx(hdc, box.left + 2, box.top + 5, nullptr);
            LineTo(hdc, box.left + 5, box.bottom - 2);
            LineTo(hdc, box.right - 2, box.top + 2);
            SelectObject(hdc, oldPen);
            DeleteObject(mark);
        }
    }
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT;
    if (data->topLevel) {
        int padX = menuBarPadX();
        textRc.left += padX;
        textRc.right -= padX;
        // Center-align top-level menu-bar items for a more native look.
        // (MFT_RIGHTJUSTIFY controls the item's position; keep right-justified items left-aligned for readability.)
        flags = DT_SINGLELINE | DT_VCENTER | (data->rightJustify ? DT_LEFT : DT_CENTER);
    }
    DrawTextW(hdc, data->text.c_str(), -1, &textRc, flags);
    if (old) SelectObject(hdc, old);
    return true;
}



SessionNumberingMode ParseSessionNumberingMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"max_number" || m == L"max" || m == L"maximum") return SessionNumberingMode::MaxNumberPlusOne;
    return SessionNumberingMode::CountPlusOne;
}


int NextSessionNumberForSuggestions(const std::vector<SessionEntry>& sessions,
                                           SessionNumberingMode mode) {
    if (mode == SessionNumberingMode::MaxNumberPlusOne) {
        int maxNumber = 0;
        for (const auto& s : sessions) {
            if (auto n = ExtractNumericKey(s.displayName)) maxNumber = std::max(maxNumber, *n);
        }
        return std::max(1, maxNumber + 1);
    }
    return std::max(1, static_cast<int>(sessions.size()) + 1);
}

static SessionAutoOpenMode ParseSessionAutoOpenMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"off") return SessionAutoOpenMode::Off;
    if (m == L"edit") return SessionAutoOpenMode::Edit;
    if (m == L"view") return SessionAutoOpenMode::View;
    return SessionAutoOpenMode::Off;
}

static bool TryParseScheduleTimeMinutes(const std::wstring& input, int& outMinutes) {
    std::wstring s = TrimWhitespace(input);
    if (s.empty()) return false;
    int hour = -1;
    int minute = -1;
    size_t colon = s.find(L':');
    if (colon != std::wstring::npos) {
        std::wstring hStr = s.substr(0, colon);
        std::wstring mStr = s.substr(colon + 1);
        if (hStr.empty() || mStr.size() != 2) return false;
        for (wchar_t ch : hStr) if (!iswdigit(ch)) return false;
        for (wchar_t ch : mStr) if (!iswdigit(ch)) return false;
        hour = std::stoi(hStr);
        minute = std::stoi(mStr);
    } else {
        if (s.size() != 3 && s.size() != 4) return false;
        for (wchar_t ch : s) if (!iswdigit(ch)) return false;
        if (s.size() == 3) {
            hour = s[0] - L'0';
            minute = (s[1] - L'0') * 10 + (s[2] - L'0');
        } else {
            hour = (s[0] - L'0') * 10 + (s[1] - L'0');
            minute = (s[2] - L'0') * 10 + (s[3] - L'0');
        }
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    outMinutes = hour * 60 + minute;
    return true;
}

static int ScheduleDayIndexFromSystem(int wday) {
    // SYSTEMTIME: Sunday=0, Monday=1 -> schedule: Monday=0 ... Sunday=6.
    return (wday + 6) % 7;
}




static std::wstring FormatSig2(double v) {
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%.2g", v);
    std::wstring s = buf;
    if (v >= 0.0 && v < 10.0 && std::floor(v) == v &&
        s.find(L'.') == std::wstring::npos &&
        s.find(L'e') == std::wstring::npos && s.find(L'E') == std::wstring::npos) {
        s += L".0";
    }
    return s;
}

static void DebugAppendLayoutLog(const wchar_t*, HWND, int, int) {}

static void EnableOwnerDrawButton(HWND hWnd);
void ApplyOwnerDrawUi(HWND hWnd);

// (moved to `src/main/bootstrap.cppinc`)


void UpdateSessionLastOpenTargetsAfterOpen() {
    RememberCurrentSessionFiles();
}

static std::filesystem::path LectureSettingsDir() {
    if (g_workspaceRoot.empty()) return {};
    return std::filesystem::path(g_workspaceRoot) / kLectureSettingsDirName;
}

static std::filesystem::path SessionLastOpenFilePath() {
    auto settingsDir = LectureSettingsDir();
    if (settingsDir.empty()) return {};
    return settingsDir / L"__tmp__" / L"session_last_open.txt";
}

static std::filesystem::path StartupLastOpenTargetFilePath() {
    auto settingsDir = LectureSettingsDir();
    if (settingsDir.empty()) return {};
    return settingsDir / L"__tmp__" / L"startup_last_open.txt";
}

static std::filesystem::path PdfViewPositionsFilePath() {
    if (g_workspaceRoot.empty()) return {};
    return std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__tmp__" / L"pdf_view_positions.txt";
}

static std::wstring EscapeSessionLastOpenField(const std::wstring& src) {
    std::wstring out;
    out.reserve(src.size());
    for (wchar_t ch : src) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'\t': out += L"\\t"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

static std::wstring UnescapeSessionLastOpenField(const std::wstring& src) {
    std::wstring out;
    out.reserve(src.size());
    bool esc = false;
    for (wchar_t ch : src) {
        if (!esc) {
            if (ch == L'\\') {
                esc = true;
            } else {
                out.push_back(ch);
            }
            continue;
        }
        switch (ch) {
        case L'\\': out.push_back(L'\\'); break;
        case L't': out.push_back(L'\t'); break;
        case L'n': out.push_back(L'\n'); break;
        case L'r': out.push_back(L'\r'); break;
        default:
            out.push_back(L'\\');
            out.push_back(ch);
            break;
        }
        esc = false;
    }
    if (esc) out.push_back(L'\\');
    return out;
}

static bool ParseSessionLastOpenLine(const std::wstring& line,
                                     std::wstring* outKey,
                                     std::wstring* outPdf,
                                     std::wstring* outNote) {
    if (!outKey || !outPdf || !outNote) return false;
    std::vector<std::wstring> fields;
    std::wstring cur;
    for (size_t i = 0; i < line.size(); ++i) {
        wchar_t ch = line[i];
        if (ch == L'\t') {
            fields.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(ch);
    }
    fields.push_back(cur);
    if (fields.size() < 3) return false;
    *outKey = UnescapeSessionLastOpenField(fields[0]);
    *outPdf = UnescapeSessionLastOpenField(fields[1]);
    *outNote = UnescapeSessionLastOpenField(fields[2]);
    return !outKey->empty();
}

static bool ParseStartupLastOpenLine(const std::wstring& line,
                                     StartupLastOpenTarget* out) {
    if (!out) return false;
    size_t split = line.find(L'\t');
    if (split == std::wstring::npos) return false;
    out->lecturePath = UnescapeSessionLastOpenField(line.substr(0, split));
    out->sessionPath = UnescapeSessionLastOpenField(line.substr(split + 1));
    return !out->lecturePath.empty();
}

#include "app/main_startup_last_open.cppinc"

static bool SaveSessionLastOpenMap() {
    auto path = SessionLastOpenFilePath();
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::vector<std::wstring> keys;
    keys.reserve(g_lastPdfBySession.size() + g_lastNoteBySession.size());
    std::unordered_set<std::wstring> seen;
    for (const auto& kv : g_lastPdfBySession) {
        if (seen.insert(kv.first).second) keys.push_back(kv.first);
    }
    for (const auto& kv : g_lastNoteBySession) {
        if (seen.insert(kv.first).second) keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());
    std::string out;
    out.reserve(keys.size() * 96);
    for (const auto& key : keys) {
        std::wstring pdf;
        std::wstring note;
        auto itPdf = g_lastPdfBySession.find(key);
        if (itPdf != g_lastPdfBySession.end()) pdf = itPdf->second;
        auto itNote = g_lastNoteBySession.find(key);
        if (itNote != g_lastNoteBySession.end()) note = itNote->second;
        std::wstring line = EscapeSessionLastOpenField(key) + L"\t" +
                            EscapeSessionLastOpenField(pdf) + L"\t" +
                            EscapeSessionLastOpenField(note) + L"\n";
        out += WideToUTF8(line);
    }
    std::filesystem::path preferredTmp;
    std::filesystem::path quarantineDir;
    if (!g_workspaceRoot.empty()) {
        std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__resource__";
        preferredTmp = resource / L"__tmp__";
        quarantineDir = resource / L"__escape__";
    }
    std::wstring err;
    return atomic_write::AtomicWriteUtf8(path, out, preferredTmp, quarantineDir, &err);
}

void LoadSessionLastOpenMap() {
    g_lastPdfBySession.clear();
    g_lastNoteBySession.clear();
    auto path = SessionLastOpenFilePath();
    if (path.empty()) return;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return;
    std::string buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (buf.empty()) return;
    std::wstring text = UTF8ToWide(buf);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring key;
        std::wstring pdf;
        std::wstring note;
        if (ParseSessionLastOpenLine(line, &key, &pdf, &note)) {
            std::error_code ec;
            if (!pdf.empty() && std::filesystem::exists(pdf, ec) && !ec) {
                g_lastPdfBySession[key] = pdf;
            }
            ec.clear();
            if (!note.empty() && std::filesystem::exists(note, ec) && !ec) {
                g_lastNoteBySession[key] = note;
            }
        }
        pos = end + 1;
    }
}

static bool ResetLectureLastOpenTimes(std::filesystem::path* outBackupPath) {
    if (outBackupPath) *outBackupPath = std::filesystem::path{};
    if (g_workspaceRoot.empty()) return false;
    if (!EnsureWorkspaceResourceDirs(nullptr)) return false;

    std::filesystem::path src = LectureLastOpenFilePath();
    if (src.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return true; // already clean
    }

    std::filesystem::path escape = std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__escape__";
    std::filesystem::path stampDir = escape / NowTimestampString();
    std::filesystem::create_directories(stampDir, ec);
    if (ec) return false;

    std::filesystem::path dest = stampDir / src.filename();
    if (std::filesystem::exists(dest, ec)) {
        dest = stampDir / (src.filename().wstring() + L"_" + std::to_wstring(GetTickCount64()));
    }

    std::error_code moveEc;
    std::filesystem::rename(src, dest, moveEc);
    if (moveEc) {
        moveEc.clear();
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, moveEc);
        if (!moveEc) {
            std::filesystem::remove(src, moveEc);
        }
    }

    if (std::filesystem::exists(src, ec)) return false;
    if (outBackupPath) *outBackupPath = dest;
    return true;
}

static bool ResetSessionLastOpenTimes(std::filesystem::path* outBackupPath) {
    if (outBackupPath) *outBackupPath = std::filesystem::path{};
    if (g_workspaceRoot.empty()) return false;
    if (!EnsureWorkspaceResourceDirs(nullptr)) return false;

    std::filesystem::path src = SessionLastOpenFilePath();
    if (src.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        g_lastNoteBySession.clear();
        g_lastPdfBySession.clear();
        return true; // already clean
    }

    std::filesystem::path escape = std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__escape__";
    std::filesystem::path stampDir = escape / NowTimestampString();
    std::filesystem::create_directories(stampDir, ec);
    if (ec) return false;

    std::filesystem::path dest = stampDir / src.filename();
    if (std::filesystem::exists(dest, ec)) {
        dest = stampDir / (src.filename().wstring() + L"_" + std::to_wstring(GetTickCount64()));
    }

    std::error_code moveEc;
    std::filesystem::rename(src, dest, moveEc);
    if (moveEc) {
        moveEc.clear();
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, moveEc);
        if (!moveEc) {
            std::filesystem::remove(src, moveEc);
        }
    }

    if (std::filesystem::exists(src, ec)) return false;
    g_lastNoteBySession.clear();
    g_lastPdfBySession.clear();
    if (outBackupPath) *outBackupPath = dest;
    return true;
}

bool IsPathUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = CanonicalOrSelf(path);
    auto r = CanonicalOrSelf(root);
    if (p.empty() || r.empty()) return false;
    auto pIt = p.begin();
    for (auto rIt = r.begin(); rIt != r.end(); ++rIt, ++pIt) {
        if (pIt == p.end()) return false;
        if (ToLowerAscii(rIt->wstring()) != ToLowerAscii(pIt->wstring())) return false;
    }
    return true;
}

static bool OpenStartupDocumentPath(HWND hWnd, const std::wstring& rawPath) {
    if (rawPath.empty()) return false;

    std::filesystem::path path(AbsoluteOrOriginalPath(rawPath));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi() ? L"The startup document was not found:\n" + path.wstring()
                                     : L"起動指定されたファイルが見つかりません:\n" + path.wstring(),
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!IsPdfFile(path) && !IsImageFile(path)) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi() ? L"Only PDF/image files can be opened by the main software:\n" + path.wstring()
                                     : L"メインソフトで開けるのはPDF/画像ファイルです:\n" + path.wstring(),
                       SoftNoticeKind::Warning);
        return false;
    }

    std::wstring openPath = CanonicalOrSelf(path).wstring();
    if (openPath.empty()) openPath = path.wstring();
    const bool opened = OpenPdfIfDifferent(hWnd, openPath);
    if (opened) {
        SyncLeftPaneSelection();
        RefreshMainWindowUiState(hWnd);
        if (g_hPdfView) SetFocus(g_hPdfView);
    }
    return opened;
}

static bool OpenPendingStartupDocument(HWND hWnd) {
    std::wstring path = ConsumePendingStartupOpenDocumentPath();
    if (path.empty()) return false;
    return OpenStartupDocumentPath(hWnd, path);
}

static std::vector<std::filesystem::path> ListEscapeBackupsByPrefix(const std::wstring& expectedNamePrefix) {
    const ULONGLONG startTick = preview_trace::TickNow();
    std::vector<std::filesystem::path> paths;
    auto escapeRoot = EscapeRootPath();
    if (escapeRoot.empty()) {
        preview_trace::Append(
            L"ListEscapeBackupsByPrefix",
            L"skip=no_escape_root prefix=" + expectedNamePrefix +
            L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return paths;
    }
    std::error_code ec;
    if (!std::filesystem::exists(escapeRoot, ec) || ec || !std::filesystem::is_directory(escapeRoot, ec)) {
        preview_trace::Append(
            L"ListEscapeBackupsByPrefix",
            L"skip=missing_or_invalid prefix=" + expectedNamePrefix +
            L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return paths;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(escapeRoot, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        std::error_code stEc;
        if (!it->is_regular_file(stEc) || stEc) continue;
        std::wstring name = it->path().filename().wstring();
        if (name.rfind(expectedNamePrefix, 0) == 0) {
            paths.push_back(it->path());
        }
    }
    std::sort(paths.begin(), paths.end());
    preview_trace::Append(
        L"ListEscapeBackupsByPrefix",
        L"end prefix=" + expectedNamePrefix +
        L" count=" + std::to_wstring(paths.size()) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    return paths;
}

static bool DeleteBackupFiles(const std::vector<std::filesystem::path>& paths,
                              const std::filesystem::path& keepPath = {}) {
    bool ok = true;
    for (const auto& p : paths) {
        if (!keepPath.empty() && CanonicalOrSelf(p) == CanonicalOrSelf(keepPath)) continue;
        std::error_code ec;
        if (!std::filesystem::remove(p, ec) && ec) ok = false;
    }
    return ok;
}

static bool PickEscapeBackupFile(HWND owner,
                                 const std::wstring& expectedNamePrefix,
                                 const std::wstring& dialogTitle,
                                 std::filesystem::path* outPath,
                                 std::wstring* outErr,
                                 bool* outCanceled) {
    if (outPath) *outPath = std::filesystem::path{};
    if (outErr) outErr->clear();
    if (outCanceled) *outCanceled = false;
    if (g_workspaceRoot.empty()) {
        if (outErr) *outErr = IsEnglishUi() ? L"No workspace is open." : L"ワークスペースが開かれていません。";
        return false;
    }
    if (!EnsureWorkspaceResourceDirs(nullptr)) {
        if (outErr) *outErr = IsEnglishUi() ? L"Failed to prepare workspace resource directories."
                                            : L"ワークスペースのリソースフォルダ準備に失敗しました。";
        return false;
    }
    std::filesystem::path escapeRoot = EscapeRootPath();
    std::error_code ec;
    if (!std::filesystem::exists(escapeRoot, ec) || ec || !std::filesystem::is_directory(escapeRoot, ec)) {
        if (outErr) *outErr = IsEnglishUi() ? L"No backup directory found." : L"バックアップフォルダがありません。";
        return false;
    }

    auto picked = PickFileUnder(owner, escapeRoot, dialogTitle);
    if (!picked) {
        if (outCanceled) *outCanceled = true;
        return false;
    }
    std::filesystem::path src(*picked);
    if (!IsPathUnderRoot(src, escapeRoot)) {
        if (outErr) *outErr = IsEnglishUi() ? L"Select a file under __resource__/__escape__."
                                            : L"__resource__/__escape__ 配下のファイルを選択してください。";
        return false;
    }
    std::wstring fileName = src.filename().wstring();
    if (fileName.rfind(expectedNamePrefix, 0) != 0) {
        if (outErr) {
            *outErr = IsEnglishUi()
                ? (L"Select a backup file whose name starts with: " + expectedNamePrefix)
                : (L"次で始まるバックアップファイルを選択してください: " + expectedNamePrefix);
        }
        return false;
    }
    if (outPath) *outPath = src;
    return true;
}

static bool RestoreTempDataFromEscape(HWND owner,
                                      const std::filesystem::path& targetPath,
                                      const std::wstring& expectedNamePrefix,
                                      const std::wstring& dialogTitle,
                                      std::wstring* outErr,
                                      bool* outCanceled) {
    std::filesystem::path src;
    if (!PickEscapeBackupFile(owner, expectedNamePrefix, dialogTitle, &src, outErr, outCanceled)) {
        return false;
    }

    std::ifstream ifs(src, std::ios::binary);
    if (!ifs) {
        if (outErr) *outErr = IsEnglishUi() ? L"Failed to read backup file." : L"バックアップファイルの読み込みに失敗しました。";
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    std::filesystem::path preferredTmp;
    std::filesystem::path quarantineDir;
    std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__resource__";
    preferredTmp = resource / L"__tmp__";
    quarantineDir = resource / L"__escape__";
    std::wstring writeErr;
    if (!atomic_write::AtomicWriteUtf8(targetPath, data, preferredTmp, quarantineDir, &writeErr)) {
        if (outErr) {
            *outErr = IsEnglishUi() ? L"Failed to restore backup file." : L"バックアップファイルの復元に失敗しました。";
            if (!writeErr.empty()) *outErr += L"\n" + writeErr;
        }
        return false;
    }
    return true;
}

static bool BackupTempFileToEscape(const std::filesystem::path& src, std::filesystem::path* outBackupPath) {
    if (outBackupPath) *outBackupPath = std::filesystem::path{};
    if (g_workspaceRoot.empty()) return false;
    if (!EnsureWorkspaceResourceDirs(nullptr)) return false;
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return true;
    if (ec) return false;
    std::filesystem::path escape = EscapeRootPath();
    std::filesystem::path stampDir = escape / NowTimestampString();
    std::filesystem::create_directories(stampDir, ec);
    if (ec) return false;
    std::filesystem::path dest = stampDir / src.filename();
    if (std::filesystem::exists(dest, ec)) {
        dest = stampDir / (src.filename().wstring() + L"_" + std::to_wstring(GetTickCount64()));
    }
    std::error_code moveEc;
    std::filesystem::rename(src, dest, moveEc);
    if (moveEc) {
        moveEc.clear();
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, moveEc);
        if (!moveEc) std::filesystem::remove(src, moveEc);
    }
    if (std::filesystem::exists(src, ec)) return false;
    if (outBackupPath) *outBackupPath = dest;
    return true;
}

static bool AskDeleteOlderBackups(HWND owner,
                                  const std::wstring& menuTitle,
                                  const std::wstring& backupKindLabel,
                                  bool* outDeleteOld) {
    if (outDeleteOld) *outDeleteOld = false;
    if (!outDeleteOld) return false;
    std::wstring msg = IsEnglishUi()
        ? (L"Older backups for \"" + backupKindLabel + L"\" already exist.\n\nDelete older backups and keep only the newest one?")
        : (L"\"" + backupKindLabel + L"\" の既存バックアップがあります。\n\n古いバックアップを削除して最新1件だけ残しますか？");
    *outDeleteOld = ConfirmMainYesNo(owner, menuTitle, msg, SoftNoticeKind::Warning,
                                     SilentDialogResult::No, SilentDialogResult::No);
    return true;
}

static long long ParseLectureOpenStamp(const std::wstring& text) {
    long long value = 0;
    int digits = 0;
    for (wchar_t ch : text) {
        if (ch >= L'0' && ch <= L'9') {
            value = value * 10 + (ch - L'0');
            ++digits;
        }
    }
    return (digits > 0) ? value : 0;
}

static bool SaveLectureOpenMap(const std::filesystem::path& path,
                               const std::unordered_map<std::wstring, std::wstring>& entries);

static std::unordered_map<std::wstring, std::wstring> LoadLectureOpenMap(const std::filesystem::path& path) {
    std::unordered_map<std::wstring, std::wstring> entries;
    if (path.empty()) return entries;
    auto readFile = [&](const std::filesystem::path& src) -> bool {
        std::ifstream ifs(src, std::ios::binary);
        if (!ifs) return false;
        std::string buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (buf.empty()) return false;
        std::wstring text = UTF8ToWide(buf);
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(L'\n', pos);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(pos, end - pos);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            size_t tab = line.find(L'\t');
            if (tab != std::wstring::npos) {
                std::wstring name = line.substr(0, tab);
                std::wstring stamp = line.substr(tab + 1);
                if (!name.empty() && !stamp.empty()) {
                    entries[name] = stamp;
                }
            }
            pos = end + 1;
        }
        return !entries.empty();
    };
    readFile(path);
    return entries;
}

static bool SaveLectureOpenMap(const std::filesystem::path& path,
                               const std::unordered_map<std::wstring, std::wstring>& entries) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::vector<std::pair<std::wstring, std::wstring>> ordered(entries.begin(), entries.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    out.reserve(ordered.size() * 32);
    for (const auto& kv : ordered) {
        std::wstring line = kv.first + L"\t" + kv.second + L"\n";
        std::string utf8 = WideToUTF8(line);
        out.append(utf8);
    }
    std::filesystem::path preferredTmp;
    std::filesystem::path quarantineDir;
    if (!g_workspaceRoot.empty()) {
        std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__resource__";
        preferredTmp = resource / L"__tmp__";
        quarantineDir = resource / L"__escape__";
    }
    std::wstring err;
    return atomic_write::AtomicWriteUtf8(path, out, preferredTmp, quarantineDir, &err);
}

std::unordered_map<std::wstring, long long> LoadLectureOpenTimes(const std::filesystem::path& path) {
    std::unordered_map<std::wstring, long long> times;
    auto entries = LoadLectureOpenMap(path);
    for (const auto& kv : entries) {
        long long stamp = ParseLectureOpenStamp(kv.second);
        if (stamp > 0) times[kv.first] = stamp;
    }
    return times;
}

bool UpdateLectureOpenTime(const std::wstring& lecturePath) {
    if (lecturePath.empty()) return false;
    std::wstring name = std::filesystem::path(lecturePath).filename().wstring();
    if (name.empty()) return false;
    auto path = LectureLastOpenFilePath();
    if (path.empty()) return false;
    auto entries = LoadLectureOpenMap(path);
    entries[name] = NowTimestampString();
    return SaveLectureOpenMap(path, entries);
}

MAYBE_UNUSED static std::wstring PreferredNoteForSession(const SessionEntry& s) {
    auto key = SessionKey(s);
    auto it = g_lastNoteBySession.find(key);
    std::error_code ec;
    if (it != g_lastNoteBySession.end() && std::filesystem::exists(it->second, ec) && !ec) {
        return it->second;
    }
    return L"";
}

MAYBE_UNUSED static std::wstring PreferredPdfForSession(const SessionEntry& s) {
    auto key = SessionKey(s);
    auto it = g_lastPdfBySession.find(key);
    std::error_code ec;
    if (it != g_lastPdfBySession.end() && std::filesystem::exists(it->second, ec) && !ec) {
        return it->second;
    }
    return L"";
}


static std::filesystem::path ExistingDialogDirectoryOrEmpty(const std::filesystem::path& candidate) {
    if (candidate.empty()) return {};
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec &&
        std::filesystem::is_directory(candidate, ec) && !ec) {
        return candidate;
    }
    ec.clear();
    if (std::filesystem::exists(candidate, ec) && !ec &&
        std::filesystem::is_regular_file(candidate, ec) && !ec) {
        std::filesystem::path parent = candidate.parent_path();
        ec.clear();
        if (!parent.empty() &&
            std::filesystem::exists(parent, ec) && !ec &&
            std::filesystem::is_directory(parent, ec) && !ec) {
            return parent;
        }
    }
    return {};
}

static std::filesystem::path DialogExeDirectory() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        std::error_code ec;
        auto cur = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path{} : cur;
    }
    return std::filesystem::path(exePath).parent_path();
}

std::filesystem::path DialogWorkspaceInitialFolder() {
    if (auto workspace = ExistingDialogDirectoryOrEmpty(std::filesystem::path(g_workspaceRoot));
        !workspace.empty()) {
        return workspace;
    }
    return ExistingDialogDirectoryOrEmpty(DialogExeDirectory());
}

static std::filesystem::path DialogKnownFolderInitialFolder(REFKNOWNFOLDERID folderId) {
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath)) && rawPath) {
        std::filesystem::path result = ExistingDialogDirectoryOrEmpty(std::filesystem::path(rawPath));
        CoTaskMemFree(rawPath);
        return result;
    }
    if (rawPath) CoTaskMemFree(rawPath);
    return {};
}

std::filesystem::path DialogDownloadsInitialFolder() {
    if (auto downloads = DialogKnownFolderInitialFolder(FOLDERID_Downloads);
        !downloads.empty()) {
        return downloads;
    }
    return DialogWorkspaceInitialFolder();
}

std::filesystem::path DialogDocumentsInitialFolder() {
    if (auto documents = DialogKnownFolderInitialFolder(FOLDERID_Documents);
        !documents.empty()) {
        return documents;
    }
    return DialogWorkspaceInitialFolder();
}

static std::filesystem::path DialogContextInitialFolder() {
    if (auto session = ExistingDialogDirectoryOrEmpty(std::filesystem::path(g_currentSessionPath));
        !session.empty()) {
        return session;
    }
    if (auto lecture = ExistingDialogDirectoryOrEmpty(std::filesystem::path(g_currentLecturePath));
        !lecture.empty()) {
        return lecture;
    }
    return DialogWorkspaceInitialFolder();
}

static std::filesystem::path ResolveDialogInitialFolder(const std::filesystem::path& initial) {
    if (auto dir = ExistingDialogDirectoryOrEmpty(initial); !dir.empty()) {
        return dir;
    }
    return DialogContextInitialFolder();
}

#include "ui/lists/main_local_path_browser.cppinc"

std::optional<std::wstring> PickFileUnder(HWND owner,
                                                 const std::filesystem::path& initial,
                                                 const std::wstring& title) {
    return PromptExistingLocalPath(owner, initial, title, /*requireDirectory=*/false);
}

std::vector<std::wstring> PickFilesUnder(HWND owner,
                                                const std::filesystem::path& initial,
                                                const std::wstring& title) {
    return PromptExistingLocalFilesAppFirst(owner, initial, title, /*allowMultiple=*/true);
}

std::vector<std::wstring> PickOfficeFilesUnder(HWND owner,
                                                      const std::filesystem::path& initial,
                                                      const std::wstring& title) {
    return PickFilesUnder(owner, initial, title);
}

static constexpr const wchar_t* kReadOnlyViewerWindowClassName = L"PdfReadonlyViewerWindow";

struct ReadOnlyViewerCloseContext {
    int requested = 0;
};

static BOOL CALLBACK CloseReadOnlyViewerWindowEnumProc(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<ReadOnlyViewerCloseContext*>(lParam);
    if (!context || !hwnd) return TRUE;
    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(sizeof(className) / sizeof(className[0]))) <= 0) {
        return TRUE;
    }
    if (wcscmp(className, kReadOnlyViewerWindowClassName) != 0) return TRUE;
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
        ++context->requested;
    }
    return TRUE;
}

static int RequestCloseAllReadOnlyViewerWindows() {
    ReadOnlyViewerCloseContext context{};
    EnumWindows(CloseReadOnlyViewerWindowEnumProc, reinterpret_cast<LPARAM>(&context));
    return context.requested;
}

static void CloseAllReadOnlyViewerWindows(HWND owner) {
    const int count = RequestCloseAllReadOnlyViewerWindows();
    if (count <= 0) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"No read-only viewer windows are open."
                                     : L"開いている閲覧専用ビューアはありません。",
                       SoftNoticeKind::Info);
        return;
    }
    ShowSoftNotice(owner,
                   IsEnglishUi() ? L"Closing read-only viewer windows: " + std::to_wstring(count)
                                 : L"閲覧専用ビューアを閉じます: " + std::to_wstring(count),
                   SoftNoticeKind::Info);
}

static std::filesystem::path CurrentMainExecutableDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) return {};
    if (len >= buffer.size()) {
        buffer.resize(32768, L'\0');
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0 || len >= buffer.size()) return {};
    }
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path();
}

static std::filesystem::path FindReadOnlyViewerExecutable() {
    std::error_code ec;
    const std::filesystem::path exeDir = CurrentMainExecutableDir();
    if (exeDir.empty()) return {};

    const std::filesystem::path candidates[] = {
        exeDir / L"readonly_viewer.exe",
    };
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate, ec) &&
            !ec && std::filesystem::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
        ec.clear();
    }
    return {};
}

static std::wstring QuoteProcessArg(const std::wstring& value) {
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

static std::wstring ColorToReadonlyThemeHex(COLORREF color) {
    wchar_t buf[8]{};
    swprintf(buf, 8, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return buf;
}

static std::wstring BuildReadonlyViewerInlineThemeSpec() {
    std::wstring spec;
    auto add = [&](const wchar_t* key, COLORREF value) {
        if (!spec.empty()) spec += L";";
        spec += key;
        spec += L"=";
        spec += ColorToReadonlyThemeHex(value);
    };
    add(L"windowBg", g_theme.windowBg);
    add(L"windowText", g_theme.windowText);
    add(L"panelBg", g_theme.panelBg);
    add(L"panelText", g_theme.panelText);
    add(L"menuBg", g_theme.menuBg);
    add(L"menuText", g_theme.menuText);
    add(L"menuSelBg", g_theme.menuSelBg);
    add(L"menuSelText", g_theme.menuSelText);
    add(L"toolbarBg", g_theme.toolbarBg);
    add(L"toolbarText", g_theme.toolbarText);
    add(L"buttonBg", g_theme.buttonBg);
    add(L"buttonText", g_theme.buttonText);
    add(L"buttonBorder", g_theme.buttonBorder);
    add(L"buttonHot", g_theme.buttonHot);
    add(L"buttonPressed", g_theme.buttonPressed);
    add(L"splitterBg", g_theme.splitterBg);
    add(L"splitterLine", g_theme.splitterLine);
    add(L"pdfBg", g_theme.pdfBg);
    add(L"pdfPageBg", g_theme.pdfPageBg);
    add(L"noteBg", g_theme.noteBg);
    add(L"noteText", g_theme.noteText);
    add(L"selectionBg", g_theme.selectionBg);
    add(L"selectionText", g_theme.selectionText);
    add(L"accent", g_theme.accent);
    return spec;
}

static std::wstring BuildReadonlyViewerThemeParams() {
    const std::wstring inlineTheme = BuildReadonlyViewerInlineThemeSpec();
    if (inlineTheme.empty()) return {};
    return L"--theme-inline " + QuoteProcessArg(inlineTheme);
}

static bool LaunchReadOnlyViewerWithParams(HWND owner, const std::wstring& params) {
    const std::filesystem::path viewer = FindReadOnlyViewerExecutable();
    const std::filesystem::path viewerDir = viewer.parent_path();
    if (viewer.empty() || viewerDir.empty()) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Read-only viewer executable was not found."
                                     : L"閲覧専用ビューアの実行ファイルが見つかりません。",
                       SoftNoticeKind::Warning);
        return false;
    }

    std::wstring commandLine = QuoteProcessArg(viewer.wstring());
    if (!params.empty()) {
        commandLine += L" ";
        commandLine += params;
    }
    std::wstring workingDir = viewerDir.wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(viewer.c_str(),
                        commandLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        workingDir.empty() ? nullptr : workingDir.c_str(),
                        &si,
                        &pi)) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Failed to launch the read-only viewer."
                                     : L"閲覧専用ビューアを起動できませんでした。",
                       SoftNoticeKind::Warning);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool LaunchReadOnlyViewerEmpty(HWND owner) {
    return LaunchReadOnlyViewerWithParams(owner, BuildReadonlyViewerThemeParams());
}

static int ReadonlyViewerInitialPageForPath(const std::wstring& path) {
    if (path.empty() || CurrentLogicalPdfPath().empty()) return 1;
    if (NormalizePathKey(path) != NormalizePathKey(CurrentLogicalPdfPath())) return 1;
    const int pageIndex = PageAtCurrentView();
    return std::max(1, pageIndex + 1);
}

bool LaunchReadOnlyViewerForPdfAt(HWND owner,
                                  const std::wstring& pdfPath,
                                  int pageIndex,
                                  double yPt,
                                  bool hasY) {
    if (pdfPath.empty()) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"No PDF is selected." : L"PDFが選択されていません。",
                       SoftNoticeKind::Info);
        return false;
    }
    std::filesystem::path path(pdfPath);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec || !std::filesystem::is_regular_file(path, ec) || ec) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"PDF file was not found:\n" + pdfPath
                                     : L"PDFファイルが見つかりません:\n" + pdfPath,
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!IsPdfFile(path)) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"The read-only viewer currently accepts PDF files only."
                                     : L"閲覧専用ビューアへ渡せるのはPDFファイルのみです。",
                       SoftNoticeKind::Info);
        return false;
    }
    std::filesystem::path launchPath = CanonicalOrSelf(path);
    if (launchPath.empty()) launchPath = path;
    const std::wstring launchPdfPath = launchPath.wstring();

    const int pageOneBased = (pageIndex >= 0) ? (pageIndex + 1)
                                             : ReadonlyViewerInitialPageForPath(launchPdfPath);
    std::wstring params = L"--pdf " + QuoteProcessArg(launchPdfPath);
    params += L" --page " + std::to_wstring(std::max(1, pageOneBased));
    if (hasY && std::isfinite(yPt)) {
        params += L" --pdf-y-pt " + std::to_wstring(std::clamp(yPt, -50000.0, 50000.0));
    }
    params += L" --annotations ";
    params += g_showAnnots ? L"on" : L"off";
    const std::wstring themeParams = BuildReadonlyViewerThemeParams();
    if (!themeParams.empty()) {
        params += L" ";
        params += themeParams;
    }
    return LaunchReadOnlyViewerWithParams(owner, params);
}

static bool LaunchReadOnlyViewerForPdf(HWND owner, const std::wstring& pdfPath) {
    return LaunchReadOnlyViewerForPdfAt(owner, pdfPath, -1, 0.0, false);
}

static bool SetMainClipboardUnicodeText(HWND owner, const std::wstring& text) {
    HWND clipOwner = owner ? owner : g_hMainWnd;
    if (!OpenClipboard(clipOwner)) return false;
    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) {
        CloseClipboard();
        return false;
    }

    void* dst = GlobalLock(hmem);
    if (!dst) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(hmem);

    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

static bool CopyPathToClipboard(HWND owner, const std::wstring& path) {
    if (path.empty()) return false;
    if (!SetMainClipboardUnicodeText(owner, path)) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Failed to copy the path." : L"パスをコピーできませんでした。",
                       SoftNoticeKind::Warning);
        return false;
    }
    ShowSoftNotice(owner,
                   IsEnglishUi() ? L"Copied path." : L"パスをコピーしました。",
                   SoftNoticeKind::Info);
    return true;
}

static bool ShowPathInExplorer(HWND owner, const std::wstring& pathText) {
    if (pathText.empty()) return false;

    std::filesystem::path target(pathText);
    if (IsUncPath(target)) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Explorer opening is limited to local paths."
                                     : L"エクスプローラーで開けるのはローカルパスだけです。",
                       SoftNoticeKind::Warning);
        return false;
    }

    bool isReparse = false;
    if (TryIsReparsePointNoFollow(target, isReparse) && isReparse) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Reparse-point paths are not opened in Explorer."
                                     : L"リパースポイントのパスはエクスプローラーで開きません。",
                       SoftNoticeKind::Warning);
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(target, ec) || ec) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"The path was not found:\n" + pathText
                                     : L"パスが見つかりません:\n" + pathText,
                       SoftNoticeKind::Warning);
        return false;
    }

    const bool isDir = std::filesystem::is_directory(target, ec) && !ec;
    ec.clear();
    const bool isFile = std::filesystem::is_regular_file(target, ec) && !ec;
    if (!isDir && !isFile) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Only files and folders can be opened in Explorer."
                                     : L"エクスプローラーで開けるのはファイルまたはフォルダだけです。",
                       SoftNoticeKind::Warning);
        return false;
    }

    std::wstring params = isDir ? QuoteProcessArg(target.wstring())
                                : (L"/select," + QuoteProcessArg(target.wstring()));
    HINSTANCE result = ShellExecuteW(owner, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Failed to open Explorer."
                                     : L"エクスプローラーを開けませんでした。",
                       SoftNoticeKind::Warning);
        return false;
    }
    return true;
}

static bool IsOfficeFileListPath(const std::wstring& path) {
    return !path.empty() && IsOfficeImportSourcePath(std::filesystem::path(path));
}

static bool CanOpenPdfListPathInApp(const std::wstring& path) {
    if (path.empty()) return false;
    return !IsOfficeFileListPath(path);
}

static std::wstring FirstOpenablePdfListPath() {
    for (const auto& file : g_pdfFiles) {
        if (CanOpenPdfListPathInApp(file.path)) return file.path;
    }
    return L"";
}

static void RestorePdfListSelectionToCurrent() {
    if (!g_hPdfList) return;
    const int index = CurrentPdfIndex();
    SendMessageW(g_hPdfList,
                 LB_SETCURSEL,
                 index >= 0 ? static_cast<WPARAM>(index) : static_cast<WPARAM>(-1),
                 0);
    InvalidateRect(g_hPdfList, nullptr, FALSE);
}

static bool PromptOfficeFileListAction(HWND owner, const std::wstring& officePath) {
    if (!owner || officePath.empty()) return false;
    std::filesystem::path path(officePath);

    SilentDialogOptions dialog;
    dialog.title = IsEnglishUi() ? L"Office file in PDF list (Experimental conversion)"
                                 : L"PDF欄のOfficeファイル（試験的変換）";
    dialog.message = IsEnglishUi()
        ? (L"This Office file is listed in the PDF area.\nPDF conversion is experimental.\nChoose whether to convert it to PDF or show it in Explorer.\n\nFile:\n" +
           path.filename().wstring())
        : (L"このOfficeファイルはPDF欄に表示されています。\nPDF変換は試験的です。\nPDFに変換するか、エクスプローラーで表示するかを選んでください。\n\n対象:\n" +
           path.filename().wstring());
    dialog.kind = SoftNoticeKind::Info;
    dialog.buttons = SilentDialogButtons::YesNoCancel;
    dialog.yesLabel = IsEnglishUi() ? L"Convert to PDF" : L"PDFに変換";
    dialog.noLabel = IsEnglishUi() ? L"Show in Explorer" : L"エクスプローラーで表示";
    dialog.cancelLabel = IsEnglishUi() ? L"Cancel" : L"キャンセル";
    dialog.defaultResult = SilentDialogResult::Yes;
    dialog.escapeResult = SilentDialogResult::Cancel;

    const SilentDialogResult result = ShowSilentDialog(owner, dialog);
    if (result == SilentDialogResult::Yes) {
        return ConvertOfficeFileToCurrentSession(owner, path);
    }
    if (result == SilentDialogResult::No) {
        return ShowPathInExplorer(owner, officePath);
    }
    return false;
}

static void ShowSelectedPathDialog(HWND owner,
                                   const std::wstring& label,
                                   const std::wstring& pathText) {
    if (!owner || pathText.empty()) return;
    const std::wstring title = label.empty()
        ? (IsEnglishUi() ? L"Work path" : L"作業パス")
        : label;
    SilentDialogOptions dialog;
    dialog.title = title;
    dialog.message = (IsEnglishUi() ? L"Path:\n\n" : L"パス:\n\n") + pathText;
    dialog.kind = SoftNoticeKind::Info;
    dialog.buttons = SilentDialogButtons::YesNoCancel;
    dialog.yesLabel = IsEnglishUi() ? L"Open in Explorer" : L"エクスプローラーで開く";
    dialog.noLabel = IsEnglishUi() ? L"Copy path" : L"パスをコピー";
    dialog.cancelLabel = IsEnglishUi() ? L"Close" : L"閉じる";
    dialog.defaultResult = SilentDialogResult::Yes;
    dialog.escapeResult = SilentDialogResult::Cancel;
    const SilentDialogResult result = ShowSilentDialog(owner, dialog);
    if (result == SilentDialogResult::Yes) {
        ShowPathInExplorer(owner, pathText);
    } else if (result == SilentDialogResult::No) {
        CopyPathToClipboard(owner, pathText);
    }
}

static std::wstring RemapChildPathAfterDirectoryMove(const std::filesystem::path& oldParent,
                                                     const std::filesystem::path& newParent,
                                                     const std::wstring& oldChildPath) {
    if (oldChildPath.empty() || oldParent.empty() || newParent.empty()) return L"";
    std::filesystem::path child(oldChildPath);
    if (!IsPathUnderRoot(child, oldParent)) return L"";
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(child, oldParent, ec);
    if (ec || relative.empty()) return L"";
    return (newParent / relative).wstring();
}

static bool IsCurrentLectureTargetPath(const std::filesystem::path& path) {
    return !g_currentLecturePath.empty() &&
           NormalizePathKeyForList(g_currentLecturePath) == NormalizePathKeyForList(path.wstring());
}

static bool IsCurrentSessionTargetPath(const std::filesystem::path& path) {
    return !g_currentSessionPath.empty() &&
           SessionKeyFromPath(g_currentSessionPath) == SessionKeyFromPath(path.wstring());
}

static bool IsWorkspaceManagedDirectoryTarget(const std::filesystem::path& path) {
    if (path.empty() || g_workspaceRoot.empty()) return false;
    if (IsPotentialNetworkPath(path)) return false;
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(path, isReparse) && isReparse) return false;
    return IsPathUnderRoot(path, std::filesystem::path(g_workspaceRoot));
}

static bool EnsureDirectoryMutationReady(HWND owner,
                                         const std::filesystem::path& target,
                                         const std::wstring& blockedAction) {
    if (target.empty()) return false;

    const bool touchesCurrentLecture = IsCurrentLectureTargetPath(target);
    const bool touchesCurrentSession = IsCurrentSessionTargetPath(target);
    const bool touchesCurrentPdf = !CurrentLogicalPdfPath().empty() &&
                                   IsPathUnderRoot(std::filesystem::path(CurrentLogicalPdfPath()), target);
    const bool touchesCurrentNote = !g_currentNotePath.empty() &&
                                    IsPathUnderRoot(std::filesystem::path(g_currentNotePath), target);
    const bool touchesCurrentState =
        touchesCurrentLecture || touchesCurrentSession || touchesCurrentPdf || touchesCurrentNote;

    if (touchesCurrentState) {
        RememberCurrentSessionFiles();
        if (!SaveNoteIfDirty(owner)) {
            PromptStayOrOpenDiffManager(
                owner,
                blockedAction,
                IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                              : L"「差分管理」で未統合差分を整理してから、もう一度実行してください。");
            return false;
        }
        if (!file_output::PrepareStagedDiffsForSwitch(owner)) {
            PromptStayOrOpenDiffManager(
                owner,
                blockedAction,
                IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                              : L"「差分管理」で未統合差分を整理してから、もう一度実行してください。");
            return false;
        }
    }

    if (!file_output::HasPendingOrStagedDiffsUnderPath(target)) return true;

    std::wstring msg = IsEnglishUi()
        ? L"There are unsaved or staged changes under this directory.\n\nSave them before continuing?\n\n[Yes] Save and continue\n[No] Cancel"
        : L"このフォルダ配下に、未保存または未統合の stage 差分があります。\n\n保存してから続行しますか？\n\n[はい] 保存して続行\n[いいえ] 中止";
    if (!ConfirmMainYesNo(owner, blockedAction, msg, SoftNoticeKind::Warning,
                          SilentDialogResult::No, SilentDialogResult::No)) {
        return false;
    }

    SaveAllManual(owner);
    if (file_output::HasPendingOrStagedDiffsUnderPath(target)) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Some unsaved or staged changes still remain under this directory. The operation was canceled."
                                     : L"このフォルダ配下に未保存または未統合の差分がまだ残っているため、操作を中止しました。",
                       SoftNoticeKind::Warning);
        return false;
    }
    return true;
}

static bool MoveDirectoryToDeletedItemsEscape(const std::filesystem::path& source,
                                              const wchar_t* prefix,
                                              std::filesystem::path* outMovedPath,
                                              std::wstring* outErr) {
    if (outMovedPath) *outMovedPath = std::filesystem::path{};
    if (outErr) outErr->clear();
    if (source.empty()) return false;

    std::filesystem::path escapeRoot = EscapeRootPath();
    if (escapeRoot.empty()) {
        if (outErr) *outErr = IsEnglishUi() ? L"Delete backup folder is not available."
                                            : L"削除退避フォルダを利用できません。";
        return false;
    }

    std::error_code ec;
    std::filesystem::path deletedRoot = escapeRoot / L"deleted_items" / NowTimestampString();
    std::filesystem::create_directories(deletedRoot, ec);
    if (ec) {
        if (outErr) {
            *outErr = (IsEnglishUi() ? L"Failed to create the delete backup folder:\n"
                                     : L"削除退避フォルダを作成できませんでした:\n") +
                      deletedRoot.wstring() + L"\n" + UTF8ToWide(ec.message());
        }
        return false;
    }

    std::filesystem::path baseName = source.filename();
    if (baseName.empty()) baseName = prefix;
    baseName = std::filesystem::path(std::wstring(prefix) + baseName.wstring());
    std::filesystem::path dest = atomic_write::MakeUniqueDestInDir(deletedRoot, baseName);
    std::filesystem::rename(source, dest, ec);
    if (ec) {
        if (outErr) {
            *outErr = (IsEnglishUi() ? L"Failed to move the directory into the delete backup folder:\n"
                                     : L"フォルダを削除退避フォルダへ移動できませんでした:\n") +
                      source.wstring() + L"\n\n" + UTF8ToWide(ec.message());
        }
        return false;
    }
    if (outMovedPath) *outMovedPath = dest;
    return true;
}

static bool RemoveTempExternalLectureFromContext(HWND owner, const std::wstring& lecturePath) {
    if (lecturePath.empty()) return false;
    if (file_output::HasPendingOrStagedDiffsUnderPath(std::filesystem::path(lecturePath))) {
        std::wstring msg = IsEnglishUi()
            ? L"There are unsaved or staged changes under this temporary external lecture.\n\nSave them before removing the temporary external lecture path?\n\n[Yes] Save and remove\n[No] Keep the temporary external lecture path"
            : (std::wstring(g_config.studentMode
                                 ? L"この一時外部授業の配下に、未保存または未統合の stage 差分があります。\n\n"
                                 : L"この一時外部上位項目の配下に、未保存または未統合の stage 差分があります。\n\n") +
               (g_config.studentMode
                    ? L"保存してから一時外部授業パスを削除しますか？\n\n"
                    : L"保存してから一時外部上位項目パスを削除しますか？\n\n") +
               L"[はい] 保存して削除\n" +
               (g_config.studentMode ? L"[いいえ] 一時外部授業パスを残す"
                                     : L"[いいえ] 一時外部上位項目パスを残す"));
        if (!ConfirmMainYesNo(owner, GetUiText().menuRemoveTempExternalLecture, msg, SoftNoticeKind::Warning,
                              SilentDialogResult::No, SilentDialogResult::No)) {
            return false;
        }
        SaveAllManual(owner);
        if (file_output::HasPendingOrStagedDiffsUnderPath(std::filesystem::path(lecturePath))) {
            ShowSoftNotice(owner,
                           IsEnglishUi() ? L"Some unsaved or staged changes still remain under this temporary external lecture. The path was not removed."
                                         : (g_config.studentMode
                                                ? L"この一時外部授業の配下に未保存または未統合の差分がまだ残っているため、パスは削除しませんでした。"
                                                : L"この一時外部上位項目の配下に未保存または未統合の差分がまだ残っているため、パスは削除しませんでした。"),
                           SoftNoticeKind::Warning);
            return false;
        }
    }
    if (!ConfirmRecommendedPdfExportBeforeRemovingTempLecture(owner, std::filesystem::path(lecturePath))) {
        return false;
    }
    const bool removingCurrent = (!g_currentLecturePath.empty() && lecturePath == g_currentLecturePath);
    if (removingCurrent) {
        RememberCurrentSessionFiles();
        if (!SaveNoteIfDirty(owner)) {
            PromptStayOrOpenDiffManager(
                owner,
                IsEnglishUi() ? L"Removing the temporary lecture path"
                              : (g_config.studentMode ? L"一時外部授業パスの削除" : L"一時外部上位項目パスの削除"),
                IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                              : L"「差分管理」で未統合差分を整理してから、もう一度実行してください。");
            return false;
        }
        ClearPdfAndNoteSelection();
        ResetSessionAndFiles();
        g_currentLecturePath.clear();
    }
    if (!RemoveTempExternalLecturePath(lecturePath)) return false;
    s_ignoreLectureSelChange = true;
    LoadLectures();
    s_ignoreLectureSelChange = false;
    UpdateWindowTitle(owner);
    RefreshMainMenuBar(owner);
    return true;
}

static bool RenameLectureDirectoryFromContext(HWND owner,
                                             const std::filesystem::path& lecturePath,
                                             const std::wstring& label) {
    if (!IsWorkspaceManagedDirectoryTarget(lecturePath)) return false;
    if (!EnsureDirectoryMutationReady(owner, lecturePath,
                                      IsEnglishUi() ? L"Renaming lecture"
                                                    : (g_config.studentMode ? L"授業名変更" : L"上位項目名変更"))) {
        return false;
    }

    std::wstring newName;
    if (!PromptSimpleText(owner,
                          IsEnglishUi() ? L"Rename Lecture" : (g_config.studentMode ? L"授業名変更" : L"上位項目名変更"),
                          lecturePath.filename().wstring(),
                          newName)) {
        return false;
    }
    newName = TrimWhitespace(newName);
    if (newName.empty()) return false;
    if (!ValidateCreateFileSystemName(owner, newName,
                                      IsEnglishUi() ? L"Rename Lecture"
                                                    : (g_config.studentMode ? L"授業名変更" : L"上位項目名変更"))) {
        return false;
    }
    if (IsWorkspaceReservedImportDirectoryName(std::filesystem::path(newName))) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Directories named __resource__ are reserved by this app."
                                     : L"__resource__ という名前のフォルダはアプリ予約領域のため使えません。",
                       SoftNoticeKind::Warning);
        return false;
    }

    std::filesystem::path dest = lecturePath.parent_path() / newName;
    if (NormalizePathKeyForList(dest.wstring()) == NormalizePathKeyForList(lecturePath.wstring())) return false;
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && !ec) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"A directory with the same name already exists."
                                     : L"同名のフォルダが既にあります。",
                       SoftNoticeKind::Warning);
        return false;
    }

    const bool wasCurrentLecture = IsCurrentLectureTargetPath(lecturePath);
    const std::wstring remappedSession = RemapChildPathAfterDirectoryMove(lecturePath, dest, g_currentSessionPath);
    const std::wstring remappedPdf = RemapChildPathAfterDirectoryMove(lecturePath, dest, CurrentLogicalPdfPath());
    const std::wstring remappedNote = RemapChildPathAfterDirectoryMove(lecturePath, dest, g_currentNotePath);

    std::filesystem::rename(lecturePath, dest, ec);
    if (ec) {
        ShowMainMessageDialog(owner,
                              IsEnglishUi() ? L"Rename Lecture" : (g_config.studentMode ? L"授業名変更" : L"上位項目名変更"),
                              (IsEnglishUi() ? L"Failed to rename the directory.\n\n"
                                             : L"フォルダ名の変更に失敗しました。\n\n") +
                                  lecturePath.wstring() + L"\n->\n" + dest.wstring() + L"\n\n" + UTF8ToWide(ec.message()),
                              SoftNoticeKind::Warning);
        return false;
    }

    s_ignoreLectureSelChange = true;
    LoadLectures();
    s_ignoreLectureSelChange = false;
    if (wasCurrentLecture) {
        ClearPdfAndNoteSelection();
        ResetSessionAndFiles();
        g_currentLecturePath.clear();
        const int lectureIndex = FindLectureIndexByPath(dest.wstring());
        if (lectureIndex >= 0) {
            SendMessageW(g_hLectureList, LB_SETCURSEL, lectureIndex, 0);
            OnLectureSelChange(owner);
            if (!remappedSession.empty()) {
                const int sessionIndex = FindSessionIndexByPath(remappedSession);
                if (sessionIndex >= 0) {
                    SetListboxSelAndNotify(g_hSessionList, sessionIndex);
                }
            }
            if (!remappedPdf.empty()) OpenPdfIfDifferent(owner, remappedPdf);
            if (!remappedNote.empty()) OpenNoteIfDifferent(owner, remappedNote);
        }
    } else {
        SyncLeftPaneSelection();
        RefreshMainWindowUiState(owner);
    }
    ShowSoftNotice(owner,
                   (IsEnglishUi() ? L"Renamed: " : L"名前を変更しました: ") + label,
                   SoftNoticeKind::Info);
    return true;
}

static bool RenameSessionDirectoryFromContext(HWND owner,
                                             const std::filesystem::path& sessionPath,
                                             const std::wstring& lecturePath,
                                             const std::wstring& label) {
    if (!IsWorkspaceManagedDirectoryTarget(sessionPath)) return false;
    if (IsDirectFilesSessionPath(sessionPath.wstring(), lecturePath)) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Rename the lecture item instead for direct-file folders."
                                     : L"直下ファイルの項目は、授業/上位項目の側で名前変更してください。",
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!EnsureDirectoryMutationReady(owner, sessionPath,
                                      IsEnglishUi() ? L"Renaming session" : L"セッション名変更")) {
        return false;
    }

    std::wstring newName;
    if (!PromptSimpleText(owner,
                          IsEnglishUi() ? L"Rename Session" : L"セッション名変更",
                          sessionPath.filename().wstring(),
                          newName)) {
        return false;
    }
    newName = TrimWhitespace(newName);
    if (newName.empty()) return false;
    if (!ValidateCreateFileSystemName(owner, newName,
                                      IsEnglishUi() ? L"Rename Session" : L"セッション名変更")) {
        return false;
    }
    if (IsWorkspaceReservedImportDirectoryName(std::filesystem::path(newName))) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Directories named __resource__ are reserved by this app."
                                     : L"__resource__ という名前のフォルダはアプリ予約領域のため使えません。",
                       SoftNoticeKind::Warning);
        return false;
    }

    std::filesystem::path dest = sessionPath.parent_path() / newName;
    if (SessionKeyFromPath(dest.wstring()) == SessionKeyFromPath(sessionPath.wstring())) return false;
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && !ec) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"A directory with the same name already exists."
                                     : L"同名のフォルダが既にあります。",
                       SoftNoticeKind::Warning);
        return false;
    }

    const bool wasCurrentSession = IsCurrentSessionTargetPath(sessionPath);
    const std::wstring remappedPdf = RemapChildPathAfterDirectoryMove(sessionPath, dest, CurrentLogicalPdfPath());
    const std::wstring remappedNote = RemapChildPathAfterDirectoryMove(sessionPath, dest, g_currentNotePath);

    std::filesystem::rename(sessionPath, dest, ec);
    if (ec) {
        ShowMainMessageDialog(owner,
                              IsEnglishUi() ? L"Rename Session" : L"セッション名変更",
                              (IsEnglishUi() ? L"Failed to rename the directory.\n\n"
                                             : L"フォルダ名の変更に失敗しました。\n\n") +
                                  sessionPath.wstring() + L"\n->\n" + dest.wstring() + L"\n\n" + UTF8ToWide(ec.message()),
                              SoftNoticeKind::Warning);
        return false;
    }

    LoadSessions(lecturePath);
    if (wasCurrentSession) {
        ClearPdfAndNoteSelection();
        ResetSessionAndFiles();
        const int sessionIndex = FindSessionIndexByPath(dest.wstring());
        if (sessionIndex >= 0) {
            SetListboxSelAndNotify(g_hSessionList, sessionIndex);
            if (!remappedPdf.empty()) OpenPdfIfDifferent(owner, remappedPdf);
            if (!remappedNote.empty()) OpenNoteIfDifferent(owner, remappedNote);
        }
    } else {
        SyncLeftPaneSelection();
        RefreshMainWindowUiState(owner);
    }
    ShowSoftNotice(owner,
                   (IsEnglishUi() ? L"Renamed: " : L"名前を変更しました: ") + label,
                   SoftNoticeKind::Info);
    return true;
}

static bool DeleteLectureDirectoryFromContext(HWND owner,
                                             int lectureIndex,
                                             const std::filesystem::path& lecturePath,
                                             const std::wstring& label) {
    if (IsTempExternalLecturePath(lecturePath.wstring())) {
        return RemoveTempExternalLectureFromContext(owner, lecturePath.wstring());
    }
    if (!IsWorkspaceManagedDirectoryTarget(lecturePath)) return false;
    if (!EnsureDirectoryMutationReady(owner, lecturePath,
                                      IsEnglishUi() ? L"Deleting lecture"
                                                    : (g_config.studentMode ? L"授業削除" : L"上位項目削除"))) {
        return false;
    }

    std::wstring confirm = IsEnglishUi()
        ? (L"Move this lecture directory into \"__resource__/__escape__/deleted_items\"?\n\n" + lecturePath.wstring())
        : (std::wstring(g_config.studentMode ? L"この授業フォルダを " : L"この上位項目フォルダを ") +
           L"\"__resource__/__escape__/deleted_items\" に退避移動しますか？\n\n" + lecturePath.wstring());
    if (!ConfirmMainYesNo(owner,
                          IsEnglishUi() ? L"Delete Lecture" : (g_config.studentMode ? L"授業削除" : L"上位項目削除"),
                          confirm, SoftNoticeKind::Warning,
                          SilentDialogResult::No, SilentDialogResult::No)) {
        return false;
    }

    std::filesystem::path movedPath;
    std::wstring err;
    if (!MoveDirectoryToDeletedItemsEscape(lecturePath, L"lecture_", &movedPath, &err)) {
        ShowMainMessageDialog(owner,
                              IsEnglishUi() ? L"Delete Lecture" : (g_config.studentMode ? L"授業削除" : L"上位項目削除"),
                              err, SoftNoticeKind::Warning);
        return false;
    }

    const bool wasCurrentLecture = IsCurrentLectureTargetPath(lecturePath);
    s_ignoreLectureSelChange = true;
    if (wasCurrentLecture) {
        ClearPdfAndNoteSelection();
        ResetSessionAndFiles();
        g_currentLecturePath.clear();
    }
    LoadLectures();
    s_ignoreLectureSelChange = false;
    if (wasCurrentLecture) {
        const int count = g_hLectureList
            ? static_cast<int>(SendMessageW(g_hLectureList, LB_GETCOUNT, 0, 0))
            : 0;
        if (count > 0) {
            SetListboxSelAndNotify(g_hLectureList, std::clamp(lectureIndex, 0, count - 1));
        } else {
            RefreshMainWindowUiState(owner);
        }
    } else {
        SyncLeftPaneSelection();
        RefreshMainWindowUiState(owner);
    }
    ShowSoftNotice(owner,
                   IsEnglishUi() ? L"Moved the deleted lecture into the emergency backup folder."
                                 : L"削除したフォルダを緊急退避フォルダへ移動しました。",
                   SoftNoticeKind::Info);
    return true;
}

static bool DeleteSessionDirectoryFromContext(HWND owner,
                                             int sessionIndex,
                                             const std::filesystem::path& sessionPath,
                                             const std::wstring& lecturePath) {
    if (!IsWorkspaceManagedDirectoryTarget(sessionPath)) return false;
    if (IsDirectFilesSessionPath(sessionPath.wstring(), lecturePath)) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Delete the lecture item instead for direct-file folders."
                                     : L"直下ファイルの項目は、授業/上位項目の側で削除してください。",
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!EnsureDirectoryMutationReady(owner, sessionPath,
                                      IsEnglishUi() ? L"Deleting session" : L"セッション削除")) {
        return false;
    }

    std::wstring confirm = IsEnglishUi()
        ? (L"Move this session directory into \"__resource__/__escape__/deleted_items\"?\n\n" + sessionPath.wstring())
        : (L"このセッションフォルダを \"__resource__/__escape__/deleted_items\" に退避移動しますか？\n\n" +
           sessionPath.wstring());
    if (!ConfirmMainYesNo(owner, IsEnglishUi() ? L"Delete Session" : L"セッション削除",
                          confirm, SoftNoticeKind::Warning,
                          SilentDialogResult::No, SilentDialogResult::No)) {
        return false;
    }

    std::filesystem::path movedPath;
    std::wstring err;
    if (!MoveDirectoryToDeletedItemsEscape(sessionPath, L"session_", &movedPath, &err)) {
        ShowMainMessageDialog(owner, IsEnglishUi() ? L"Delete Session" : L"セッション削除",
                              err, SoftNoticeKind::Warning);
        return false;
    }

    const bool wasCurrentSession = IsCurrentSessionTargetPath(sessionPath);
    if (wasCurrentSession) {
        ClearPdfAndNoteSelection();
        ResetSessionAndFiles();
    }
    LoadSessions(lecturePath);
    if (wasCurrentSession) {
        const int count = g_hSessionList
            ? static_cast<int>(SendMessageW(g_hSessionList, LB_GETCOUNT, 0, 0))
            : 0;
        if (count > 0) {
            SetListboxSelAndNotify(g_hSessionList, std::clamp(sessionIndex, 0, count - 1));
        } else {
            RefreshMainWindowUiState(owner);
        }
    } else {
        SyncLeftPaneSelection();
        RefreshMainWindowUiState(owner);
    }
    ShowSoftNotice(owner,
                   IsEnglishUi() ? L"Moved the deleted session into the emergency backup folder."
                                 : L"削除したフォルダを緊急退避フォルダへ移動しました。",
                   SoftNoticeKind::Info);
    return true;
}

static bool TryGetShortcutPanelRect(const RECT& client, const ui::SplitBands& bands, RECT* out) {
    if (!out) return false;
    int bottomY = bands.hMain.bottom;
    if (bottomY < client.top) bottomY = client.top;
    if (bottomY >= client.bottom) return false;

    if (g_leftPaneCollapsed) return false;
    int leftW = bands.vLeft.left;
    if (leftW <= client.left) return false;
    leftW = std::clamp(leftW, static_cast<int>(client.left), static_cast<int>(client.right));
    if (leftW <= client.left) return false;
    *out = RECT{ client.left, bottomY, leftW, client.bottom };
    return true;
}

static void RedrawWorkspaceLayout(HWND hWnd) {
    ui::RedrawLayoutTree(hWnd);
}

static void ResetSplitToDefault(HWND hWnd) {
    RECT rc{};
    GetClientRect(hWnd, &rc);
    int cw = static_cast<int>(rc.right - rc.left);
    int ch = static_cast<int>(rc.bottom - rc.top);
    if (cw <= 0 || ch <= 0) return;
    int split = ui::SplitBandThickness();
    int maxLeft = cw - kMinPane - kMinPane - split * 2;
    int defLeft = (g_config.defaultLeftWidth > 0) ? g_config.defaultLeftWidth : kDefaultLeftPane;
    g_leftWidth = std::clamp(defLeft, kMinPane, std::max(kMinPane, maxLeft));
    int maxRight = cw - g_leftWidth - kMinPane - split * 2;
    int defRight = (g_config.defaultRightWidth > 0) ? g_config.defaultRightWidth : kDefaultRightPane;
    g_rightWidth = std::clamp(defRight, kMinPane, std::max(kMinPane, maxRight));
    int desiredTop = (g_config.defaultTopHeight > 0)
        ? g_config.defaultTopHeight
        : static_cast<int>(std::round(ch * kTopSplitRatio));
    int maxTop = ch - split - kMinPane;
    g_topHeight = std::clamp(desiredTop, kMinPane, std::max(kMinPane, maxTop));

    // reset left column inner splits to "thirds" of the available list area (band-aware)
    constexpr int kNewBtnH = 28;
    constexpr int kNewBtnGap = 6;
    constexpr int kNewBtnTop = 6;
    int listTopOffset = kNewBtnTop + kNewBtnH + kNewBtnGap;
    int available = std::max(0, g_topHeight - listTopOffset);
    int listSpace = std::max(0, available - split * 2);
    int seg = (listSpace > 0) ? (listSpace / 3) : 0;
    int defSplit1 = g_config.defaultLeftSplit1;
    int defSplit2 = g_config.defaultLeftSplit2;
    if (defSplit1 > 0 && defSplit2 > 0 && defSplit1 < defSplit2) {
        g_leftSplit1 = defSplit1;
        g_leftSplit2 = defSplit2;
    } else {
        g_leftSplit1 = listTopOffset + seg + split;
        g_leftSplit2 = listTopOffset + seg * 2 + split * 2;
    }

    ui::ApplyLayout(hWnd, ui::LayoutPass::Commit);
    RedrawWorkspaceLayout(hWnd);
    PersistConfig();
}

static int CalcListItemHeight() {
    HDC hdc = GetDC(nullptr);
    if (!hdc) return 0;
    HFONT old = nullptr;
    if (g_hUIFont) old = static_cast<HFONT>(SelectObject(hdc, g_hUIFont));
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    if (old) SelectObject(hdc, old);
    ReleaseDC(nullptr, hdc);
    int h = static_cast<int>(tm.tmHeight + tm.tmExternalLeading + 4);
    return h;
}

static void ScrollPdfToFileStart() {
    if (!g_hPdfView || g_pdf.pages.empty()) return;
    g_pdf.scrollX = 0.0;
    JumpToPage(g_hPdfView, 0);
}

static void ScrollNoteToFileStart() {
    if (!g_hNoteEdit) return;
    POINT topLeft{0, 0};
    if (!SendMessageW(g_hNoteEdit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&topLeft))) {
        SendMessageW(g_hNoteEdit, WM_VSCROLL, SB_TOP, 0);
        SendMessageW(g_hNoteEdit, WM_HSCROLL, SB_LEFT, 0);
    }
}

static void FocusNoteFromListAtFileStart(HWND owner) {
    if (!g_hNoteEdit) return;
    SendMessageW(g_hNoteEdit, EM_SETSEL, 0, 0);
    g_noteNormalCaret = 0;
    ScrollNoteToFileStart();
    if (g_noteVimModeEnabled && owner) {
        EnterNoteNormalMode(owner);
        return;
    }
    SetFocus(g_hNoteEdit);
    SendMessageW(g_hNoteEdit, EM_SETSEL, 0, 0);
    SendMessageW(g_hNoteEdit, EM_SCROLLCARET, 0, 0);
    g_noteNormalMode = false;
    OnExitNoteNormalMode();
    if (g_hBottomNote) InvalidateRect(g_hBottomNote, nullptr, FALSE);
}

static bool OpenOrFocusSelectedNoteAtFileStart(HWND owner) {
    if (!g_hNoteList) return false;
    bool changed = EnsureListboxSelection(g_hNoteList);
    int sel = static_cast<int>(SendMessageW(g_hNoteList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_noteFiles.size())) return false;
    const std::wstring selPath = g_noteFiles[static_cast<size_t>(sel)].path;
    if (selPath.empty()) return false;
    if (changed || NormalizePathKey(selPath) != NormalizePathKey(g_currentNotePath)) {
        if (!OpenNoteIfDifferent(owner, selPath)) return false;
    }
    FocusNoteFromListAtFileStart(owner);
    return true;
}

static void CaptureNoteCaretForNormalMode() {
    if (!g_hNoteEdit) return;
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_hNoteEdit, EM_GETSEL,
                 reinterpret_cast<WPARAM>(&selStart),
                 reinterpret_cast<LPARAM>(&selEnd));
    g_noteNormalCaret = selEnd;
}

static void FocusNoteEditAtNormalCaret() {
    if (!g_hNoteEdit) return;
    int len = GetWindowTextLengthW(g_hNoteEdit);
    if (len < 0) len = 0;
    DWORD caret = std::min<DWORD>(g_noteNormalCaret, static_cast<DWORD>(len));
    SetFocus(g_hNoteEdit);
    SendMessageW(g_hNoteEdit, EM_SETSEL, caret, caret);
    SendMessageW(g_hNoteEdit, EM_SCROLLCARET, 0, 0);
    g_noteNormalMode = false;
    OnExitNoteNormalMode();
    if (g_hBottomNote) InvalidateRect(g_hBottomNote, nullptr, FALSE);
}

static void CancelAndDisableMainIme(HWND hWnd) {
    if (!hWnd) return;
    HIMC himc = ImmGetContext(hWnd);
    if (himc) {
        ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmSetOpenStatus(himc, FALSE);
        ImmReleaseContext(hWnd, himc);
    }
    ImmAssociateContextEx(hWnd, nullptr, 0);
}

static bool IsEditableTextInputControl(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return false;
    wchar_t cls[64]{};
    if (GetClassNameW(hWnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) <= 0) {
        return false;
    }
    if (_wcsicmp(cls, L"Edit") == 0 ||
        _wcsicmp(cls, MSFTEDIT_CLASS) == 0 ||
        _wcsnicmp(cls, L"RICHEDIT", 8) == 0) {
        LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        return (style & ES_READONLY) == 0;
    }
    if (_wcsicmp(cls, L"ComboBox") == 0) {
        LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        // DropDownList has no text input surface; treat only editable combos as IME targets.
        return (style & CBS_DROPDOWNLIST) == 0;
    }
    return false;
}

static bool IsExplicitImeTargetWindow(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return false;
    if (hWnd == g_hPdfView) {
        return g_pdf.editingText;
    }
    if (hWnd == g_hNoteEdit || (g_hNoteEdit && IsChild(g_hNoteEdit, hWnd))) {
        return true;
    }
    return IsEditableTextInputControl(hWnd);
}

static void DisableImeForControl(HWND hWnd) {
    if (!hWnd) return;
    HIMC himc = ImmGetContext(hWnd);
    if (himc) {
        ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmSetOpenStatus(himc, FALSE);
        ImmReleaseContext(hWnd, himc);
    }
    ImmAssociateContextEx(hWnd, nullptr, 0);
}

static void EnableImeForControl(HWND hWnd) {
    if (!hWnd) return;
    ImmAssociateContextEx(hWnd, nullptr, IACE_DEFAULT);
}

static HWND ResolveImeTargetWindow(const MSG& msg) {
    HWND focused = GetFocus();
    if (IsExplicitImeTargetWindow(msg.hwnd)) {
        return msg.hwnd;
    }
    if (IsExplicitImeTargetWindow(focused)) {
        return focused;
    }
    return msg.hwnd ? msg.hwnd : focused;
}

static bool IsImeComposingOnWindow(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return false;
    HIMC himc = ImmGetContext(hWnd);
    if (!himc) return false;
    const LONG compBytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(hWnd, himc);
    return compBytes > 0;
}

void EnforceImePolicyForWindow(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return;
    if (GetWindowThreadProcessId(hWnd, nullptr) != GetCurrentThreadId()) return;
    if (IsExplicitImeTargetWindow(hWnd)) {
        EnableImeForControl(hWnd);
        return;
    }
    DisableImeForControl(hWnd);
}

bool ShouldSkipImeMessageInLoop(const MSG& msg) {
    HWND focused = GetFocus();
    if (focused) {
        EnforceImePolicyForWindow(focused);
    }
    // While typing in an explicit text target, don't let parent-window messages
    // force-close IME state for that focused input control.
    if (msg.hwnd && msg.hwnd != focused && !IsExplicitImeTargetWindow(focused)) {
        EnforceImePolicyForWindow(msg.hwnd);
    }
    bool isImeMsg = (msg.message == WM_IME_STARTCOMPOSITION ||
                     msg.message == WM_IME_COMPOSITION ||
                     msg.message == WM_IME_ENDCOMPOSITION);
    HWND imeTarget = ResolveImeTargetWindow(msg);
    if (isImeMsg && !IsExplicitImeTargetWindow(imeTarget)) {
        DisableImeForControl(imeTarget);
        return true;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_PROCESSKEY &&
        !IsExplicitImeTargetWindow(imeTarget)) {
        return true;
    }
    return false;
}

void EnterNoteNormalMode(HWND hWnd) {
    if (!CommitActiveNoteEditBoundary(hWnd)) return;
    CaptureNoteCaretForNormalMode();
    g_noteNormalMode = g_noteVimModeEnabled;
    if (g_noteNormalMode) {
        OnEnterNoteNormalMode();
    } else {
        OnExitNoteNormalMode();
    }
    s_skipAutoChildFocusOnce = true;
    CancelAndDisableMainIme(hWnd);
    SetFocus(hWnd);
    if (g_hNoteEdit) InvalidateRect(g_hNoteEdit, nullptr, FALSE);
    if (g_hBottomNote) InvalidateRect(g_hBottomNote, nullptr, FALSE);
}

void FocusMainWindowForNoteNormalMode() {
    HWND owner = (g_hMainWnd && IsWindow(g_hMainWnd)) ? g_hMainWnd : nullptr;
    if (!owner) return;
    s_skipAutoChildFocusOnce = true;
    CancelAndDisableMainIme(owner);
    SetFocus(owner);
    if (g_hNoteEdit) InvalidateRect(g_hNoteEdit, nullptr, FALSE);
    if (g_hBottomNote) InvalidateRect(g_hBottomNote, nullptr, FALSE);
}

static void ActivateOrScrollSelection(HWND owner, ListClickKind kind, int sel) {
    if (sel < 0) return;
    if (kind == ListClickKind::Pdf) {
        if (sel >= static_cast<int>(g_pdfFiles.size())) return;
        const std::wstring selPath = g_pdfFiles[static_cast<size_t>(sel)].path;
        if (IsOfficeFileListPath(selPath)) {
            PromptOfficeFileListAction(owner, selPath);
            RestorePdfListSelectionToCurrent();
            return;
        }
        std::wstring openKey = NormalizePathKey(CurrentLogicalPdfPath());
        std::wstring selKey = NormalizePathKey(selPath);
        if (!openKey.empty() && openKey == selKey) {
            ScrollPdfToFileStart();
        } else {
            OpenPdfIfDifferent(owner, selPath);
        }
        return;
    }

    if (sel >= static_cast<int>(g_noteFiles.size())) return;
    const std::wstring selPath = g_noteFiles[static_cast<size_t>(sel)].path;
    std::wstring openKey = NormalizePathKey(g_currentNotePath);
    std::wstring selKey = NormalizePathKey(selPath);
    if (!openKey.empty() && openKey == selKey) {
        HWND focused = GetFocus();
        bool focusInNoteEdit =
            (focused == g_hNoteEdit) || (focused && g_hNoteEdit && IsChild(g_hNoteEdit, focused));
        if (!focusInNoteEdit) {
            FocusNoteEditAtNormalCaret();
        } else {
            ScrollNoteToFileStart();
        }
    } else {
        OpenNoteIfDifferent(owner, selPath);
    }
}

static void NotifyListboxSelChange(HWND list) {
    if (!list) return;
    HWND owner = GetParent(list);
    if (!owner) return;
    SendMessageW(owner, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
}

static bool SetListboxSelAndNotify(HWND list, int newSel) {
    if (!list) return false;
    int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    if (count <= 0) return false;
    newSel = std::clamp(newSel, 0, count - 1);
    int curSel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (curSel == newSel) return false;
    SendMessageW(list, LB_SETCURSEL, newSel, 0);
    NotifyListboxSelChange(list);
    return true;
}

static bool EnsureListboxSelection(HWND list) {
    if (!list) return false;
    int curSel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (curSel >= 0) return false;
    return SetListboxSelAndNotify(list, 0);
}

static int FindFileEntryIndexByPath(const std::vector<FileEntry>& files, const std::wstring& path) {
    if (path.empty()) return -1;
    const std::wstring wanted = NormalizePathKey(std::filesystem::path(path));
    if (wanted.empty()) return -1;
    for (size_t i = 0; i < files.size(); ++i) {
        if (NormalizePathKey(std::filesystem::path(files[i].path)) == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static bool SelectStartupLastOpenFileListItem(HWND list,
                                               const std::vector<FileEntry>& files,
                                               const std::wstring& path) {
    if (!list) return false;
    const int idx = FindFileEntryIndexByPath(files, path);
    if (idx < 0) return false;
    return SetListboxSelAndNotify(list, idx);
}

static bool IsStartupLastOpenFileInList(const std::vector<FileEntry>& files,
                                        const std::wstring& path) {
    return FindFileEntryIndexByPath(files, path) >= 0;
}

static void TraceStartupLastOpenRestore(const std::wstring& step,
                                        ULONGLONG startTick,
                                        const StartupLastOpenTarget& target,
                                        bool lectureRestored,
                                        bool sessionRestored,
                                        bool pdfRestored,
                                        bool noteRestored) {
    preview_trace::Append(
        L"StartupLastOpenRestore",
        L"step=" + step +
        L" lectureRestored=" + preview_trace::Bool(lectureRestored) +
        L" sessionRestored=" + preview_trace::Bool(sessionRestored) +
        L" pdfRestored=" + preview_trace::Bool(pdfRestored) +
        L" noteRestored=" + preview_trace::Bool(noteRestored) +
        L" targetLecture=" + target.lecturePath +
        L" targetSession=" + target.sessionPath +
        L" currentLecture=" + g_currentLecturePath +
        L" currentSession=" + g_currentSessionPath +
        L" currentPdf=" + CurrentLogicalPdfPath() +
        L" currentNote=" + g_currentNotePath +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

static void ShowStartupLastOpenPartialNotice(HWND hWnd, const std::vector<std::wstring>& missing) {
    if (missing.empty()) return;
    std::wstring msg = IsEnglishUi()
        ? L"Restored the available last-open items. Not restored: "
        : L"lastopen の復元可能な範囲だけ復元しました。未復元: ";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) msg += L", ";
        msg += missing[i];
    }
    ShowSoftNotice(hWnd ? hWnd : g_hMainWnd, msg, SoftNoticeKind::Warning);
}

static bool RestoreStartupLastOpenSelection(HWND hWnd) {
    const ULONGLONG startTick = preview_trace::TickNow();
    StartupLastOpenTarget target;
    std::vector<std::wstring> missing;
    bool lectureRestored = false;
    bool sessionRestored = false;
    bool pdfRestored = false;
    bool noteRestored = false;

    auto finish = [&](const std::wstring& step) -> bool {
        TraceStartupLastOpenRestore(step, startTick, target,
                                    lectureRestored, sessionRestored, pdfRestored, noteRestored);
        if (lectureRestored && !missing.empty()) {
            ShowStartupLastOpenPartialNotice(hWnd, missing);
        }
        return lectureRestored || sessionRestored || pdfRestored || noteRestored;
    };

    if (!hWnd || !g_hLectureList) return finish(L"skip=no_window");
    if (!LoadStartupLastOpenTarget(&target)) return finish(L"skip=no_target");

    std::error_code ec;
    std::filesystem::path lecturePath(target.lecturePath);
    if (!std::filesystem::exists(lecturePath, ec) || ec ||
        !std::filesystem::is_directory(lecturePath, ec) || ec) {
        missing.push_back(IsEnglishUi() ? L"lecture" : L"上位項目");
        return finish(L"missing=lecture");
    }

    int lectureIdx = FindLectureIndexByPath(target.lecturePath);
    if (lectureIdx < 0) {
        missing.push_back(IsEnglishUi() ? L"lecture list item" : L"上位項目リスト項目");
        return finish(L"missing=lecture_index");
    }

    bool lectureChanged = SetListboxSelAndNotify(g_hLectureList, lectureIdx);
    if (!lectureChanged &&
        NormalizePathKeyForList(g_currentLecturePath) != NormalizePathKeyForList(target.lecturePath)) {
        NotifyListboxSelChange(g_hLectureList);
    }
    lectureRestored =
        NormalizePathKeyForList(g_currentLecturePath) == NormalizePathKeyForList(target.lecturePath);
    if (!lectureRestored) {
        missing.push_back(IsEnglishUi() ? L"lecture selection" : L"上位項目選択");
        return finish(L"failed=lecture_select");
    }

    if (target.sessionPath.empty()) {
        return finish(L"end=lecture_only");
    }

    std::filesystem::path sessionPath(target.sessionPath);
    ec.clear();
    if (!std::filesystem::exists(sessionPath, ec) || ec ||
        !std::filesystem::is_directory(sessionPath, ec) || ec) {
        missing.push_back(IsEnglishUi() ? L"session" : L"下位項目");
        return finish(L"missing=session_path");
    }

    if (g_sessions.empty()) {
        sessionRestored = SessionKeyFromPath(g_currentSessionPath) == SessionKeyFromPath(target.sessionPath);
        if (!sessionRestored) missing.push_back(IsEnglishUi() ? L"session list" : L"下位項目リスト");
        return finish(sessionRestored ? L"end=direct_session" : L"missing=session_list");
    }

    int sessionIdx = FindSessionIndexByPath(target.sessionPath);
    if (sessionIdx < 0) {
        missing.push_back(IsEnglishUi() ? L"session list item" : L"下位項目リスト項目");
        return finish(L"missing=session_index");
    }

    bool sessionChanged = SetListboxSelAndNotify(g_hSessionList, sessionIdx);
    if (!sessionChanged &&
        SessionKeyFromPath(g_currentSessionPath) != SessionKeyFromPath(target.sessionPath)) {
        NotifyListboxSelChange(g_hSessionList);
    }
    sessionRestored = SessionKeyFromPath(g_currentSessionPath) == SessionKeyFromPath(target.sessionPath);
    if (!sessionRestored) {
        missing.push_back(IsEnglishUi() ? L"session selection" : L"下位項目選択");
        return finish(L"failed=session_select");
    }

    SessionEntry targetSession;
    if (sessionIdx >= 0 && sessionIdx < static_cast<int>(g_sessions.size())) {
        targetSession = g_sessions[static_cast<size_t>(sessionIdx)];
    } else {
        targetSession.displayName = std::filesystem::path(target.sessionPath).filename().wstring();
        targetSession.path = target.sessionPath;
    }

    std::wstring preferredPdf = PreferredPdfForSession(targetSession);
    std::wstring preferredNote = PreferredNoteForSession(targetSession);
    const bool hasPreferredPdf = !preferredPdf.empty();
    const bool hasPreferredNote = !preferredNote.empty();

    if (ParseSessionAutoOpenMode(g_config.sessionAutoOpenMode) == SessionAutoOpenMode::Off) {
        LoadFiles(targetSession, preferredPdf, preferredNote);
        const bool selectedPdf = SelectStartupLastOpenFileListItem(g_hPdfList, g_pdfFiles, preferredPdf);
        const bool selectedNote = SelectStartupLastOpenFileListItem(g_hNoteList, g_noteFiles, preferredNote);
        SyncLeftPaneSelection();
        pdfRestored = hasPreferredPdf &&
            IsStartupLastOpenFileInList(g_pdfFiles, preferredPdf);
        noteRestored = hasPreferredNote &&
            IsStartupLastOpenFileInList(g_noteFiles, preferredNote);
        if (hasPreferredPdf && !selectedPdf) missing.push_back(L"PDF");
        if (hasPreferredNote && !selectedNote) missing.push_back(IsEnglishUi() ? L"note" : L"ノート");
        RefreshMainWindowUiState(hWnd);
        return finish(L"end=selected_files_only");
    }

    LoadFiles(targetSession, preferredPdf, preferredNote);

    if (hasPreferredPdf) {
        if (CanOpenPdfListPathInApp(preferredPdf)) {
            pdfRestored = OpenPdfIfDifferent(hWnd, preferredPdf);
        } else {
            pdfRestored = IsStartupLastOpenFileInList(g_pdfFiles, preferredPdf);
        }
        if (!pdfRestored) missing.push_back(L"PDF");
    }
    if (hasPreferredNote) {
        noteRestored = OpenNoteIfDifferent(hWnd, preferredNote);
        if (!noteRestored) missing.push_back(IsEnglishUi() ? L"note" : L"ノート");
    }
    SyncLeftPaneSelection();
    RefreshMainWindowUiState(hWnd);
    return finish(L"end=open_files");
}

static int ListboxItemCount(HWND list) {
    if (!list) return 0;
    int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    return std::max(0, count);
}

static void FocusHierarchyPrevFromList(HWND list);
static void FocusHierarchyNextFromList(HWND list);
static bool OpenSelectionForList(HWND list);
bool HandlePaneDirectionalNavigation(HWND owner, PaneNavContext context, HWND source, WPARAM vkey);

static int CurrentLogicalListIndex(HWND list) {
    if (!list) return -1;
    if (list == g_hLectureList) return CurrentLectureIndex();
    if (list == g_hSessionList) return CurrentSessionIndex();
    if (list == g_hPdfList) return CurrentPdfIndex();
    if (list == g_hNoteList) return CurrentNoteIndex();
    return static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
}

static void MoveListboxSelOnly(HWND list, int delta) {
    if (!list) return;
    int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    if (count <= 0) return;
    int curSel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    int logicalSel = CurrentLogicalListIndex(list);
    // Prefer the actual listbox selection for navigation; fall back to the
    // logical "currently open" item only when nothing is selected.
    int baseSel = (curSel >= 0) ? curSel : ((logicalSel >= 0) ? logicalSel : 0);
    int newSel = std::clamp(baseSel + delta, 0, count - 1);
    if (curSel == newSel) return;
    SendMessageW(list, LB_SETCURSEL, newSel, 0);
}

static bool HandleHjklListNavigation(HWND list, UINT msg, WPARAM wParam) {
    if (!list) return false;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_PROCESSKEY) return true;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetKeyState(VK_MENU) & 0x8000) == 0 &&
            (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            if (HandlePaneDirectionalNavigation(GetParent(list), PaneNavContext::LeftPaneList, list, wParam)) {
                return true;
            }
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
            (GetKeyState(VK_MENU) & 0x8000) == 0) {
            if (wParam == 'H') {
                FocusHierarchyPrevFromList(list);
                return true;
            }
            if (wParam == 'L') {
                OpenSelectionForList(list);
                return true;
            }
            if (wParam == 'J') {
                MoveListboxSelOnly(list, +1);
                return true;
            }
            if (wParam == 'K') {
                MoveListboxSelOnly(list, -1);
                return true;
            }
        }
        if (wParam == VK_LEFT) {
            FocusHierarchyPrevFromList(list);
            return true;
        }
        if (wParam == VK_RIGHT) {
            OpenSelectionForList(list);
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void InvalidateDirectoryListFocusFrame(HWND hWnd) {
    if (!hWnd) return;
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
}

static void DrawDirectoryListFocusFrame(HWND hWnd) {
    if (!hWnd || GetFocus() != hWnd) return;
    HDC hdc = GetWindowDC(hWnd);
    if (!hdc) return;
    RECT rc{};
    if (GetWindowRect(hWnd, &rc)) {
        OffsetRect(&rc, -rc.left, -rc.top);
        const COLORREF outerColor = g_theme.accent;
        const COLORREF innerColor = BlendColor(g_theme.accent, g_theme.panelBg, 0.35);
        HPEN outerPen = CreatePen(PS_SOLID, 1, outerColor);
        HPEN innerPen = CreatePen(PS_SOLID, 1, innerColor);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        HGDIOBJ oldPen = SelectObject(hdc, outerPen);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        InflateRect(&rc, -1, -1);
        SelectObject(hdc, innerPen);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(innerPen);
        DeleteObject(outerPen);
    }
    ReleaseDC(hWnd, hdc);
}

static void FocusHierarchyPrevFromList(HWND list) {
    if (list == g_hSessionList && g_hLectureList) {
        SetFocus(g_hLectureList);
        return;
    }
    if (list == g_hPdfList && g_hSessionList) {
        SetFocus(g_hSessionList);
        return;
    }
    if (list == g_hNoteList && g_hPdfList) {
        SetFocus(g_hPdfList);
        return;
    }
}

[[maybe_unused]] static void FocusHierarchyNextFromList(HWND list) {
    HWND owner = list ? GetParent(list) : nullptr;
    if (list == g_hLectureList && g_hSessionList) {
        bool changed = EnsureListboxSelection(g_hLectureList);
        if (!changed && ListboxItemCount(g_hSessionList) <= 0) {
            int curSel = static_cast<int>(SendMessageW(g_hLectureList, LB_GETCURSEL, 0, 0));
            if (curSel >= 0 && curSel < static_cast<int>(g_lectures.size())) {
                const std::wstring& selectedLecture = g_lectures[static_cast<size_t>(curSel)];
                if (g_currentLecturePath.empty() || selectedLecture != g_currentLecturePath) {
                    NotifyListboxSelChange(g_hLectureList);
                }
            }
        }
        SetFocus(g_hSessionList);
        EnsureListboxSelection(g_hSessionList); // may trigger session load
        return;
    }
    if (list == g_hSessionList && g_hPdfList) {
        bool changed = EnsureListboxSelection(g_hSessionList);
        if (!changed && ListboxItemCount(g_hPdfList) <= 0 && ListboxItemCount(g_hNoteList) <= 0) {
            int curSel = static_cast<int>(SendMessageW(g_hSessionList, LB_GETCURSEL, 0, 0));
            if (curSel >= 0) NotifyListboxSelChange(g_hSessionList);
        }
        SetFocus(g_hPdfList);
        EnsureListboxSelection(g_hPdfList);
        return;
    }
    if (list == g_hPdfList && g_hNoteList) {
        SetFocus(g_hNoteList);
        EnsureListboxSelection(g_hNoteList);
        return;
    }
    if (list == g_hNoteList) {
        OpenOrFocusSelectedNoteAtFileStart(owner);
        return;
    }
}

static bool OpenSelectedLecture(HWND owner) {
    if (!g_hLectureList) return false;
    int sel = static_cast<int>(SendMessageW(g_hLectureList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_lectures.size())) return false;
    const std::wstring selected = g_lectures[static_cast<size_t>(sel)];
    OnLectureSelChange(owner);
    return !selected.empty() && selected == g_currentLecturePath;
}

static bool OpenSelectedSession(HWND owner) {
    if (!g_hSessionList) return false;
    int sel = static_cast<int>(SendMessageW(g_hSessionList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_sessions.size())) return false;
    const auto& target = g_sessions[static_cast<size_t>(sel)];
    auto targetCanon = CanonicalOrSelf(std::filesystem::path(target.path));
    OnSessionSelChange(owner);
    auto currentCanon = g_currentSessionPath.empty()
        ? std::filesystem::path{}
        : CanonicalOrSelf(std::filesystem::path(g_currentSessionPath));
    return !targetCanon.empty() && !currentCanon.empty() && targetCanon == currentCanon;
}

static bool OpenSelectedPdf(HWND owner) {
    if (!g_hPdfList) return false;
    int sel = static_cast<int>(SendMessageW(g_hPdfList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_pdfFiles.size())) return false;
    const auto& f = g_pdfFiles[static_cast<size_t>(sel)];
    if (IsOfficeFileListPath(f.path)) {
        PromptOfficeFileListAction(owner, f.path);
        RestorePdfListSelectionToCurrent();
        return false;
    }
    const std::wstring selected = f.path;
    OnPdfSelChange(owner);
    return !selected.empty() && selected == CurrentLogicalPdfPath();
}

static bool OpenSelectionForList(HWND list) {
    HWND owner = list ? GetParent(list) : nullptr;
    if (!owner) return false;
    if (list == g_hLectureList) {
        const bool opened = OpenSelectedLecture(owner);
        if (opened && g_hSessionList) {
            SetFocus(g_hSessionList);
            EnsureListboxSelection(g_hSessionList);
        }
        return opened;
    }
    if (list == g_hSessionList) {
        const bool opened = OpenSelectedSession(owner);
        if (opened && g_hPdfList) {
            SetFocus(g_hPdfList);
            EnsureListboxSelection(g_hPdfList);
        }
        return opened;
    }
    if (list == g_hPdfList) {
        const bool opened = OpenSelectedPdf(owner);
        if (opened && g_hNoteList) {
            SetFocus(g_hNoteList);
            EnsureListboxSelection(g_hNoteList);
        }
        return opened;
    }
    if (list == g_hNoteList) {
        return OpenOrFocusSelectedNoteAtFileStart(owner);
    }
    return false;
}

static bool IsCtrlPaneNavChar(WPARAM ch) {
    return ch == L'\b' || ch == L'\n' || ch == 0x0B || ch == 0x0C;
}

static void ExitNoteNormalModeForPaneSwitch() {
    if (!g_noteNormalMode) return;
    g_noteNormalMode = false;
    OnExitNoteNormalMode();
    if (g_hNoteEdit) InvalidateRect(g_hNoteEdit, nullptr, FALSE);
    if (g_hBottomNote) InvalidateRect(g_hBottomNote, nullptr, FALSE);
}

static bool FocusListPaneForPaneNav(HWND list) {
    if (!list || !IsWindow(list) || !IsWindowVisible(list)) return false;
    SetFocus(list);
    EnsureListboxSelection(list);
    return true;
}

static bool FocusPdfPaneForPaneNav(HWND owner) {
    (void)owner;
    if (!g_hPdfView || !IsWindow(g_hPdfView)) return false;
    SetFocus(g_hPdfView);
    return true;
}

static bool FocusNotePaneForPaneNav(HWND owner) {
    if (!g_hNoteEdit || !IsWindow(g_hNoteEdit)) return false;
    HWND resolvedOwner = owner ? owner : (g_hMainWnd ? g_hMainWnd : GetParent(g_hNoteEdit));
    if (g_noteVimModeEnabled && resolvedOwner) {
        EnterNoteNormalMode(resolvedOwner);
        return true;
    }
    SetFocus(g_hNoteEdit);
    return true;
}

bool HandlePaneDirectionalNavigation(HWND owner, PaneNavContext context, HWND source, WPARAM vkey) {
    switch (context) {
    case PaneNavContext::LeftPaneList:
        switch (vkey) {
        case 'J':
            if (source == g_hNoteList && owner) {
                (void)OpenOrFocusSelectedNoteAtFileStart(owner);
            }
            return FocusNotePaneForPaneNav(owner);
        case 'K':
            if (source == g_hPdfList && owner) {
                (void)OpenSelectedPdf(owner);
            }
            return FocusPdfPaneForPaneNav(owner);
        case 'L':
            if (source == g_hNoteList) {
                if (owner) {
                    (void)OpenOrFocusSelectedNoteAtFileStart(owner);
                }
                return FocusNotePaneForPaneNav(owner);
            }
            if (source == g_hPdfList && owner) {
                (void)OpenSelectedPdf(owner);
            }
            return FocusPdfPaneForPaneNav(owner);
        default:
            return false;
        }
    case PaneNavContext::PdfPane:
        switch (vkey) {
        case 'H':
            return FocusListPaneForPaneNav(g_hPdfList);
        case 'J':
            return FocusNotePaneForPaneNav(owner);
        default:
            return false;
        }
    case PaneNavContext::NotePane:
        switch (vkey) {
        case 'H':
            ExitNoteNormalModeForPaneSwitch();
            return FocusListPaneForPaneNav(g_hNoteList);
        case 'K':
            ExitNoteNormalModeForPaneSwitch();
            return FocusPdfPaneForPaneNav(owner);
        default:
            return false;
        }
    default:
        return false;
    }
}

#include "ui/lists/main_pdf_list_context.cppinc"

static LRESULT CALLBACK PdfNoteListProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR, DWORD_PTR refData) {
    auto* state = reinterpret_cast<ListClickState*>(refData);
    switch (msg) {
    case WM_DROPFILES:
        HandlePdfNoteListDrop(hWnd, reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_SETFOCUS:
        EnforceImePolicyForWindow(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_KILLFOCUS:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        EnforceImePolicyForWindow(hWnd);
        return 0;
    case WM_LBUTTONDOWN: {
        DWORD hit = static_cast<DWORD>(SendMessageW(hWnd, LB_ITEMFROMPOINT, 0, lParam));
        int idx = static_cast<int>(LOWORD(hit));
        bool outside = HIWORD(hit) != 0;
        if (state) {
            state->suppressNextScroll = false;
            state->pendingSel = -1;
            state->openPath = (state->kind == ListClickKind::Pdf) ? CurrentLogicalPdfPath() : g_currentNotePath;
            state->preSel = static_cast<int>(SendMessageW(hWnd, LB_GETCURSEL, 0, 0));
            state->hitIndex = outside ? -1 : idx;
            state->tracking = !outside;
            // Re-select detection must not fire on double-click. Delay action until the
            // double-click time elapses, and cancel if a WM_LBUTTONDBLCLK arrives.
            KillTimer(hWnd, kListReselectTimerId);
            if (!outside && idx >= 0 && idx == state->preSel) {
                state->pendingSel = idx;
                UINT delay = std::max<UINT>(1, GetDoubleClickTime() + 10);
                SetTimer(hWnd, kListReselectTimerId, delay, nullptr);
            }
        }
        break;
    }
    case WM_LBUTTONDBLCLK:
        if (state) {
            state->suppressNextScroll = true;
            state->pendingSel = -1;
            KillTimer(hWnd, kListReselectTimerId);
        }
        break;
    case WM_CONTEXTMENU:
        if (ShowPdfListContextMenu(hWnd, lParam)) {
            return 0;
        }
        break;
    case WM_LBUTTONUP: {
        LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);
        if (state) {
            state->preSel = -1;
            state->hitIndex = -1;
            state->tracking = false;
            state->openPath.clear();
        }
        return res;
    }
    case WM_TIMER:
        if (wParam == kListReselectTimerId) {
            KillTimer(hWnd, kListReselectTimerId);
            if (!state) return 0;
            int curSel = static_cast<int>(SendMessageW(hWnd, LB_GETCURSEL, 0, 0));
            if (!state->suppressNextScroll && state->pendingSel >= 0 && curSel == state->pendingSel) {
                ActivateOrScrollSelection(GetParent(hWnd), state->kind, state->pendingSel);
            }
            state->pendingSel = -1;
            state->suppressNextScroll = false;
            return 0;
        }
        break;
    case kMsgListMaybeScroll: {
        if (!state) return 0;
        if (state->suppressNextScroll) {
            state->suppressNextScroll = false;
            state->pendingSel = -1;
            state->openPath.clear();
            return 0;
        }
        if (state->pendingSel >= 0) {
            ActivateOrScrollSelection(GetParent(hWnd), state->kind, state->pendingSel);
            state->pendingSel = -1;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (!state) break;
        if (wParam == VK_TAB) {
            if (g_hPdfView) SetFocus(g_hPdfView);
            return 0;
        }
        if (HandleHjklListNavigation(hWnd, msg, wParam)) {
            return 0;
        }
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            int curSel = static_cast<int>(SendMessageW(hWnd, LB_GETCURSEL, 0, 0));
            if (curSel >= 0) {
                ActivateOrScrollSelection(GetParent(hWnd), state->kind, curSel);
                return 0;
            }
        }
        break;
    }
    case WM_CHAR:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && IsCtrlPaneNavChar(wParam)) {
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        // Capture changes can happen during normal listbox click handling; do not
        // cancel re-select detection here. (We verify selection again on timer fire.)
        break;
    case WM_CANCELMODE:
        if (state) {
            state->preSel = -1;
            state->hitIndex = -1;
            state->tracking = false;
            state->suppressNextScroll = false;
            state->pendingSel = -1;
            state->openPath.clear();
            KillTimer(hWnd, kListReselectTimerId);
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static int ListboxItemFromClientPoint(HWND list, LPARAM pointParam, bool* outside) {
    DWORD hit = static_cast<DWORD>(SendMessageW(list, LB_ITEMFROMPOINT, 0, pointParam));
    if (outside) *outside = HIWORD(hit) != 0;
    return static_cast<int>(LOWORD(hit));
}

static void ResetDirectoryListDblClick(HWND list, DirectoryListDblClickState* state) {
    if (!state) return;
    state->pending = false;
    state->cancelled = false;
    state->hitIndex = -1;
    state->start = {};
    if (list && GetCapture() == list) {
        ReleaseCapture();
    }
}

static bool IsDirectoryListDblClickDeferred(HWND list) {
    DirectoryListDblClickState* state = nullptr;
    if (list == g_hLectureList) {
        state = &s_lectureListDblClick;
    } else if (list == g_hSessionList) {
        state = &s_sessionListDblClick;
    }
    return state && state->pending;
}

static void NotifyDirectoryListDblClick(HWND list) {
    HWND owner = list ? GetParent(list) : nullptr;
    if (!owner) return;
    SendMessageW(owner, WM_COMMAND, MAKEWPARAM(0, LBN_DBLCLK), reinterpret_cast<LPARAM>(list));
}

enum class DirectoryHierarchyItemKind {
    Info,
    Folder,
    Pdf,
    Image,
    Note
};

struct DirectoryHierarchyItem {
    std::wstring text;
    std::wstring path;
    DirectoryHierarchyItemKind kind = DirectoryHierarchyItemKind::Info;
};

struct DirectoryHierarchyPopupData {
    std::wstring title;
    std::vector<DirectoryHierarchyItem> items;
    HWND hTitle = nullptr;
    HWND hList = nullptr;
};

static HWND s_hDirectoryHierarchyPopup = nullptr;
static bool s_directoryHierarchyContextMenuActive = false;
static constexpr wchar_t kDirectoryHierarchyPopupClass[] = L"PdfNoteDirectoryHierarchyPopup";
static constexpr size_t kDirectoryHierarchyMaxLines = 600;
static constexpr int kDirectoryHierarchyMaxDepth = 6;
static std::unordered_set<std::wstring> s_hierarchyTempPdfKeys;
static std::unordered_set<std::wstring> s_hierarchyTempNoteKeys;
static std::wstring s_hierarchyTempOriginLectureKey;

static bool StartsWithInsensitive(const std::wstring& text, const std::wstring& prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (towlower(text[i]) != towlower(prefix[i])) return false;
    }
    return true;
}

static bool IsPotentialNetworkPath(const std::filesystem::path& path) {
    std::wstring s = path.native();
    if (StartsWithInsensitive(s, L"\\\\?\\UNC\\")) return true;
    if (StartsWithInsensitive(s, L"\\\\") && !StartsWithInsensitive(s, L"\\\\?\\")) return true;
    return false;
}

static std::wstring ListBoxTextAt(HWND list, int index) {
    if (!list || index < 0) return {};
    LRESULT len = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
    if (len == LB_ERR || len < 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    LRESULT got = SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(index),
                               reinterpret_cast<LPARAM>(text.data()));
    if (got == LB_ERR) return {};
    text.resize(static_cast<size_t>(got));
    return text;
}

static std::wstring DirectoryHierarchyDisplayName(const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    if (!name.empty()) return name;
    name = path.wstring();
    return name.empty() ? L"." : name;
}

static std::wstring DirectoryHierarchyTreePrefix(const std::vector<bool>& ancestorLast, bool isLast) {
    std::wstring prefix;
    for (bool last : ancestorLast) {
        prefix += last ? L"   " : L"│  ";
    }
    prefix += isLast ? L"└─ " : L"├─ ";
    return prefix;
}

static void AddDirectoryHierarchyInfo(std::vector<DirectoryHierarchyItem>& items,
                                      const std::vector<bool>& ancestorLast,
                                      const std::wstring& text) {
    items.push_back({DirectoryHierarchyTreePrefix(ancestorLast, true) + text,
                     {},
                     DirectoryHierarchyItemKind::Info});
}

static void AddDirectoryHierarchyTruncatedLine(std::vector<DirectoryHierarchyItem>& items,
                                               const std::vector<bool>& ancestorLast) {
    if (!items.empty() && items.back().text.find(L"...") != std::wstring::npos) return;
    AddDirectoryHierarchyInfo(
        items, ancestorLast, IsEnglishUi() ? L"... (more entries omitted)" : L"... (以降の項目は省略)");
}

static void AppendDirectoryHierarchyChildren(const std::filesystem::path& dir,
                                             int depth,
                                             std::vector<bool>& ancestorLast,
                                             std::vector<DirectoryHierarchyItem>& items) {
    if (items.size() >= kDirectoryHierarchyMaxLines) {
        AddDirectoryHierarchyTruncatedLine(items, ancestorLast);
        return;
    }
    if (depth > kDirectoryHierarchyMaxDepth) {
        AddDirectoryHierarchyTruncatedLine(items, ancestorLast);
        return;
    }

    struct ScanEntry {
        std::filesystem::path path;
        std::wstring name;
        DirectoryHierarchyItemKind kind = DirectoryHierarchyItemKind::Info;
    };

    std::vector<ScanEntry> children;
    std::error_code ec;
    std::filesystem::directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto path = it->path();
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(path, isReparse) && isReparse) continue;
        std::error_code stEc;
        if (it->is_directory(stEc) && !stEc) {
            children.push_back({path, DirectoryHierarchyDisplayName(path), DirectoryHierarchyItemKind::Folder});
            continue;
        }
        stEc.clear();
        if (!it->is_regular_file(stEc) || stEc) continue;
        if (IsPdfFile(path)) {
            children.push_back({path, DirectoryHierarchyDisplayName(path), DirectoryHierarchyItemKind::Pdf});
        } else if (IsImageFile(path)) {
            children.push_back({path, DirectoryHierarchyDisplayName(path), DirectoryHierarchyItemKind::Image});
        } else if (IsNoteFile(path)) {
            children.push_back({path, DirectoryHierarchyDisplayName(path), DirectoryHierarchyItemKind::Note});
        }
    }

    std::sort(children.begin(), children.end(),
              [](const ScanEntry& a, const ScanEntry& b) {
                  if (a.name != b.name) return a.name < b.name;
                  return static_cast<int>(a.kind) < static_cast<int>(b.kind);
              });

    for (size_t i = 0; i < children.size(); ++i) {
        const auto& child = children[i];
        const bool isLast = (i + 1 == children.size());
        if (items.size() >= kDirectoryHierarchyMaxLines) {
            AddDirectoryHierarchyTruncatedLine(items, ancestorLast);
            return;
        }
        const wchar_t* prefix = L"[?] ";
        switch (child.kind) {
        case DirectoryHierarchyItemKind::Folder: prefix = L"[D] "; break;
        case DirectoryHierarchyItemKind::Pdf: prefix = L"[P] "; break;
        case DirectoryHierarchyItemKind::Image: prefix = L"[I] "; break;
        case DirectoryHierarchyItemKind::Note: prefix = L"[N] "; break;
        case DirectoryHierarchyItemKind::Info: break;
        }
        items.push_back({DirectoryHierarchyTreePrefix(ancestorLast, isLast) + prefix + child.name,
                         child.path.wstring(),
                         child.kind});
        if (child.kind == DirectoryHierarchyItemKind::Folder) {
            ancestorLast.push_back(isLast);
            AppendDirectoryHierarchyChildren(child.path, depth + 1, ancestorLast, items);
            ancestorLast.pop_back();
        }
    }
}

static std::vector<DirectoryHierarchyItem> BuildDirectoryHierarchyItems(const std::filesystem::path& root) {
    std::vector<DirectoryHierarchyItem> items;
    items.reserve(128);
    items.push_back({L"[D] " + DirectoryHierarchyDisplayName(root),
                     root.wstring(),
                     DirectoryHierarchyItemKind::Folder});

    if (IsPotentialNetworkPath(root)) {
        std::vector<bool> ancestors;
        AddDirectoryHierarchyInfo(items, ancestors, IsEnglishUi()
                                                ? L"(network paths are not enumerated)"
                                                : L"(ネットワークパスは列挙しません)");
        return items;
    }

    bool isReparse = false;
    if (TryIsReparsePointNoFollow(root, isReparse) && isReparse) {
        std::vector<bool> ancestors;
        AddDirectoryHierarchyInfo(items, ancestors, IsEnglishUi()
                                                ? L"(reparse-point folders are not enumerated)"
                                                : L"(リパースポイントのフォルダは列挙しません)");
        return items;
    }

    const size_t before = items.size();
    std::vector<bool> ancestors;
    AppendDirectoryHierarchyChildren(root, 1, ancestors, items);
    if (items.size() == before) {
        AddDirectoryHierarchyInfo(items, ancestors, IsEnglishUi()
                                                ? L"(no folders, PDFs, or notes)"
                                                : L"(フォルダ/PDF/ノートなし)");
    }
    return items;
}

static void AddTemporaryOpenedFileToFileList(const std::wstring& path,
                                             DirectoryHierarchyItemKind kind,
                                             bool markSearchResult) {
    if (path.empty()) return;
    if (kind != DirectoryHierarchyItemKind::Pdf &&
        kind != DirectoryHierarchyItemKind::Image &&
        kind != DirectoryHierarchyItemKind::Note) {
        return;
    }

    const bool isNote = (kind == DirectoryHierarchyItemKind::Note);
    HWND list = isNote ? g_hNoteList : g_hPdfList;
    if (!list) return;

    auto& files = isNote ? g_noteFiles : g_pdfFiles;
    auto& hierarchyTempKeys = isNote ? s_hierarchyTempNoteKeys : s_hierarchyTempPdfKeys;
    auto& searchTempKeys = isNote ? s_searchTempNoteKeys : s_searchTempPdfKeys;
    const std::wstring targetKey = NormalizePathKeyForList(path);
    int targetIndex = -1;
    for (size_t i = 0; i < files.size(); ++i) {
        if (NormalizePathKeyForList(files[i].path) == targetKey) {
            targetIndex = static_cast<int>(i);
            files[i].isPdf = kind == DirectoryHierarchyItemKind::Pdf;
            break;
        }
    }
    if (targetIndex < 0) {
        files.push_back({path, kind == DirectoryHierarchyItemKind::Pdf});
        targetIndex = static_cast<int>(files.size() - 1);
        if (markSearchResult) {
            searchTempKeys.insert(targetKey);
        } else {
            hierarchyTempKeys.insert(targetKey);
        }
        s_hierarchyTempOriginLectureKey = NormalizePathKeyForList(g_currentLecturePath);
    }

    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::filesystem::path displayRoot = !g_currentSessionPath.empty()
        ? std::filesystem::path(g_currentSessionPath)
        : std::filesystem::path(path).parent_path();
    for (const auto& file : files) {
        std::wstring label = FileDisplayLabelForPath(file.path, files, displayRoot);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(targetIndex), 0);
    SetListWide(list);
    InvalidateRect(list, nullptr, TRUE);
}

static void AddHierarchyOpenedFileToFileList(const std::wstring& path,
                                             DirectoryHierarchyItemKind kind) {
    AddTemporaryOpenedFileToFileList(path, kind, false);
}

bool OpenSearchResultFileInCurrentFileList(HWND owner, const std::wstring& path) {
    if (path.empty()) return false;
    DirectoryHierarchyItemKind kind = DirectoryHierarchyItemKind::Info;
    if (IsPdfFile(std::filesystem::path(path))) {
        kind = DirectoryHierarchyItemKind::Pdf;
    } else if (IsImageFile(std::filesystem::path(path))) {
        kind = DirectoryHierarchyItemKind::Image;
    } else if (IsNoteFile(std::filesystem::path(path))) {
        kind = DirectoryHierarchyItemKind::Note;
    } else {
        return false;
    }

    AddTemporaryOpenedFileToFileList(path, kind, true);
    HWND targetOwner = owner ? owner : g_hMainWnd;
    if (kind == DirectoryHierarchyItemKind::Note) {
        OnNoteSelChange(targetOwner);
        return NormalizePathKeyForList(g_currentNotePath) == NormalizePathKeyForList(path);
    }
    OnPdfSelChange(targetOwner);
    return NormalizePathKeyForList(CurrentLogicalPdfPath()) == NormalizePathKeyForList(path);
}

static void RebuildFileListBox(HWND list, const std::vector<FileEntry>& files) {
    if (!list) return;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::filesystem::path displayRoot = !g_currentSessionPath.empty()
        ? std::filesystem::path(g_currentSessionPath)
        : std::filesystem::path(g_currentLecturePath);
    for (const auto& file : files) {
        std::wstring label = FileDisplayLabelForPath(file.path, files, displayRoot);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SetListWide(list);
}

static int FindFileListIndexByPath(const std::vector<FileEntry>& files, const std::wstring& path) {
    if (path.empty()) return -1;
    const std::wstring key = NormalizePathKeyForList(path);
    for (size_t i = 0; i < files.size(); ++i) {
        if (NormalizePathKeyForList(files[i].path) == key) return static_cast<int>(i);
    }
    return -1;
}

static bool RemoveHierarchyTemporaryFilesFromList(std::vector<FileEntry>& files,
                                                  const std::unordered_set<std::wstring>& tempKeys) {
    if (tempKeys.empty() || files.empty()) return false;
    const auto before = files.size();
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const FileEntry& file) {
                                   return tempKeys.find(NormalizePathKeyForList(file.path)) != tempKeys.end();
                               }),
                files.end());
    return files.size() != before;
}

static void ClearHierarchyTemporaryFileDisplaysIfSafe() {
    if (s_hierarchyTempPdfKeys.empty() && s_hierarchyTempNoteKeys.empty()) return;
    if (s_hierarchyTempOriginLectureKey.empty()) return;
    if (NormalizePathKeyForList(g_currentLecturePath) == s_hierarchyTempOriginLectureKey) return;
    if (file_output::HasAnyStagedDiffs()) return;

    const bool removedPdf = RemoveHierarchyTemporaryFilesFromList(g_pdfFiles, s_hierarchyTempPdfKeys);
    const bool removedNote = RemoveHierarchyTemporaryFilesFromList(g_noteFiles, s_hierarchyTempNoteKeys);
    s_hierarchyTempPdfKeys.clear();
    s_hierarchyTempNoteKeys.clear();
    s_hierarchyTempOriginLectureKey.clear();

    if (removedPdf) {
        RebuildFileListBox(g_hPdfList, g_pdfFiles);
        int idx = FindFileListIndexByPath(g_pdfFiles, CurrentLogicalPdfPath());
        SendMessageW(g_hPdfList, LB_SETCURSEL, idx >= 0 ? static_cast<WPARAM>(idx) : static_cast<WPARAM>(-1), 0);
        InvalidateRect(g_hPdfList, nullptr, TRUE);
    }
    if (removedNote) {
        RebuildFileListBox(g_hNoteList, g_noteFiles);
        int idx = FindFileListIndexByPath(g_noteFiles, g_currentNotePath);
        SendMessageW(g_hNoteList, LB_SETCURSEL, idx >= 0 ? static_cast<WPARAM>(idx) : static_cast<WPARAM>(-1), 0);
        InvalidateRect(g_hNoteList, nullptr, TRUE);
    }
}

static bool OpenDirectoryHierarchySelection(HWND list) {
    if (!list) return false;
    HWND popup = GetAncestor(list, GA_ROOT);
    if (!popup) return false;
    auto* data = reinterpret_cast<DirectoryHierarchyPopupData*>(GetWindowLongPtrW(popup, GWLP_USERDATA));
    if (!data) return false;
    int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(data->items.size())) return false;

    const auto item = data->items[static_cast<size_t>(sel)];
    HWND owner = GetWindow(popup, GW_OWNER);
    if (!owner) owner = GetParent(popup);
    if (!owner) owner = g_hMainWnd;

    switch (item.kind) {
    case DirectoryHierarchyItemKind::Pdf:
    case DirectoryHierarchyItemKind::Image:
        if (!OpenPdfIfDifferent(owner, item.path)) return false;
        AddHierarchyOpenedFileToFileList(item.path, item.kind);
        return true;
    case DirectoryHierarchyItemKind::Note:
        if (!OpenNoteIfDifferent(owner, item.path)) return false;
        AddHierarchyOpenedFileToFileList(item.path, item.kind);
        return true;
    default:
        return false;
    }
}

static bool IsDirectoryHierarchyAppOpenable(DirectoryHierarchyItemKind kind) {
    return kind == DirectoryHierarchyItemKind::Pdf ||
           kind == DirectoryHierarchyItemKind::Image ||
           kind == DirectoryHierarchyItemKind::Note;
}

static bool IsDirectoryHierarchyExplorerOpenable(DirectoryHierarchyItemKind kind) {
    return kind == DirectoryHierarchyItemKind::Folder ||
           kind == DirectoryHierarchyItemKind::Pdf ||
           kind == DirectoryHierarchyItemKind::Image ||
           kind == DirectoryHierarchyItemKind::Note;
}

static bool OpenDirectoryHierarchyItemInExplorer(HWND owner, const DirectoryHierarchyItem& item) {
    if (item.path.empty() || !IsDirectoryHierarchyExplorerOpenable(item.kind)) return false;
    return ShowPathInExplorer(owner, item.path);
}

#include "ui/lists/main_directory_list_context.cppinc"

static LRESULT CALLBACK DirectoryListProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR, DWORD_PTR refData) {
    auto* dblClick = reinterpret_cast<DirectoryListDblClickState*>(refData);
    switch (msg) {
    case WM_SETFOCUS:
        EnforceImePolicyForWindow(hWnd);
        InvalidateDirectoryListFocusFrame(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_KILLFOCUS:
        ResetDirectoryListDblClick(hWnd, dblClick);
        InvalidateDirectoryListFocusFrame(hWnd);
        SendMessageW(hWnd, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        EnforceImePolicyForWindow(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemePanelBrush ? g_hThemePanelBrush : CreateSolidBrush(g_theme.panelBg);
        FillRect(hdc, &rc, bg);
        if (!g_hThemePanelBrush) DeleteObject(bg);
        return 1;
    }
    case WM_KEYDOWN:
        if (wParam == VK_TAB) {
            if (g_hPdfView) SetFocus(g_hPdfView);
            return 0;
        }
        if (HandleHjklListNavigation(hWnd, msg, wParam)) {
            return 0;
        }
        break;
    case WM_CHAR:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && IsCtrlPaneNavChar(wParam)) {
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        ResetDirectoryListDblClick(hWnd, dblClick);
        break;
    case WM_LBUTTONDBLCLK: {
        if (!dblClick) return 0;
        bool outside = false;
        int idx = ListboxItemFromClientPoint(hWnd, lParam, &outside);
        if (outside || idx < 0) {
            ResetDirectoryListDblClick(hWnd, dblClick);
            return 0;
        }
        dblClick->pending = true;
        dblClick->cancelled = false;
        dblClick->hitIndex = idx;
        dblClick->start = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        SetCapture(hWnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dblClick && dblClick->pending && (wParam & MK_LBUTTON)) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int dragX = std::max<int>(1, GetSystemMetrics(SM_CXDRAG));
            const int dragY = std::max<int>(1, GetSystemMetrics(SM_CYDRAG));
            bool outside = false;
            int idx = ListboxItemFromClientPoint(hWnd, lParam, &outside);
            if (std::abs(pt.x - dblClick->start.x) > dragX ||
                std::abs(pt.y - dblClick->start.y) > dragY ||
                outside || idx != dblClick->hitIndex) {
                dblClick->cancelled = true;
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (dblClick && dblClick->pending) {
            bool outside = false;
            int idx = ListboxItemFromClientPoint(hWnd, lParam, &outside);
            bool accept = !dblClick->cancelled && !outside && idx == dblClick->hitIndex;
            ResetDirectoryListDblClick(hWnd, dblClick);
            if (accept) {
                NotifyDirectoryListDblClick(hWnd);
            }
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (dblClick && dblClick->pending && reinterpret_cast<HWND>(lParam) != hWnd) {
            ResetDirectoryListDblClick(hWnd, dblClick);
        }
        break;
    case WM_CANCELMODE:
        ResetDirectoryListDblClick(hWnd, dblClick);
        break;
    case WM_CONTEXTMENU:
        ResetDirectoryListDblClick(hWnd, dblClick);
        if (ShowDirectoryListContextMenu(hWnd, lParam)) {
            return 0;
        }
        break;
    case WM_PAINT: {
        LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);
        DrawDirectoryListFocusFrame(hWnd);
        return res;
    }
    case WM_NCPAINT: {
        LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);
        DrawDirectoryListFocusFrame(hWnd);
        return res;
    }
    case WM_ENABLE:
    case WM_THEMECHANGED:
        InvalidateDirectoryListFocusFrame(hWnd);
        break;
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

namespace {
constexpr std::array<int, 9> kToolbarTextFontPt10Values = {100, 120, 140, 160, 180, 200, 240, 280, 320};
constexpr DWORD_PTR kToolbarTextFontFixedFlag = 0x10000;
constexpr double kToolbarTextFontRatioBasePt = 14.0;
constexpr double kA4WidthPt = 595.0;
constexpr double kA4HeightPt = 842.0;
constexpr double kA4SizeTolerancePt = 6.0;
}

static DWORD_PTR EncodeToolbarTextFontComboData(bool useA4Scale, int pt10) {
    DWORD_PTR data = static_cast<DWORD_PTR>(std::max(0, pt10));
    if (!useA4Scale) data |= kToolbarTextFontFixedFlag;
    return data;
}

static bool ToolbarTextFontComboUsesA4Scale(DWORD_PTR data) {
    return (data & kToolbarTextFontFixedFlag) == 0;
}

static int ToolbarTextFontComboPt10(DWORD_PTR data) {
    return static_cast<int>(data & ~kToolbarTextFontFixedFlag);
}

static bool IsA4LikePageSizePt(double widthPt, double heightPt) {
    auto closeEnough = [](double a, double b) {
        return std::abs(a - b) <= kA4SizeTolerancePt;
    };
    return (closeEnough(widthPt, kA4WidthPt) && closeEnough(heightPt, kA4HeightPt)) ||
           (closeEnough(widthPt, kA4HeightPt) && closeEnough(heightPt, kA4WidthPt));
}

static int CurrentToolbarTextFontPageIndex() {
    if (!g_hPdfView || g_pdf.pages.empty()) return -1;
    int pageIndex = PageAtCurrentView();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(g_pdf.pages.size())) return -1;
    return pageIndex;
}

static bool ShouldShowFixedToolbarTextFontChoices(int pageIndex) {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(g_pdf.pages.size())) return false;
    const auto& page = g_pdf.pages[static_cast<size_t>(pageIndex)];
    return !IsA4LikePageSizePt(page.widthPt, page.heightPt);
}

static std::wstring BuildAdaptiveToolbarTextFontLabel(int pt10) {
    double ratio = static_cast<double>(pt10) / (kToolbarTextFontRatioBasePt * 10.0);
    double pt = static_cast<double>(pt10) / 10.0;
    if (IsEnglishUi()) {
        return L"x" + FormatSig2(ratio) + L" A4 size ratio (" + FormatSig2(pt) + L"pt)";
    }
    return L"x" + FormatSig2(ratio) + L" A4基準サイズ比 (" + FormatSig2(pt) + L"pt)";
}

static std::wstring BuildFixedToolbarTextFontLabel(int pt10) {
    double pt = static_cast<double>(pt10) / 10.0;
    if (IsEnglishUi()) {
        return L"Fixed " + FormatSig2(pt) + L"pt";
    }
    return L"固定 " + FormatSig2(pt) + L"pt";
}

static void SelectToolbarTextFontComboChoice(HWND combo, DWORD_PTR wantData) {
    if (!combo) return;
    int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    int fallbackIndex = -1;
    int closestIndex = -1;
    int closestDelta = std::numeric_limits<int>::max();
    for (int i = 0; i < count; ++i) {
        DWORD_PTR data = static_cast<DWORD_PTR>(SendMessageW(combo, CB_GETITEMDATA, i, 0));
        if (data == wantData) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            return;
        }
        if (ToolbarTextFontComboPt10(data) == ToolbarTextFontComboPt10(wantData) &&
            ToolbarTextFontComboUsesA4Scale(data)) {
            fallbackIndex = i;
        }
        int delta = std::abs(ToolbarTextFontComboPt10(data) - ToolbarTextFontComboPt10(wantData));
        if (delta < closestDelta) {
            closestDelta = delta;
            closestIndex = i;
        }
    }
    if (fallbackIndex >= 0) {
        SendMessageW(combo, CB_SETCURSEL, fallbackIndex, 0);
    } else if (closestIndex >= 0) {
        SendMessageW(combo, CB_SETCURSEL, closestIndex, 0);
    } else if (count > 0) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    } else {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
}

static void SaveActiveToolbarFontSizeSlot() {
    if (std::clamp(g_textFontActiveSizeSlot, 0, 1) == 1) {
        g_textFontPtSlotB = std::clamp(g_textFontPt, 6.0, 96.0);
        g_textFontUseA4ScaleSlotB = g_textFontUseA4Scale;
    } else {
        g_textFontPtSlotA = std::clamp(g_textFontPt, 6.0, 96.0);
        g_textFontUseA4ScaleSlotA = g_textFontUseA4Scale;
    }
}

static void LoadActiveToolbarFontSizeSlot() {
    g_textFontActiveSizeSlot = std::clamp(g_textFontActiveSizeSlot, 0, 1);
    if (g_textFontActiveSizeSlot == 1) {
        g_textFontPt = std::clamp(g_textFontPtSlotB, 6.0, 96.0);
        g_textFontUseA4Scale = g_textFontUseA4ScaleSlotB;
    } else {
        g_textFontPt = std::clamp(g_textFontPtSlotA, 6.0, 96.0);
        g_textFontUseA4Scale = g_textFontUseA4ScaleSlotA;
    }
}

static void SetActiveToolbarFontSizeSlot(int slot) {
    slot = std::clamp(slot, 0, 1);
    if (slot == std::clamp(g_textFontActiveSizeSlot, 0, 1)) return;
    SaveActiveToolbarFontSizeSlot();
    g_textFontActiveSizeSlot = slot;
    LoadActiveToolbarFontSizeSlot();
    SyncToolbarFontSizeCombo();
    PersistConfig();
    if (g_hPdfView) PostMessageW(g_hPdfView, kMsgTextBoxFontUpdate, 0, 0);
}

static bool ReadToolbarFontSizeComboValue(HWND combo, double* outPt, bool* outUseA4Scale) {
    if (!combo) return false;
    int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel < 0) return false;
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    if (data == CB_ERR) return false;
    DWORD_PTR itemData = static_cast<DWORD_PTR>(data);
    if (outUseA4Scale) *outUseA4Scale = ToolbarTextFontComboUsesA4Scale(itemData);
    if (outPt) *outPt = static_cast<double>(ToolbarTextFontComboPt10(itemData)) / 10.0;
    return true;
}

static bool ApplyToolbarFontSizeComboSelection(HWND combo) {
    double pt = g_textFontPt;
    bool useA4Scale = g_textFontUseA4Scale;
    if (!ReadToolbarFontSizeComboValue(combo, &pt, &useA4Scale)) return false;
    const bool slotB = (combo == g_hComboFontSizeAlt);
    if (slotB) {
        g_textFontPtSlotB = pt;
        g_textFontUseA4ScaleSlotB = useA4Scale;
    } else {
        g_textFontPtSlotA = pt;
        g_textFontUseA4ScaleSlotA = useA4Scale;
    }
    if ((slotB ? 1 : 0) == std::clamp(g_textFontActiveSizeSlot, 0, 1)) {
        g_textFontPt = pt;
        g_textFontUseA4Scale = useA4Scale;
    }
    return true;
}

static void SyncToolbarFontSizeComboForSlot(HWND combo, double pt, bool useA4Scale) {
    if (!combo || !IsWindow(combo)) return;
    if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0) return;

    const int pageIndex = CurrentToolbarTextFontPageIndex();
    const bool showFixed = ShouldShowFixedToolbarTextFontChoices(pageIndex);
    const int pt10 = static_cast<int>(std::llround(std::clamp(pt, 6.0, 96.0) * 10.0));
    const DWORD_PTR wantData = EncodeToolbarTextFontComboData(useA4Scale, pt10);
    const int expectedCount = static_cast<int>(kToolbarTextFontPt10Values.size()) * (showFixed ? 2 : 1);

    struct FontSizeComboCache {
        HWND combo = nullptr;
        int pageIndex = std::numeric_limits<int>::min();
        bool showFixed = false;
        DWORD_PTR wantData = std::numeric_limits<DWORD_PTR>::max();
    };
    static FontSizeComboCache s_cache[2];
    FontSizeComboCache& cache = (combo == g_hComboFontSizeAlt) ? s_cache[1] : s_cache[0];

    int currentCount = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    bool needRebuild = (combo != cache.combo) ||
                       (pageIndex != cache.pageIndex) ||
                       (showFixed != cache.showFixed) ||
                       (currentCount != expectedCount);
    bool needReselect = needRebuild || (wantData != cache.wantData);

    if (needRebuild) {
        SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (int pt10Value : kToolbarTextFontPt10Values) {
            std::wstring label = BuildAdaptiveToolbarTextFontLabel(pt10Value);
            int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
            if (idx >= 0) {
                SendMessageW(combo, CB_SETITEMDATA, idx,
                             static_cast<LPARAM>(EncodeToolbarTextFontComboData(true, pt10Value)));
            }
        }
        if (showFixed) {
            for (int pt10Value : kToolbarTextFontPt10Values) {
                std::wstring label = BuildFixedToolbarTextFontLabel(pt10Value);
                int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
                if (idx >= 0) {
                    SendMessageW(combo, CB_SETITEMDATA, idx,
                                 static_cast<LPARAM>(EncodeToolbarTextFontComboData(false, pt10Value)));
                }
            }
        }
        SendMessageW(combo, CB_SETDROPPEDWIDTH, 240, 0);
        SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
    }

    if (needReselect) {
        SelectToolbarTextFontComboChoice(combo, wantData);
    }

    cache.combo = combo;
    cache.pageIndex = pageIndex;
    cache.showFixed = showFixed;
    cache.wantData = wantData;
}

void SyncToolbarFontSizeCombo() {
    g_textFontActiveSizeSlot = std::clamp(g_textFontActiveSizeSlot, 0, 1);
    SyncToolbarFontSizeComboForSlot(g_hComboFontSize, g_textFontPtSlotA, g_textFontUseA4ScaleSlotA);
    SyncToolbarFontSizeComboForSlot(g_hComboFontSizeAlt, g_textFontPtSlotB, g_textFontUseA4ScaleSlotB);
    if (g_hRadioFontSizeSlotA) {
        SendMessageW(g_hRadioFontSizeSlotA, BM_SETCHECK,
                     g_textFontActiveSizeSlot == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hRadioFontSizeSlotB) {
        SendMessageW(g_hRadioFontSizeSlotB, BM_SETCHECK,
                     g_textFontActiveSizeSlot == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}


#include "workspace/workspace_controller.cppinc"

#include "ui/core/main_view_layout.cppinc"


static void EnableOwnerDrawButton(HWND hWnd) {
    if (!hWnd) return;
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    style &= ~BS_TYPEMASK;
    style |= BS_OWNERDRAW;
    SetWindowLongPtrW(hWnd, GWL_STYLE, style);
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void RestoreButtonType(HWND hWnd, LONG_PTR typeStyle) {
    if (!hWnd) return;
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    style &= ~BS_TYPEMASK;
    style |= (typeStyle & BS_TYPEMASK);
    SetWindowLongPtrW(hWnd, GWL_STYLE, style);
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void InvalidateThemeButton(HWND hWnd) {
    if (!hWnd) return;
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

static bool EnsureToolbarPaletteButtons() {
    if (!g_hPdfToolbar) return false;
    bool changed = false;
    while (g_colorButtons.size() > g_palette.size()) {
        HWND h = g_colorButtons.back();
        g_colorButtons.pop_back();
        if (h && IsWindow(h)) {
            DestroyWindow(h);
            changed = true;
        }
    }
    while (g_colorButtons.size() < g_palette.size()) {
        const size_t i = g_colorButtons.size();
        HWND btn = CreateWindowExW(0, L"BUTTON", L"",
                                   WS_CHILD | BS_OWNERDRAW,
                                   0, 0, 24, 24, g_hPdfToolbar,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TOOL_COLOR_BASE + static_cast<int>(i))),
                                   g_hInst, nullptr);
        if (!btn) break;
        SetUIFont(btn);
        g_colorButtons.push_back(btn);
        changed = true;
    }
    return changed;
}

static void ResetMenuBackgroundToSystem(HMENU menu) {
    if (!menu) return;
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = GetSysColorBrush(COLOR_MENU);
    SetMenuInfo(menu, &mi);
}

static void RemoveMenuOwnerDraw(HMENU menu) {
    if (!menu) return;
    ResetMenuBackgroundToSystem(menu);

    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;

        if (mii.fType & MFT_OWNERDRAW) {
            auto* data = reinterpret_cast<MenuItemData*>(mii.dwItemData);
            ReleaseMenuItemData(data);
            mii.fType &= ~MFT_OWNERDRAW;
            mii.dwItemData = 0;
            SetMenuItemInfoW(menu, i, TRUE, &mii);
        }
        if (mii.hSubMenu) {
            RemoveMenuOwnerDraw(mii.hSubMenu);
        }
    }
}

static void RemoveOwnerDrawButtonsOnly(HWND hWnd) {
    RestoreButtonType(g_hBtnNewSession, BS_PUSHBUTTON);
    RestoreButtonType(g_hBtnNewNote, BS_PUSHBUTTON);
    RestoreButtonType(g_hBtnToggleAnn, BS_PUSHBUTTON);
    RestoreButtonType(g_hAnnotShow, BS_AUTOCHECKBOX);

    RestoreButtonType(g_hBtnModeSelect, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModePan, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeMagnifier, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeMarker, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeMarkerFree, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeMarkerLine, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeMarkerArrow, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeMarkerWave, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeText, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeLine, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeArrow, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeWave, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeFreehand, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeShape, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hBtnModeEraser, BS_AUTORADIOBUTTON);

    RestoreButtonType(g_hBtnPaletteCustom, BS_PUSHBUTTON);
    RestoreButtonType(g_hRadioFontSizeSlotA, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hRadioFontSizeSlotB, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hChkTextReadableBackground, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hRadioTextReadableBackgroundNormal, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hRadioTextReadableBackgroundInverted, BS_AUTORADIOBUTTON);
    RestoreButtonType(g_hAnnotSettings, BS_PUSHBUTTON);
    RestoreButtonType(g_hAnnotClear, BS_PUSHBUTTON);
    RestoreButtonType(g_hChkShortcutHeading1, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hChkShortcutHeading2, BS_PUSHBUTTON);
    RestoreButtonType(g_hBtnShortcutHeadingLevelUp, BS_PUSHBUTTON);
    RestoreButtonType(g_hChkShortcutBack, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hChkShortcutChar, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hChkShortcutBold, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hChkShortcutItalic, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hChkShortcutStrike, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hChkShortcutUnderline, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hChkShortcutLinkDecor, BS_AUTOCHECKBOX);
    RestoreButtonType(g_hBtnShortcutBackPalette, BS_PUSHBUTTON);
    RestoreButtonType(g_hBtnShortcutCharPalette, BS_PUSHBUTTON);
    RestoreButtonType(g_hBtnShortcutInput, BS_PUSHBUTTON);
    RestoreButtonType(g_hBtnShortcutPdfLink, BS_PUSHBUTTON);
    if (hWnd) RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void ApplyOwnerDrawUi(HWND hWnd) {
    static bool s_lastOwnerDraw = false;
    const bool wantOwnerDraw = g_config.ownerDrawUi;

    auto rebuildMenuBar = [&](bool prevOwnerDraw) {
        if (!hWnd) return;
        HMENU oldMenu = GetMenu(hWnd);
        if (!oldMenu) oldMenu = g_hMainMenu;

        if (wantOwnerDraw && !prevOwnerDraw) {
            // Safe: previous menu is non-owner-drawn, so it doesn't reference MenuItemData pointers.
            g_menuItemData.clear();
        }

        HMENU bar = BuildMenuBar(); // uses current g_config.ownerDrawUi
        if (!bar) return;
        SetMenu(hWnd, bar);
        g_hMainMenu = bar;

        // Ensure the status text is restored on the new menu.
        RefreshStatusDisplay(hWnd);

        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        DrawMenuBar(hWnd);

        if (oldMenu && oldMenu != bar) {
            DestroyMenu(oldMenu);
        }
        if (!wantOwnerDraw) {
            // Now safe: the new menu is not owner-drawn, so no menu references MenuItemData.
            g_menuItemData.clear();
        }
    };

    if (wantOwnerDraw != s_lastOwnerDraw) {
        if (hWnd && (GetMenu(hWnd) || g_hMainMenu)) {
            rebuildMenuBar(s_lastOwnerDraw);
        }
        s_lastOwnerDraw = wantOwnerDraw;
    }

    if (!g_config.ownerDrawUi) {
        RemoveOwnerDrawButtonsOnly(hWnd);
        return;
    }
    EnableOwnerDrawButton(g_hBtnNewSession);
    EnableOwnerDrawButton(g_hBtnNewNote);
    EnableOwnerDrawButton(g_hBtnToggleAnn);
    EnableOwnerDrawButton(g_hAnnotShow);
    EnableOwnerDrawButton(g_hBtnModeSelect);
    EnableOwnerDrawButton(g_hBtnModePan);
    EnableOwnerDrawButton(g_hBtnModeMagnifier);
    EnableOwnerDrawButton(g_hBtnModeMarker);
    EnableOwnerDrawButton(g_hBtnModeMarkerFree);
    EnableOwnerDrawButton(g_hBtnModeMarkerLine);
    EnableOwnerDrawButton(g_hBtnModeMarkerArrow);
    EnableOwnerDrawButton(g_hBtnModeMarkerWave);
    EnableOwnerDrawButton(g_hBtnModeText);
    EnableOwnerDrawButton(g_hBtnModeLine);
    EnableOwnerDrawButton(g_hBtnModeArrow);
    EnableOwnerDrawButton(g_hBtnModeWave);
    EnableOwnerDrawButton(g_hBtnModeFreehand);
    EnableOwnerDrawButton(g_hBtnModeShape);
    EnableOwnerDrawButton(g_hBtnModeEraser);
    EnableOwnerDrawButton(g_hBtnPaletteCustom);
    EnableOwnerDrawButton(g_hRadioFontSizeSlotA);
    EnableOwnerDrawButton(g_hRadioFontSizeSlotB);
    EnableOwnerDrawButton(g_hAnnotSettings);
    EnableOwnerDrawButton(g_hAnnotClear);
    EnableOwnerDrawButton(g_hChkShortcutHeading1);
    EnableOwnerDrawButton(g_hChkShortcutHeading2);
    EnableOwnerDrawButton(g_hBtnShortcutHeadingLevelUp);
    EnableOwnerDrawButton(g_hChkShortcutBack);
    EnableOwnerDrawButton(g_hChkShortcutChar);
    EnableOwnerDrawButton(g_hChkShortcutBold);
    EnableOwnerDrawButton(g_hChkShortcutItalic);
    EnableOwnerDrawButton(g_hChkShortcutStrike);
    EnableOwnerDrawButton(g_hChkShortcutUnderline);
    EnableOwnerDrawButton(g_hChkShortcutLinkDecor);
    EnableOwnerDrawButton(g_hBtnShortcutBackPalette);
    EnableOwnerDrawButton(g_hBtnShortcutCharPalette);
    EnableOwnerDrawButton(g_hBtnShortcutInput);
    EnableOwnerDrawButton(g_hBtnShortcutPdfLink);
    if (hWnd) RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void EnsureOwnerDrawUi(HWND hWnd) {
    ApplyOwnerDrawUi(hWnd);
}



// ---------------------------------------------------------------------
// Listbox selection handlers
// ---------------------------------------------------------------------
static void OnLectureSelChange(HWND hWnd) {
    const std::wstring previousLecturePath = g_currentLecturePath;
    const std::wstring previousSessionPath = g_currentSessionPath;
    const std::wstring previousPdfPath = CurrentLogicalPdfPath();
    const std::wstring previousNotePath = g_currentNotePath;
    const ULONGLONG startTick = preview_trace::TickNow();
    try {
    fault_injection::MaybeThrow(L"OnLectureSelChange:start");
    if (s_ignoreLectureSelChange) return;
    int sel = static_cast<int>(SendMessageW(g_hLectureList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_lectures.size())) return;
    const std::wstring nextLecture = g_lectures[static_cast<size_t>(sel)];
    SwitchTimingScope timing(L"lecture_switch", nextLecture);
    timing.Mark(L"selected");
    auto curCanon = g_currentLecturePath.empty() ? std::filesystem::path{} : CanonicalOrSelf(std::filesystem::path(g_currentLecturePath));
    auto nextCanon = CanonicalOrSelf(std::filesystem::path(nextLecture));
    if (!curCanon.empty() && !nextCanon.empty() && curCanon == nextCanon) {
        timing.SetOutcome(L"noop_same_path");
        return;
    }
    RememberCurrentSessionFiles();
    if (!SaveNoteIfDirty(hWnd)) {
        timing.SetOutcome(L"cancelled_save_note");
        SyncLeftPaneSelection();
        PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching lectures"
                                                        : (g_config.studentMode ? L"授業切替" : L"上位項目切替"),
                                    IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                  : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
        return;
    }
    timing.Mark(L"SaveNoteIfDirty");
    if (!VerifyWorkspaceWritableForEditing(hWnd) ||
        !VerifyDirReadableWritableForEditing(hWnd, std::filesystem::path(nextLecture),
                                             g_config.studentMode ? L"授業フォルダ" : L"上位項目フォルダ",
                                             g_config.studentMode ? L"Lecture folder" : L"Parent item folder")) {
        timing.SetOutcome(L"cancelled_verify_dir");
        SyncLeftPaneSelection();
        return;
    }
    timing.Mark(L"VerifyWritable");
    // Include a pending PDF endpoint in the staged annotations before the
    // lecture switch can close and clear the current PDF.
    PreparePendingLinkForPdfSwitch(hWnd);
    if (!file_output::PrepareStagedDiffsForSwitch(hWnd)) {
        timing.SetOutcome(L"cancelled_stage_before_switch");
        SyncLeftPaneSelection();
        PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching lectures"
                                                        : (g_config.studentMode ? L"授業切替" : L"上位項目切替"),
                                    IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                  : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
        return;
    }
    timing.Mark(L"PrepareStagedDiffsForSwitch");
    {
        ScopedDeferredMainWindowUiRefresh deferredUi(hWnd);
        ScopedDeferredLeftPaneSelection deferredLeftPaneSelection;
        if (g_pdfPreviewActive) {
            DisableIntegratedPdfPreview(hWnd, true);
        }
        ClearPdfAndNoteSelection();
        g_currentSessionPath.clear();
        g_currentLecturePath = nextLecture;
        LoadSessions(g_currentLecturePath);
        SaveStartupLastOpenTarget();
        timing.Mark(L"LoadSessions");
        if (g_sessions.empty()) {
            // Direct files mode: list PDF/notes directly under the selected lecture directory.
            SessionEntry direct;
            direct.displayName = std::filesystem::path(g_currentLecturePath).filename().wstring();
            direct.path = g_currentLecturePath;
            std::wstring preferredPdf;
            std::wstring preferredNote;
            ApplySessionAutoOpenPreference(direct, preferredPdf, preferredNote);
            LoadFiles(direct, preferredPdf, preferredNote);
            timing.Mark(L"DirectFilesLoadFiles");
            timing.Mark(L"DirectFilesAutoOpenPdf");
            timing.Mark(L"DirectFilesAutoOpenNote");
        } else {
            SendMessageW(g_hPdfList, LB_RESETCONTENT, 0, 0);
            SendMessageW(g_hNoteList, LB_RESETCONTENT, 0, 0);
            g_pdfFiles.clear();
            g_noteFiles.clear();
            InvalidateRect(g_hPdfList, nullptr, TRUE);
            InvalidateRect(g_hNoteList, nullptr, TRUE);
            timing.Mark(L"ClearFileLists");
        }
        SyncLeftPaneSelection();
        FlushDeferredLeftPaneSelection();
        timing.Mark(L"SyncLeftPaneSelection");
        RefreshMainWindowUiState(hWnd);
        file_output::ScheduleIntegrateAfterSwitch(hWnd);
        ClearHierarchyTemporaryFileDisplaysIfSafe();
        timing.Mark(L"ScheduleIntegrateAfterSwitch");
    }
    timing.Mark(L"FlushDeferredMainWindowUiRefresh");
    timing.SetOutcome(L"ok");
    preview_trace::Append(
        L"OnLectureSelChange",
        L"end lecturePath=" + g_currentLecturePath +
        L" sessionCount=" + std::to_wstring(g_sessions.size()) +
        L" pdfCount=" + std::to_wstring(g_pdfFiles.size()) +
        L" noteCount=" + std::to_wstring(g_noteFiles.size()) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("OnLectureSelChange", ex.what());
        RestoreLectureSessionStateAfterException(hWnd, previousLecturePath, previousSessionPath,
                                                previousPdfPath, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching lectures"
                                                         : (g_config.studentMode ? L"授業切替" : L"上位項目切替"));
    } catch (...) {
        AppendMainOperationExceptionLog("OnLectureSelChange", nullptr);
        RestoreLectureSessionStateAfterException(hWnd, previousLecturePath, previousSessionPath,
                                                previousPdfPath, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching lectures"
                                                         : (g_config.studentMode ? L"授業切替" : L"上位項目切替"));
    }
}

static void OnSessionSelChange(HWND hWnd) {
    const std::wstring previousLecturePath = g_currentLecturePath;
    const std::wstring previousSessionPath = g_currentSessionPath;
    const std::wstring previousPdfPath = CurrentLogicalPdfPath();
    const std::wstring previousNotePath = g_currentNotePath;
    const ULONGLONG startTick = preview_trace::TickNow();
    try {
    fault_injection::MaybeThrow(L"OnSessionSelChange:start");
    int sel = static_cast<int>(SendMessageW(g_hSessionList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_sessions.size())) return;
    const auto& target = g_sessions[static_cast<size_t>(sel)];
    SwitchTimingScope timing(L"session_switch", target.path);
    timing.Mark(L"selected");
    auto curCanon = g_currentSessionPath.empty() ? std::filesystem::path{} : CanonicalOrSelf(std::filesystem::path(g_currentSessionPath));
    auto tgtCanon = CanonicalOrSelf(std::filesystem::path(target.path));
    bool samePath = (!curCanon.empty() && tgtCanon == curCanon);
    if (samePath) {
        timing.SetOutcome(L"noop_same_path");
        return;
    }

    RememberCurrentSessionFiles();
    if (!SaveNoteIfDirty(hWnd)) {
        timing.SetOutcome(L"cancelled_save_note");
        SyncLeftPaneSelection();
        return;
    }
    timing.Mark(L"SaveNoteIfDirty");
    if (!VerifyWorkspaceWritableForEditing(hWnd) ||
        !VerifyDirReadableWritableForEditing(hWnd, std::filesystem::path(target.path),
                                             L"セッションフォルダ", L"Session folder")) {
        timing.SetOutcome(L"cancelled_verify_dir");
        SyncLeftPaneSelection();
        return;
    }
    timing.Mark(L"VerifyWritable");
    // Keep the first PDF endpoint when the session switch closes the current
    // PDF; the pending link id remains available for the next endpoint.
    PreparePendingLinkForPdfSwitch(hWnd);
    if (!file_output::PrepareStagedDiffsForSwitch(hWnd)) {
        timing.SetOutcome(L"cancelled_stage_before_switch");
        SyncLeftPaneSelection();
        PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching sessions" : L"セッション切替",
                                    IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                  : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
        return;
    }
    timing.Mark(L"PrepareStagedDiffsForSwitch");
    {
        ScopedDeferredMainWindowUiRefresh deferredUi(hWnd);
        ScopedDeferredLeftPaneSelection deferredLeftPaneSelection;
        if (g_pdfPreviewActive) {
            DisableIntegratedPdfPreview(hWnd, true);
        }
        ClearPdfAndNoteSelection();
        std::wstring preferredPdf;
        std::wstring preferredNote;
        ApplySessionAutoOpenPreference(target, preferredPdf, preferredNote);
        LoadFiles(target, preferredPdf, preferredNote);
        timing.Mark(L"LoadFiles");
        AutoOpenSingleSessionFiles(hWnd);
        timing.Mark(L"AutoOpenPdf");
        timing.Mark(L"AutoOpenNote");
        SyncLeftPaneSelection();
        FlushDeferredLeftPaneSelection();
        timing.Mark(L"SyncLeftPaneSelection");
        RefreshMainWindowUiState(hWnd);
        file_output::ScheduleIntegrateAfterSwitch(hWnd);
        timing.Mark(L"ScheduleIntegrateAfterSwitch");
    }
    timing.Mark(L"FlushDeferredMainWindowUiRefresh");
    timing.SetOutcome(L"ok");
    preview_trace::Append(
        L"OnSessionSelChange",
        L"end sessionPath=" + g_currentSessionPath +
        L" pdfCount=" + std::to_wstring(g_pdfFiles.size()) +
        L" noteCount=" + std::to_wstring(g_noteFiles.size()) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("OnSessionSelChange", ex.what());
        RestoreLectureSessionStateAfterException(hWnd, previousLecturePath, previousSessionPath,
                                                previousPdfPath, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching sessions" : L"セッション切替");
    } catch (...) {
        AppendMainOperationExceptionLog("OnSessionSelChange", nullptr);
        RestoreLectureSessionStateAfterException(hWnd, previousLecturePath, previousSessionPath,
                                                previousPdfPath, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching sessions" : L"セッション切替");
    }
}

static void OnLectureDblClk(HWND hWnd) {
    OnLectureSelChange(hWnd);
}

static void OnSessionDblClk(HWND hWnd) {
    OnSessionSelChange(hWnd);
}

static bool OpenNoteIfDifferent(HWND hWnd, const std::wstring& path) {
    const std::wstring previousNotePath = g_currentNotePath;
    try {
    fault_injection::MaybeThrow(L"OpenNoteIfDifferent:start");
    if (path.empty()) return false;
    if (!g_currentNotePath.empty() && path == g_currentNotePath) {
        SyncLeftPaneSelection();
        return true;
    }
    ScopedDeferredMainWindowUiRefresh deferredUi(hWnd);
    if (!SaveNoteIfDirty(hWnd)) {
        SyncLeftPaneSelection();
        return PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替",
                                           IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                         : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
    }
    if (!VerifyWorkspaceWritableForEditing(hWnd) ||
        !VerifyDirReadableWritableForEditing(hWnd, std::filesystem::path(path).parent_path(),
                                             L"ノート保存先フォルダ", L"Note folder")) {
        SyncLeftPaneSelection();
        return false;
    }
    if (!file_output::PrepareStagedDiffsForSwitch(hWnd)) {
        SyncLeftPaneSelection();
        return PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替",
                                           IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                         : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
    }
    LoadNoteFile(hWnd, path);
    SyncBottomPaneAfterNoteLoad(hWnd);
    SyncLeftPaneSelection();
    RefreshMainWindowUiState(hWnd);
    file_output::ScheduleIntegrateAfterSwitch(hWnd);
    return true;
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("OpenNoteIfDifferent", ex.what());
        RestorePreviousNoteAfterException(hWnd, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替");
        return false;
    } catch (...) {
        AppendMainOperationExceptionLog("OpenNoteIfDifferent", nullptr);
        RestorePreviousNoteAfterException(hWnd, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替");
        return false;
    }
}

void SyncBottomPaneAfterNoteLoad(HWND hWnd) {
    const ULONGLONG startTick = preview_trace::TickNow();
    HWND owner = hWnd;
    if (!owner && g_hNoteEdit) {
        owner = GetParent(g_hNoteEdit);
    }
    preview_trace::Append(
        L"SyncBottomPaneAfterNoteLoad",
        L"begin owner=" + preview_trace::Window(owner) +
        L" bottomPanePin=" + std::to_wstring(static_cast<int>(g_bottomPanePin)));
    if (g_bottomPanePin == BottomPanePin::Math) {
        if (owner) {
            LayoutChildren(owner);
            preview_trace::Append(
                L"SyncBottomPaneAfterNoteLoad",
                L"after_layout_children elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        }
        RefreshBottomPaneView();
        preview_trace::Append(
            L"SyncBottomPaneAfterNoteLoad",
            L"after_refresh_bottom_pane elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        if (g_hBottomMath) {
            RedrawWindow(g_hBottomMath, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
            preview_trace::Append(
                L"SyncBottomPaneAfterNoteLoad",
                L"after_bottom_math_redraw elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        }
    }
    preview_trace::Append(
        L"SyncBottomPaneAfterNoteLoad",
        L"end elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

void AutoOpenSingleSessionFiles(HWND hWnd) {
    if (ParseSessionAutoOpenMode(g_config.sessionAutoOpenMode) == SessionAutoOpenMode::Off) {
        return;
    }
    const std::wstring autoOpenPdfPath = FirstOpenablePdfListPath();
    if (g_pdfFiles.size() == 1 && !autoOpenPdfPath.empty()) {
        if (g_pdfPreviewActive) {
            DisableIntegratedPdfPreview(hWnd, true);
        }
        OpenPdfWithAnnotations(hWnd, autoOpenPdfPath);
    }
    if (g_noteFiles.size() == 1 && !g_noteFiles.front().path.empty()) {
        LoadNoteFile(hWnd, g_noteFiles.front().path);
        SyncBottomPaneAfterNoteLoad(hWnd);
    }
}

bool OpenPdfIfDifferent(HWND hWnd, const std::wstring& path) {
    const std::wstring previousPdfPath = CurrentLogicalPdfPath();
    try {
    fault_injection::MaybeThrow(L"OpenPdfIfDifferent:start");
    if (path.empty()) return false;
    if (!CurrentLogicalPdfPath().empty() && path == CurrentLogicalPdfPath()) {
        SyncLeftPaneSelection();
        return true;
    }
    ScopedDeferredMainWindowUiRefresh deferredUi(hWnd);
    if (!SaveNoteIfDirty(hWnd)) {
        SyncLeftPaneSelection();
        return PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替",
                                           IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                         : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
    }
    if (!VerifyWorkspaceWritableForEditing(hWnd) ||
        !VerifyDirReadableWritableForEditing(hWnd, std::filesystem::path(path).parent_path(),
                                             L"PDF保存先フォルダ", L"PDF folder")) {
        SyncLeftPaneSelection();
        return false;
    }
    if (!file_output::PrepareStagedDiffsForSwitch(hWnd)) {
        SyncLeftPaneSelection();
        return PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替",
                                           IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                         : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
    }
    if (g_pdfPreviewActive) {
        DisableIntegratedPdfPreview(hWnd, true);
        if (!CurrentLogicalPdfPath().empty() && path == CurrentLogicalPdfPath()) {
            SyncLeftPaneSelection();
            return true;
        }
    }
    PreparePendingLinkForPdfSwitch(hWnd);
    if (OpenPdfWithAnnotations(hWnd, path)) {
        SyncLeftPaneSelection();
        RefreshMainWindowUiState(hWnd);
        file_output::ScheduleIntegrateAfterSwitch(hWnd);
        return true;
    }
    SyncLeftPaneSelection();
    return false;
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("OpenPdfIfDifferent", ex.what());
        RestorePreviousPdfAfterException(hWnd, previousPdfPath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替");
        return false;
    } catch (...) {
        AppendMainOperationExceptionLog("OpenPdfIfDifferent", nullptr);
        RestorePreviousPdfAfterException(hWnd, previousPdfPath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替");
        return false;
    }
}

static void OnNoteSelChange(HWND hWnd) {
    const std::wstring previousNotePath = g_currentNotePath;
    try {
    fault_injection::MaybeThrow(L"OnNoteSelChange:start");
    int sel = static_cast<int>(SendMessageW(g_hNoteList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_noteFiles.size())) return;
    const auto& f = g_noteFiles[static_cast<size_t>(sel)];
    if (f.path.empty()) return;
    if (!g_currentNotePath.empty() && f.path == g_currentNotePath) return;
    SwitchTimingScope timing(L"note_switch", f.path);
    timing.Mark(L"selected");
    if (!SaveNoteIfDirty(hWnd)) {
        timing.SetOutcome(L"cancelled_save_note");
        SyncLeftPaneSelection();
        PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替",
                                    IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                  : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
        return;
    }
    timing.Mark(L"SaveNoteIfDirty");
    if (!VerifyWorkspaceWritableForEditing(hWnd) ||
        !VerifyDirReadableWritableForEditing(hWnd, std::filesystem::path(f.path).parent_path(),
                                             L"ノート保存先フォルダ", L"Note folder")) {
        timing.SetOutcome(L"cancelled_verify_dir");
        SyncLeftPaneSelection();
        return;
    }
    timing.Mark(L"VerifyWritable");
    if (!file_output::PrepareStagedDiffsForSwitch(hWnd)) {
        timing.SetOutcome(L"cancelled_stage_before_switch");
        SyncLeftPaneSelection();
        PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替",
                                    IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                  : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
        return;
    }
    timing.Mark(L"PrepareStagedDiffsForSwitch");
    {
        ScopedDeferredMainWindowUiRefresh deferredUi(hWnd);
        ScopedDeferredLeftPaneSelection deferredLeftPaneSelection;
        SwitchBackdropGuard blankGuard(hWnd);
        blankGuard.AddWindow(g_hNoteEdit);
        blankGuard.AddWindow(g_hNoteRender);
        blankGuard.Activate();
        LoadNoteFile(hWnd, f.path);
        SyncBottomPaneAfterNoteLoad(hWnd);
        timing.Mark(L"LoadNoteFile");
        SyncLeftPaneSelection();
        FlushDeferredLeftPaneSelection();
        timing.Mark(L"SyncLeftPaneSelection");
        RefreshMainWindowUiState(hWnd);
        file_output::ScheduleIntegrateAfterSwitch(hWnd);
        timing.Mark(L"ScheduleIntegrateAfterSwitch");
    }
    timing.Mark(L"FlushDeferredMainWindowUiRefresh");
    timing.SetOutcome(L"ok");
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("OnNoteSelChange", ex.what());
        RestorePreviousNoteAfterException(hWnd, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替");
    } catch (...) {
        AppendMainOperationExceptionLog("OnNoteSelChange", nullptr);
        RestorePreviousNoteAfterException(hWnd, previousNotePath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching notes" : L"ノート切替");
    }
}

static void OnPdfSelChange(HWND hWnd) {
    const std::wstring previousPdfPath = CurrentLogicalPdfPath();
    try {
    fault_injection::MaybeThrow(L"OnPdfSelChange:start");
    int sel = static_cast<int>(SendMessageW(g_hPdfList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_pdfFiles.size())) return;
    const auto& f = g_pdfFiles[static_cast<size_t>(sel)];
    if (f.path.empty()) return;
    if (IsOfficeFileListPath(f.path)) {
        PromptOfficeFileListAction(hWnd, f.path);
        RestorePdfListSelectionToCurrent();
        SyncLeftPaneSelection();
        return;
    }
    if (!CurrentLogicalPdfPath().empty() && f.path == CurrentLogicalPdfPath()) {
        SyncLeftPaneSelection();
        return;
    }
    SwitchTimingScope timing(L"pdf_switch", f.path);
    timing.Mark(L"selected");
    if (!SaveNoteIfDirty(hWnd)) {
        timing.SetOutcome(L"cancelled_save_note");
        SyncLeftPaneSelection();
        PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替",
                                    IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                  : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
        return;
    }
    timing.Mark(L"SaveNoteIfDirty");
    if (!VerifyWorkspaceWritableForEditing(hWnd) ||
        !VerifyDirReadableWritableForEditing(hWnd, std::filesystem::path(f.path).parent_path(),
                                             L"PDF保存先フォルダ", L"PDF folder")) {
        timing.SetOutcome(L"cancelled_verify_dir");
        SyncLeftPaneSelection();
        return;
    }
    timing.Mark(L"VerifyWritable");
    if (!file_output::PrepareStagedDiffsForSwitch(hWnd)) {
        timing.SetOutcome(L"cancelled_stage_before_switch");
        SyncLeftPaneSelection();
        PromptStayOrOpenDiffManager(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替",
                                    IsEnglishUi() ? L"Please resolve the pending diff from Diff Manager, then try again."
                                                  : L"「差分管理」で未統合差分を整理してから、もう一度切り替えてください。");
        return;
    }
    timing.Mark(L"PrepareStagedDiffsForSwitch");
    bool openedPdf = false;
    {
        ScopedDeferredMainWindowUiRefresh deferredUi(hWnd);
        ScopedDeferredLeftPaneSelection deferredLeftPaneSelection;
        if (g_pdfPreviewActive) {
            DisableIntegratedPdfPreview(hWnd, true);
            if (!CurrentLogicalPdfPath().empty() && f.path == CurrentLogicalPdfPath()) {
                SyncLeftPaneSelection();
                return;
            }
        }
        PreparePendingLinkForPdfSwitch(hWnd);
        timing.Mark(L"PreparePendingLinkForPdfSwitch");
        if (OpenPdfWithAnnotations(hWnd, f.path)) {
            openedPdf = true;
            timing.Mark(L"OpenPdfWithAnnotations");
            SyncLeftPaneSelection();
            FlushDeferredLeftPaneSelection();
            timing.Mark(L"SyncLeftPaneSelection");
            RefreshMainWindowUiState(hWnd);
            file_output::ScheduleIntegrateAfterSwitch(hWnd);
            timing.Mark(L"ScheduleIntegrateAfterSwitch");
        } else {
            timing.SetOutcome(L"failed_open_pdf");
            SyncLeftPaneSelection();
            FlushDeferredLeftPaneSelection();
        }
    }
    if (openedPdf) {
        timing.Mark(L"FlushDeferredMainWindowUiRefresh");
        timing.SetOutcome(L"ok");
    }
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("OnPdfSelChange", ex.what());
        RestorePreviousPdfAfterException(hWnd, previousPdfPath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替");
    } catch (...) {
        AppendMainOperationExceptionLog("OnPdfSelChange", nullptr);
        RestorePreviousPdfAfterException(hWnd, previousPdfPath);
        ReportMainOperationException(hWnd, IsEnglishUi() ? L"Switching PDFs" : L"PDF切替");
    }
}

static void OnPdfDblClk(HWND hWnd);
static void OnNoteDblClk(HWND hWnd);

static void OnPdfDblClk(HWND hWnd) {
    OnPdfSelChange(hWnd);
}

static void OnNoteDblClk(HWND hWnd) {
    OnNoteSelChange(hWnd);
}

void ApplyActiveColorForMode(HWND hWnd, ToolMode mode) {
    bool modeHasColor = (mode == ToolMode::TextBox ||
                         mode == ToolMode::MarkerLine ||
                         mode == ToolMode::MarkerArrow ||
                         mode == ToolMode::MarkerWave ||
                         mode == ToolMode::Line ||
                         mode == ToolMode::Arrow ||
                         mode == ToolMode::Wave ||
                         mode == ToolMode::Freehand ||
                         mode == ToolMode::Shape ||
                         mode == ToolMode::MarkerFree ||
                         mode == ToolMode::MarkerTextUnderline ||
                         mode == ToolMode::MarkerTextColor ||
                         mode == ToolMode::MarkerText);
    if (modeHasColor) {
        COLORREF want = ToolColorForMode(mode);
        if (want != g_activeColor) {
            g_activeColor = want;
        }
    }
    if (g_hPdfToolbar) {
        RedrawWindow(g_hPdfToolbar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    const HWND modeButtons[] = {
        g_hBtnModeSelect,
        g_hBtnModePan,
        g_hBtnModeMagnifier,
        g_hBtnModeMarker,
        g_hBtnModeMarkerFree,
        g_hBtnModeMarkerLine,
        g_hBtnModeMarkerArrow,
        g_hBtnModeMarkerWave,
        g_hBtnModeText,
        g_hBtnModeLine,
        g_hBtnModeArrow,
        g_hBtnModeWave,
        g_hBtnModeFreehand,
        g_hBtnModeShape,
        g_hBtnModeEraser
    };
    for (HWND btn : modeButtons) {
        if (btn) InvalidateThemeButton(btn);
    }
}

void ApplyPaletteCustomColor(HWND hWnd, COLORREF color) {
    COLORREF prev = g_paletteCustomColor;
    SetPaletteCustomColor(color);
    COLORREF custom[16]{};
    LoadUserPaletteColorsForSettings(custom, std::size(custom));
    const size_t customSlotIdx = g_palette.empty() ? 7 : (g_palette.size() - 1);
    if (customSlotIdx < std::size(custom) && custom[customSlotIdx] != color) {
        custom[customSlotIdx] = color;
        SaveUserPaletteColorsForSettings(custom, std::size(custom));
    }
    if (g_activeColor == prev && prev != color) {
        g_activeColor = color;
        StoreToolColorForMode(g_toolMode, color);
    }
    PersistConfig();
    if (g_hPdfToolbar) {
        RedrawWindow(g_hPdfToolbar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    if (hWnd) UpdateToolbarUI(hWnd);
}

enum class WidthProfile { None, LineLike, MarkerFree, MarkerText, Eraser };
enum class AlphaProfile { None, Marker, LineLike, ShapeTransparency };

static WidthProfile WidthProfileForMode(ToolMode mode) {
    switch (mode) {
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::MarkerWave:
        return WidthProfile::MarkerFree;
    case ToolMode::Line:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Freehand:
        return WidthProfile::LineLike;
    case ToolMode::MarkerFree:
        return WidthProfile::MarkerFree;
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerText:
        return WidthProfile::MarkerText;
    case ToolMode::Eraser:
        return WidthProfile::Eraser;
    default:
        return WidthProfile::None;
    }
}

static double WidthValueForMode(ToolMode mode) {
    return ToolWidthPtForMode(mode);
}

static void SetWidthValueForMode(ToolMode mode, double pt) {
    StoreToolWidthPtForMode(mode, pt);
}

static AlphaProfile AlphaProfileForMode(ToolMode mode) {
    switch (mode) {
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerFree:
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::MarkerWave:
        return AlphaProfile::Marker;
    case ToolMode::Line:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Freehand:
        return AlphaProfile::LineLike;
    case ToolMode::Shape:
        return AlphaProfile::ShapeTransparency;
    default:
        return AlphaProfile::None;
    }
}

static double AlphaValueForMode(ToolMode mode) {
    return ToolAlphaForMode(mode);
}

static void SetAlphaValueForMode(ToolMode mode, double alpha) {
    StoreToolAlphaForMode(mode, alpha);
}

static int SelectedComboPt10(HWND cb) {
    if (!cb) return -1;
    int sel = static_cast<int>(SendMessageW(cb, CB_GETCURSEL, 0, 0));
    if (sel < 0) return -1;
    LRESULT data = SendMessageW(cb, CB_GETITEMDATA, sel, 0);
    if (data == CB_ERR) return -1;
    return static_cast<int>(data);
}

static int SelectedComboData(HWND cb) {
    if (!cb) return -1;
    int sel = static_cast<int>(SendMessageW(cb, CB_GETCURSEL, 0, 0));
    if (sel < 0) return -1;
    LRESULT data = SendMessageW(cb, CB_GETITEMDATA, sel, 0);
    if (data == CB_ERR) return -1;
    return static_cast<int>(data);
}

bool SyncWidthComboToMode(ToolMode mode) {
    if (!g_hComboWidth) return false;
    WidthProfile profile = WidthProfileForMode(mode);
    static WidthProfile s_profile = WidthProfile::None;

    int before = SelectedComboPt10(g_hComboWidth);
    bool changed = false;

    if (profile != s_profile) {
        SendMessageW(g_hComboWidth, CB_RESETCONTENT, 0, 0);
        auto add = [&](const wchar_t* label, int pt10) {
            double pt = static_cast<double>(pt10) / 10.0;
            std::wstring s = std::wstring(label) + L" (" + FormatSig2(pt) + L"pt)";
            int idx = static_cast<int>(SendMessageW(g_hComboWidth, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str())));
            if (idx >= 0) SendMessageW(g_hComboWidth, CB_SETITEMDATA, idx, static_cast<LPARAM>(pt10));
        };
        if (profile == WidthProfile::MarkerFree) {
            add(L"細", 60);
            add(L"中", 80);
            add(L"太", 100);
        } else if (profile == WidthProfile::MarkerText) {
            add(L"細", 20);
            add(L"中", 40);
            add(L"太", 60);
        } else if (profile == WidthProfile::LineLike) {
            add(L"細", 15);
            add(L"中", 25);
            add(L"太", 40);
        } else if (profile == WidthProfile::Eraser) {
            add(L"細", 20);
            add(L"中", 40);
            add(L"太", 80);
        }
        s_profile = profile;
        changed = true;
    }

    if (profile == WidthProfile::None) return changed;

    int wantPt10 = static_cast<int>(std::llround(WidthValueForMode(mode) * 10.0));
    int count = static_cast<int>(SendMessageW(g_hComboWidth, CB_GETCOUNT, 0, 0));
    int bestIdx = -1;
    int bestDelta = 0;
    for (int i = 0; i < count; ++i) {
        int pt10 = static_cast<int>(SendMessageW(g_hComboWidth, CB_GETITEMDATA, i, 0));
        int delta = std::abs(pt10 - wantPt10);
        if (bestIdx < 0 || delta < bestDelta) {
            bestIdx = i;
            bestDelta = delta;
        }
    }
    if (bestIdx >= 0) {
        SendMessageW(g_hComboWidth, CB_SETCURSEL, bestIdx, 0);
    }

    int after = SelectedComboPt10(g_hComboWidth);
    if (after != before) changed = true;
    return changed;
}

static bool SyncAlphaComboToMode(ToolMode mode) {
    if (!g_hComboMarkerAlpha) return false;
    AlphaProfile profile = AlphaProfileForMode(mode);
    static AlphaProfile s_profile = AlphaProfile::None;
    int before = SelectedComboData(g_hComboMarkerAlpha);

    bool changed = false;
    if (profile != s_profile) {
        SendMessageW(g_hComboMarkerAlpha, CB_RESETCONTENT, 0, 0);
        auto add = [&](const wchar_t* label, double alpha) {
            int permille = static_cast<int>(std::llround(alpha * 1000.0));
            int pct = static_cast<int>(std::llround(alpha * 100.0));
            std::wstring s = std::wstring(label) + L" (" + std::to_wstring(pct) + L"%)";
            int idx = static_cast<int>(SendMessageW(g_hComboMarkerAlpha, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str())));
            if (idx >= 0) SendMessageW(g_hComboMarkerAlpha, CB_SETITEMDATA, idx, static_cast<LPARAM>(permille));
        };
        if (profile != AlphaProfile::None) {
            const wchar_t* labels[] = { L"薄", L"標準", L"濃", L"最大" };
            const int count = ToolAlphaOptionCountForMode(mode);
            for (int i = 0; i < count; ++i) {
                add(labels[std::min(i, 3)], ToolAlphaOptionValueForMode(mode, i));
            }
        }
        s_profile = profile;
        changed = true;
    }

    if (profile == AlphaProfile::None) return changed;

    int want = static_cast<int>(std::llround(std::clamp(AlphaValueForMode(mode), 0.0, 1.0) * 1000.0));
    int count = static_cast<int>(SendMessageW(g_hComboMarkerAlpha, CB_GETCOUNT, 0, 0));
    int bestIdx = -1;
    int bestDelta = 0;
    for (int i = 0; i < count; ++i) {
        int data = static_cast<int>(SendMessageW(g_hComboMarkerAlpha, CB_GETITEMDATA, i, 0));
        int delta = std::abs(data - want);
        if (bestIdx < 0 || delta < bestDelta) {
            bestIdx = i;
            bestDelta = delta;
        }
    }
    if (bestIdx >= 0) {
        SendMessageW(g_hComboMarkerAlpha, CB_SETCURSEL, bestIdx, 0);
    }
    int after = SelectedComboData(g_hComboMarkerAlpha);
    if (after != before) changed = true;
    return changed;
}

static bool SyncMagnifierShapeCombo(MagnifierShape shape) {
    if (!g_hComboMagnifierShape) return false;
    int before = SelectedComboData(g_hComboMagnifierShape);
    int want = static_cast<int>(shape);
    int count = static_cast<int>(SendMessageW(g_hComboMagnifierShape, CB_GETCOUNT, 0, 0));
    int matchIdx = -1;
    for (int i = 0; i < count; ++i) {
        int data = static_cast<int>(SendMessageW(g_hComboMagnifierShape, CB_GETITEMDATA, i, 0));
        if (data == want) {
            matchIdx = i;
            break;
        }
    }
    if (matchIdx < 0 && count > 0) matchIdx = 0;
    if (matchIdx >= 0) {
        SendMessageW(g_hComboMagnifierShape, CB_SETCURSEL, matchIdx, 0);
    }
    int after = SelectedComboData(g_hComboMagnifierShape);
    return after != before;
}

static bool SyncShapeKindCombo(ShapeKind kind) {
    if (!g_hComboShapeKind) return false;
    int before = SelectedComboData(g_hComboShapeKind);
    int want = static_cast<int>(kind);
    int count = static_cast<int>(SendMessageW(g_hComboShapeKind, CB_GETCOUNT, 0, 0));
    int matchIdx = -1;
    for (int i = 0; i < count; ++i) {
        int data = static_cast<int>(SendMessageW(g_hComboShapeKind, CB_GETITEMDATA, i, 0));
        if (data == want) {
            matchIdx = i;
            break;
        }
    }
    if (matchIdx < 0 && count > 0) matchIdx = 0;
    if (matchIdx >= 0) {
        SendMessageW(g_hComboShapeKind, CB_SETCURSEL, matchIdx, 0);
    }
    int after = SelectedComboData(g_hComboShapeKind);
    return after != before;
}

static std::wstring AnnotMethodLabel(ToolMode mode) {
    switch (mode) {
    case ToolMode::MarkerText: return IsEnglishUi() ? L"Text" : L"テキスト";
    case ToolMode::MarkerTextUnderline: return IsEnglishUi() ? L"Underline" : L"下線";
    case ToolMode::MarkerTextColor: return IsEnglishUi() ? L"Text color" : L"文字色";
    case ToolMode::MarkerFree: return IsEnglishUi() ? L"Freehand" : L"フリーハンド";
    case ToolMode::MarkerLine: return IsEnglishUi() ? L"Marker line" : L"マーカー直線";
    case ToolMode::MarkerArrow: return IsEnglishUi() ? L"Marker arrow" : L"マーカー矢印";
    case ToolMode::MarkerWave: return IsEnglishUi() ? L"Marker wave" : L"マーカー波線";
    case ToolMode::Line: return IsEnglishUi() ? L"Line" : L"直線";
    case ToolMode::Arrow: return IsEnglishUi() ? L"Arrow" : L"矢印";
    case ToolMode::Wave: return IsEnglishUi() ? L"Wave" : L"波線";
    case ToolMode::Freehand: return IsEnglishUi() ? L"Freehand" : L"フリーハンド";
    case ToolMode::Shape: return IsEnglishUi() ? L"Shape" : L"図形";
    default: return IsEnglishUi() ? L"Method" : L"描画方法";
    }
}

static std::wstring CorrectionPenLabel() {
    return IsEnglishUi() ? L"Line correction" : L"線補正";
}

static std::wstring FreehandCorrectionModeLabel(const std::wstring& correction) {
    std::wstring mode = NormalizeCorrectionPenMode(correction);
    if (mode == L"auto") return IsEnglishUi() ? L"Auto" : L"自動";
    if (mode == L"hold") return IsEnglishUi() ? L"Pause" : L"静止";
    return IsEnglishUi() ? L"Smooth" : L"ならす";
}

static void AddAnnotMethodComboItem(HWND combo, const std::wstring& label, int data,
                                    int currentData, int& firstIdx, int& matchIdx) {
    int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0,
                                            reinterpret_cast<LPARAM>(label.c_str())));
    if (idx < 0) return;
    SendMessageW(combo, CB_SETITEMDATA, idx, static_cast<LPARAM>(data));
    if (firstIdx < 0) firstIdx = idx;
    if (data == currentData) matchIdx = idx;
}

static std::wstring ShapeDetailLabel(ShapeDetail detail) {
    switch (detail) {
    case ShapeDetail::Line: return IsEnglishUi() ? L"Line" : L"直線";
    case ShapeDetail::Arrow: return IsEnglishUi() ? L"Arrow" : L"矢印";
    case ShapeDetail::Wave: return IsEnglishUi() ? L"Wave" : L"波線";
    case ShapeDetail::Rectangle: return IsEnglishUi() ? L"Rectangle" : L"長方形";
    case ShapeDetail::Ellipse: return IsEnglishUi() ? L"Ellipse" : L"円／楕円";
    case ShapeDetail::Triangle: return IsEnglishUi() ? L"Triangle" : L"三角形";
    case ShapeDetail::Diamond: return IsEnglishUi() ? L"Diamond" : L"菱形";
    default: return IsEnglishUi() ? L"Line" : L"直線";
    }
}

static bool SyncAnnotMethodCombo(ToolMode mode) {
    if (!g_hComboAnnotMethod) return false;
    AnnotToolFamily family = AnnotToolFamilyForMode(mode);
    if (family != AnnotToolFamily::Marker &&
        family != AnnotToolFamily::Pen &&
        family != AnnotToolFamily::Shape) return false;

    int before = SelectedComboData(g_hComboAnnotMethod);
    if (family == AnnotToolFamily::Shape) {
        const int currentData = static_cast<int>(g_shapeDetail);
        SendMessageW(g_hComboAnnotMethod, WM_SETREDRAW, FALSE, 0);
        SendMessageW(g_hComboAnnotMethod, CB_RESETCONTENT, 0, 0);
        int matchIdx = -1;
        int firstIdx = -1;
        for (ShapeDetail detail : OrderedShapeDetails()) {
            ToolMode target = ToolModeForShapeDetail(detail);
            if (AnnotToolModeUiStateFor(target) != AnnotToolUiState::Enabled) continue;
            AddAnnotMethodComboItem(g_hComboAnnotMethod, ShapeDetailLabel(detail),
                                    static_cast<int>(detail), currentData, firstIdx, matchIdx);
        }
        if (matchIdx < 0) matchIdx = firstIdx;
        if (matchIdx >= 0) SendMessageW(g_hComboAnnotMethod, CB_SETCURSEL, matchIdx, 0);
        SendMessageW(g_hComboAnnotMethod, CB_SETDROPPEDWIDTH, 180, 0);
        SendMessageW(g_hComboAnnotMethod, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hComboAnnotMethod, nullptr, FALSE);
        return SelectedComboData(g_hComboAnnotMethod) != before;
    }

    int currentData = static_cast<int>(mode);
    if (mode == ToolMode::Freehand) {
        currentData = FreehandCorrectionPenActive(g_config.freehandCorrection)
                          ? kAnnotMethodDataCorrectionPen
                          : static_cast<int>(ToolMode::Freehand);
    }
    SendMessageW(g_hComboAnnotMethod, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hComboAnnotMethod, CB_RESETCONTENT, 0, 0);
    int matchIdx = -1;
    int firstIdx = -1;
    auto appendCandidate = [&](ToolMode candidate) {
        if (AnnotToolFamilyForMode(candidate) != family) return;
        if (AnnotToolModeUiStateFor(candidate) != AnnotToolUiState::Enabled) return;
        if (family == AnnotToolFamily::Pen && candidate == ToolMode::Freehand) {
            AddAnnotMethodComboItem(g_hComboAnnotMethod, AnnotMethodLabel(candidate),
                                    static_cast<int>(ToolMode::Freehand),
                                    currentData, firstIdx, matchIdx);
            AddAnnotMethodComboItem(g_hComboAnnotMethod, CorrectionPenLabel(),
                                    kAnnotMethodDataCorrectionPen,
                                    currentData, firstIdx, matchIdx);
            return;
        }
        AddAnnotMethodComboItem(g_hComboAnnotMethod, AnnotMethodLabel(candidate),
                                static_cast<int>(candidate), currentData, firstIdx, matchIdx);
    };
    for (ToolMode candidate : AnnotToolModeUiOrder()) {
        appendCandidate(candidate);
    }
    if (matchIdx < 0) matchIdx = firstIdx;
    if (matchIdx >= 0) {
        SendMessageW(g_hComboAnnotMethod, CB_SETCURSEL, matchIdx, 0);
    }
    SendMessageW(g_hComboAnnotMethod, CB_SETDROPPEDWIDTH, 180, 0);
    SendMessageW(g_hComboAnnotMethod, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hComboAnnotMethod, nullptr, FALSE);

    int after = SelectedComboData(g_hComboAnnotMethod);
    return after != before;
}

static std::wstring ShapeGeometryLabel(AnnotToolGeometry geometry) {
    switch (geometry) {
    case AnnotToolGeometry::Line: return IsEnglishUi() ? L"Line" : L"直線";
    case AnnotToolGeometry::Wave: return IsEnglishUi() ? L"Wave" : L"波線";
    case AnnotToolGeometry::Arrow: return IsEnglishUi() ? L"Arrow" : L"矢印";
    case AnnotToolGeometry::Shape: return IsEnglishUi() ? L"Shape" : L"図形";
    default: return IsEnglishUi() ? L"Shape" : L"図形";
    }
}

static bool SyncShapeGeometryCombo(ToolMode mode) {
    if (!g_hComboShapeGeometry) return false;
    (void)mode;
    int before = SelectedComboData(g_hComboShapeGeometry);
    const AnnotToolPresentation presentation = g_shapeToolSelection.presentation;
    const AnnotToolGeometry current = g_shapeToolSelection.geometry;
    SendMessageW(g_hComboShapeGeometry, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hComboShapeGeometry, CB_RESETCONTENT, 0, 0);
    int matchIdx = -1;
    int firstIdx = -1;
    for (AnnotToolGeometry geometry : OrderedShapeToolGeometries()) {
        const auto target = ToolModeForShapeToolSelection({ presentation, geometry });
        if (!target || AnnotToolModeUiStateFor(*target) != AnnotToolUiState::Enabled) continue;
        int idx = static_cast<int>(SendMessageW(g_hComboShapeGeometry, CB_ADDSTRING, 0,
                                                reinterpret_cast<LPARAM>(ShapeGeometryLabel(geometry).c_str())));
        if (idx < 0) continue;
        SendMessageW(g_hComboShapeGeometry, CB_SETITEMDATA, idx, static_cast<LPARAM>(geometry));
        if (firstIdx < 0) firstIdx = idx;
        if (geometry == current) matchIdx = idx;
    }
    if (matchIdx < 0) matchIdx = firstIdx;
    if (matchIdx >= 0) SendMessageW(g_hComboShapeGeometry, CB_SETCURSEL, matchIdx, 0);
    SendMessageW(g_hComboShapeGeometry, CB_SETDROPPEDWIDTH, 140, 0);
    SendMessageW(g_hComboShapeGeometry, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hComboShapeGeometry, nullptr, FALSE);
    return SelectedComboData(g_hComboShapeGeometry) != before;
}

static bool SyncFreehandCorrectionCombo() {
    if (!g_hComboFreehandCorrection) return false;
    int before = SelectedComboData(g_hComboFreehandCorrection);
    SendMessageW(g_hComboFreehandCorrection, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hComboFreehandCorrection, CB_RESETCONTENT, 0, 0);

    const std::wstring modes[] = { L"smooth", L"hold", L"auto" };
    int matchIdx = -1;
    std::wstring current = NormalizeCorrectionPenMode(g_config.freehandCorrection);
    for (int i = 0; i < static_cast<int>(std::size(modes)); ++i) {
        int idx = static_cast<int>(SendMessageW(
            g_hComboFreehandCorrection, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(FreehandCorrectionModeLabel(modes[static_cast<size_t>(i)]).c_str())));
        if (idx < 0) continue;
        SendMessageW(g_hComboFreehandCorrection, CB_SETITEMDATA, idx, static_cast<LPARAM>(i));
        if (modes[static_cast<size_t>(i)] == current) matchIdx = idx;
    }

    if (matchIdx < 0) matchIdx = 0;
    SendMessageW(g_hComboFreehandCorrection, CB_SETCURSEL, matchIdx, 0);
    SendMessageW(g_hComboFreehandCorrection, CB_SETDROPPEDWIDTH, 160, 0);
    SendMessageW(g_hComboFreehandCorrection, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hComboFreehandCorrection, nullptr, FALSE);

    int after = SelectedComboData(g_hComboFreehandCorrection);
    return after != before;
}

static bool SyncMarkerTextStyleCombo() {
    if (!g_hComboMarkerTextStyle) return false;
    int before = SelectedComboData(g_hComboMarkerTextStyle);
    int want = g_markerTextUnderline ? 1 : 0;
    int count = static_cast<int>(SendMessageW(g_hComboMarkerTextStyle, CB_GETCOUNT, 0, 0));
    int matchIdx = -1;
    for (int i = 0; i < count; ++i) {
        int data = static_cast<int>(SendMessageW(g_hComboMarkerTextStyle, CB_GETITEMDATA, i, 0));
        if (data == want) {
            matchIdx = i;
            break;
        }
    }
    if (matchIdx < 0 && count > 0) matchIdx = 0;
    if (matchIdx >= 0) {
        SendMessageW(g_hComboMarkerTextStyle, CB_SETCURSEL, matchIdx, 0);
    }
    int after = SelectedComboData(g_hComboMarkerTextStyle);
    return after != before;
}


static bool SyncLineDashStyleCombo() {
    if (!g_hComboLineDashStyle) return false;
    int before = SelectedComboData(g_hComboLineDashStyle);
    int want = (g_lineDashStyle == L"dash") ? 1 : 0;
    int count = static_cast<int>(SendMessageW(g_hComboLineDashStyle, CB_GETCOUNT, 0, 0));
    int matchIdx = -1;
    for (int i = 0; i < count; ++i) {
        int data = static_cast<int>(SendMessageW(g_hComboLineDashStyle, CB_GETITEMDATA, i, 0));
        if (data == want) {
            matchIdx = i;
            break;
        }
    }
    if (matchIdx < 0 && count > 0) matchIdx = 0;
    if (matchIdx >= 0) {
        SendMessageW(g_hComboLineDashStyle, CB_SETCURSEL, matchIdx, 0);
    }
    int after = SelectedComboData(g_hComboLineDashStyle);
    return after != before;
}
static bool SyncShapeDrawModeCombo(ShapeDrawMode mode) {
    if (!g_hComboShapeDrawMode) return false;
    int before = SelectedComboData(g_hComboShapeDrawMode);
    int want = static_cast<int>(mode);
    int count = static_cast<int>(SendMessageW(g_hComboShapeDrawMode, CB_GETCOUNT, 0, 0));
    int matchIdx = -1;
    for (int i = 0; i < count; ++i) {
        int data = static_cast<int>(SendMessageW(g_hComboShapeDrawMode, CB_GETITEMDATA, i, 0));
        if (data == want) {
            matchIdx = i;
            break;
        }
    }
    if (matchIdx < 0 && count > 0) matchIdx = 0;
    if (matchIdx >= 0) {
        SendMessageW(g_hComboShapeDrawMode, CB_SETCURSEL, matchIdx, 0);
    }
    int after = SelectedComboData(g_hComboShapeDrawMode);
    return after != before;
}

static bool PreviewAllowsPdfTool(ToolMode mode) {
    return mode == ToolMode::Select || mode == ToolMode::Pan || mode == ToolMode::Magnifier;
}

static bool ToolModeEnabledForCurrentPdfView(ToolMode mode) {
    if (AnnotToolModeUiStateFor(mode) != AnnotToolUiState::Enabled) return false;
    if (g_pdf.kind == DocKind::Image && !PreviewAllowsPdfTool(mode)) return false;
    if (IsSaveTransactionRunning()) return false;
    if (IsPdfPreviewReadOnlyActive() && !PreviewAllowsPdfTool(mode)) return false;
    return true;
}

static AnnotToolUiState EffectiveToolUiStateForCurrentPdfView(ToolMode mode);

static AnnotToolUiState EffectiveToolFamilyUiStateForCurrentPdfView(AnnotToolFamily family) {
    bool anyEnabled = false;
    bool anyDisabled = false;
    for (ToolMode mode : AnnotToolModeUiOrder()) {
        if (AnnotToolFamilyForMode(mode) != family) continue;
        AnnotToolUiState s = EffectiveToolUiStateForCurrentPdfView(mode);
        if (s == AnnotToolUiState::Enabled) anyEnabled = true;
        else if (s == AnnotToolUiState::Disabled) anyDisabled = true;
    }
    if (anyEnabled) return AnnotToolUiState::Enabled;
    if (anyDisabled) return AnnotToolUiState::Disabled;
    return AnnotToolUiState::Hidden;
}

static bool FirstEnabledModeForFamily(AnnotToolFamily family, ToolMode& out) {
    for (ToolMode mode : AnnotToolModeUiOrder()) {
        if (AnnotToolFamilyForMode(mode) != family) continue;
        if (!ToolModeEnabledForCurrentPdfView(mode)) continue;
        out = mode;
        return true;
    }
    return false;
}

static AnnotToolUiState EffectiveToolUiStateForCurrentPdfView(ToolMode mode) {
    AnnotToolUiState state = AnnotToolModeUiStateFor(mode);
    if (state == AnnotToolUiState::Hidden) return state;
    if (g_pdf.kind == DocKind::Image && !PreviewAllowsPdfTool(mode)) {
        return AnnotToolUiState::Disabled;
    }
    if (IsSaveTransactionRunning()) {
        return AnnotToolUiState::Disabled;
    }
    if (IsPdfPreviewReadOnlyActive() && !PreviewAllowsPdfTool(mode)) {
        return AnnotToolUiState::Disabled;
    }
    return state;
}

#include "ui/menus/main_toolbar_ui.cppinc"

// ---------------------------------------------------------------------
// Command handler
// ---------------------------------------------------------------------
static bool ShortcutChkIsChecked(HWND h) {
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static std::wstring ColorToHexW(COLORREF color) {
    wchar_t buf[8]{};
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%02X%02X%02X",
             GetRValue(color), GetGValue(color), GetBValue(color));
    return buf;
}

static bool IsRichEditControlWindow(HWND hWnd);
static std::wstring GetNoteEditTextForIndexing(HWND hEdit);

static std::wstring GetNoteSelectionText() {
    if (!g_hNoteEdit) return {};
    DWORD start = 0, end = 0;
    SendMessageW(g_hNoteEdit, EM_GETSEL,
                 reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));
    if (end < start) std::swap(start, end);
    if (end <= start) return {};

    const int len = std::max(0, GetWindowTextLengthW(g_hNoteEdit));
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
    if (IsRichEditControlWindow(g_hNoteEdit)) {
        LRESULT copied = SendMessageW(g_hNoteEdit, EM_GETSELTEXT, 0, reinterpret_cast<LPARAM>(buf.data()));
        if (copied > 0) {
            return std::wstring(buf.data(), static_cast<size_t>(copied));
        }
    }

    const std::wstring text = GetNoteEditTextForIndexing(g_hNoteEdit);
    const size_t safeStart = std::min<size_t>(start, text.size());
    const size_t safeEnd = std::min<size_t>(end, text.size());
    if (safeEnd <= safeStart) return {};
    return text.substr(safeStart, safeEnd - safeStart);
}

static std::wstring GetShortcutBodyInput() {
    if (!g_hShortcutTagEdit) return {};
    int len = GetWindowTextLengthW(g_hShortcutTagEdit);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(g_hShortcutTagEdit, text.data(), len + 1);
    if (copied < 0) copied = 0;
    text.resize(static_cast<size_t>(copied));
    auto isAnySpace = [](wchar_t ch) {
        return (iswspace(ch) != 0) || ch == 0x3000; // include full-width space explicitly
    };
    text.erase(std::remove_if(text.begin(), text.end(), isAnySpace), text.end());
    return text;
}

struct ShortcutTagSpec {
    std::wstring tag;
    std::vector<std::wstring> attrs;
};

static void UpdateShortcutHeadingLevelLabel() {
    g_noteShortcutHeadingLevel = std::clamp(g_noteShortcutHeadingLevel, 1, 9);
    if (g_hShortcutHeadingLevelLabel) {
        std::wstring level = std::to_wstring(g_noteShortcutHeadingLevel);
        SetWindowTextW(g_hShortcutHeadingLevelLabel, level.c_str());
    }
}

static std::wstring ResolveShortcutTextTagKey() {
    std::wstring key = g_config.noteShortcutTextTagKey;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    if (key == L"c") return L"c";
    return L"char";
}

static ShortcutTagSpec CollectShortcutTagSpec(bool attachLink) {
    ShortcutTagSpec spec{};
    if (ShortcutChkIsChecked(g_hChkShortcutHeading1)) {
        g_noteShortcutHeadingLevel = std::clamp(g_noteShortcutHeadingLevel, 1, 9);
        spec.tag = L"h" + std::to_wstring(g_noteShortcutHeadingLevel);
    }
    if (ShortcutChkIsChecked(g_hChkShortcutBack)) {
        spec.attrs.push_back(L"back=" + ColorToHexW(g_noteShortcutBackColor));
    }
    if (ShortcutChkIsChecked(g_hChkShortcutChar)) {
        spec.attrs.push_back(ResolveShortcutTextTagKey() + L"=" + ColorToHexW(g_noteShortcutTextColor));
    }
    if (ShortcutChkIsChecked(g_hChkShortcutBold)) {
        spec.attrs.push_back(L"b");
    }
    if (ShortcutChkIsChecked(g_hChkShortcutItalic)) {
        spec.attrs.push_back(L"i");
    }
    if (ShortcutChkIsChecked(g_hChkShortcutStrike)) {
        spec.attrs.push_back(L"x");
    }
    if (ShortcutChkIsChecked(g_hChkShortcutUnderline)) {
        spec.attrs.push_back(L"u");
    }
    if (attachLink && g_linkPending.active && !g_linkPending.id.empty()) {
        spec.attrs.push_back(L"l=" + g_linkPending.id);
        if (ShortcutChkIsChecked(g_hChkShortcutLinkDecor)) {
            spec.attrs.push_back(L"la");
            spec.attrs.push_back(L"lu");
        }
    }
    if (g_hShortcutIndentEdit) {
        wchar_t buf[64]{};
        GetWindowTextW(g_hShortcutIndentEdit, buf, static_cast<int>(std::size(buf)));
        std::wstring text(buf);
        text.erase(std::remove_if(text.begin(), text.end(), iswspace), text.end());
        if (!text.empty()) {
            wchar_t* end = nullptr;
            long val = wcstol(text.c_str(), &end, 10);
            if (end != text.c_str()) {
                spec.attrs.push_back(L"d=" + std::to_wstring(val));
            }
        }
    }
    if (g_hShortcutMarginEdit) {
        int len = GetWindowTextLengthW(g_hShortcutMarginEdit);
        if (len > 0) {
            std::wstring raw(static_cast<size_t>(len) + 1, L'\0');
            int copied = GetWindowTextW(g_hShortcutMarginEdit, raw.data(), len + 1);
            if (copied < 0) copied = 0;
            raw.resize(static_cast<size_t>(copied));
            auto isAnySpace = [](wchar_t ch) {
                return (iswspace(ch) != 0) || ch == 0x3000; // include full-width space explicitly
            };
            raw.erase(std::remove_if(raw.begin(), raw.end(), isAnySpace), raw.end());
            if (!raw.empty()) {
                wchar_t* end = nullptr;
                double units = wcstod(raw.c_str(), &end);
                if (end != raw.c_str() && end && *end == L'\0' && units >= 0.0) {
                    spec.attrs.push_back(L"m=" + raw);
                }
            }
        }
    }
    if (g_hShortcutFontSizeEdit) {
        wchar_t buf[64]{};
        GetWindowTextW(g_hShortcutFontSizeEdit, buf, static_cast<int>(std::size(buf)));
        std::wstring text(buf);
        text.erase(std::remove_if(text.begin(), text.end(), iswspace), text.end());
        if (!text.empty()) {
            wchar_t* end = nullptr;
            long val = wcstol(text.c_str(), &end, 10);
            if (end != text.c_str() && val > 0) {
                spec.attrs.push_back(L"s=" + std::to_wstring(val));
            }
        }
    }
    return spec;
}

static std::wstring BuildOpeningTag(const ShortcutTagSpec& spec) {
    std::wstring opening = L"<";
    if (!spec.tag.empty()) {
        opening += spec.tag;
    }
    if (!spec.attrs.empty()) {
        if (!spec.tag.empty()) opening += L", ";
        for (size_t i = 0; i < spec.attrs.size(); ++i) {
            if (i > 0) opening += L", ";
            opening += spec.attrs[i];
        }
    }
    opening += L">";
    return opening;
}

static std::wstring BuildNoteShortcutSnippet() {
    if (!g_hNoteEdit) return {};
    const bool attachLink = (g_linkPending.active && !g_linkPending.id.empty());
    std::wstring bodyOverride = GetShortcutBodyInput();
    ShortcutTagSpec spec = CollectShortcutTagSpec(attachLink);
    std::wstring opening = BuildOpeningTag(spec);
    std::wstring body = bodyOverride;
    bool usesSelection = false;
    if (body.empty() && !attachLink) {
        body = GetNoteSelectionText();
        usesSelection = !body.empty();
    }
    if (body.empty()) {
        body = attachLink ? (L"LINK:" + g_linkPending.id + L"(ID)") : L"入力";
    }
    if (spec.tag.empty() && spec.attrs.empty()) {
        return usesSelection ? body : (body + L"\r\n");
    }
    std::wstring snippet = opening + body + L"</>";
    if (!usesSelection) snippet += L"\r\n";
    return snippet;
}

static bool IsRichEditControlWindow(HWND hWnd) {
    if (!hWnd) return false;
    wchar_t cls[32]{};
    if (!GetClassNameW(hWnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0])))) return false;
    // Accept both "RICHEDIT50W" (MSFTEDIT) and "RichEdit20W" (Riched20) regardless of casing.
    return _wcsnicmp(cls, L"RICHEDIT", 8) == 0;
}

static std::wstring GetNoteEditTextForIndexing(HWND hEdit) {
    if (!hEdit) return {};
    int len = GetWindowTextLengthW(hEdit);
    if (len < 0) len = 0;
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) {
        int copied = GetWindowTextW(hEdit, text.data(), len + 1);
        if (copied < 0) copied = 0;
        text.resize(static_cast<size_t>(copied));
    } else {
        text.clear();
    }
    if (!IsRichEditControlWindow(hEdit)) return text;

    // RichEdit's internal indices count line breaks as a single '\r', while GetWindowTextW returns CRLF.
    // Normalize to an index-compatible representation by collapsing CRLF -> CR.
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        normalized.push_back(ch);
        if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
            ++i; // skip '\n'
        }
    }
    return normalized;
}

struct MarkupTagTokenHit {
    size_t open = 0;
    size_t close = 0;
    bool closing = false;
};

static bool IsLiteralEscapedMarkupCharAt(const std::wstring& text, size_t pos) {
    return pos >= 2 && text[pos - 2] == L'!' && text[pos - 1] == L'/';
}

static bool IsMarkupTagIdentifier(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t ch : s) {
        if (!(iswalnum(ch) || ch == L'_' || ch == L'-')) return false;
    }
    return true;
}

static bool IsRecognizedMarkupTagKey(const std::wstring& keyLower) {
    // Keep in sync with the note markup parser (note_view.cpp).
    // Headings accept any numeric level like "h1", "h2", "h3", ...
    if (keyLower.size() >= 2 && keyLower[0] == L'h') {
        bool allDigits = true;
        for (size_t i = 1; i < keyLower.size(); ++i) {
            wchar_t ch = keyLower[i];
            if (ch < L'0' || ch > L'9') { allDigits = false; break; }
        }
        if (allDigits) return true;
    }
    if (keyLower == L"c") return true;
    if (keyLower.size() >= 2 && keyLower[0] == L'c') {
        bool allDigits = true;
        for (size_t i = 1; i < keyLower.size(); ++i) {
            wchar_t ch = keyLower[i];
            if (ch < L'0' || ch > L'9') { allDigits = false; break; }
        }
        if (allDigits) return true;
    }
    return keyLower == L"h1" ||
           keyLower == L"h2" ||
           keyLower == L"b" ||
           keyLower == L"i" ||
           keyLower == L"u" ||
           keyLower == L"x" ||
           keyLower == L"d" ||
           keyLower == L"m" ||
           keyLower == L"s" ||
           keyLower == L"fs" ||
           keyLower == L"size" ||
           keyLower == L"char" ||
           keyLower == L"c" ||
           keyLower == L"back" ||
           keyLower == L"l" ||
           keyLower == L"link" ||
           keyLower == L"la" ||
           keyLower == L"linkaccent" ||
           keyLower == L"lu" ||
           keyLower == L"linkunderline" ||
           keyLower == L"font" ||
           keyLower == L"f" ||
           keyLower == L"math";
}

static bool IsLikelyMarkupTagTokenContent(const std::wstring& rawContent, bool* outClosing = nullptr) {
    if (outClosing) *outClosing = false;

    std::wstring content = TrimWhitespace(rawContent);
    if (content.empty()) return false;
    if (content.find(L'<') != std::wstring::npos || content.find(L'>') != std::wstring::npos) return false;
    if (content.find(L'\r') != std::wstring::npos || content.find(L'\n') != std::wstring::npos) return false;

    if (content[0] == L'/') {
        if (outClosing) *outClosing = true;
        std::wstring rest = TrimWhitespace(content.substr(1));
        if (rest.empty()) return true; // </>
        std::wstring restLower = ToLowerAscii(rest);
        if (rest.find(L'=') != std::wstring::npos || rest.find(L',') != std::wstring::npos) return false;
        return IsMarkupTagIdentifier(rest);
    }

    // Accept <math ...> even when it has additional attributes.
    std::wstring lower = ToLowerAscii(content);
    if (lower.rfind(L"math", 0) == 0) {
        if (lower.size() == 4) return true;
        wchar_t delim = lower[4];
        if (iswspace(delim) || delim == L',') return true;
    }

    bool hasRecognizedKey = false;
    size_t i = 0;
    while (i < content.size()) {
        while (i < content.size() && (iswspace(content[i]) || content[i] == L',')) ++i;
        if (i >= content.size()) break;

        size_t keyStart = i;
        while (i < content.size() &&
               !iswspace(content[i]) &&
               content[i] != L'=' &&
               content[i] != L',') {
            ++i;
        }
        size_t keyEnd = i;
        if (keyEnd <= keyStart) {
            ++i;
            continue;
        }

        std::wstring keyLower = ToLowerAscii(content.substr(keyStart, keyEnd - keyStart));
        if (IsRecognizedMarkupTagKey(keyLower)) {
            hasRecognizedKey = true;
        }

        while (i < content.size() && iswspace(content[i])) ++i;
        if (i >= content.size() || content[i] != L'=') continue;
        ++i;
        while (i < content.size() && iswspace(content[i])) ++i;

        if (i < content.size() && (content[i] == L'"' || content[i] == L'\'')) {
            wchar_t quote = content[i++];
            while (i < content.size() && content[i] != quote) ++i;
            if (i < content.size() && content[i] == quote) ++i;
        } else {
            while (i < content.size() && !iswspace(content[i]) && content[i] != L',') ++i;
        }
    }
    return hasRecognizedKey;
}

static std::optional<MarkupTagTokenHit> FindMarkupTagTokenAtPos(const std::wstring& text, size_t pos) {
    if (text.empty()) return std::nullopt;
    pos = std::min(pos, text.size());

    // NOTE: pos is an insertion index (like EM_GETSEL), so it can point directly at '<'.
    // Use rfind that includes pos so nested tags on the same line don't hide the token at pos.
    size_t scan = 0;
    if (pos >= text.size()) {
        if (text.empty()) return std::nullopt;
        scan = text.size() - 1;
    } else {
        scan = pos;
    }
    size_t open = text.rfind(L'<', scan);
    while (open != std::wstring::npos && IsLiteralEscapedMarkupCharAt(text, open)) {
        if (open == 0) return std::nullopt;
        open = text.rfind(L'<', open - 1);
    }
    if (open == std::wstring::npos) return std::nullopt;

    size_t close = text.find(L'>', open + 1);
    if (close == std::wstring::npos) return std::nullopt;

    if (pos < open || pos > close) return std::nullopt;

    const std::wstring tokenContent = text.substr(open + 1, close - open - 1);
    bool closing = false;
    if (!IsLikelyMarkupTagTokenContent(tokenContent, &closing)) {
        return std::nullopt;
    }

    return MarkupTagTokenHit{open, close, closing};
}

static bool IsInsertPosInsideMarkupTagToken(const std::wstring& text, size_t pos) {
    if (auto token = FindMarkupTagTokenAtPos(text, pos)) {
        // pos == token->open means insertion is before '<' (safe). Inside means splitting "<...>" content.
        return pos > token->open;
    }
    return false;
}

static bool IsHighSurrogateW(wchar_t ch) {
    return (ch >= 0xD800 && ch <= 0xDBFF);
}

static bool IsLowSurrogateW(wchar_t ch) {
    return (ch >= 0xDC00 && ch <= 0xDFFF);
}

static size_t NormalizeInsertPosForEditControlText(const std::wstring& text, size_t pos) {
    pos = std::min(pos, text.size());
    // Keep CRLF together.
    if (pos > 0 && pos < text.size() && text[pos - 1] == L'\r' && text[pos] == L'\n') {
        pos -= 1;
    }
    // Avoid splitting surrogate pairs (defensive: edit controls should not place caret here, but we normalize anyway).
    if (pos > 0 && pos < text.size() && IsHighSurrogateW(text[pos - 1]) && IsLowSurrogateW(text[pos])) {
        pos = std::min(text.size(), pos + 1);
    }
    return pos;
}

static size_t NormalizeInsertPosOutsideMarkupTagToken(const std::wstring& text, size_t pos) {
    if (auto token = FindMarkupTagTokenAtPos(text, pos)) {
        // NOTE:
        // - For opening tags, prefer inserting *after* the tag token so the insertion stays within its scope.
        // - For closing tags, prefer inserting *before* the tag token so we don't unexpectedly jump outside.
        if (token->closing) {
            return std::min(text.size(), token->open);
        }
        return std::min(text.size(), token->close + 1);
    }
    return std::min(pos, text.size());
}

static bool IsLinkTokenBoundaryForInsert(wchar_t ch) {
    return iswspace(ch) || ch == L',' || ch == L';' || ch == L'(' || ch == L')' ||
           ch == L'[' || ch == L']' || ch == L'<' || ch == L'>';
}

static bool IsSafeLinkIdForMarkupAttr(const std::wstring& linkId) {
    if (linkId.empty()) return false;
    for (wchar_t ch : linkId) {
        if (iswspace(ch)) return false;
        if (ch == L',' || ch == L'<' || ch == L'>' || ch == L'\"' || ch == L'\'' || ch == L'=') return false;
    }
    return true;
}

static bool UpsertLinkAttributeInsideMarkupTag(const std::wstring& tokenContent,
                                               const std::wstring& linkId,
                                               std::wstring* outContent) {
    if (!outContent) return false;
    if (!IsSafeLinkIdForMarkupAttr(linkId)) return false;
    size_t nonSpace = 0;
    while (nonSpace < tokenContent.size() && iswspace(tokenContent[nonSpace])) ++nonSpace;
    if (nonSpace < tokenContent.size() && tokenContent[nonSpace] == L'/') return false;

    auto lowerAscii = [](const std::wstring& s) {
        std::wstring out = s;
        for (wchar_t& ch : out) {
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return out;
    };

    size_t i = 0;
    while (i < tokenContent.size()) {
        while (i < tokenContent.size() && (iswspace(tokenContent[i]) || tokenContent[i] == L',')) ++i;
        if (i >= tokenContent.size()) break;
        size_t keyStart = i;
        while (i < tokenContent.size() &&
               !iswspace(tokenContent[i]) &&
               tokenContent[i] != L'=' &&
               tokenContent[i] != L',') {
            ++i;
        }
        size_t keyEnd = i;
        if (keyEnd <= keyStart) {
            ++i;
            continue;
        }
        std::wstring key = lowerAscii(tokenContent.substr(keyStart, keyEnd - keyStart));
        while (i < tokenContent.size() && iswspace(tokenContent[i])) ++i;
        if (i >= tokenContent.size() || tokenContent[i] != L'=') continue;
        ++i;
        while (i < tokenContent.size() && iswspace(tokenContent[i])) ++i;

        size_t valueStart = i;
        size_t valueEnd = i;
        if (i < tokenContent.size() && (tokenContent[i] == L'"' || tokenContent[i] == L'\'')) {
            wchar_t quote = tokenContent[i++];
            valueStart = i;
            while (i < tokenContent.size() && tokenContent[i] != quote) ++i;
            valueEnd = i;
            if (i < tokenContent.size() && tokenContent[i] == quote) ++i;
        } else {
            while (i < tokenContent.size() && !iswspace(tokenContent[i]) && tokenContent[i] != L',') ++i;
            valueEnd = i;
        }

        if (key == L"l" || key == L"link") {
            std::wstring updated = tokenContent;
            updated.replace(valueStart, valueEnd - valueStart, linkId);
            *outContent = std::move(updated);
            return true;
        }
    }

    size_t end = tokenContent.size();
    while (end > 0 && iswspace(tokenContent[end - 1])) --end;
    std::wstring trailing = tokenContent.substr(end);
    std::wstring updated = tokenContent.substr(0, end);
    if (updated.empty()) {
        updated = L"l=" + linkId;
    } else if (updated.back() == L',') {
        updated += L" l=" + linkId;
    } else {
        updated += L", l=" + linkId;
    }
    updated += trailing;
    *outContent = std::move(updated);
    return true;
}

static bool IsMathOpeningTagTokenContent(const std::wstring& rawContent) {
    std::wstring content = TrimWhitespace(rawContent);
    if (content.empty()) return false;
    std::wstring lower = ToLowerAscii(content);
    if (lower.rfind(L"math", 0) != 0) return false;
    if (lower.size() == 4) return true;
    wchar_t delim = lower[4];
    return iswspace(delim) || delim == L',';
}

static bool TryAttachPendingLinkToMarkupTagAtSelection(size_t* outAnchorPos,
                                                       bool* outInsideOpeningTagToken = nullptr) {
    if (!g_hNoteEdit) return false;
    if (!g_linkPending.active || g_linkPending.id.empty()) return false;
    ReleaseNoteChangeSuppressionForUserEdit();
    if (outInsideOpeningTagToken) *outInsideOpeningTagToken = false;
    if (outAnchorPos) *outAnchorPos = 0;

    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_hNoteEdit, EM_GETSEL,
                 reinterpret_cast<WPARAM>(&selStart),
                 reinterpret_cast<LPARAM>(&selEnd));
    size_t pos = static_cast<size_t>(std::min(selStart, selEnd));

    std::wstring text = GetNoteEditTextForIndexing(g_hNoteEdit);
    pos = std::min(pos, text.size());

    auto token = FindMarkupTagTokenAtPos(text, pos);
    if (!token || token->closing) return false;
    if (outInsideOpeningTagToken) *outInsideOpeningTagToken = true;

    std::wstring content = text.substr(token->open + 1, token->close - token->open - 1);
    if (IsMathOpeningTagTokenContent(content)) {
        // <math ...> does not apply style stack; attaching link here would be misleading.
        return false;
    }
    std::wstring updatedContent;
    if (!UpsertLinkAttributeInsideMarkupTag(content, g_linkPending.id, &updatedContent)) return false;

    const std::wstring replacement = L"<" + updatedContent + L">";
    SendMessageW(g_hNoteEdit, EM_SETSEL,
                 static_cast<WPARAM>(token->open),
                 static_cast<LPARAM>(token->close + 1));
    SendMessageW(g_hNoteEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(replacement.c_str()));

    size_t caretPos = token->open + replacement.size();
    SendMessageW(g_hNoteEdit, EM_SETSEL,
                 static_cast<WPARAM>(caretPos),
                 static_cast<LPARAM>(caretPos));
    if (outAnchorPos) *outAnchorPos = caretPos;
    if (HWND parent = GetParent(g_hNoteEdit)) {
        PostMessageW(parent, kMsgNoteOverlayUpdate, 0, static_cast<LPARAM>(CurrentEditRevision()));
    }
    return true;
}

static std::wstring BuildNoteClickLinkSnippet(bool needPrefixBreak, bool rawLikeInsert) {
    if (!g_linkPending.active || g_linkPending.id.empty()) return {};
    (void)rawLikeInsert;
    std::wstring body = GetShortcutBodyInput();
    if (body.empty()) body = L"リンク";
    ShortcutTagSpec spec = CollectShortcutTagSpec(true);
    std::wstring opening = BuildOpeningTag(spec);

    std::wstring out;
    if (needPrefixBreak) out += L" ";
    out += opening + body + L"</>";
    return out;
}

static bool InsertSnippetIntoNoteAt(size_t pos, const std::wstring& snippet) {
    return InsertSnippetIntoCurrentNoteAt(pos, snippet);
}

static void OnShortcutColorPick(HWND owner, COLORREF& target, HWND preview) {
    COLORREF picked = target;
    if (!PickColorDialog(owner, target, &picked)) return;
    if (picked == target) return;
    target = picked;
    ApplyPaletteCustomColor(owner, picked);
    if (preview) InvalidateRect(preview, nullptr, TRUE);
}

static std::wstring ToBase36(ULONGLONG value) {
    static constexpr wchar_t kDigits[] = L"0123456789abcdefghijklmnopqrstuvwxyz";
    if (value == 0) return L"0";
    std::wstring out;
    while (value > 0) {
        unsigned int rem = static_cast<unsigned int>(value % 36ULL);
        out.push_back(kDigits[rem]);
        value /= 36ULL;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

static bool TextContainsLinkId(const std::wstring& text, const std::wstring& linkId) {
    if (text.empty() || linkId.empty()) return false;
    if (text.find(L"link:" + linkId) != std::wstring::npos) return true;
    if (text.find(L"LINK:" + linkId) != std::wstring::npos) return true;
    if (text.find(L"l=" + linkId) != std::wstring::npos) return true;
    if (text.find(L"link=" + linkId) != std::wstring::npos) return true;
    return false;
}

static bool FileContainsAsciiToken(const std::filesystem::path& path, const std::string& token) {
    if (path.empty() || token.empty()) return false;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::string buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (buf.empty()) return false;
    return buf.find(token) != std::string::npos;
}

static bool CurrentNoteBufferContainsLinkId(const std::wstring& linkId) {
    if (!g_hNoteEdit || linkId.empty()) return false;
    int len = GetWindowTextLengthW(g_hNoteEdit);
    if (len <= 0) return false;
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(g_hNoteEdit, text.data(), len + 1);
    if (copied <= 0) return false;
    text.resize(static_cast<size_t>(copied));
    return TextContainsLinkId(text, linkId);
}

static bool LinkIdExistsOnDisk(const std::wstring& linkId) {
    if (linkId.empty()) return false;
    std::string utf8 = WideToUTF8(linkId);
    if (utf8.empty()) return false;

    const std::string noteToken1 = "link:" + utf8;
    const std::string noteToken2 = "LINK:" + utf8;
    const std::string noteToken3 = "l=" + utf8;
    const std::string noteToken4 = "link=" + utf8;
    const std::string clropToken = "\"link_id\":\"" + utf8 + "\"";

    for (const auto& f : g_noteFiles) {
        std::filesystem::path p(f.path);
        if (p.empty()) continue;
        if (FileContainsAsciiToken(p, noteToken1) ||
            FileContainsAsciiToken(p, noteToken2) ||
            FileContainsAsciiToken(p, noteToken3) ||
            FileContainsAsciiToken(p, noteToken4)) {
            return true;
        }
    }

    for (const auto& f : g_pdfFiles) {
        std::filesystem::path clropPath(f.path);
        if (clropPath.empty()) continue;
        clropPath.replace_extension(L".clrop");
        std::error_code ec;
        if (!std::filesystem::exists(clropPath, ec) || ec) continue;
        if (FileContainsAsciiToken(clropPath, clropToken)) {
            return true;
        }
    }
    return false;
}

static bool LinkIdAlreadyUsed(const std::wstring& linkId) {
    if (linkId.empty()) return true;
    if (g_linkPending.active && g_linkPending.id == linkId) return true;
    for (const auto& ann : g_annots) {
        if (ann.type != Annotation::Type::LinkMarker) continue;
        if (ann.linkId == linkId) return true;
    }
    if (CurrentNoteBufferContainsLinkId(linkId)) return true;
    if (LinkIdExistsOnDisk(linkId)) return true;
    return false;
}

static std::wstring GenerateLinkId() {
    static ULONGLONG s_lastIssuedEpochSecond = 0;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    ULONGLONG candidateSec = (sec > 0) ? static_cast<ULONGLONG>(sec) : 0ULL;
    if (candidateSec <= s_lastIssuedEpochSecond) {
        candidateSec = s_lastIssuedEpochSecond + 1ULL;
    }

    for (;;) {
        std::wstring candidate = ToBase36(candidateSec);
        if (!LinkIdAlreadyUsed(candidate)) {
            s_lastIssuedEpochSecond = candidateSec;
            return candidate;
        }
        ++candidateSec;
    }
}

static int PendingLinkPointCount() {
    return g_linkPending.notePoints + g_linkPending.pdfPoints;
}

static void RememberPendingLinkPdfPath(const std::wstring& pdfPath) {
    if (pdfPath.empty()) return;
    if (std::find(g_linkPending.pendingPdfPaths.begin(),
                  g_linkPending.pendingPdfPaths.end(),
                  pdfPath) == g_linkPending.pendingPdfPaths.end()) {
        g_linkPending.pendingPdfPaths.push_back(pdfPath);
    }
}

static bool DiscardPendingLinkMarkersFromPdfPath(HWND owner,
                                                 const std::wstring& pdfPath,
                                                 const std::wstring& linkId) {
    if (pdfPath.empty() || linkId.empty()) return false;
    if (NormalizePathKey(std::filesystem::path(pdfPath)) ==
        NormalizePathKey(std::filesystem::path(CurrentLogicalPdfPath()))) {
        return DiscardPendingPdfLinkMarkers(linkId);
    }

    std::wstring err;
    std::vector<Annotation> annots;
    std::filesystem::path savePath = file_output::FindLatestStagedClropPathForPdf(pdfPath);
    bool loaded = false;
    if (!savePath.empty()) {
        loaded = file_output::LoadResolvedStagedAnnotations(pdfPath, savePath, &annots, &err);
    }
    if (!loaded) {
        savePath = clrop_bridge::ClropPathForPdf(pdfPath);
        if (savePath.empty()) return false;
        std::error_code ec;
        if (!std::filesystem::exists(savePath, ec) || ec) return false;
        bool mismatch = false;
        loaded = clrop_bridge::LoadAnnotations(savePath.wstring(), pdfPath, annots, mismatch, nullptr, err);
    }
    if (!loaded) {
        if (owner) {
            ShowSoftNotice(owner,
                           IsEnglishUi()
                               ? L"Could not remove an unfinished PDF link marker."
                               : L"未完成のPDFリンクマーカーを削除できませんでした。",
                           SoftNoticeKind::Warning);
        }
        return false;
    }

    const auto oldSize = annots.size();
    annots.erase(std::remove_if(annots.begin(), annots.end(), [&](const Annotation& ann) {
                     return ann.type == Annotation::Type::LinkMarker && ann.linkId == linkId;
                 }),
                 annots.end());
    if (annots.size() == oldSize) return false;

    if (!clrop_bridge::SaveAnnotations(savePath.wstring(), pdfPath, annots, err)) {
        if (owner) {
            ShowSoftNotice(owner,
                           IsEnglishUi()
                               ? L"Could not save removal of an unfinished PDF link marker."
                               : L"未完成のPDFリンクマーカー削除を保存できませんでした。",
                           SoftNoticeKind::Warning);
        }
        return false;
    }
    return true;
}

static bool UpdatePendingLinkMarkerNotePathInPdfPath(HWND owner,
                                                     const std::wstring& pdfPath,
                                                     const std::wstring& linkId,
                                                     const std::wstring& notePath) {
    if (pdfPath.empty() || linkId.empty() || notePath.empty()) return false;
    if (NormalizePathKey(std::filesystem::path(pdfPath)) ==
        NormalizePathKey(std::filesystem::path(CurrentLogicalPdfPath()))) {
        return FinalizePendingPdfLinkMarkers(owner, linkId, notePath);
    }

    std::wstring err;
    std::vector<Annotation> annots;
    std::filesystem::path savePath = file_output::FindLatestStagedClropPathForPdf(pdfPath);
    bool loaded = false;
    if (!savePath.empty()) {
        loaded = file_output::LoadResolvedStagedAnnotations(pdfPath, savePath, &annots, &err);
    }
    if (!loaded) {
        savePath = clrop_bridge::ClropPathForPdf(pdfPath);
        if (savePath.empty()) return false;
        std::error_code ec;
        if (!std::filesystem::exists(savePath, ec) || ec) return false;
        bool mismatch = false;
        loaded = clrop_bridge::LoadAnnotations(savePath.wstring(), pdfPath, annots, mismatch, nullptr, err);
    }
    if (!loaded) {
        if (owner) {
            ShowSoftNotice(owner,
                           IsEnglishUi()
                               ? L"Could not update a cross-PDF link marker."
                               : L"PDFをまたぐリンクマーカーを更新できませんでした。",
                           SoftNoticeKind::Warning);
        }
        return false;
    }

    bool changed = false;
    for (auto& ann : annots) {
        if (ann.type != Annotation::Type::LinkMarker) continue;
        if (ann.linkId != linkId) continue;
        if (ann.linkNotePath == notePath) continue;
        ann.linkNotePath = notePath;
        changed = true;
    }
    if (!changed) return false;
    if (!clrop_bridge::SaveAnnotations(savePath.wstring(), pdfPath, annots, err)) {
        if (owner) {
            ShowSoftNotice(owner,
                           IsEnglishUi()
                               ? L"Could not save a cross-PDF link marker update."
                               : L"PDFをまたぐリンクマーカー更新を保存できませんでした。",
                           SoftNoticeKind::Warning);
        }
        return false;
    }
    return true;
}

static void FinalizePendingLinkModeIfReady(HWND owner) {
    if (!g_linkPending.active) return;
    if (g_linkPending.id.empty()) return;
    if (PendingLinkPointCount() < 2) return;
    if (g_linkPending.pdfPoints > 0) {
        if (FinalizePendingPdfLinkMarkers(owner, g_linkPending.id, g_linkPending.notePath)) {
            RememberPendingLinkPdfPath(CurrentLogicalPdfPath());
        }
        if (!g_linkPending.notePath.empty()) {
            for (const auto& pdfPath : g_linkPending.pendingPdfPaths) {
                UpdatePendingLinkMarkerNotePathInPdfPath(owner, pdfPath, g_linkPending.id, g_linkPending.notePath);
            }
        }
    }
    g_linkPending = {};
    UpdateLinkModeButtonState(false);
}

void CancelPendingLinkMode(HWND owner) {
    if (!g_linkPending.active) return;
    const bool complete = (PendingLinkPointCount() >= 2);
    std::wstring pendingId = g_linkPending.id;
    std::vector<std::wstring> pendingPdfPaths = g_linkPending.pendingPdfPaths;
    g_linkPending = {};
    UpdateLinkModeButtonState(false);
    if (!complete && !pendingId.empty()) {
        DiscardPendingPdfLinkMarkers(pendingId);
        for (const auto& pdfPath : pendingPdfPaths) {
            DiscardPendingLinkMarkersFromPdfPath(owner, pdfPath, pendingId);
        }
        if (owner) UpdateWindowTitle(owner);
    }
}

static void PreparePendingLinkForPdfSwitch(HWND owner) {
    if (!g_linkPending.active || g_linkPending.id.empty()) return;
    if (g_linkPending.pdfPoints <= 0 && !g_linkPending.havePdf) return;

    if (FinalizePendingPdfLinkMarkers(owner, g_linkPending.id, g_linkPending.notePath)) {
        RememberPendingLinkPdfPath(CurrentLogicalPdfPath());
    }
    g_linkPending.pdfAnnotIndex = -1;

    if (owner) UpdateWindowTitle(owner);
    UpdateLinkModeButtonState(true);
}

static void ToggleLinkMode(HWND owner) {
    if (g_linkPending.active) {
        CancelPendingLinkMode(owner);
        return;
    }
    g_linkPending = {};
    g_linkPending.active = true;
    g_linkPending.id = GenerateLinkId();
    UpdateLinkModeButtonState(true);
}


// --- Restored functions ---
void RememberCurrentSessionFiles() {
    if (g_currentSessionPath.empty()) return;
    if (IsTempExternalLecturePath(g_currentLecturePath)) return;
    auto key = SessionKeyFromPath(g_currentSessionPath);
    if (!key.empty()) {
        bool changed = false;
        if (!g_currentNotePath.empty()) {
            auto it = g_lastNoteBySession.find(key);
            if (it == g_lastNoteBySession.end() || it->second != g_currentNotePath) {
                g_lastNoteBySession[key] = g_currentNotePath;
                changed = true;
            }
        }
        const std::wstring& logicalPdfPath = CurrentLogicalPdfPath();
        if (!logicalPdfPath.empty()) {
            auto it = g_lastPdfBySession.find(key);
            if (it == g_lastPdfBySession.end() || it->second != logicalPdfPath) {
                g_lastPdfBySession[key] = logicalPdfPath;
                changed = true;
            }
        }
        if (changed) {
            SaveSessionLastOpenMap();
        }
    }
}

void ApplySessionAutoOpenPreference(const SessionEntry& session,
                                           std::wstring& preferredPdf,
                                           std::wstring& preferredNote) {
    if (ParseSessionAutoOpenMode(g_config.sessionAutoOpenMode) != SessionAutoOpenMode::View) {
        return;
    }
    if (preferredPdf.empty()) {
        preferredPdf = PreferredPdfForSession(session);
    }
    if (preferredNote.empty()) {
        preferredNote = PreferredNoteForSession(session);
    }
}

void SortSessionsForDisplay(std::vector<SessionEntry>& sessions, SessionSortMode mode) {
    if (mode == SessionSortMode::Name) {
        std::sort(sessions.begin(), sessions.end(),
            [](const SessionEntry& a, const SessionEntry& b) { return a.displayName < b.displayName; });
        return;
    }
    std::sort(sessions.begin(), sessions.end(), [&](const SessionEntry& a, const SessionEntry& b) {
        auto na = ExtractNumericKey(a.displayName);
        auto nb = ExtractNumericKey(b.displayName);
        if (na && nb && *na != *nb) {
            return mode == SessionSortMode::NumericDesc ? *na > *nb : *na < *nb;
        }
        if (na && !nb) return true;
        if (!na && nb) return false;
        return mode == SessionSortMode::NumericDesc ? a.displayName > b.displayName
                                                    : a.displayName < b.displayName;
    });
}

std::filesystem::path LectureLastOpenFilePath() {
    auto settingsDir = LectureSettingsDir();
    if (settingsDir.empty()) return {};
    return settingsDir / L"__tmp__" / kLectureLastOpenFileName;
}

LectureSortMode ParseLectureSortMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"name") return LectureSortMode::Name;
    if (m == L"schedule") return LectureSortMode::Schedule;
    return LectureSortMode::Recent;
}

void InitScheduleSortBaseTime() {
    if (g_scheduleSortTimeSet) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    g_scheduleSortDayIndex = ScheduleDayIndexFromSystem(st.wDayOfWeek);
    g_scheduleSortMinutes = static_cast<int>(st.wHour) * 60 + static_cast<int>(st.wMinute);
    g_scheduleSortTimeSet = true;
}

SessionSortMode ParseSessionSortMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"numeric_desc" || m == L"desc" || m == L"number_desc") return SessionSortMode::NumericDesc;
    if (m == L"name" || m == L"lexical") return SessionSortMode::Name;
    return SessionSortMode::NumericAsc;
}

ScheduleRank NextScheduleRank(const std::wstring& lectureName,
                                     int todayIndex,
                                     int nowMinutes,
                                     int dayMask,
                                     int periods,
                                     const std::vector<std::wstring>& cells,
                                     const std::vector<std::wstring>& times) {
    ScheduleRank result{};
    if (lectureName.empty()) return result;
    const long long kWeekMinutes = 7LL * 24 * 60;
    for (int p = 0; p < periods; ++p) {
        for (int d = 0; d < 7; ++d) {
            if ((dayMask & (1 << d)) == 0) continue;
            size_t idx = static_cast<size_t>(d + p * 7);
            if (idx >= cells.size() || idx >= times.size()) continue;
            if (cells[idx] != lectureName) continue;
            int minutes = 0;
            if (!TryParseScheduleTimeMinutes(times[idx], minutes)) continue;
            int dayDiff = (d - todayIndex + 7) % 7;
            long long delta = static_cast<long long>(dayDiff) * 24 * 60 + (minutes - nowMinutes);
            if (delta < 0) delta += kWeekMinutes;
            if (delta < result.delta || (delta == result.delta && dayDiff < result.dayOffset)) {
                result.delta = delta;
                result.dayOffset = dayDiff;
                result.hasSchedule = true;
            }
        }
    }
    return result;
}

std::wstring FileDisplayLabelForPath(const std::wstring& filePath,
                                            const std::vector<FileEntry>& files,
                                            const std::filesystem::path& root) {
    if (filePath.empty()) return L"-";

    std::filesystem::path path(filePath);
    const std::wstring pathKey = NormalizePathKeyForList(filePath);
    const bool searchTemp =
        (!pathKey.empty() &&
         (s_searchTempPdfKeys.find(pathKey) != s_searchTempPdfKeys.end() ||
          s_searchTempNoteKeys.find(pathKey) != s_searchTempNoteKeys.end()));
    const bool hierarchyTemp =
        (!pathKey.empty() &&
         (s_hierarchyTempPdfKeys.find(pathKey) != s_hierarchyTempPdfKeys.end() ||
          s_hierarchyTempNoteKeys.find(pathKey) != s_hierarchyTempNoteKeys.end()));
    std::wstring prefix;
    if (searchTemp) {
        prefix = IsEnglishUi() ? L"[Search] " : L"[検索] ";
    } else if (hierarchyTemp) {
        prefix = IsEnglishUi() ? L"[Temp] " : L"[一時] ";
    }
    std::wstring base = FilenameOrPath(path);
    size_t duplicates = 0;
    std::wstring baseKey = ToLowerAscii(base);
    for (const auto& item : files) {
        std::wstring other = FilenameOrPath(std::filesystem::path(item.path));
        if (ToLowerAscii(other) == baseKey) {
            ++duplicates;
        }
    }
    if (duplicates <= 1) return prefix + base;

    std::wstring rel = RelativeOrPathForDisplay(path, root);
    if (!rel.empty() && rel != base) return prefix + rel;

    std::wstring parent = path.parent_path().filename().wstring();
    if (parent.empty()) parent = path.parent_path().wstring();
    if (parent.empty()) return prefix + base;
    return prefix + base + L" [" + parent + L"]";
}


#include "app/command_dispatch.cppinc"

// ---------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------
static HWND MainWindowHandle() {
    if (g_hNoteEdit) return GetParent(g_hNoteEdit);
    if (g_hBottomNote) return GetParent(g_hBottomNote);
    if (g_hPdfView) return GetParent(g_hPdfView);
    return nullptr;
}

static void ApplyMainMenuBar(HWND hWnd, const MainMenuStateSnapshot& menuState) {
    if (!hWnd) return;
    const ULONGLONG startTick = preview_trace::TickNow();
    preview_trace::Append(
        L"ApplyMainMenuBar",
        L"begin owner=" + preview_trace::Window(hWnd) +
        L" ownerDrawUi=" + preview_trace::Bool(g_config.ownerDrawUi));
    HMENU oldMenu = GetMenu(hWnd);
    if (g_config.ownerDrawUi) {
        g_menuItemData.clear();
    }
    HMENU bar = BuildMenuBarForState(menuState);
    if (!bar) {
        preview_trace::Append(
            L"ApplyMainMenuBar",
            L"abort=build_failed elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return;
    }
    preview_trace::Append(
        L"ApplyMainMenuBar",
        L"after_build_menu elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    SetMenu(hWnd, bar);
    g_hMainMenu = bar;
    RefreshStatusDisplay(hWnd);
    preview_trace::Append(
        L"ApplyMainMenuBar",
        L"after_refresh_status elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    s_lastMainMenuStateSnapshot = menuState;
    preview_trace::Append(
        L"ApplyMainMenuBar",
        L"after_commit_state elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    DrawMenuBar(hWnd);
    preview_trace::Append(
        L"ApplyMainMenuBar",
        L"after_draw_menu elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    if (oldMenu && oldMenu != bar) {
        // Avoid temporarily removing the menu bar; that causes a non-client relayout flicker.
        DestroyMenu(oldMenu);
    }
    preview_trace::Append(
        L"ApplyMainMenuBar",
        L"end elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

void RefreshMainMenuBar(HWND hWnd) {
    if (!hWnd) return;
    const ULONGLONG startTick = preview_trace::TickNow();
    preview_trace::Append(
        L"RefreshMainMenuBar",
        L"begin owner=" + preview_trace::Window(hWnd));
    const MainMenuStateSnapshot menuState = CaptureMainMenuStateSnapshot();
    ApplyMainMenuBar(hWnd, menuState);
    preview_trace::Append(
        L"RefreshMainMenuBar",
        L"end elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

std::wstring BuildStatusDisplayText() {
    std::wstring lecture = g_currentLecturePath.empty()
        ? L"-"
        : LectureDisplayLabelForPath(g_currentLecturePath, g_lectures);
    std::wstring session = L"-";
    if (!g_currentSessionPath.empty()) {
        if (IsDirectFilesSessionPath(g_currentSessionPath, g_currentLecturePath)) {
            session = DirectFilesSessionLabel();
        } else {
            auto it = std::find_if(g_sessions.begin(), g_sessions.end(),
                                   [&](const SessionEntry& item) {
                                       return SessionKey(item) == SessionKeyFromPath(g_currentSessionPath);
                                   });
            if (it != g_sessions.end()) {
                session = SessionDisplayLabelForEntry(*it, g_sessions, g_currentLecturePath);
            } else {
                SessionEntry fallback;
                fallback.displayName = FilenameOrPath(std::filesystem::path(g_currentSessionPath));
                fallback.path = g_currentSessionPath;
                session = SessionDisplayLabelForEntry(fallback, g_sessions, g_currentLecturePath);
            }
        }
    }
    std::filesystem::path sessionRoot = g_currentSessionPath.empty()
        ? std::filesystem::path{}
        : std::filesystem::path(g_currentSessionPath);
    const std::wstring& logicalPdfPath = CurrentLogicalPdfPath();
    std::wstring pdf = FileDisplayLabelForPath(logicalPdfPath, g_pdfFiles, sessionRoot);
    if (logicalPdfPath.empty()) pdf = L"-";
    std::wstring note = FileDisplayLabelForPath(g_currentNotePath, g_noteFiles, sessionRoot);
    if (g_currentNotePath.empty()) note = L"-";
    std::wstring text = (g_config.studentMode ? L"講義: " : L"上位: ") + lecture +
                        (g_config.studentMode ? L" | 回次: " : L" | 下位: ") + session +
                        L" | PDF: " + pdf + L" | Note: " + note;
    std::wstring officeProgress = BuildOfficeConversionProgressStatusText();
    if (!officeProgress.empty()) {
        text += L" | " + officeProgress;
    }
    text += L" | " + BuildSaveStateStatusText();
    return text;
}

void RefreshMainWindowUiState(HWND hWnd) {
    HWND owner = g_hMainWnd ? g_hMainWnd : hWnd;
    if (!owner) return;
    if (s_deferredMainWindowUiRefreshDepth > 0) {
        s_deferredMainWindowUiRefreshPending = true;
        s_deferredMainWindowUiRefreshOwner = owner;
        preview_trace::Append(
            L"RefreshMainWindowUiState",
            L"deferred owner=" + preview_trace::Window(owner) +
            L" depth=" + std::to_wstring(s_deferredMainWindowUiRefreshDepth));
        return;
    }
    ClearHierarchyTemporaryFileDisplaysIfSafe();
    const ULONGLONG startTick = preview_trace::TickNow();
    preview_trace::Append(
        L"RefreshMainWindowUiState",
        L"begin owner=" + preview_trace::Window(owner));
    UpdateWindowTitle(owner);
    preview_trace::Append(
        L"RefreshMainWindowUiState",
        L"after_update_title elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    const MainMenuStateSnapshot menuState = CaptureMainMenuStateSnapshot();
    const bool menuChanged =
        !s_lastMainMenuStateSnapshot.has_value() || !(*s_lastMainMenuStateSnapshot == menuState);
    if (menuChanged) {
        preview_trace::Append(
            L"RefreshMainWindowUiState",
            L"menu_changed elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        ApplyMainMenuBar(owner, menuState);
        preview_trace::Append(
            L"RefreshMainWindowUiState",
            L"end menu_refreshed elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return;
    }
    const StatusDisplayStateSnapshot statusState{BuildStatusDisplayText()};
    preview_trace::Append(
        L"RefreshMainWindowUiState",
        L"after_build_status elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    const bool statusChanged =
        !s_lastStatusDisplayStateSnapshot.has_value() || !(*s_lastStatusDisplayStateSnapshot == statusState);
    if (statusChanged) {
        RefreshStatusDisplay(owner);
        preview_trace::Append(
            L"RefreshMainWindowUiState",
            L"after_refresh_status elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    }
    preview_trace::Append(
        L"RefreshMainWindowUiState",
        L"end menuChanged=" + preview_trace::Bool(menuChanged) +
        L" statusChanged=" + preview_trace::Bool(statusChanged) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

static void DrawShortcutPreview(LPDRAWITEMSTRUCT dis, COLORREF color) {
    if (!dis) return;
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dis->hDC, &dis->rcItem, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
              dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);
}

static void DisableIntegratedPdfPreview(HWND owner, bool restoreOriginalPdf) {
    (void)owner;
    (void)restoreOriginalPdf;
}

void EndIntegratedPdfPreviewMode(HWND owner, bool restoreOriginalPdf) {
    DisableIntegratedPdfPreview(owner, restoreOriginalPdf);
}

static void SetBottomPanePinMode(BottomPanePin pin, BottomNoteMode noteMode) {
    g_bottomPanePin = pin;
    g_bottomNoteMode = noteMode;
    g_config.bottomPanePin = BottomPanePinToString(pin);
    g_config.bottomNoteMode = BottomNoteModeToString(noteMode);
    UpdateBottomPaneMenuChecks();
    ApplyBottomPaneEdgeStyle();
    PersistConfig();
    if (HWND owner = MainWindowHandle()) {
        LayoutChildren(owner);
    }
    RefreshBottomPaneView();
    if (g_hBottomNote) InvalidateRect(g_hBottomNote, nullptr, FALSE);
    if (g_hBottomRight) InvalidateRect(g_hBottomRight, nullptr, FALSE);
    if (g_hBottomMath) InvalidateRect(g_hBottomMath, nullptr, FALSE);
}

// ---------------------------------------------------------------------
HACCEL BuildAccelerators() {
    ACCEL acc[] = {
        { FCONTROL | FVIRTKEY, static_cast<WORD>('S'), static_cast<WORD>(ID_FILE_SAVE_ALL) },
        { FCONTROL | FVIRTKEY, static_cast<WORD>('F'), static_cast<WORD>(ID_SEARCH) }
    };
    return CreateAcceleratorTableW(acc, static_cast<int>(std::size(acc)));
}


static bool ShouldSuppressAnnotToolShortcutForFocus(const MSG& msg) {
    if (g_pdf.editingText) return true;
    if (IsNoteCmdlineActive() || IsNoteImeComposing()) return true;
    HWND focus = GetFocus();
    if (IsExplicitImeTargetWindow(focus) || IsExplicitImeTargetWindow(msg.hwnd)) return true;
    if (focus && g_hNoteEdit && (focus == g_hNoteEdit || IsChild(g_hNoteEdit, focus))) return true;
    return false;
}

static bool MainLoopShortcutMessageBelongsToOwner(HWND owner, const MSG& msg) {
    if (!owner || !msg.hwnd) return true;
    HWND root = GetAncestor(msg.hwnd, GA_ROOT);
    return !root || root == owner;
}



HWND MainDialogOwner(HWND owner) {
    return owner ? owner : g_hMainWnd;
}

static void ShowMainSoftNotice(HWND owner, const std::wstring& text, SoftNoticeKind kind) {
    ShowSoftNotice(MainDialogOwner(owner), text, kind);
}

void ShowMainMessageDialog(HWND owner, const std::wstring& title,
                                   const std::wstring& message, SoftNoticeKind kind) {
    ShowSilentMessageDialog(MainDialogOwner(owner), title, message, kind);
}

static HWND ResolveManagedAbnormalExitOwner(HWND owner) {
    if (owner && IsWindow(owner)) {
        if (HWND root = GetAncestor(owner, GA_ROOT)) return root;
        return owner;
    }
    return g_hMainWnd;
}

static void ClearManagedAbnormalExitRequest() {
    s_managedAbnormalExit.requested = false;
    s_managedAbnormalExit.triggerTitle.clear();
    s_managedAbnormalExit.triggerMessage.clear();
}

static std::wstring ManagedAbnormalExitLabel(bool ok, bool hadDirtyState,
                                             const wchar_t* cleanJa,
                                             const wchar_t* cleanEn) {
    if (!hadDirtyState) {
        return IsEnglishUi() ? cleanEn : cleanJa;
    }
    return ok
        ? (IsEnglishUi() ? L"saved to stage" : L"stage へ退避しました")
        : (IsEnglishUi() ? L"failed to save to stage" : L"stage へ退避できませんでした");
}

static bool ManagedAbnormalExitTextContains(const std::wstring& text, const wchar_t* token) {
    if (text.empty() || !token || *token == L'\0') return false;
    const std::wstring textLower = ToLowerAscii(text);
    const std::wstring tokenLower = ToLowerAscii(token);
    return textLower.find(tokenLower) != std::wstring::npos;
}

static bool ManagedAbnormalExitTriggerContains(const wchar_t* token) {
    return ManagedAbnormalExitTextContains(s_managedAbnormalExit.triggerTitle, token) ||
           ManagedAbnormalExitTextContains(s_managedAbnormalExit.triggerMessage, token);
}

static void AppendManagedAbnormalExitPathBlock(std::wstringstream& ss,
                                               const std::wstring& label,
                                               const std::wstring& path,
                                               const std::wstring& emptyLabel) {
    ss << label << L":\n";
    ss << L"  " << (path.empty() ? emptyLabel : path) << L"\n";
}

static void AppendManagedAbnormalExitStagePaths(std::wstringstream& ss) {
    const auto entries = file_output::ListStagedDiffEntries();
    if (entries.empty()) {
        ss << (IsEnglishUi()
                   ? L"Stage diff files:\n  (none)\n"
                   : L"stage 差分ファイル:\n  (なし)\n");
        return;
    }

    if (IsEnglishUi()) {
        ss << L"Stage diff files: " << entries.size() << L"\n";
    } else {
        ss << L"stage 差分ファイル: " << entries.size() << L" 件\n";
    }

    const size_t limit = std::min<size_t>(entries.size(), 8);
    for (size_t i = 0; i < limit; ++i) {
        const auto& entry = entries[i];
        const bool isNote = entry.kind == file_output::StagedDiffKind::Note;
        if (IsEnglishUi()) {
            ss << L"[" << (i + 1) << L"] "
               << (isNote ? L"Note" : L"Annotations")
               << (entry.isLatest ? L" (latest)" : L" (old)") << L"\n";
            ss << L"  Original: " << (entry.targetPath.empty() ? L"(unknown)" : entry.targetPath) << L"\n";
            ss << L"  Stage: " << (entry.stagePath.empty() ? L"(unknown)" : entry.stagePath.wstring()) << L"\n";
            if (!entry.destPath.empty() && entry.destPath.wstring() != entry.targetPath) {
                ss << L"  Integrate to: " << entry.destPath.wstring() << L"\n";
            }
        } else {
            ss << L"[" << (i + 1) << L"] "
               << (isNote ? L"ノート" : L"注釈")
               << (entry.isLatest ? L" (最新)" : L" (旧)") << L"\n";
            ss << L"  原本: " << (entry.targetPath.empty() ? L"(不明)" : entry.targetPath) << L"\n";
            ss << L"  stage: " << (entry.stagePath.empty() ? L"(不明)" : entry.stagePath.wstring()) << L"\n";
            if (!entry.destPath.empty() && entry.destPath.wstring() != entry.targetPath) {
                ss << L"  統合先: " << entry.destPath.wstring() << L"\n";
            }
        }
    }
    if (entries.size() > limit) {
        if (IsEnglishUi()) {
            ss << L"... and " << (entries.size() - limit) << L" more stage diffs.\n";
        } else {
            ss << L"... ほか " << (entries.size() - limit) << L" 件の stage 差分があります。\n";
        }
    }
}

static void AppendManagedAbnormalExitCauseSummary(std::wstringstream& ss) {
    if (ManagedAbnormalExitTriggerContains(L"note persistence record was not found")) {
        if (IsEnglishUi()) {
            ss << L"Predicted cause:\n";
            ss << L"The internal persistence record for the current note may be missing.\n";
            ss << L"This can prevent the app from matching the current note to its durable save history.\n";
            ss << L"As a result, the same warning may repeat during save or stage preservation.\n";
            ss << L"To avoid touching original files in an inconsistent state, the app preserved unsaved edits to stage and then closed.\n";
        } else {
            ss << L"予測される原因:\n";
            ss << L"現在のノートについて、保存履歴を追跡する内部記録が見つからない可能性があります。\n";
            ss << L"そのため、このノートを既存の保存履歴と正しく結び付けられず、保存や stage 退避のたびに同じ警告が再発した可能性があります。\n";
            ss << L"原本を不整合な状態で触らないため、未保存分を stage に退避してから終了しました。\n";
        }
        return;
    }

    if (ManagedAbnormalExitTriggerContains(L"stage") &&
        (ManagedAbnormalExitTriggerContains(L"failed") || ManagedAbnormalExitTriggerContains(L"失敗"))) {
        if (IsEnglishUi()) {
            ss << L"Predicted cause:\n";
            ss << L"A stage save or stage integration problem was reported.\n";
            ss << L"If the app kept running, repeated warnings or partial cleanup could have made the state harder to understand.\n";
            ss << L"The app therefore closed through the abnormal-exit path after preserving what it could without overwriting originals.\n";
        } else {
            ss << L"予測される原因:\n";
            ss << L"stage への保存または stage の統合で問題が発生した可能性があります。\n";
            ss << L"このまま動かし続けると、警告の再発や状態の把握しづらさにつながるおそれがあるため、原本を上書きせずに異常終了経路へ入りました。\n";
        }
        return;
    }

    if (IsEnglishUi()) {
        ss << L"Predicted cause:\n";
        ss << L"A warning or error dialog indicated that the current editing state was no longer safe to continue normally.\n";
        ss << L"To avoid data loss or writing inconsistent content to original files, the app preserved unsaved edits to stage and then closed.\n";
    } else {
        ss << L"予測される原因:\n";
        ss << L"警告またはエラーにより、このまま通常動作を続けるのが安全ではない状態になったと判断しました。\n";
        ss << L"データ消失や不整合な上書きを避けるため、未保存分を stage に退避してから終了しました。\n";
    }
}

static std::wstring BuildManagedAbnormalExitReportText(bool noteHadDirty,
                                                       bool noteStageOk,
                                                       bool annotHadDirty,
                                                       bool annotStageOk) {
    std::wstringstream ss;
    const auto escapeRoot = EscapeRootPath();
    if (IsEnglishUi()) {
        ss << L"This app performed a managed abnormal exit.\n";
        ss << L"The main window has already been closed.\n\n";
        AppendManagedAbnormalExitCauseSummary(ss);
        ss << L"\nStage preservation:\n";
        ss << L"- Note: " << ManagedAbnormalExitLabel(noteStageOk, noteHadDirty, L"", L"already clean") << L"\n";
        ss << L"- Annotations: " << ManagedAbnormalExitLabel(annotStageOk, annotHadDirty, L"", L"already clean") << L"\n";
        ss << L"- Staged diffs present: " << (file_output::HasAnyStagedDiffs() ? L"yes" : L"no") << L"\n";
        ss << L"\nWhere your files are:\n";
        AppendManagedAbnormalExitPathBlock(ss, L"Workspace", g_workspaceRoot, L"(none)");
        AppendManagedAbnormalExitPathBlock(ss, L"Current note original", g_currentNotePath, L"(none)");
        AppendManagedAbnormalExitPathBlock(ss, L"Current PDF original", CurrentLogicalPdfPath(), L"(none)");
        AppendManagedAbnormalExitPathBlock(
            ss,
            L"Emergency backup root (__resource__\\__escape__)",
            escapeRoot.wstring(),
            L"(not created)");
        AppendManagedAbnormalExitStagePaths(ss);
        ss << L"\nOriginal files were not overwritten by this abnormal-exit path.\n\n";
        ss << L"Trigger dialog:\n";
        ss << L"Title: "
           << (s_managedAbnormalExit.triggerTitle.empty() ? L"(untitled)" : s_managedAbnormalExit.triggerTitle) << L"\n";
        ss << L"Message:\n";
        ss << L"  "
           << (s_managedAbnormalExit.triggerMessage.empty() ? L"(no message)" : s_managedAbnormalExit.triggerMessage);
    } else {
        ss << L"管理された異常終了を実行しました。\n";
        ss << L"メインウィンドウはすでに終了しています。\n\n";
        AppendManagedAbnormalExitCauseSummary(ss);
        ss << L"\nstage 保持状況:\n";
        ss << L"- ノート: " << ManagedAbnormalExitLabel(noteStageOk, noteHadDirty, L"もともと未保存なし", L"") << L"\n";
        ss << L"- 注釈: " << ManagedAbnormalExitLabel(annotStageOk, annotHadDirty, L"もともと未保存なし", L"") << L"\n";
        ss << L"- stage 差分あり: " << (file_output::HasAnyStagedDiffs() ? L"はい" : L"いいえ") << L"\n";
        ss << L"\n関連パスと退避先:\n";
        AppendManagedAbnormalExitPathBlock(ss, L"ワークスペース", g_workspaceRoot, L"(なし)");
        AppendManagedAbnormalExitPathBlock(ss, L"現在のノート原本", g_currentNotePath, L"(なし)");
        AppendManagedAbnormalExitPathBlock(ss, L"現在の PDF 原本", CurrentLogicalPdfPath(), L"(なし)");
        AppendManagedAbnormalExitPathBlock(
            ss,
            L"緊急退避ルート (__resource__\\__escape__)",
            escapeRoot.wstring(),
            L"(未作成)");
        AppendManagedAbnormalExitStagePaths(ss);
        ss << L"\nこの異常終了経路では原本ファイルを上書きしていません。\n\n";
        ss << L"きっかけになったダイアログ:\n";
        ss << L"タイトル: "
           << (s_managedAbnormalExit.triggerTitle.empty() ? L"(無題)" : s_managedAbnormalExit.triggerTitle) << L"\n";
        ss << L"本文:\n";
        ss << L"  "
           << (s_managedAbnormalExit.triggerMessage.empty() ? L"(メッセージなし)" : s_managedAbnormalExit.triggerMessage);
    }
    return ss.str();
}

bool CanRequestManagedAbnormalExitFromDialog(HWND owner, SoftNoticeKind kind) {
    if (kind == SoftNoticeKind::Info) return false;
    if (IsUiAutomationEnabled()) return false;
    if (s_exitInProgress || s_exitPending) return false;
    if (!g_hMainWnd || !IsWindow(g_hMainWnd)) return false;
    return ResolveManagedAbnormalExitOwner(owner) == g_hMainWnd;
}

void RequestManagedAbnormalExitFromDialog(HWND owner, const std::wstring& title, const std::wstring& message) {
    if (!CanRequestManagedAbnormalExitFromDialog(owner, SoftNoticeKind::Error) &&
        !CanRequestManagedAbnormalExitFromDialog(owner, SoftNoticeKind::Warning)) {
        return;
    }
    s_managedAbnormalExit.requested = true;
    s_managedAbnormalExit.completed = false;
    s_managedAbnormalExit.reportPending = false;
    s_managedAbnormalExit.exitCode = kManagedAbnormalExitCode;
    s_managedAbnormalExit.triggerTitle = title;
    s_managedAbnormalExit.triggerMessage = message;
    s_managedAbnormalExit.reportText.clear();
    preview_trace::Append(
        L"ManagedAbnormalExit",
        L"requested title=" + title +
        L" message_len=" + std::to_wstring(message.size()));
    AppendCrashLogWide(L"ManagedAbnormalExitRequest",
                       title + L"\n" + message);
    if (g_hMainWnd && IsWindow(g_hMainWnd)) {
        PostMessageW(g_hMainWnd, WM_CLOSE, 0, 0);
    }
}

void ShowManagedAbnormalExitReportIfPending() {
    if (!s_managedAbnormalExit.reportPending) return;
    s_managedAbnormalExit.reportPending = false;
    if (s_managedAbnormalExit.reportText.empty()) return;
    SilentDialogOptions options;
    options.title = IsEnglishUi() ? L"Abnormal exit report" : L"異常終了レポート";
    options.message = s_managedAbnormalExit.reportText;
    options.kind = SoftNoticeKind::Error;
    options.buttons = SilentDialogButtons::Ok;
    options.defaultResult = SilentDialogResult::Ok;
    options.escapeResult = SilentDialogResult::Ok;
    options.preferredWidthPx = 760;
    (void)ShowSilentDialog(nullptr, options);
}

int OverrideExitCodeForManagedAbnormalExit(int defaultCode) {
    return s_managedAbnormalExit.completed ? s_managedAbnormalExit.exitCode : defaultCode;
}

void AppendMainOperationExceptionLog(const char* area, const char* detail) {
    AppendCrashLogLine(area ? area : "main", detail);
}

void ReportMainOperationException(HWND owner, const wchar_t* operation) {
    std::wstring msg = operation ? operation : L"この操作";
    msg += IsEnglishUi()
        ? L" failed due to an unexpected internal error. The operation was canceled."
        : L" で予期しない内部エラーが発生したため、この操作を中止しました。";
    if (IsUiAutomationEnabled()) {
        ShowMainSoftNotice(owner, msg, SoftNoticeKind::Error);
        return;
    }
    ShowMainMessageDialog(owner,
                          IsEnglishUi() ? L"Operation canceled" : L"操作を中止しました",
                          msg,
                          SoftNoticeKind::Error);
}

#include "features/automation/main_ui_automation.cppinc"


static void RestorePreviousNoteAfterException(HWND owner, const std::wstring& previousNotePath) {
    try {
        if (!previousNotePath.empty()) {
            LoadNoteFile(owner, previousNotePath);
            SyncBottomPaneAfterNoteLoad(owner);
        } else {
            ClearNoteEditorSilently(owner, L"");
            g_previewNote.clear();
            RefreshBottomPaneView();
        }
        UpdateWindowTitle(owner);
        RefreshMainMenuBar(owner);
        SyncLeftPaneSelection();
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("RestorePreviousNoteAfterException", ex.what());
        SyncLeftPaneSelection();
    } catch (...) {
        AppendMainOperationExceptionLog("RestorePreviousNoteAfterException", nullptr);
        SyncLeftPaneSelection();
    }
}

static void RestorePreviousPdfAfterException(HWND owner, const std::wstring& previousPdfPath) {
    try {
        if (!previousPdfPath.empty()) {
            EndIntegratedPdfPreviewMode(owner, true);
            OpenPdfWithAnnotations(owner, previousPdfPath);
        } else {
            ClearPdfState();
            if (g_hPdfView) InvalidateRect(g_hPdfView, nullptr, FALSE);
            SyncLeftPaneSelection();
        }
        UpdateWindowTitle(owner);
        RefreshMainMenuBar(owner);
        SyncLeftPaneSelection();
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("RestorePreviousPdfAfterException", ex.what());
        SyncLeftPaneSelection();
    } catch (...) {
        AppendMainOperationExceptionLog("RestorePreviousPdfAfterException", nullptr);
        SyncLeftPaneSelection();
    }
}

static void RestoreLectureSessionStateAfterException(HWND owner,
                                                     const std::wstring& previousLecturePath,
                                                     const std::wstring& previousSessionPath,
                                                     const std::wstring& previousPdfPath,
                                                     const std::wstring& previousNotePath) {
    try {
        if (previousLecturePath.empty()) {
            ClearPdfAndNoteSelection();
            ResetSessionAndFiles();
            g_currentLecturePath.clear();
            g_currentSessionPath.clear();
            UpdateWindowTitle(owner);
            RefreshMainMenuBar(owner);
            SyncLeftPaneSelection();
            return;
        }

        std::wstring desiredSessionName;
        if (!previousSessionPath.empty()) {
            desiredSessionName = std::filesystem::path(previousSessionPath).filename().wstring();
        }
        ReloadSessionsAndSelect(previousLecturePath, desiredSessionName, /*reopenFiles=*/false);
        if (!previousPdfPath.empty()) {
            EndIntegratedPdfPreviewMode(owner, true);
            OpenPdfWithAnnotations(owner, previousPdfPath);
        }
        if (!previousNotePath.empty()) {
            LoadNoteFile(owner, previousNotePath);
            SyncBottomPaneAfterNoteLoad(owner);
        } else {
            ClearNoteEditorSilently(owner, L"");
            g_previewNote.clear();
            RefreshBottomPaneView();
        }
        UpdateWindowTitle(owner);
        RefreshMainMenuBar(owner);
        SyncLeftPaneSelection();
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("RestoreLectureSessionStateAfterException", ex.what());
        SyncLeftPaneSelection();
    } catch (...) {
        AppendMainOperationExceptionLog("RestoreLectureSessionStateAfterException", nullptr);
        SyncLeftPaneSelection();
    }
}

bool ConfirmMainYesNo(HWND owner, const std::wstring& title, const std::wstring& message,
                             SoftNoticeKind kind, SilentDialogResult defaultResult,
                             SilentDialogResult escapeResult) {
    SilentDialogOptions options;
    options.title = title;
    options.message = message;
    options.kind = kind;
    options.buttons = SilentDialogButtons::YesNo;
    options.defaultResult = defaultResult;
    options.escapeResult = escapeResult;
    return ShowSilentDialog(MainDialogOwner(owner), options) == SilentDialogResult::Yes;
}

static bool HasExportablePdf() {
    return CurrentLogicalPdfDocument() != nullptr;
}

static bool HasExportableNote() {
    return !g_currentNotePath.empty();
}

static bool CanStartExportCommand(HWND hWnd, UINT id) {
    const bool needPdf =
        (id == ID_FILE_EXPORT_PDF || id == ID_FILE_EXPORT_PDF_QUICK ||
         id == ID_FILE_EXPORT_PDF_PAGES || id == ID_FILE_EXPORT_PNG_PAGE);
    const bool needNote =
        (id == ID_FILE_EXPORT_NOTE_TEXT_QUICK || id == ID_FILE_EXPORT_NOTE_MARKDOWN_QUICK ||
         id == ID_FILE_EXPORT_NOTE_HTML_QUICK || id == ID_FILE_EXPORT_NOTE_TEXT || id == ID_FILE_EXPORT_NOTE_MARKUP ||
         id == ID_FILE_EXPORT_NOTE_HTML);
    if (needPdf && !HasExportablePdf()) {
        ShowSoftNotice(hWnd, L"PDFが開かれていません。", SoftNoticeKind::Warning);
        return false;
    }
    if (needNote && !HasExportableNote()) {
        ShowSoftNotice(hWnd, L"ノートが開かれていません。", SoftNoticeKind::Warning);
        return false;
    }
    return true;
}

static void ArchiveWorkspaceLogFiles(HWND owner) {
    std::wstring scanErr;
    const std::vector<WorkspaceLogFileInfo> logFiles = EnumerateWorkspaceLogFiles(&scanErr);
    if (!scanErr.empty()) {
        ShowMainMessageDialog(owner, DebugMenuLabel(), scanErr, SoftNoticeKind::Warning);
        return;
    }
    if (logFiles.empty()) {
        ShowMainSoftNotice(owner,
                           IsEnglishUi() ? L"No log files were found." : L"ログファイルがありません。",
                           SoftNoticeKind::Info);
        return;
    }

    const std::filesystem::path logDir = WorkspaceLogDirectory();
    if (logDir.empty()) {
        ShowMainSoftNotice(owner,
                           IsEnglishUi() ? L"No workspace is open." : L"ワークスペースが開かれていません。",
                           SoftNoticeKind::Warning);
        return;
    }

    const std::filesystem::path archivePath =
        atomic_write::MakeUniqueDestInDir(logDir, WorkspaceLogArchiveBaseName());
    SaveOperationGuard guard;
    std::wstring err;
    if (!WriteWorkspaceLogZipArchive(logFiles, archivePath, &err)) {
        std::wstring msg = IsEnglishUi()
            ? L"Failed to create the ZIP archive."
            : L"ZIPアーカイブの作成に失敗しました。";
        if (!err.empty()) msg += L"\n\n" + err;
        ShowMainMessageDialog(owner, DebugMenuLabel(), msg, SoftNoticeKind::Error);
        return;
    }

    std::wstring msg = IsEnglishUi()
        ? (L"Archived " + std::to_wstring(logFiles.size()) + L" log file(s): " + archivePath.filename().wstring())
        : (std::to_wstring(logFiles.size()) + L" 件のログを ZIP 保存しました: " + archivePath.filename().wstring());
    ShowMainSoftNotice(owner, msg, SoftNoticeKind::Info);
}

static void DeleteWorkspaceLogFiles(HWND owner) {
    std::wstring scanErr;
    const std::vector<WorkspaceLogFileInfo> logFiles = EnumerateWorkspaceLogFiles(&scanErr);
    if (!scanErr.empty()) {
        ShowMainMessageDialog(owner, DebugMenuLabel(), scanErr, SoftNoticeKind::Warning);
        return;
    }
    if (logFiles.empty()) {
        ShowMainSoftNotice(owner,
                           IsEnglishUi() ? L"No log files were found." : L"ログファイルがありません。",
                           SoftNoticeKind::Info);
        RefreshMainMenuBar(owner);
        return;
    }

    SilentDialogOptions dialog;
    dialog.title = DebugMenuLabel();
    dialog.kind = SoftNoticeKind::Warning;
    dialog.buttons = SilentDialogButtons::YesNo;
    dialog.defaultResult = SilentDialogResult::No;
    dialog.escapeResult = SilentDialogResult::No;
    dialog.yesLabel = IsEnglishUi() ? L"Delete" : L"削除";
    dialog.noLabel = IsEnglishUi() ? L"Cancel" : L"キャンセル";
    dialog.message = IsEnglishUi()
        ? (L"Delete " + std::to_wstring(logFiles.size()) + L" managed log file(s) under:\n\n" +
           WorkspaceLogDirectory().wstring() +
           BuildWorkspaceLogDisplayList(logFiles) +
           L"\n\nOnly app-managed local log files are removed. ZIP archives are kept.")
        : (std::to_wstring(logFiles.size()) + L" 件の管理ログを削除します。\n\n" +
           WorkspaceLogDirectory().wstring() +
           BuildWorkspaceLogDisplayList(logFiles) +
           L"\n\n削除対象はアプリ管理のローカルログのみで、ZIP アーカイブは残します。");
    if (ShowSilentDialog(owner, dialog) != SilentDialogResult::Yes) {
        return;
    }

    SaveOperationGuard guard;
    size_t deleted = 0;
    std::vector<std::wstring> failures;
    failures.reserve(logFiles.size());
    for (const auto& logFile : logFiles) {
        std::wstring err;
        if (DeleteWorkspaceLogFile(logFile, &err)) {
            ++deleted;
            continue;
        }
        std::wstring line = logFile.displayName + L": ";
        line += err.empty()
            ? (IsEnglishUi() ? L"not deleted." : L"削除できませんでした。")
            : err;
        failures.push_back(std::move(line));
    }

    RefreshMainMenuBar(owner);
    if (failures.empty()) {
        std::wstring msg = IsEnglishUi()
            ? (L"Deleted " + std::to_wstring(deleted) + L" log file(s). Active logs will be recreated on the next write.")
            : (std::to_wstring(deleted) + L" 件のログを削除しました。有効なログは次回出力時に再作成されます。");
        ShowMainSoftNotice(owner, msg, SoftNoticeKind::Info);
        return;
    }

    std::wstring msg = IsEnglishUi()
        ? (L"Deleted " + std::to_wstring(deleted) + L" log file(s), but some files could not be removed.")
        : (std::to_wstring(deleted) + L" 件のログを削除しましたが、一部は削除できませんでした。");
    for (const auto& line : failures) {
        msg += L"\n- " + line;
    }
    ShowMainMessageDialog(owner, DebugMenuLabel(), msg, SoftNoticeKind::Warning);
}

static bool AreAllDebugLogsEnabled(const AppDebugLogConfig& cfg) {
    return cfg.previewTrace && cfg.switchTiming && cfg.crash && cfg.startupWatchdog;
}

static void ToggleAllDebugLogs(HWND owner) {
    if (g_workspaceRoot.empty()) {
        ShowMainSoftNotice(owner,
                           IsEnglishUi() ? L"No workspace is open." : L"ワークスペースが開かれていません。",
                           SoftNoticeKind::Warning);
        return;
    }

    const bool enable = !AreAllDebugLogsEnabled(g_config.debugLogs);
    WorkspaceConfig next = g_config;
    next.debugLogs.previewTrace = enable;
    next.debugLogs.switchTiming = enable;
    next.debugLogs.crash = enable;
    next.debugLogs.startupWatchdog = enable;

    const std::filesystem::path configPath = std::filesystem::path(g_workspaceRoot) / L"workspace.json";
    if (!SaveWorkspaceConfigToFile(configPath, next)) {
        ShowMainMessageDialog(owner, DebugMenuLabel(),
                              IsEnglishUi()
                                  ? L"Failed to save the debug log setting."
                                  : L"デバッグログ設定を保存できませんでした。",
                              SoftNoticeKind::Warning);
        return;
    }

    g_config = next;
    RefreshMainMenuBar(owner);
    ShowMainSoftNotice(owner,
                       enable
                           ? (IsEnglishUi()
                                  ? L"Debug logs will be ON after the next restart."
                                  : L"デバッグログは次回起動時に ON になります。")
                           : (IsEnglishUi()
                                  ? L"Debug logs will be OFF after the next restart."
                                  : L"デバッグログは次回起動時に OFF になります。"),
                       SoftNoticeKind::Info);
}

static std::wstring BuildSaveStateStatusText() {
    // Only expose "busy" for the explicit save transaction UI.
    // Internal/background stage writes also use SaveOperationGuard, but showing
    // them here can leave the status text stuck until another manual refresh.
    if (IsSaveTransactionRunning()) {
        return IsEnglishUi() ? L"Save: background" : L"保存: バックグラウンド中";
    }

    std::vector<std::wstring> parts;
    if (g_noteDirty) {
        parts.push_back(IsEnglishUi() ? L"note unsaved" : L"ノート未保存");
    }
    if (g_annotsDirty) {
        parts.push_back(IsEnglishUi() ? L"annotations unsaved" : L"注釈未保存");
    }

    size_t noteStageCount = 0;
    size_t clropStageCount = 0;
    for (const auto& entry : file_output::ListStagedDiffEntries()) {
        if (entry.kind == file_output::StagedDiffKind::Note) {
            ++noteStageCount;
        } else if (entry.kind == file_output::StagedDiffKind::Clrop) {
            ++clropStageCount;
        }
    }

    if (noteStageCount > 0) {
        parts.push_back((IsEnglishUi() ? L"note staged " : L"未統合ノート ") +
                        std::to_wstring(noteStageCount));
    }
    if (clropStageCount > 0) {
        parts.push_back((IsEnglishUi() ? L"annotations staged " : L"未統合注釈 ") +
                        std::to_wstring(clropStageCount));
    }

    if (parts.empty()) {
        return IsEnglishUi() ? L"Save: clean" : L"保存: クリーン";
    }

    std::wstring text = IsEnglishUi() ? L"Save: " : L"保存: ";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) text += IsEnglishUi() ? L" / " : L" / ";
        text += parts[i];
    }
    return text;
}

static std::wstring BuildOfficeConversionProgressStatusText() {
    if (!s_officeConversionProgress.active) return L"";

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG started = s_officeConversionProgress.startedTick ? s_officeConversionProgress.startedTick : now;
    const unsigned long long elapsedSec = static_cast<unsigned long long>((now - started) / 1000);

    const bool finishing = s_officeConversionProgress.finishingProcessTree;
    std::wstring text = finishing
        ? (IsEnglishUi() ? L"Finishing: " : L"終了待ち: ")
        : (IsEnglishUi() ? L"Converting: " : L"変換中: ");
    size_t current = s_officeConversionProgress.current;
    size_t total = s_officeConversionProgress.total;
    if (total > 0) {
        if (current == 0) current = 1;
        text += std::to_wstring(current) + L"/" + std::to_wstring(total);
    } else {
        text += L"-";
    }
    if (!s_officeConversionProgress.fileName.empty()) {
        text += L" ";
        std::wstring fileName = s_officeConversionProgress.fileName;
        constexpr size_t kMaxProgressFileNameChars = 28;
        if (fileName.size() > kMaxProgressFileNameChars) {
            fileName = fileName.substr(0, kMaxProgressFileNameChars - 3) + L"...";
        }
        text += fileName;
    }
    text += L" ";
    text += std::to_wstring(elapsedSec);
    text += IsEnglishUi() ? L"s" : L"秒";
    if (finishing) {
        text += IsEnglishUi()
            ? L" / waiting for LibreOffice exit"
            : L" / LibreOffice終了待ち";
        if (s_officeConversionProgress.activeProcessCount > 0) {
            text += L" (" + std::to_wstring(s_officeConversionProgress.activeProcessCount) +
                    (IsEnglishUi() ? L" proc)" : L" プロセス)");
        }
    }
    return text;
}

static void RefreshOfficeConversionProgressWindowText() {
    if (!s_officeConversionProgress.label) return;
    std::wstring text = BuildOfficeConversionProgressStatusText();
    text += IsEnglishUi()
        ? L"\nThe source file is unchanged. You can safely cancel the conversion."
        : L"\n変換元ファイルは変更しません。安全に中止できます。";
    SetWindowTextW(s_officeConversionProgress.label, text.c_str());
}

void RequestOfficeConversionCancel(bool closeOwnerAfterEnd) {
    if (!s_officeConversionProgress.active) return;
    s_officeConversionProgress.cancelRequested = true;
    s_officeConversionProgress.closeOwnerAfterEnd =
        s_officeConversionProgress.closeOwnerAfterEnd || closeOwnerAfterEnd;
    if (s_officeConversionProgress.cancelButton) {
        EnableWindow(s_officeConversionProgress.cancelButton, FALSE);
        SetWindowTextW(s_officeConversionProgress.cancelButton,
                       IsEnglishUi() ? L"Stopping..." : L"中止処理中...");
    }
    if (s_officeConversionProgress.label) {
        SetWindowTextW(s_officeConversionProgress.label,
                       IsEnglishUi()
                           ? L"Stopping LibreOffice..."
                           : L"LibreOfficeを停止しています...");
    }
}

bool IsOfficeConversionCancelRequested() {
    return s_officeConversionProgress.active && s_officeConversionProgress.cancelRequested;
}

static LRESULT CALLBACK OfficeConversionProgressProc(HWND hWnd, UINT msg,
                                                     WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        s_officeConversionProgress.window = hWnd;
        s_officeConversionProgress.label = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            18, 16, 474, 48, hWnd, nullptr, g_hInst, nullptr);
        s_officeConversionProgress.progress = CreateWindowExW(
            0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
            18, 72, 474, 18, hWnd, nullptr, g_hInst, nullptr);
        s_officeConversionProgress.cancelButton = CreateWindowExW(
            0, L"BUTTON", IsEnglishUi() ? L"Cancel" : L"中止",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            392, 104, 100, 30, hWnd, reinterpret_cast<HMENU>(IDCANCEL), g_hInst, nullptr);
        SetUIFont(s_officeConversionProgress.label);
        SetUIFont(s_officeConversionProgress.cancelButton);
        if (s_officeConversionProgress.progress) {
            SendMessageW(s_officeConversionProgress.progress, PBM_SETMARQUEE, TRUE, 40);
        }
        RefreshOfficeConversionProgressWindowText();
        ApplyThemeToDialog(hWnd);
        SetFocus(s_officeConversionProgress.cancelButton);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush
            ? g_hThemeWindowBrush
            : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return ThemeCtlColorPanel(reinterpret_cast<HWND>(lParam),
                                  reinterpret_cast<HDC>(wParam));
    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL) {
            RequestOfficeConversionCancel();
            return 0;
        }
        break;
    case WM_CLOSE:
        RequestOfficeConversionCancel();
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool ShowOfficeConversionProgressWindow(HWND owner) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = OfficeConversionProgressProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PdfNoteOfficeConversionProgress";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    s_officeConversionProgress.owner = owner ? owner : g_hMainWnd;
    HWND dialogOwner = s_officeConversionProgress.owner;
    if (dialogOwner) {
        s_officeConversionProgress.ownerWasEnabled = IsWindowEnabled(dialogOwner) != FALSE;
        if (s_officeConversionProgress.ownerWasEnabled) EnableWindow(dialogOwner, FALSE);
    }

    constexpr int width = 530;
    constexpr int height = 180;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    RECT ownerRect{};
    if (dialogOwner && GetWindowRect(dialogOwner, &ownerRect)) {
        x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
        y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    }
    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        wc.lpszClassName,
        IsEnglishUi() ? L"Office to PDF conversion (Experimental)" : L"Office PDF変換（試験的）",
        WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE,
        x, y, width, height, dialogOwner, nullptr, g_hInst, nullptr);
    if (!window) {
        if (dialogOwner && s_officeConversionProgress.ownerWasEnabled) {
            EnableWindow(dialogOwner, TRUE);
        }
        s_officeConversionProgress.ownerWasEnabled = false;
        return false;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return true;
}

void UpdateOfficeConversionProgress(HWND owner, size_t current, size_t total,
                                           const std::filesystem::path& source) {
    const ULONGLONG now = GetTickCount64();
    const bool wasActive = s_officeConversionProgress.active;
    s_officeConversionProgress.active = true;
    s_officeConversionProgress.current = current;
    s_officeConversionProgress.total = total;
    s_officeConversionProgress.fileName = source.filename().wstring();
    s_officeConversionProgress.finishingProcessTree = false;
    s_officeConversionProgress.activeProcessCount = 0;
    if (!wasActive || s_officeConversionProgress.startedTick == 0) {
        s_officeConversionProgress.startedTick = now;
    }
    s_officeConversionProgress.lastRefreshTick = now;
    if (!wasActive && !ShowOfficeConversionProgressWindow(owner)) {
        s_officeConversionProgress.cancelRequested = true;
    }
    RefreshOfficeConversionProgressWindowText();
    RefreshStatusDisplay(owner ? owner : g_hMainWnd);
}

void BeginOfficeConversionProgress(HWND owner, size_t total,
                                          const std::filesystem::path& source) {
    s_officeConversionProgress = OfficeConversionProgressState{};
    UpdateOfficeConversionProgress(owner, total > 0 ? 1 : 0, total, source);
}

void PulseOfficeConversionProgress(HWND owner) {
    if (!s_officeConversionProgress.active) return;
    const ULONGLONG now = GetTickCount64();
    if (now - s_officeConversionProgress.lastRefreshTick < 1000) return;
    s_officeConversionProgress.lastRefreshTick = now;
    RefreshOfficeConversionProgressWindowText();
    RefreshStatusDisplay(owner ? owner : g_hMainWnd);
}

void EndOfficeConversionProgress(HWND owner) {
    if (!s_officeConversionProgress.active) return;
    HWND window = s_officeConversionProgress.window;
    HWND dialogOwner = s_officeConversionProgress.owner;
    const bool ownerWasEnabled = s_officeConversionProgress.ownerWasEnabled;
    const bool closeOwnerAfterEnd = s_officeConversionProgress.closeOwnerAfterEnd;
    if (window) DestroyWindow(window);
    if (dialogOwner && ownerWasEnabled) {
        EnableWindow(dialogOwner, TRUE);
        SetActiveWindow(dialogOwner);
    }
    s_officeConversionProgress = OfficeConversionProgressState{};
    RefreshStatusDisplay(owner ? owner : g_hMainWnd);
    if (closeOwnerAfterEnd && dialogOwner) {
        PostMessageW(dialogOwner, WM_CLOSE, 0, 0);
    }
}

void RefreshStatusDisplay(HWND hWnd) {
    const ULONGLONG startTick = preview_trace::TickNow();
    HMENU menu = GetMenu(hWnd);
    if (!menu) menu = g_hMainMenu;
    if (!menu) {
        preview_trace::Append(
            L"RefreshStatusDisplay",
            L"skip=no_menu elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return;
    }
    std::wstring text = BuildStatusDisplayText();
    preview_trace::Append(
        L"RefreshStatusDisplay",
        L"after_build_text text_len=" + std::to_wstring(text.size()) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    if (g_config.ownerDrawUi) {
        // Keep the menu item owner-drawn; only update the text payload used by DrawThemedMenuItem.
        UpdateMenuItemText(menu, ID_STATUS_DISPLAY, text);
    } else {
        ModifyMenuW(menu, ID_STATUS_DISPLAY,
                    MF_BYCOMMAND | MF_STRING | MF_DISABLED | MFT_RIGHTJUSTIFY,
                    ID_STATUS_DISPLAY, text.c_str());
    }
    s_lastStatusDisplayStateSnapshot = StatusDisplayStateSnapshot{std::move(text)};
    preview_trace::Append(
        L"RefreshStatusDisplay",
        L"after_modify_menu elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    DrawMenuBar(hWnd);
    preview_trace::Append(
        L"RefreshStatusDisplay",
        L"end elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

static bool IsSameWindowOrChild(HWND hwnd, HWND root) {
    return hwnd && root && (hwnd == root || IsChild(root, hwnd));
}

static bool IsSaveScrollTarget(HWND hwnd) {
    if (!hwnd) return false;
    const HWND roots[] = {
        g_hPdfView,
        g_hNoteEdit,
        g_hLectureList,
        g_hSessionList,
        g_hPdfList,
        g_hNoteList,
        g_hAnnotList,
        g_hAnnotSummary,
        g_hBottomNote,
        g_hBottomMath,
        g_hBottomRight,
    };
    for (HWND root : roots) {
        if (IsSameWindowOrChild(hwnd, root)) return true;
    }
    return false;
}

static bool IsSavePumpPaintTarget(HWND hwnd, HWND owner) {
    HWND root = owner ? owner : g_hMainWnd;
    return IsSameWindowOrChild(hwnd, root) || IsSaveScrollTarget(hwnd);
}

static HWND SavePumpWheelTarget(const MSG& msg) {
    if (msg.message != WM_MOUSEWHEEL && msg.message != WM_MOUSEHWHEEL) return nullptr;
    POINT screenPt{ GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam) };
    if (screenPt.x == 0 && screenPt.y == 0) {
        DWORD pos = GetMessagePos();
        screenPt.x = GET_X_LPARAM(pos);
        screenPt.y = GET_Y_LPARAM(pos);
    }
    HWND underCursor = WindowFromPoint(screenPt);
    if (IsSaveScrollTarget(underCursor)) return underCursor;
    return IsSaveScrollTarget(msg.hwnd) ? msg.hwnd : nullptr;
}

static bool IsSavePumpAllowedMessage(const MSG& msg, HWND owner) {
    switch (msg.message) {
    case WM_VSCROLL:
    case WM_HSCROLL:
        return IsSaveScrollTarget(msg.hwnd);
    case WM_PAINT:
    case WM_NCPAINT:
    case WM_ERASEBKGND:
        return IsSavePumpPaintTarget(msg.hwnd, owner);
    case kMsgPdfVirtualRenderComplete:
        return msg.hwnd == g_hPdfView;
    default:
        return false;
    }
}

void PumpSaveScrollMessages(HWND owner) {
    if (!IsSaveTransactionRunning()) return;
    MSG msg{};
    int processed = 0;
    while (processed < 32 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (RouteWheelToComboListIfNeeded(msg)) {
            ++processed;
            continue;
        }
        if (HWND wheelTarget = SavePumpWheelTarget(msg)) {
            SendMessageW(wheelTarget, msg.message, msg.wParam, msg.lParam);
            ++processed;
            continue;
        }
        if (IsSavePumpAllowedMessage(msg, owner)) {
            DispatchMessageW(&msg);
        }
        ++processed;
    }
}

static std::wstring NormalizePathKeyForList(const std::wstring& path) {
    if (path.empty()) return L"";
    std::filesystem::path p(path);
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(p, ec);
    std::wstring key = ec ? p.wstring() : canon.wstring();
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    return key;
}

static bool IsOpenItemInList(HWND list, int index) {
    if (!list || index < 0) return false;
    if (list == g_hLectureList) {
        if (index >= static_cast<int>(g_lectures.size())) return false;
        std::wstring selKey = NormalizePathKeyForList(g_lectures[static_cast<size_t>(index)]);
        std::wstring curKey = NormalizePathKeyForList(g_currentLecturePath);
        return !selKey.empty() && selKey == curKey;
    }
    if (list == g_hSessionList) {
        if (index >= static_cast<int>(g_sessions.size())) return false;
        std::wstring selKey = NormalizePathKeyForList(g_sessions[static_cast<size_t>(index)].path);
        std::wstring curKey = NormalizePathKeyForList(g_currentSessionPath);
        return !selKey.empty() && selKey == curKey;
    }
    if (list == g_hPdfList) {
        if (index >= static_cast<int>(g_pdfFiles.size())) return false;
        std::wstring selKey = NormalizePathKeyForList(g_pdfFiles[static_cast<size_t>(index)].path);
        std::wstring curKey = NormalizePathKeyForList(CurrentLogicalPdfPath());
        return !selKey.empty() && selKey == curKey;
    }
    if (list == g_hNoteList) {
        if (index >= static_cast<int>(g_noteFiles.size())) return false;
        std::wstring selKey = NormalizePathKeyForList(g_noteFiles[static_cast<size_t>(index)].path);
        std::wstring curKey = NormalizePathKeyForList(g_currentNotePath);
        return !selKey.empty() && selKey == curKey;
    }
    return false;
}

static bool DrawLeftPaneListItem(const DRAWITEMSTRUCT* dis) {
    if (!dis) return false;
    if (dis->CtlType != ODT_LISTBOX || !dis->hwndItem) return false;
    HWND list = dis->hwndItem;
    if (list != g_hLectureList && list != g_hSessionList &&
        list != g_hPdfList && list != g_hNoteList) {
        return false;
    }

    const int idx = static_cast<int>(dis->itemID);
    RECT rc = dis->rcItem;

    COLORREF baseBg = g_theme.panelBg;
    COLORREF baseText = g_theme.panelText;
    const bool isSelected = (dis->itemState & ODS_SELECTED) != 0;
    const bool isOpen = IsOpenItemInList(list, idx);

    COLORREF openBg = BlendColor(g_theme.selectionBg, g_theme.panelBg, 0.50);

    // Keep the explicit selection colors exact.  An open but unselected item
    // remains a subdued version of that same theme-defined selection color.
    COLORREF bg = baseBg;
    COLORREF text = baseText;
    if (isSelected) {
        bg = g_theme.selectionBg;
        text = g_theme.selectionText;
    } else if (isOpen) {
        bg = openBg;
    }

    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    if (idx >= 0) {
        wchar_t textBuf[512]{};
        LRESULT len = SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(idx),
                                   reinterpret_cast<LPARAM>(textBuf));
        if (len != LB_ERR) {
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, text);
            RECT textRc = rc;
            textRc.left += 6;
            DrawTextW(dis->hDC, textBuf, -1, &textRc,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    if ((dis->itemState & ODS_FOCUS) != 0) {
        DrawFocusRect(dis->hDC, &rc);
    }
    return true;
}



// ---------------------------------------------------------------------
// Main window procedure
// ---------------------------------------------------------------------
#include "ui/core/main_window_proc.cppinc"



#include "app/bootstrap.cppinc"
bool HandleFixedAnnotToolNavigationShortcutInLoop(HWND owner, const MSG& msg) {
    if ((msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) ||
        (msg.lParam & 0x40000000) != 0) {
        return false;
    }
    if (!MainLoopShortcutMessageBelongsToOwner(owner, msg)) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!ctrl || !alt || shift) return false;
    const bool previous = msg.wParam == VK_LEFT || msg.wParam == VK_UP;
    const bool next = msg.wParam == VK_RIGHT || msg.wParam == VK_DOWN;
    if (!previous && !next) return false;
    if (ShouldSuppressAnnotToolShortcutForFocus(msg)) return false;

    if (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT) {
        const auto& families = AnnotToolFamilyUiOrder();
        if (families.empty()) return true;
        AnnotToolFamily currentFamily = AnnotToolFamilyForMode(g_toolMode);
        auto it = std::find(families.begin(), families.end(), currentFamily);
        size_t index = it == families.end() ? 0 : static_cast<size_t>(std::distance(families.begin(), it));
        index = previous
            ? (index + families.size() - 1) % families.size()
            : (index + 1) % families.size();
        SelectAnnotToolFamily(owner, families[index], false, false);
        return true;
    }

    AnnotToolFamily family = AnnotToolFamilyForMode(g_toolMode);
    if (family == AnnotToolFamily::Shape) {
        CycleShapeToolFamilyOption(owner, previous);
        return true;
    }
    std::vector<ToolMode> details = EnabledTopOptionModesForFamily(family);
    if (details.size() <= 1) return true;
    auto it = std::find(details.begin(), details.end(), g_toolMode);
    size_t index = it == details.end() ? 0 : static_cast<size_t>(std::distance(details.begin(), it));
    index = previous
        ? (index + details.size() - 1) % details.size()
        : (index + 1) % details.size();
    SelectAnnotToolMode(owner, details[index], false, false);
    return true;
}

bool HandleAnnotToolShortcutInLoop(HWND owner, const MSG& msg) {
    if ((msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) ||
        (msg.lParam & 0x40000000) != 0) {
        return false;
    }
    if (!MainLoopShortcutMessageBelongsToOwner(owner, msg)) return false;
    AnnotToolShortcutBinding binding;
    if (!ResolveAnnotToolShortcut(msg, &binding)) return false;
    if (ShouldSuppressAnnotToolShortcutForFocus(msg)) return false;
    if (binding.targetKind == AnnotToolShortcutTargetKind::Category) {
        SelectAnnotToolFamily(owner, binding.family, false, false);
    } else {
        SelectAnnotToolMode(owner, binding.mode, false, false);
    }
    return true;
}

bool HandleAnnotColorCycleShortcutInLoop(HWND owner, const MSG& msg) {
    if (msg.message != WM_KEYDOWN || (msg.lParam & 0x40000000) != 0) return false;
    if (!MainLoopShortcutMessageBelongsToOwner(owner, msg)) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!ctrl || alt || shift) return false;
    const bool next = (msg.wParam == VK_DOWN);
    const bool prev = (msg.wParam == VK_UP);
    if (!next && !prev) return false;
    if (ShouldSuppressAnnotToolShortcutForFocus(msg)) return false;
    return ApplyActivePaletteColorStep(g_hPdfView ? g_hPdfView : owner, next ? 1 : -1, true);
}

bool HandleMainPdfZoomShortcutInLoop(HWND owner, const MSG& msg) {
    if (msg.message != WM_KEYDOWN) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) return false;
    if (!g_hPdfView || !IsWindow(g_hPdfView) || g_pdf.kind == DocKind::None) return false;
    if (g_pdf.editingText || IsNoteCmdlineActive() || IsNoteImeComposing()) return false;

    HWND targetRoot = msg.hwnd ? GetAncestor(msg.hwnd, GA_ROOT) : nullptr;
    if (owner && targetRoot && targetRoot != owner) return false;
    HWND focus = GetFocus();
    if (IsExplicitImeTargetWindow(focus) || IsExplicitImeTargetWindow(msg.hwnd)) return false;
    if (focus && g_hNoteEdit && (focus == g_hNoteEdit || IsChild(g_hNoteEdit, focus))) return false;

    const bool zoomIn = (msg.wParam == VK_OEM_PLUS) || (msg.wParam == VK_ADD);
    const bool zoomOut = (msg.wParam == VK_OEM_MINUS) || (msg.wParam == VK_SUBTRACT);
    const bool zoomReset = msg.wParam == '0';
    if (!zoomIn && !zoomOut && !zoomReset) return false;

    RECT rc{};
    GetClientRect(g_hPdfView, &rc);
    POINT focusPt{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
    if (zoomReset) {
        SetPdfScale(g_hPdfView, 1.0, focusPt);
    } else {
        const double factor = zoomIn ? 1.15 : (1.0 / 1.15);
        const double base = (std::isfinite(g_pdf.scale) && g_pdf.scale > 0.0) ? g_pdf.scale : 1.0;
        SetPdfScale(g_hPdfView, base * factor, focusPt);
    }
    return true;
}

