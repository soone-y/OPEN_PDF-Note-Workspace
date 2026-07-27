#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <optional>
#include <filesystem>
#include "core/ui_notify.h" // For SoftNoticeKind
void ShowMainMessageDialog(HWND owner, const std::wstring& title, const std::wstring& message, SoftNoticeKind kind);
bool ConfirmMainYesNo(HWND owner, const std::wstring& title, const std::wstring& message, SoftNoticeKind kind, SilentDialogResult defaultResult, SilentDialogResult escapeResult);
void AppendMainOperationExceptionLog(const char* label, const char* what);
void ReportMainOperationException(HWND hWnd, const wchar_t* titleJa);
bool CanRequestManagedAbnormalExitFromDialog(HWND owner, SoftNoticeKind kind);
void RequestManagedAbnormalExitFromDialog(HWND owner, const std::wstring& title, const std::wstring& message);
void ShowManagedAbnormalExitReportIfPending();
int OverrideExitCodeForManagedAbnormalExit(int defaultCode);
void LoadNoteFile(HWND hWnd, const std::wstring& notePath);
void SyncBottomPaneAfterNoteLoad(HWND hWnd);
std::filesystem::path CanonicalOrSelf(const std::filesystem::path& p);
bool IsTempExternalLecturePath(const std::wstring& lecturePath);
std::optional<std::wstring> PickFileUnder(HWND owner, const std::filesystem::path& root, const std::wstring& title);
void ApplyOwnerDrawUi(HWND hWnd);
void LayoutChildren(HWND hWnd);
#include "core/app_core.h" // For ToolMode
void ApplyActiveColorForMode(HWND hWnd, ToolMode mode);

#include <unordered_set>
extern std::unordered_set<std::wstring> s_searchTempPdfKeys;
extern std::unordered_set<std::wstring> s_searchTempNoteKeys;
struct TempExternalLecture {
    std::wstring path; // canonical path
};

extern bool s_ignoreLectureSelChange;
extern std::vector<TempExternalLecture> g_tempExternalLectures;
std::filesystem::path DialogWorkspaceInitialFolder();
std::filesystem::path DialogDownloadsInitialFolder();
std::filesystem::path DialogDocumentsInitialFolder();
bool PersistTempExternalLecturesToSetup();
std::optional<std::wstring> PromptExistingLocalPath(HWND owner, const std::filesystem::path& initialDir, const std::wstring& title, bool requireDirectory);
std::optional<std::wstring> PromptExistingLocalPathAppFirst(HWND owner, const std::filesystem::path& initialDir, const std::wstring& title, bool requireDirectory);


enum class SessionNumberingMode { CountPlusOne, MaxNumberPlusOne };
void LoadLectures();
bool UpdateLectureOpenTime(const std::wstring& lecturePath);
SessionNumberingMode ParseSessionNumberingMode(const std::wstring& mode);
int NextSessionNumberForSuggestions(const std::vector<SessionEntry>& sessions, SessionNumberingMode mode);
// Workspace items access
const std::vector<std::wstring>& GetLectures();
const std::vector<struct SessionEntry>& GetSessions();
const std::vector<struct FileEntry>& GetPdfFiles();
const std::vector<struct FileEntry>& GetNoteFiles();

std::wstring BestMatchByStem(const std::filesystem::path& anchor,
                             const std::vector<struct FileEntry>& candidates);
std::wstring ToLowerAscii(std::wstring s);
HWND MainDialogOwner(HWND hWnd);
void ReloadSessionsAndSelect(const std::wstring& lecturePath, const std::wstring& selectSession, bool editNew);
std::vector<std::wstring> PromptExistingLocalFolders(HWND owner, const std::filesystem::path& initialDir, const std::wstring& title, bool allowMultiple);
int CurrentSessionIndex();
bool RefreshCurrentSessionFiles();
bool OpenPdfIfDifferent(HWND hWnd, const std::wstring& path);
bool IsPathUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root);
struct OfficeConversionProgressState {
    bool active = false;
    bool cancelRequested = false;
    bool closeOwnerAfterEnd = false;
    size_t current = 0;
    size_t total = 0;
    std::wstring fileName;
    ULONGLONG startedTick = 0;
    ULONGLONG lastRefreshTick = 0;
    bool finishingProcessTree = false;
    DWORD activeProcessCount = 0;
    HWND owner = nullptr;
    HWND window = nullptr;
    HWND label = nullptr;
    HWND progress = nullptr;
    HWND cancelButton = nullptr;
    bool ownerWasEnabled = false;
};

extern OfficeConversionProgressState s_officeConversionProgress;
void PulseOfficeConversionProgress(HWND owner);
bool IsOfficeConversionCancelRequested();
void RequestOfficeConversionCancel(bool closeOwnerAfterEnd = false);
void EndOfficeConversionProgress(HWND owner);
std::wstring NowTimestampString();

void CancelPendingLinkMode(HWND owner);
void RememberCurrentSessionFiles();

void AppendUiAutomationTrace(const std::wstring& text);
bool RunUiAutomationStartupLastOpenScenario(HWND owner, std::wstring* outError);
bool PrepareUiAutomationStagedExitScenario(HWND owner, std::wstring* outError);

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
HACCEL BuildAccelerators();

int RunMainMessageLoop(HWND hWnd);
void AcknowledgeStartupAbort();

constexpr wchar_t kToolbarHostClass[] = L"PdfWorkspaceToolbarHost";
constexpr UINT kMsgStartupWatchdogAbort = WM_APP + 200;
constexpr UINT kMsgSingleInstanceShutdownRequest = WM_APP + 201;

extern HANDLE g_hSingleInstanceReadyEvent;

bool IsNoteCmdlineActive();
bool IsNoteCmdlineImeComposing();
void CancelNoteCmdline();
bool IsNoteImeComposing();
bool ClearNoteNormalVisualMode();
bool ClearNoteNormalPendingState();
enum class PaneNavContext {
    LeftPaneList,
    PdfPane,
    NotePane,
};
bool HandlePaneDirectionalNavigation(HWND owner, PaneNavContext context, HWND source, WPARAM vkey);
void EnterNoteNormalMode(HWND hWnd);
bool HandleMainPdfZoomShortcutInLoop(HWND hWnd, const MSG& msg);
bool HandleAnnotColorCycleShortcutInLoop(HWND hWnd, const MSG& msg);
bool HandleAnnotToolShortcutInLoop(HWND hWnd, const MSG& msg);

void BeginOfficeConversionProgress(HWND owner, size_t total, const std::filesystem::path& source);

bool IsUiAutomationEnabled();
void UpdateOfficeConversionProgress(HWND owner, size_t current, size_t total, const std::filesystem::path& source);
std::vector<std::wstring> PickOfficeFilesUnder(HWND owner, const std::filesystem::path& root, const std::wstring& title);
std::vector<std::wstring> PickFilesUnder(HWND owner, const std::filesystem::path& root, const std::wstring& title);

bool CopyFileForImportSafely(const std::filesystem::path& src, const std::filesystem::path& dest, std::wstring* outErr);
