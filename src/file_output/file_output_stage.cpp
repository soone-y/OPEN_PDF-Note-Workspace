#include "file_output/file_output.h"

#include "bridge/view_bridge.h"
#include "clrop/bridge.h"
#include "clrop/hash.h"
#include "core/annot_commands.h"
#include "core/atomic_write.h"
#include "core/preview_trace.h"
#include "core/ui_notify.h"
#include "note/note_identity.h"
#include "note/note_identity_store.h"
#include "note/note_persistence.h"


#include <richedit.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>


namespace {

constexpr UINT kAutoStageRetryDelayMs = 2000;
constexpr UINT kAutoStageDeferredWriteDelayMs = 700;
constexpr UINT kAutoStageBlockedPollDelayMs = 250;
constexpr DWORD kAnnotJournalReadRetryDelayMs = 40;
constexpr int kAnnotJournalReadMaxAttempts = 3;
constexpr UINT kNoteStageIdleBaseDelayMs = 20 * 1000;
constexpr UINT kNoteStageIdleMinDelayMs = 6 * 1000;
constexpr UINT kNoteStageIdleStepReduceMs = 2 * 1000;
constexpr int kNoteStageIdleCharsPerStep = 10;
constexpr size_t kMaxBackupGenerationsPerFile = 3;

ULONGLONG g_autoStageDirtySinceTick = 0;
ULONGLONG g_autoStageLastEditTick = 0;
ULONGLONG g_autoStagePauseTick = 0;
ULONGLONG g_noteStageLastEditTick = 0;
int g_noteStagePersistedCharCount = 0;
bool g_noteStagePersistedCharCountKnown = false;

struct PreparedNoteStageSnapshot {
  std::wstring targetPath;
  std::string bytes;
  note::SnapshotIdentity identity{};
  uint64_t revision = 0;
};

struct PreparedAnnotStageSnapshot {
  std::wstring targetPath;
  std::vector<Annotation> annotations;
  uint64_t revision = 0;
};

struct NoteJournalSegment {
  std::filesystem::path path;
  std::wstring stageHash;
  uint64_t revision = 0;
};

struct NoteEditJournalSegment {
  std::filesystem::path path;
  std::wstring stageHash;
  uint64_t revision = 0;
};

struct NoteStageState {
  uint64_t appliedJournalRevision = 0;
  uint64_t appliedEditJournalRevision = 0;
  uint64_t blankTailCount = 0;
  std::string textTailBytes;
};

struct NoteByteEdit {
  size_t start = 0;
  std::string deletedBytes;
  std::string insertedBytes;
};

struct ResolvedNoteStageData {
  std::string snapshotBytes;
  std::string committedBytes;
  std::string resolvedBytes;
  NoteStageState state;
};

std::optional<NoteByteEdit> ParseNoteByteEdit(std::string_view bytes,
                                              std::wstring *outErr = nullptr);
bool ApplyNoteByteEdit(std::string *bytes, const NoteByteEdit &edit,
                       std::wstring *outErr = nullptr);
std::wstring TargetHash(const std::wstring &targetPath);
bool EnsureDir(const std::filesystem::path &dir);
void RefreshDirtyUi(HWND owner);

std::optional<PreparedAnnotStageSnapshot> g_preparedAnnotStageSnapshot;
uint64_t g_preparedAutoStageRevision = 0;
HWND g_preparedAutoStageOwner = nullptr;
int g_batchSaveUiDepth = 0;
bool g_batchSaveUiPending = false;
HWND g_batchSaveUiOwner = nullptr;

bool IsAutoStageTimingBlocked() {
  return IsNoteTyping() || IsNoteImeComposing() || g_pdf.editingText ||
         g_pdf.imeComposing;
}

bool UsesAutoStageTimerMode() {
  return NormalizeAutoStageSaveSeconds(g_config.autoSaveSeconds) > 0;
}

bool HasPendingAutoStageWork() { return g_annotsDirty; }

int CurrentNoteEditorCharCount() {
  if (!g_hNoteEdit)
    return 0;
  return std::max(0, GetWindowTextLengthW(g_hNoteEdit));
}

std::optional<file_output::StagedNoteRestoreViewState>
CaptureCurrentNoteRestoreViewState(const std::wstring &notePath,
                                   uint64_t contentRevision) {
  if (!g_hNoteEdit || !IsWindow(g_hNoteEdit) || notePath.empty() ||
      g_currentNotePath.empty()) {
    return std::nullopt;
  }
  if (NormalizePathKey(std::filesystem::path(notePath)) !=
      NormalizePathKey(std::filesystem::path(g_currentNotePath))) {
    return std::nullopt;
  }
  const int textLen = std::max(0, GetWindowTextLengthW(g_hNoteEdit));
  DWORD selStart = 0;
  DWORD selEnd = 0;
  SendMessageW(g_hNoteEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart),
               reinterpret_cast<LPARAM>(&selEnd));
  POINT scroll{};
  SendMessageW(g_hNoteEdit, EM_GETSCROLLPOS, 0,
               reinterpret_cast<LPARAM>(&scroll));
  file_output::StagedNoteRestoreViewState state{};
  state.contentRevision = contentRevision;
  state.selectionStart = static_cast<uint64_t>(
      std::min<DWORD>(selStart, static_cast<DWORD>(textLen)));
  state.selectionEnd = static_cast<uint64_t>(
      std::min<DWORD>(selEnd, static_cast<DWORD>(textLen)));
  state.scrollX = scroll.x;
  state.scrollY = scroll.y;
  state.firstVisibleLine =
      static_cast<int>(SendMessageW(g_hNoteEdit, EM_GETFIRSTVISIBLELINE, 0, 0));
  return state;
}

void RecordPersistedNoteEditorLength() {
  if (g_currentNotePath.empty() || !g_hNoteEdit) {
    g_noteStagePersistedCharCount = 0;
    g_noteStagePersistedCharCountKnown = false;
    return;
  }
  g_noteStagePersistedCharCount = CurrentNoteEditorCharCount();
  g_noteStagePersistedCharCountKnown = true;
}

void ResetNoteStageEditTracking() { g_noteStageLastEditTick = 0; }

UINT ComputeNoteStageDelayMs() {
  if (!g_noteDirty ||
      (g_currentNotePath.empty() && g_currentSessionPath.empty()))
    return 0;
  const ULONGLONG now = GetTickCount64();
  if (g_noteStageLastEditTick == 0)
    g_noteStageLastEditTick = now;
  if (IsNoteImeComposing())
    return kAutoStageBlockedPollDelayMs;
  if (IsNoteTyping())
    return std::min<UINT>(1000u, kAutoStageBlockedPollDelayMs * 2u);

  int growth = 0;
  if (g_noteStagePersistedCharCountKnown) {
    growth = std::max(0, CurrentNoteEditorCharCount() -
                             g_noteStagePersistedCharCount);
  }
  const int reduceSteps = std::max(0, growth / kNoteStageIdleCharsPerStep);
  const int reducedDelay =
      static_cast<int>(kNoteStageIdleBaseDelayMs) -
      reduceSteps * static_cast<int>(kNoteStageIdleStepReduceMs);
  const UINT delayMs = static_cast<UINT>(
      std::max(static_cast<int>(kNoteStageIdleMinDelayMs), reducedDelay));
  const ULONGLONG deadline =
      g_noteStageLastEditTick + static_cast<ULONGLONG>(delayMs);
  if (deadline <= now)
    return 1;
  const ULONGLONG remaining = deadline - now;
  return static_cast<UINT>(
      std::min<ULONGLONG>(remaining, static_cast<ULONGLONG>(UINT_MAX)));
}

void ResetAutoStageEditTracking() {
  g_autoStageDirtySinceTick = 0;
  g_autoStageLastEditTick = 0;
  g_autoStagePauseTick = 0;
}

void ClearPreparedAutoStageSnapshots() {
  g_preparedAnnotStageSnapshot.reset();
  g_preparedAutoStageRevision = 0;
  g_preparedAutoStageOwner = nullptr;
}

struct ScopedBatchSaveUiRefresh {
  explicit ScopedBatchSaveUiRefresh(HWND owner) {
    ++g_batchSaveUiDepth;
    if (owner)
      g_batchSaveUiOwner = owner;
  }
  ~ScopedBatchSaveUiRefresh() {
    if (g_batchSaveUiDepth <= 0)
      return;
    --g_batchSaveUiDepth;
    if (g_batchSaveUiDepth != 0 || !g_batchSaveUiPending)
      return;
    HWND owner = g_batchSaveUiOwner ? g_batchSaveUiOwner : g_hMainWnd;
    g_batchSaveUiPending = false;
    g_batchSaveUiOwner = nullptr;
    if (owner)
      RefreshMainWindowUiState(owner);
  }
};

void ResumeAutoStageEditTrackingIfPaused() {
  if (g_autoStagePauseTick == 0)
    return;
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG pausedMs =
      (now >= g_autoStagePauseTick) ? (now - g_autoStagePauseTick) : 0;
  if (pausedMs > 0) {
    if (g_autoStageDirtySinceTick != 0)
      g_autoStageDirtySinceTick += pausedMs;
    if (g_autoStageLastEditTick != 0)
      g_autoStageLastEditTick += pausedMs;
  }
  g_autoStagePauseTick = 0;
}

void NoteAutoStageEditActivity() {
  const ULONGLONG now = GetTickCount64();
  ResumeAutoStageEditTrackingIfPaused();
  if (g_autoStageDirtySinceTick == 0 || !HasPendingAutoStageWork()) {
    g_autoStageDirtySinceTick = now;
  }
  g_autoStageLastEditTick = now;
}

UINT ComputeAutoStageDelayMs() {
  if (!HasPendingAutoStageWork() || !UsesAutoStageTimerMode())
    return 0;

  if (IsAutoStageTimingBlocked()) {
    if (g_autoStagePauseTick == 0) {
      g_autoStagePauseTick = GetTickCount64();
    }
    return 250;
  }

  ResumeAutoStageEditTrackingIfPaused();

  const ULONGLONG now = GetTickCount64();
  if (g_autoStageDirtySinceTick == 0)
    g_autoStageDirtySinceTick = now;
  if (g_autoStageLastEditTick == 0)
    g_autoStageLastEditTick = now;

  const int autoSaveSeconds =
      NormalizeAutoStageSaveSeconds(g_config.autoSaveSeconds);
  const ULONGLONG deadline = g_autoStageLastEditTick +
                             static_cast<ULONGLONG>(autoSaveSeconds) * 1000ull;

  if (deadline <= now)
    return 1;
  const ULONGLONG remaining = deadline - now;
  return static_cast<UINT>(
      std::min<ULONGLONG>(remaining, static_cast<ULONGLONG>(UINT_MAX)));
}

HWND NoticeOwner(HWND owner) { return owner ? owner : g_hMainWnd; }

void ShowStageSoftNotice(HWND owner, const std::wstring &text,
                         SoftNoticeKind kind = SoftNoticeKind::Info) {
  ShowSoftNotice(NoticeOwner(owner), text, kind);
}

void ShowStageMessageDialog(HWND owner, const std::wstring &title,
                            const std::wstring &message,
                            SoftNoticeKind kind = SoftNoticeKind::Error) {
  ShowSilentMessageDialog(NoticeOwner(owner), title, message, kind);
}

void TraceSaveFailure(const wchar_t *area, const std::wstring &step,
                      const std::wstring &target = {},
                      const std::filesystem::path &stagePath = {},
                      const std::filesystem::path &destPath = {},
                      const std::wstring &err = {}) {
  std::wstring line = L"fail step=" + step;
  if (!target.empty())
    line += L" target=" + target;
  if (!stagePath.empty())
    line += L" stage=" + stagePath.wstring();
  if (!destPath.empty())
    line += L" dest=" + destPath.wstring();
  if (!err.empty())
    line += L" err=" + err;
  line += L" noteDirty=" + preview_trace::Bool(g_noteDirty);
  line += L" noteNeedsIntegrate=" + preview_trace::Bool(g_noteNeedsIntegrate);
  line += L" annotsDirty=" + preview_trace::Bool(g_annotsDirty);
  line +=
      L" annotsNeedsIntegrate=" + preview_trace::Bool(g_annotsNeedsIntegrate);
  preview_trace::Append(area, line);
}

void TraceAnnotStageTiming(const std::wstring &step, ULONGLONG stepStartTick,
                           ULONGLONG saveStartTick, const std::wstring &pdfPath,
                           size_t annotCount = static_cast<size_t>(-1)) {
  std::wstring line = L"step=" + step;
  line += L" elapsed_ms=" + preview_trace::ElapsedMs(stepStartTick);
  line += L" total_elapsed_ms=" + preview_trace::ElapsedMs(saveStartTick);
  if (!pdfPath.empty())
    line += L" pdfPath=" + pdfPath;
  if (annotCount != static_cast<size_t>(-1)) {
    line += L" annots=" +
            std::to_wstring(static_cast<unsigned long long>(annotCount));
  }
  line +=
      L" revision=" +
      std::to_wstring(static_cast<unsigned long long>(CurrentEditRevision()));
  preview_trace::Append(L"StageSaveAnnotTiming", line);
}

void RefreshSaveTransactionUi(HWND owner) {
  HWND statusOwner = owner ? owner : g_hMainWnd;
  if (!statusOwner)
    return;
  UpdateToolbarUI(statusOwner);
  RefreshStatusDisplay(statusOwner);
  RedrawWindow(statusOwner, nullptr, nullptr,
               RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME | RDW_NOCHILDREN);
  UpdateWindow(statusOwner);
  GdiFlush();
}

struct ScopedSaveTransactionEnd {
  explicit ScopedSaveTransactionEnd(HWND owner) : owner(owner) {}
  ~ScopedSaveTransactionEnd() {
    EndSaveTransaction();
    RefreshSaveTransactionUi(owner);
  }

  HWND owner = nullptr;
};

std::wstring NormalizePathKey(const std::filesystem::path &path) {
  if (path.empty())
    return {};
  std::error_code ec;
  std::filesystem::path canon = std::filesystem::weakly_canonical(path, ec);
  std::wstring out = ec ? path.wstring() : canon.wstring();
  std::replace(out.begin(), out.end(), L'/', L'\\');
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return out;
}

bool IsSamePath(const std::filesystem::path &lhs,
                const std::filesystem::path &rhs) {
  if (lhs.empty() || rhs.empty())
    return false;
  return NormalizePathKey(lhs) == NormalizePathKey(rhs);
}

bool IsPathUnderRoot(const std::filesystem::path &path,
                     const std::filesystem::path &root) {
  const std::wstring pathKey = NormalizePathKey(path);
  const std::wstring rootKey = NormalizePathKey(root);
  if (pathKey.empty() || rootKey.empty())
    return false;
  if (pathKey == rootKey)
    return true;
  if (pathKey.size() <= rootKey.size())
    return false;
  if (pathKey.compare(0, rootKey.size(), rootKey) != 0)
    return false;
  const wchar_t next = pathKey[rootKey.size()];
  return next == L'\\' || next == L'/';
}

bool IsUnsupportedPath(const std::filesystem::path &path) {
  const std::wstring w = path.wstring();
  return w.rfind(L"\\\\", 0) == 0;
}

std::wstring SanitizeNoteFileNamePart(std::wstring name) {
  for (wchar_t &ch : name) {
    switch (ch) {
    case L'\\':
    case L'/':
    case L':':
    case L'*':
    case L'?':
    case L'"':
    case L'<':
    case L'>':
    case L'|':
      ch = L'_';
      break;
    default:
      if (ch < 0x20)
        ch = L'_';
      break;
    }
  }
  while (!name.empty() && iswspace(name.front()))
    name.erase(name.begin());
  while (!name.empty() && (iswspace(name.back()) || name.back() == L'.'))
    name.pop_back();
  if (name.empty() || name == L"." || name == L"..")
    return L"note";
  return name;
}

bool NoteCandidateHasStageConflict(const std::filesystem::path &candidate) {
  const auto stageDir = g_workspaceRoot.empty()
                            ? std::filesystem::path{}
                            : (WorkspaceCachePath(g_workspaceRoot, g_config) /
                               L"__stage__" / L"note");
  if (stageDir.empty())
    return false;
  const std::wstring prefix =
      L"note_" + TargetHash(candidate.wstring()) + L"_r";
  std::error_code ec;
  if (!std::filesystem::exists(stageDir, ec) || ec)
    return false;
  for (const auto &entry : std::filesystem::directory_iterator(stageDir, ec)) {
    if (ec)
      break;
    std::error_code stEc;
    if (!entry.is_regular_file(stEc) || stEc)
      continue;
    const std::wstring name = entry.path().filename().wstring();
    if (name.rfind(prefix, 0) == 0 &&
        name.find(L".stage") != std::wstring::npos) {
      return true;
    }
  }
  return false;
}

std::wstring DefaultDeferredNoteBaseName() {
  const std::wstring pdfPath = CurrentLogicalPdfPath();
  if (!pdfPath.empty() && !g_currentSessionPath.empty() &&
      IsPathUnderRoot(std::filesystem::path(pdfPath),
                      std::filesystem::path(g_currentSessionPath))) {
    std::wstring base = std::filesystem::path(pdfPath).stem().wstring();
    if (!base.empty())
      return SanitizeNoteFileNamePart(base);
  }

  std::wstring lectureName = L"Lecture";
  if (!g_currentLecturePath.empty()) {
    lectureName =
        std::filesystem::path(g_currentLecturePath).filename().wstring();
  }

  std::wstring sessionName = L"Session";
  if (!g_currentSessionPath.empty()) {
    sessionName =
        std::filesystem::path(g_currentSessionPath).filename().wstring();
  }

  return SanitizeNoteFileNamePart(lectureName + L"_" + sessionName +
                                  L"_ノート");
}

std::filesystem::path
ResolveDeferredCurrentNotePath(std::wstring *outErr = nullptr) {
  if (outErr)
    outErr->clear();
  if (!g_currentNotePath.empty())
    return std::filesystem::path(g_currentNotePath);
  if (g_currentSessionPath.empty()) {
    if (outErr) {
      *outErr = IsEnglishUi()
                    ? L"Open a session before saving notes."
                    : (g_config.studentMode
                           ? L"ノート保存には回次を開いてください。"
                           : L"ノート保存には下位項目を開いてください。");
    }
    return {};
  }

  const std::filesystem::path sessionRoot(g_currentSessionPath);
  const std::filesystem::path noteDir = sessionRoot / L"note";
  if (!EnsureDir(noteDir)) {
    if (outErr)
      *outErr = L"ノートフォルダを作成できません。";
    return {};
  }

  const std::wstring baseName = DefaultDeferredNoteBaseName();
  const std::wstring pdfPath = CurrentLogicalPdfPath();
  const bool usePdfName =
      !pdfPath.empty() &&
      IsPathUnderRoot(std::filesystem::path(pdfPath), sessionRoot);
  auto makeName = [&](int idx) {
    if (!usePdfName) {
      if (idx == 0)
        return baseName + L".md";
      return baseName + L"(" + std::to_wstring(idx) + L").md";
    }
    if (idx <= 1)
      return baseName + L".md";
    return baseName + L"_Page" + std::to_wstring(idx) + L".md";
  };

  std::error_code ec;
  const int startIdx = usePdfName ? 1 : 0;
  for (int i = startIdx; i < startIdx + 1000; ++i) {
    const std::filesystem::path candidate = noteDir / makeName(i);
    const bool exists = std::filesystem::exists(candidate, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!exists && !NoteCandidateHasStageConflict(candidate)) {
      g_currentNotePath = candidate.wstring();
      SyncLeftPaneSelection();
      RefreshDirtyUi(g_hMainWnd);
      return candidate;
    }
  }
  if (outErr)
    *outErr = L"ノート保存先ファイル名を決定できません。";
  return {};
}

bool EnsureCurrentNotePathForStageImpl(HWND owner) {
  std::wstring err;
  const auto path = ResolveDeferredCurrentNotePath(&err);
  if (!path.empty())
    return true;
  if (err.empty()) {
    err = IsEnglishUi() ? L"Could not decide the note save path."
                        : L"ノート保存先を決定できません。";
  }
  ShowStageSoftNotice(owner, err, SoftNoticeKind::Warning);
  return false;
}

void AddIntegratedCurrentNoteToListIfNeeded(HWND owner,
                                            const std::wstring &notePath) {
  if (notePath.empty() ||
      NormalizePathKey(notePath) != NormalizePathKey(g_currentNotePath)) {
    return;
  }
  const std::wstring noteKey = NormalizePathKey(notePath);
  for (const auto &entry : g_noteFiles) {
    if (NormalizePathKey(entry.path) == noteKey) {
      SyncLeftPaneSelection();
      return;
    }
  }

  g_noteFiles.push_back({notePath, false});
  std::sort(g_noteFiles.begin(), g_noteFiles.end(),
            [](const FileEntry &lhs, const FileEntry &rhs) {
              return std::filesystem::path(lhs.path).filename().wstring() <
                     std::filesystem::path(rhs.path).filename().wstring();
            });

  if (g_hNoteList) {
    SendMessageW(g_hNoteList, LB_RESETCONTENT, 0, 0);
    for (const auto &entry : g_noteFiles) {
      const std::wstring label =
          std::filesystem::path(entry.path).filename().wstring();
      SendMessageW(g_hNoteList, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(label.c_str()));
    }
    SetListWide(g_hNoteList);
  }
  SyncLeftPaneSelection();
  RefreshMainWindowUiState(owner ? owner : g_hMainWnd);
}

std::filesystem::path WorkspaceTmpRoot() {
  if (g_workspaceRoot.empty())
    return {};
  return WorkspaceCachePath(g_workspaceRoot, g_config);
}

std::filesystem::path WorkspaceEscapeRoot() {
  if (g_workspaceRoot.empty())
    return {};
  return std::filesystem::path(g_workspaceRoot) / L"__resource__" /
         L"__escape__";
}

std::filesystem::path StageRoot() {
  auto root = WorkspaceTmpRoot();
  if (root.empty())
    return {};
  return root / L"__stage__";
}

std::filesystem::path StageDir(file_output::StagedDiffKind kind) {
  auto root = StageRoot();
  if (root.empty())
    return {};
  return root /
         (kind == file_output::StagedDiffKind::Note ? L"note" : L"clrop");
}

std::filesystem::path BackupDir(file_output::StagedDiffKind kind) {
  auto root = WorkspaceEscapeRoot();
  if (root.empty())
    return {};
  return root / L"backup" /
         (kind == file_output::StagedDiffKind::Note ? L"note" : L"clrop");
}

std::filesystem::path AnnotJournalDir() {
  auto root = StageRoot();
  if (root.empty())
    return {};
  return root / L"clrop_journal";
}

std::filesystem::path NoteJournalDir() {
  auto root = StageRoot();
  if (root.empty())
    return {};
  return root / L"note_journal";
}

std::filesystem::path NoteStateDir() {
  auto root = StageRoot();
  if (root.empty())
    return {};
  return root / L"note_state";
}

bool EnsureDir(const std::filesystem::path &dir) {
  if (dir.empty())
    return false;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec;
}

std::optional<std::string> ReadFileBytes(const std::filesystem::path &path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs)
    return std::nullopt;
  ifs.seekg(0, std::ios::end);
  std::streamsize size = ifs.tellg();
  if (size < 0)
    size = 0;
  ifs.seekg(0, std::ios::beg);
  std::string out(static_cast<size_t>(size), '\0');
  if (size > 0) {
    ifs.read(out.data(), size);
    if (!ifs)
      return std::nullopt;
  }
  return out;
}

std::wstring ReadCurrentNoteText() {
  if (!g_hNoteEdit)
    return {};
  int len = GetWindowTextLengthW(g_hNoteEdit);
  if (len <= 0)
    return {};
  std::wstring text(static_cast<size_t>(len), L'\0');
  int copied = GetWindowTextW(g_hNoteEdit, text.data(), len + 1);
  if (copied < 0)
    copied = 0;
  text.resize(static_cast<size_t>(copied));
  return text;
}

bool IsNoteSaveBlockedByIme() { return IsNoteImeComposing(); }

bool IsAnnotationSaveBlockedByIme() { return g_pdf.imeComposing; }

void ShowImeBlockedSaveNotice(HWND owner, bool noteTarget) {
  const std::wstring text =
      noteTarget
          ? (IsEnglishUi()
                 ? L"Finish IME composition in the note before saving."
                 : L"ノートの IME 変換を確定してから保存してください。")
          : (IsEnglishUi()
                 ? L"Finish IME composition in the annotation text before "
                   L"saving."
                 : L"注釈テキストの IME 変換を確定してから保存してください。");
  ShowStageSoftNotice(owner, text, SoftNoticeKind::Warning);
}

bool EnsureFastLoadedAnnotationsStrongValidatedBeforeSave(
    HWND owner, const std::wstring &pdfPath, std::wstring *outErr = nullptr) {
  if (outErr)
    outErr->clear();
  if (!g_annotsRequireStrongValidation)
    return true;

  auto fail = [&](const std::wstring &reason) {
    std::wstring msg =
        IsEnglishUi()
            ? L"Annotation save was stopped because the loaded .clrop has not "
              L"been strongly verified against the current PDF."
            : L"読み込んだ .clrop と現在の PDF "
              L"の強照合が完了していないため、注釈保存を中止しました。";
    if (!reason.empty())
      msg += L"\n" + reason;
    msg += IsEnglishUi() ? L"\n\nReopen the PDF and resolve the .clrop "
                           L"mismatch before saving annotations."
                         : L"\n\nPDF を開き直し、.clrop "
                           L"の不一致を確認してから注釈を保存してください。";
    if (outErr)
      *outErr = msg;
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Stage annotations" : L"注釈 stage 保存", msg);
    TraceSaveFailure(L"StageSave", L"annot_strong_validation_required", pdfPath,
                     {}, {}, msg);
    return false;
  };

  if (pdfPath.empty()) {
    return fail(IsEnglishUi() ? L"Current PDF path is empty."
                              : L"現在の PDF パスが空です。");
  }
  if (!g_annotsLoadedPdfIdValid) {
    return fail(IsEnglishUi() ? L"The loaded .clrop identity is unavailable."
                              : L"読み込んだ .clrop の識別情報がありません。");
  }

  const clrop::PdfId currentId = clrop::ComputePdfId(pdfPath);
  if (currentId.sha256.empty()) {
    return fail(
        IsEnglishUi()
            ? L"Could not compute the current PDF SHA-256 for strong "
              L"validation."
            : L"現在の PDF の SHA-256 を計算できず、強照合できません。");
  }
  if (!clrop::PdfIdStrongMatches(g_annotsLoadedPdfId, currentId)) {
    return fail(
        IsEnglishUi()
            ? L"The current PDF bytes differ from the .clrop that was loaded."
            : L"現在の PDF の内容が、読み込んだ .clrop の対象 PDF "
              L"と一致しません。");
  }

  g_annotsLoadedPdfId = currentId;
  g_annotsLoadedPdfIdValid = true;
  g_annotsRequireStrongValidation = false;
  return true;
}

std::string
CaptureCurrentNoteBytes(note::SnapshotIdentity *outIdentity = nullptr) {
  if (outIdentity)
    *outIdentity = {};
  std::string textCoreBytes;
  if (CaptureCurrentNoteTextCoreUtf8(g_currentNotePath, &textCoreBytes,
                                     outIdentity)) {
    return textCoreBytes;
  }
  std::string bytes = WideToUTF8(ReadCurrentNoteText());
  if (outIdentity) {
    note::SnapshotIdentity current = CaptureCurrentNoteSnapshotIdentity();
    *outIdentity =
        note::BuildSnapshotIdentity(current.note_id, current.content_revision,
                                    current.persistence_revision, bytes);
  }
  return bytes;
}

std::optional<PreparedNoteStageSnapshot>
PrepareNoteStageSnapshotFromCurrentState() {
  PreparedNoteStageSnapshot snapshot;
  snapshot.targetPath = g_currentNotePath;
  snapshot.bytes = CaptureCurrentNoteBytes(&snapshot.identity);
  snapshot.revision = CurrentEditRevision();
  return snapshot;
}

std::optional<PreparedAnnotStageSnapshot>
PrepareAnnotStageSnapshotFromCurrentState(std::wstring *outErr = nullptr) {
  const std::wstring pdfPath = CurrentLogicalPdfPath();
  if (pdfPath.empty())
    return std::nullopt;
  PreparedAnnotStageSnapshot snapshot;
  snapshot.targetPath = pdfPath;
  if (!TryBuildCurrentAnnotationSaveSnapshot(&snapshot.annotations)) {
    if (outErr)
      *outErr =
          IsEnglishUi()
              ? L"Finish IME composition in the annotation text before saving."
              : L"注釈テキストの IME 変換を確定してから保存してください。";
    return std::nullopt;
  }
  snapshot.revision = CurrentEditRevision();
  return snapshot;
}

std::optional<PreparedAnnotStageSnapshot>
ReusePreparedAnnotStageSnapshotIfFresh() {
  if (!g_preparedAnnotStageSnapshot.has_value())
    return std::nullopt;
  const std::wstring pdfPath = CurrentLogicalPdfPath();
  if (pdfPath.empty() || g_preparedAnnotStageSnapshot->targetPath != pdfPath)
    return std::nullopt;
  if (g_preparedAnnotStageSnapshot->revision != CurrentEditRevision())
    return std::nullopt;
  return g_preparedAnnotStageSnapshot;
}

bool PrepareDeferredAutoStageSnapshots(HWND owner) {
  const uint64_t revision = CurrentEditRevision();
  if (g_preparedAutoStageRevision == revision &&
      ((g_annotsDirty && g_preparedAnnotStageSnapshot.has_value()) ||
       !g_annotsDirty)) {
    if (!g_preparedAutoStageOwner)
      g_preparedAutoStageOwner = NoticeOwner(owner);
    return g_preparedAnnotStageSnapshot.has_value() || g_annotsDirty;
  }

  g_preparedAnnotStageSnapshot.reset();
  if (g_annotsDirty) {
    std::vector<AnnotCommand> pendingCommands;
    const std::wstring pdfPath = CurrentLogicalPdfPath();
    const bool canAppendWithoutCheckpoint =
        !pdfPath.empty() && !g_pdf.editingText &&
        CollectPendingAnnotStageCommands(pdfPath, &pendingCommands) &&
        !pendingCommands.empty();
    if (!canAppendWithoutCheckpoint) {
      std::wstring err;
      auto preparedAnnot = PrepareAnnotStageSnapshotFromCurrentState(&err);
      if (!preparedAnnot.has_value()) {
        if (!err.empty())
          ShowStageSoftNotice(owner, err, SoftNoticeKind::Warning);
        return false;
      }
      g_preparedAnnotStageSnapshot = std::move(preparedAnnot);
    }
  }

  g_preparedAutoStageRevision = revision;
  g_preparedAutoStageOwner = NoticeOwner(owner);
  return g_annotsDirty || g_preparedAnnotStageSnapshot.has_value();
}

uint64_t HashPathKey64(const std::wstring &key) {
  constexpr uint64_t kOffset = 1469598103934665603ull;
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t hash = kOffset;
  for (wchar_t ch : key) {
    uint16_t value = static_cast<uint16_t>(ch);
    hash ^= static_cast<uint8_t>(value & 0xFFu);
    hash *= kPrime;
    hash ^= static_cast<uint8_t>((value >> 8) & 0xFFu);
    hash *= kPrime;
  }
  return hash;
}

std::wstring Hex64(uint64_t value) {
  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring out(16, L'0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kHex[value & 0xFu];
    value >>= 4;
  }
  return out;
}

std::wstring TargetHash(const std::wstring &targetPath) {
  return Hex64(
      HashPathKey64(NormalizePathKey(std::filesystem::path(targetPath))));
}

std::optional<uint64_t> ParseUint64(const std::wstring &text);

std::wstring StagePathHash(const std::filesystem::path &stagePath) {
  return Hex64(HashPathKey64(NormalizePathKey(stagePath)));
}

std::wstring NoteJournalFilename(const std::filesystem::path &stagePath,
                                 uint64_t revision) {
  return L"notej_" + StagePathHash(stagePath) + L"_r" +
         std::to_wstring(revision) + L".bin";
}

std::wstring NoteEditJournalFilename(const std::filesystem::path &stagePath,
                                     uint64_t revision) {
  return L"notee_" + StagePathHash(stagePath) + L"_r" +
         std::to_wstring(revision) + L".bin";
}

std::filesystem::path
BuildNoteJournalPath(const std::filesystem::path &stagePath,
                     uint64_t revision) {
  return NoteJournalDir() / NoteJournalFilename(stagePath, revision);
}

std::filesystem::path
BuildNoteEditJournalPath(const std::filesystem::path &stagePath,
                         uint64_t revision) {
  return NoteJournalDir() / NoteEditJournalFilename(stagePath, revision);
}

std::filesystem::path NoteStatePath(const std::filesystem::path &stagePath) {
  return NoteStateDir() / (L"notes_" + StagePathHash(stagePath) + L".state");
}

std::optional<NoteJournalSegment>
ParseNoteJournalSegmentPath(const std::filesystem::path &path) {
  const std::wstring name = path.filename().wstring();
  if (name.size() < 20 || name.rfind(L"notej_", 0) != 0)
    return std::nullopt;
  if (name.size() < 5 || name.substr(name.size() - 4) != L".bin")
    return std::nullopt;
  const size_t revPos = name.find(L"_r", 6);
  if (revPos == std::wstring::npos)
    return std::nullopt;
  const std::wstring hash = name.substr(6, revPos - 6);
  const std::wstring revText =
      name.substr(revPos + 2, name.size() - (revPos + 2) - 4);
  auto revision = ParseUint64(revText);
  if (!revision.has_value())
    return std::nullopt;
  NoteJournalSegment seg;
  seg.path = path;
  seg.stageHash = hash;
  seg.revision = *revision;
  return seg;
}

std::optional<NoteEditJournalSegment>
ParseNoteEditJournalSegmentPath(const std::filesystem::path &path) {
  const std::wstring name = path.filename().wstring();
  if (name.size() < 20 || name.rfind(L"notee_", 0) != 0)
    return std::nullopt;
  if (name.size() < 5 || name.substr(name.size() - 4) != L".bin")
    return std::nullopt;
  const size_t revPos = name.find(L"_r", 6);
  if (revPos == std::wstring::npos)
    return std::nullopt;
  const std::wstring hash = name.substr(6, revPos - 6);
  const std::wstring revText =
      name.substr(revPos + 2, name.size() - (revPos + 2) - 4);
  auto revision = ParseUint64(revText);
  if (!revision.has_value())
    return std::nullopt;
  NoteEditJournalSegment seg;
  seg.path = path;
  seg.stageHash = hash;
  seg.revision = *revision;
  return seg;
}

std::vector<NoteJournalSegment>
ListNoteJournalSegmentsForStage(const std::filesystem::path &stagePath) {
  std::vector<NoteJournalSegment> out;
  const auto dir = NoteJournalDir();
  const std::wstring stageHash = StagePathHash(stagePath);
  std::error_code ec;
  if (dir.empty() || !std::filesystem::exists(dir, ec) || ec)
    return out;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || ec)
      continue;
    auto parsed = ParseNoteJournalSegmentPath(entry.path());
    if (!parsed.has_value())
      continue;
    if (parsed->stageHash != stageHash)
      continue;
    out.push_back(std::move(*parsed));
  }
  std::sort(out.begin(), out.end(),
            [](const NoteJournalSegment &lhs, const NoteJournalSegment &rhs) {
              if (lhs.revision != rhs.revision)
                return lhs.revision < rhs.revision;
              return lhs.path.wstring() < rhs.path.wstring();
            });
  return out;
}

std::vector<NoteEditJournalSegment>
ListNoteEditJournalSegmentsForStage(const std::filesystem::path &stagePath) {
  std::vector<NoteEditJournalSegment> out;
  const auto dir = NoteJournalDir();
  const std::wstring stageHash = StagePathHash(stagePath);
  std::error_code ec;
  if (dir.empty() || !std::filesystem::exists(dir, ec) || ec)
    return out;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || ec)
      continue;
    auto parsed = ParseNoteEditJournalSegmentPath(entry.path());
    if (!parsed.has_value())
      continue;
    if (parsed->stageHash != stageHash)
      continue;
    out.push_back(std::move(*parsed));
  }
  std::sort(
      out.begin(), out.end(),
      [](const NoteEditJournalSegment &lhs, const NoteEditJournalSegment &rhs) {
        if (lhs.revision != rhs.revision)
          return lhs.revision < rhs.revision;
        return lhs.path.wstring() < rhs.path.wstring();
      });
  return out;
}

void DiscardNoteJournalSegmentsForStage(
    const std::filesystem::path &stagePath) {
  for (const auto &seg : ListNoteJournalSegmentsForStage(stagePath)) {
    std::error_code ec;
    std::filesystem::remove(seg.path, ec);
  }
  for (const auto &seg : ListNoteEditJournalSegmentsForStage(stagePath)) {
    std::error_code ec;
    std::filesystem::remove(seg.path, ec);
  }
}

void DiscardNoteStageArtifactsForStage(const std::filesystem::path &stagePath) {
  if (stagePath.empty())
    return;
  DiscardNoteJournalSegmentsForStage(stagePath);
  std::error_code ec;
  std::filesystem::remove(NoteStatePath(stagePath), ec);
}

uint64_t
NextNoteJournalRevisionForStage(const std::filesystem::path &stagePath) {
  uint64_t best = 0;
  for (const auto &seg : ListNoteJournalSegmentsForStage(stagePath)) {
    best = std::max(best, seg.revision);
  }
  return best + 1;
}

uint64_t
NextNoteEditJournalRevisionForStage(const std::filesystem::path &stagePath) {
  uint64_t best = 0;
  for (const auto &seg : ListNoteEditJournalSegmentsForStage(stagePath)) {
    best = std::max(best, seg.revision);
  }
  return best + 1;
}

std::string BuildBlankTailBytes(uint64_t blankTailCount) {
  std::string out;
  if (blankTailCount == 0)
    return out;
  out.reserve(
      static_cast<size_t>(std::min<uint64_t>(blankTailCount, SIZE_MAX / 2)) *
      2u);
  for (uint64_t i = 0; i < blankTailCount; ++i) {
    out += "\r\n";
  }
  return out;
}

bool LoadNoteStageState(const std::filesystem::path &stagePath,
                        NoteStageState *outState,
                        std::wstring *outErr = nullptr) {
  if (outState)
    *outState = {};
  const auto statePath = NoteStatePath(stagePath);
  std::error_code ec;
  if (statePath.empty() || !std::filesystem::exists(statePath, ec) || ec) {
    return true;
  }
  auto bytes = ReadFileBytes(statePath);
  if (!bytes.has_value()) {
    if (outErr)
      *outErr = L"note state の読み込みに失敗しました。";
    return false;
  }
  static const std::string kMarker = "\n--DATA--\n";
  const size_t markerPos = bytes->find(kMarker);
  if (markerPos == std::string::npos) {
    if (outErr)
      *outErr = L"note state の形式が不正です。";
    return false;
  }

  NoteStageState state;
  std::istringstream iss(bytes->substr(0, markerPos + 1));
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "applied_rev") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (!parsed.has_value()) {
        if (outErr)
          *outErr = L"note state の applied_rev が不正です。";
        return false;
      }
      state.appliedJournalRevision = *parsed;
    } else if (key == "applied_edit_rev") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (!parsed.has_value()) {
        if (outErr)
          *outErr = L"note state の applied_edit_rev が不正です。";
        return false;
      }
      state.appliedEditJournalRevision = *parsed;
    } else if (key == "blank_tail") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (!parsed.has_value()) {
        if (outErr)
          *outErr = L"note state の blank_tail が不正です。";
        return false;
      }
      state.blankTailCount = *parsed;
    }
  }
  state.textTailBytes = bytes->substr(markerPos + kMarker.size());
  if (outState)
    *outState = std::move(state);
  return true;
}

bool SaveNoteStageState(const std::filesystem::path &stagePath,
                        const NoteStageState &state,
                        std::wstring *outErr = nullptr) {
  const auto statePath = NoteStatePath(stagePath);
  if (statePath.empty()) {
    if (outErr)
      *outErr = L"note state path が不正です。";
    return false;
  }
  const bool stateEmpty = state.appliedJournalRevision == 0 &&
                          state.appliedEditJournalRevision == 0 &&
                          state.blankTailCount == 0 &&
                          state.textTailBytes.empty();
  if (stateEmpty) {
    std::error_code ec;
    std::filesystem::remove(statePath, ec);
    return true;
  }
  if (!EnsureDir(statePath.parent_path())) {
    if (outErr)
      *outErr = L"note state フォルダを作成できません。";
    return false;
  }
  std::string bytes;
  bytes += "applied_rev=" + std::to_string(state.appliedJournalRevision) + "\n";
  bytes +=
      "applied_edit_rev=" + std::to_string(state.appliedEditJournalRevision) +
      "\n";
  bytes += "blank_tail=" + std::to_string(state.blankTailCount) + "\n";
  bytes += "--DATA--\n";
  bytes += state.textTailBytes;
  std::wstring err;
  if (!atomic_write::AtomicWriteBytes(statePath, bytes.data(), bytes.size(),
                                      statePath.parent_path(),
                                      WorkspaceEscapeRoot(), &err)) {
    if (outErr)
      *outErr = err;
    return false;
  }
  return true;
}

bool WriteNoteJournalSegment(const std::filesystem::path &stagePath,
                             uint64_t revision, std::string_view bytes,
                             std::wstring *outErr = nullptr) {
  if (stagePath.empty()) {
    if (outErr)
      *outErr = L"note journal stage path が不正です。";
    return false;
  }
  if (bytes.empty()) {
    if (outErr)
      *outErr = L"note journal の内容がありません。";
    return false;
  }
  const auto dir = NoteJournalDir();
  if (!EnsureDir(dir)) {
    if (outErr)
      *outErr = L"note journal フォルダを作成できません。";
    return false;
  }
  const auto path = BuildNoteJournalPath(stagePath, revision);
  std::wstring err;
  if (!atomic_write::AtomicWriteBytes(path, bytes.data(), bytes.size(),
                                      path.parent_path(), WorkspaceEscapeRoot(),
                                      &err)) {
    if (outErr)
      *outErr = err;
    return false;
  }
  return true;
}

uint64_t CountTrailingBlankTailPairs(std::string_view bytes) {
  uint64_t count = 0;
  size_t pos = bytes.size();
  while (pos >= 2 && bytes[pos - 2] == '\r' && bytes[pos - 1] == '\n') {
    ++count;
    pos -= 2;
  }
  return count;
}

void DecomposeNoteRemainder(std::string_view remainder,
                            std::string *outJournalAdd,
                            std::string *outTextTail,
                            uint64_t *outBlankTailCount) {
  if (outJournalAdd)
    outJournalAdd->clear();
  if (outTextTail)
    outTextTail->clear();
  if (outBlankTailCount)
    *outBlankTailCount = 0;

  const uint64_t blankTailCount = CountTrailingBlankTailPairs(remainder);
  if (outBlankTailCount)
    *outBlankTailCount = blankTailCount;
  const size_t blankBytes = static_cast<size_t>(std::min<uint64_t>(
                                blankTailCount, remainder.size() / 2u)) *
                            2u;
  const std::string_view noBlank =
      (blankBytes <= remainder.size())
          ? remainder.substr(0, remainder.size() - blankBytes)
          : std::string_view{};

  if (blankTailCount > 0) {
    if (outJournalAdd)
      outJournalAdd->assign(noBlank.begin(), noBlank.end());
    return;
  }

  const size_t crlfPos = noBlank.rfind("\r\n");
  if (crlfPos == std::string_view::npos) {
    if (outTextTail)
      outTextTail->assign(noBlank.begin(), noBlank.end());
    return;
  }
  if (outJournalAdd)
    outJournalAdd->assign(noBlank.begin(),
                          noBlank.begin() + static_cast<ptrdiff_t>(crlfPos));
  if (outTextTail)
    outTextTail->assign(noBlank.begin() + static_cast<ptrdiff_t>(crlfPos),
                        noBlank.end());
}

std::wstring StageExtension(file_output::StagedDiffKind kind) {
  return kind == file_output::StagedDiffKind::Note ? L".stage"
                                                   : L".clrop.stage";
}

std::filesystem::path StageMetaPath(const std::filesystem::path &stagePath) {
  return std::filesystem::path(stagePath.wstring() + L".meta.txt");
}

struct StageMeta {
  file_output::StagedDiffKind kind = file_output::StagedDiffKind::Note;
  std::filesystem::path stagePath;
  std::filesystem::path metaPath;
  std::filesystem::path destPath;
  std::wstring targetPath;
  std::optional<bool> destinationExistedAtStage;
  std::optional<uint64_t> destinationFingerprintAtStage;
  std::optional<uint64_t> revision;
  std::optional<uint64_t> noteId;
  std::optional<uint64_t> contentRevision;
  std::optional<uint64_t> basePersistenceRevision;
  std::optional<uint64_t> persistenceRevision;
  std::optional<file_output::StagedNoteRestoreViewState> noteRestoreView;
};

bool CaptureStageDestinationObservation(const std::filesystem::path &path,
                                        bool *outExisted,
                                        uint64_t *outFingerprint,
                                        std::wstring *outErr = nullptr) {
  if (outExisted)
    *outExisted = false;
  if (outFingerprint)
    *outFingerprint = 0;
  if (outErr)
    outErr->clear();
  if (path.empty()) {
    if (outErr)
      *outErr = L"統合先が不正です。";
    return false;
  }

  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return true;
    }
    if (outErr)
      *outErr = L"統合先を確認できません。";
    return false;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    if (outErr)
      *outErr = L"統合先が通常ファイルではありません。";
    return false;
  }

  auto bytes = ReadFileBytes(path);
  if (!bytes.has_value()) {
    if (outErr)
      *outErr = L"統合先の内容を読み取れません。";
    return false;
  }
  if (outExisted)
    *outExisted = true;
  if (outFingerprint)
    *outFingerprint = note::FingerprintSnapshotBytes(*bytes);
  return true;
}

bool DestinationStillMatchesStageObservation(const StageMeta &meta,
                                             std::wstring *outErr = nullptr) {
  if (!meta.destinationExistedAtStage.has_value() ||
      !meta.destinationFingerprintAtStage.has_value()) {
    if (outErr) {
      *outErr = L"この stage には統合先の世代情報がありません。原本を保護するため統合しません。";
    }
    return false;
  }

  bool existsNow = false;
  uint64_t fingerprintNow = 0;
  if (!CaptureStageDestinationObservation(meta.destPath, &existsNow,
                                          &fingerprintNow, outErr)) {
    return false;
  }
  if (existsNow != *meta.destinationExistedAtStage ||
      fingerprintNow != *meta.destinationFingerprintAtStage) {
    if (outErr) {
      *outErr = L"stage 作成後に統合先が外部変更されたため、原本を保護して統合を中止しました。";
    }
    return false;
  }
  return true;
}

file_output::StagedNoteRestoreViewState &
EnsureNoteRestoreView(StageMeta &meta) {
  if (!meta.noteRestoreView.has_value())
    meta.noteRestoreView.emplace();
  return *meta.noteRestoreView;
}

std::wstring StageKindName(file_output::StagedDiffKind kind) {
  return kind == file_output::StagedDiffKind::Note ? L"note" : L"clrop";
}

std::optional<uint64_t> ParseUint64(const std::wstring &text) {
  if (text.empty())
    return std::nullopt;
  uint64_t value = 0;
  for (wchar_t ch : text) {
    if (ch < L'0' || ch > L'9')
      return std::nullopt;
    uint64_t digit = static_cast<uint64_t>(ch - L'0');
    if (value > (UINT64_MAX - digit) / 10u)
      return std::nullopt;
    value = value * 10u + digit;
  }
  return value;
}

struct AnnotJournalSegment {
  std::filesystem::path path;
  std::wstring targetHash;
  uint64_t baseRevision = 0;
  uint64_t revision = 0;
};

std::wstring AnnotJournalFilename(const std::wstring &targetPath,
                                  uint64_t baseRevision, uint64_t revision) {
  return L"clropj_" + TargetHash(targetPath) + L"_b" +
         std::to_wstring(baseRevision) + L"_r" + std::to_wstring(revision) +
         L".json";
}

std::filesystem::path BuildAnnotJournalPath(const std::wstring &targetPath,
                                            uint64_t baseRevision,
                                            uint64_t revision) {
  return AnnotJournalDir() /
         AnnotJournalFilename(targetPath, baseRevision, revision);
}

std::optional<AnnotJournalSegment>
ParseAnnotJournalSegmentPath(const std::filesystem::path &path) {
  const std::wstring name = path.filename().wstring();
  if (name.size() < 32 || name.rfind(L"clropj_", 0) != 0)
    return std::nullopt;
  if (name.size() < 6 || name.substr(name.size() - 5) != L".json")
    return std::nullopt;
  const size_t basePos = name.find(L"_b", 7);
  const size_t revPos =
      name.find(L"_r", basePos == std::wstring::npos ? basePos : basePos + 2);
  if (basePos == std::wstring::npos || revPos == std::wstring::npos)
    return std::nullopt;
  const std::wstring hash = name.substr(7, basePos - 7);
  const std::wstring baseText =
      name.substr(basePos + 2, revPos - (basePos + 2));
  const std::wstring revText =
      name.substr(revPos + 2, name.size() - (revPos + 2) - 5);
  auto baseRevision = ParseUint64(baseText);
  auto revision = ParseUint64(revText);
  if (!baseRevision.has_value() || !revision.has_value())
    return std::nullopt;
  AnnotJournalSegment seg;
  seg.path = path;
  seg.targetHash = hash;
  seg.baseRevision = *baseRevision;
  seg.revision = *revision;
  return seg;
}

std::filesystem::path DestinationPathFor(file_output::StagedDiffKind kind,
                                         const std::wstring &targetPath) {
  if (kind == file_output::StagedDiffKind::Note) {
    return std::filesystem::path(targetPath);
  }
  return std::filesystem::path(clrop_bridge::ClropPathForPdf(targetPath));
}

std::filesystem::path BuildStagePath(file_output::StagedDiffKind kind,
                                     const std::wstring &targetPath,
                                     uint64_t revision) {
  const auto dir = StageDir(kind);
  const std::wstring base = StageKindName(kind) + L"_" +
                            TargetHash(targetPath) + L"_r" +
                            std::to_wstring(revision);
  const std::wstring ext = StageExtension(kind);
  std::filesystem::path candidate = dir / (base + ext);
  std::error_code ec;
  for (int suffix = 1; std::filesystem::exists(candidate, ec) && !ec;
       ++suffix) {
    candidate = dir / (base + L"_" + std::to_wstring(suffix) + ext);
    ec.clear();
  }
  return candidate;
}

bool WriteStageMetaFile(const StageMeta &meta, std::wstring *outErr = nullptr) {
  if (meta.metaPath.empty() || meta.stagePath.empty() ||
      meta.targetPath.empty()) {
    if (outErr)
      *outErr = L"Invalid stage meta path.";
    return false;
  }
  std::ostringstream oss;
  oss << "kind=" << WideToUTF8(StageKindName(meta.kind)) << "\n";
  oss << "target=" << WideToUTF8(meta.targetPath) << "\n";
  oss << "dest=" << WideToUTF8(meta.destPath.wstring()) << "\n";
  if (meta.destinationExistedAtStage.has_value() &&
      meta.destinationFingerprintAtStage.has_value()) {
    oss << "dest_existed=" << (*meta.destinationExistedAtStage ? 1 : 0) << "\n";
    oss << "dest_fingerprint=" << *meta.destinationFingerprintAtStage << "\n";
  }
  if (meta.revision.has_value()) {
    oss << "rev=" << *meta.revision << "\n";
  }
  if (meta.noteId.has_value()) {
    oss << "note_id=" << *meta.noteId << "\n";
  }
  if (meta.contentRevision.has_value()) {
    oss << "content_rev=" << *meta.contentRevision << "\n";
  }
  if (meta.basePersistenceRevision.has_value()) {
    oss << "base_persist_rev=" << *meta.basePersistenceRevision << "\n";
  }
  if (meta.persistenceRevision.has_value()) {
    oss << "persist_rev=" << *meta.persistenceRevision << "\n";
  }
  if (meta.noteRestoreView.has_value()) {
    const auto &restore = *meta.noteRestoreView;
    oss << "restore_content_rev=" << restore.contentRevision << "\n";
    oss << "restore_sel_start=" << restore.selectionStart << "\n";
    oss << "restore_sel_end=" << restore.selectionEnd << "\n";
    oss << "restore_scroll_x=" << restore.scrollX << "\n";
    oss << "restore_scroll_y=" << restore.scrollY << "\n";
    oss << "restore_first_visible_line=" << restore.firstVisibleLine << "\n";
  }
  oss << "stage=" << WideToUTF8(meta.stagePath.wstring()) << "\n";
  std::wstring err;
  const auto preferredTmp = meta.metaPath.parent_path();
  const auto quarantine = WorkspaceEscapeRoot();
  if (!atomic_write::AtomicWriteUtf8(meta.metaPath, oss.str(), preferredTmp,
                                     quarantine, &err)) {
    if (outErr)
      *outErr = err;
    return false;
  }
  return true;
}

bool UpdateStageMetaContentIdentity(const StageMeta &source,
                                    const note::SnapshotIdentity &identity,
                                    std::string_view bytes,
                                    std::wstring *outErr = nullptr) {
  if (!identity.valid() || identity.content_revision == 0) {
    return true;
  }
  if (identity.content_fingerprint != note::FingerprintSnapshotBytes(bytes)) {
    if (outErr)
      *outErr = L"note stage のTextCore identityが本文と一致しません。";
    return false;
  }
  StageMeta updated = source;
  const auto currentRecord =
      note::FindRuntimeNotePersistenceRecord(identity.note_id);
  if (source.noteId.has_value() && *source.noteId != identity.note_id.value) {
    const auto sourceRecord =
        note::FindRuntimeNotePersistenceRecord(note::NoteId{*source.noteId});
    if (source.basePersistenceRevision.has_value() ||
        source.persistenceRevision.has_value() || sourceRecord.has_value()) {
      if (outErr)
        *outErr = L"note stage の永続IDが対象ノートと一致しません。";
      return false;
    }
  }
  if (currentRecord.has_value()) {
    updated.noteId = identity.note_id.value;
  } else {
    updated.noteId.reset();
    updated.basePersistenceRevision.reset();
    updated.persistenceRevision.reset();
  }
  updated.contentRevision = identity.content_revision;
  if (auto restore = CaptureCurrentNoteRestoreViewState(
          source.targetPath, identity.content_revision)) {
    updated.noteRestoreView = *restore;
  }
  if (source.contentRevision == updated.contentRevision &&
      source.noteId == updated.noteId &&
      source.basePersistenceRevision == updated.basePersistenceRevision &&
      source.persistenceRevision == updated.persistenceRevision &&
      source.noteRestoreView.has_value() ==
          updated.noteRestoreView.has_value() &&
      (!source.noteRestoreView.has_value() ||
       (source.noteRestoreView->contentRevision ==
            updated.noteRestoreView->contentRevision &&
        source.noteRestoreView->selectionStart ==
            updated.noteRestoreView->selectionStart &&
        source.noteRestoreView->selectionEnd ==
            updated.noteRestoreView->selectionEnd &&
        source.noteRestoreView->scrollX == updated.noteRestoreView->scrollX &&
        source.noteRestoreView->scrollY == updated.noteRestoreView->scrollY &&
        source.noteRestoreView->firstVisibleLine ==
            updated.noteRestoreView->firstVisibleLine))) {
    return true;
  }
  return WriteStageMetaFile(updated, outErr);
}

bool LoadStageMetaFile(const std::filesystem::path &metaPath,
                       StageMeta *outMeta) {
  if (outMeta)
    *outMeta = {};
  auto bytes = ReadFileBytes(metaPath);
  if (!bytes.has_value())
    return false;
  StageMeta meta;
  meta.metaPath = metaPath;
  meta.stagePath = std::filesystem::path(metaPath.wstring().substr(
      0, metaPath.wstring().size() - std::wstring(L".meta.txt").size()));

  std::istringstream iss(*bytes);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "kind") {
      const std::wstring kind = UTF8ToWide(value);
      meta.kind = (kind == L"clrop") ? file_output::StagedDiffKind::Clrop
                                     : file_output::StagedDiffKind::Note;
    } else if (key == "target") {
      meta.targetPath = UTF8ToWide(value);
    } else if (key == "dest") {
      meta.destPath = std::filesystem::path(UTF8ToWide(value));
    } else if (key == "dest_existed") {
      if (value == "0") {
        meta.destinationExistedAtStage = false;
      } else if (value == "1") {
        meta.destinationExistedAtStage = true;
      } else {
        return false;
      }
    } else if (key == "dest_fingerprint") {
      meta.destinationFingerprintAtStage = ParseUint64(UTF8ToWide(value));
    } else if (key == "rev") {
      meta.revision = ParseUint64(UTF8ToWide(value));
    } else if (key == "note_id") {
      meta.noteId = ParseUint64(UTF8ToWide(value));
    } else if (key == "content_rev") {
      meta.contentRevision = ParseUint64(UTF8ToWide(value));
    } else if (key == "base_persist_rev") {
      meta.basePersistenceRevision = ParseUint64(UTF8ToWide(value));
    } else if (key == "persist_rev") {
      meta.persistenceRevision = ParseUint64(UTF8ToWide(value));
    } else if (key == "restore_content_rev") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (parsed.has_value())
        EnsureNoteRestoreView(meta).contentRevision = *parsed;
    } else if (key == "restore_sel_start") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (parsed.has_value())
        EnsureNoteRestoreView(meta).selectionStart = *parsed;
    } else if (key == "restore_sel_end") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (parsed.has_value())
        EnsureNoteRestoreView(meta).selectionEnd = *parsed;
    } else if (key == "restore_scroll_x") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (parsed.has_value() && *parsed <= static_cast<uint64_t>(INT_MAX)) {
        EnsureNoteRestoreView(meta).scrollX = static_cast<int>(*parsed);
      }
    } else if (key == "restore_scroll_y") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (parsed.has_value() && *parsed <= static_cast<uint64_t>(INT_MAX)) {
        EnsureNoteRestoreView(meta).scrollY = static_cast<int>(*parsed);
      }
    } else if (key == "restore_first_visible_line") {
      auto parsed = ParseUint64(UTF8ToWide(value));
      if (parsed.has_value() && *parsed <= static_cast<uint64_t>(INT_MAX)) {
        EnsureNoteRestoreView(meta).firstVisibleLine =
            static_cast<int>(*parsed);
      }
    } else if (key == "stage") {
      const auto parsed = std::filesystem::path(UTF8ToWide(value));
      if (!parsed.empty())
        meta.stagePath = parsed;
    }
  }
  if (meta.targetPath.empty())
    return false;
  if (meta.destPath.empty()) {
    meta.destPath = DestinationPathFor(meta.kind, meta.targetPath);
  }
  if (meta.stagePath.empty()) {
    meta.stagePath = std::filesystem::path(metaPath.wstring().substr(
        0, metaPath.wstring().size() - std::wstring(L".meta.txt").size()));
  }
  if (meta.destinationExistedAtStage.has_value() !=
      meta.destinationFingerprintAtStage.has_value()) {
    return false;
  }
  if (meta.kind == file_output::StagedDiffKind::Note) {
    if (meta.noteId.has_value() && *meta.noteId == 0) {
      return false;
    }
    const bool hasPersistenceFields =
        meta.basePersistenceRevision.has_value() ||
        meta.persistenceRevision.has_value();
    if (hasPersistenceFields &&
        (!meta.noteId.has_value() ||
         !meta.basePersistenceRevision.has_value() ||
         !meta.persistenceRevision.has_value() ||
         *meta.basePersistenceRevision == UINT64_MAX ||
         *meta.persistenceRevision != *meta.basePersistenceRevision + 1)) {
      return false;
    }
    if (meta.contentRevision.has_value() && *meta.contentRevision == 0) {
      return false;
    }
  }
  if (outMeta)
    *outMeta = std::move(meta);
  return true;
}

bool RemoveStageAndMeta(const std::filesystem::path &stagePath) {
  if (stagePath.empty())
    return true;
  std::error_code ec;
  std::filesystem::remove(stagePath, ec);
  ec.clear();
  std::filesystem::remove(StageMetaPath(stagePath), ec);
  return true;
}

bool CopyFileWithDirs(const std::filesystem::path &src,
                      const std::filesystem::path &dest,
                      std::wstring *outErr = nullptr) {
  if (src.empty() || dest.empty()) {
    if (outErr)
      *outErr = L"Invalid copy path.";
    return false;
  }
  if (!dest.parent_path().empty() && !EnsureDir(dest.parent_path())) {
    if (outErr)
      *outErr = L"Failed to create destination directory.";
    return false;
  }
  std::error_code ec;
  std::filesystem::copy_file(
      src, dest, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    if (outErr)
      *outErr =
          L"Failed to copy file: " + src.wstring() + L" -> " + dest.wstring();
    return false;
  }
  return true;
}

struct BackupMetaInfo {
  file_output::StagedDiffKind kind = file_output::StagedDiffKind::Note;
  std::filesystem::path destPath;
  std::filesystem::path backupPath;
  std::filesystem::path metaPath;
};

bool LoadBackupMetaFile(const std::filesystem::path &metaPath,
                        BackupMetaInfo *outMeta) {
  if (outMeta)
    *outMeta = {};
  auto bytes = ReadFileBytes(metaPath);
  if (!bytes.has_value())
    return false;

  BackupMetaInfo meta;
  meta.metaPath = metaPath;
  std::istringstream iss(*bytes);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "dest") {
      meta.destPath = std::filesystem::path(UTF8ToWide(value));
    } else if (key == "backup") {
      meta.backupPath = std::filesystem::path(UTF8ToWide(value));
    } else if (key == "kind") {
      const std::wstring kind = UTF8ToWide(value);
      meta.kind = (kind == L"clrop") ? file_output::StagedDiffKind::Clrop
                                     : file_output::StagedDiffKind::Note;
    }
  }
  if (meta.backupPath.empty()) {
    const std::wstring suffix = L".meta.txt";
    const std::wstring raw = metaPath.wstring();
    if (raw.size() > suffix.size() &&
        raw.rfind(suffix) == raw.size() - suffix.size()) {
      meta.backupPath =
          std::filesystem::path(raw.substr(0, raw.size() - suffix.size()));
    }
  }
  if (meta.destPath.empty() || meta.backupPath.empty())
    return false;
  if (outMeta)
    *outMeta = std::move(meta);
  return true;
}

bool DeleteBackupMetaFiles(const std::filesystem::path &backupMetaPath,
                           std::wstring *outErr = nullptr) {
  if (outErr)
    outErr->clear();

  BackupMetaInfo meta;
  if (!LoadBackupMetaFile(backupMetaPath, &meta)) {
    if (outErr)
      *outErr = L"バックアップ metadata の読み込みに失敗しました。";
    return false;
  }

  std::error_code ec;
  ec.clear();
  if (!meta.backupPath.empty() &&
      std::filesystem::exists(meta.backupPath, ec) && !ec) {
    ec.clear();
    if (!std::filesystem::remove(meta.backupPath, ec) && ec) {
      if (outErr)
        *outErr = L"バックアップ本体の削除に失敗しました。";
      return false;
    }
  }

  ec.clear();
  if (std::filesystem::exists(meta.metaPath, ec) && !ec) {
    ec.clear();
    if (!std::filesystem::remove(meta.metaPath, ec) && ec) {
      if (outErr)
        *outErr = L"バックアップ metadata の削除に失敗しました。";
      return false;
    }
  }
  return true;
}

void TrimBackupGenerationsForDestination(
    file_output::StagedDiffKind kind, const std::filesystem::path &destPath,
    const std::filesystem::path &keepMetaPath) {
  if (destPath.empty())
    return;
  const auto backupDir = BackupDir(kind);
  if (backupDir.empty())
    return;

  std::error_code ec;
  if (!std::filesystem::exists(backupDir, ec) || ec)
    return;

  std::vector<BackupMetaInfo> matches;
  for (const auto &entry : std::filesystem::directory_iterator(backupDir, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || ec)
      continue;
    const auto path = entry.path();
    const std::wstring filename = path.filename().wstring();
    if (filename.size() < 9 ||
        filename.rfind(L".meta.txt") != filename.size() - 9)
      continue;
    BackupMetaInfo meta;
    if (!LoadBackupMetaFile(path, &meta))
      continue;
    if (meta.kind != kind)
      continue;
    if (!IsSamePath(meta.destPath, destPath))
      continue;
    matches.push_back(std::move(meta));
  }

  std::sort(matches.begin(), matches.end(),
            [&](const BackupMetaInfo &lhs, const BackupMetaInfo &rhs) {
              const bool lhsKeep = !keepMetaPath.empty() &&
                                   IsSamePath(lhs.metaPath, keepMetaPath);
              const bool rhsKeep = !keepMetaPath.empty() &&
                                   IsSamePath(rhs.metaPath, keepMetaPath);
              if (lhsKeep != rhsKeep)
                return lhsKeep;

              std::error_code lhsEc;
              std::error_code rhsEc;
              const auto lhsTime =
                  std::filesystem::last_write_time(lhs.metaPath, lhsEc);
              const auto rhsTime =
                  std::filesystem::last_write_time(rhs.metaPath, rhsEc);
              if (!lhsEc && !rhsEc && lhsTime != rhsTime)
                return lhsTime > rhsTime;
              return lhs.metaPath.wstring() > rhs.metaPath.wstring();
            });

  for (size_t i = kMaxBackupGenerationsPerFile; i < matches.size(); ++i) {
    DeleteBackupMetaFiles(matches[i].metaPath, nullptr);
  }
}

bool CreateBackupIfNeeded(file_output::StagedDiffKind kind,
                          const std::filesystem::path &destPath,
                          std::filesystem::path *outBackupPath,
                          std::filesystem::path *outBackupMetaPath,
                          std::wstring *outErr = nullptr) {
  if (outBackupPath)
    outBackupPath->clear();
  if (outBackupMetaPath)
    outBackupMetaPath->clear();
  if (destPath.empty())
    return true;

  std::error_code ec;
  if (!std::filesystem::exists(destPath, ec) || ec) {
    return true;
  }

  const auto backupDir = BackupDir(kind);
  if (!EnsureDir(backupDir)) {
    if (outErr)
      *outErr = L"Failed to create backup directory.";
    return false;
  }
  std::filesystem::path bak = atomic_write::MakeUniqueDestInDir(
      backupDir,
      std::filesystem::path(destPath.filename().wstring() + L".bak"));
  if (!CopyFileWithDirs(destPath, bak, outErr)) {
    return false;
  }

  std::filesystem::path meta = StageMetaPath(bak);
  std::ostringstream oss;
  oss << "kind=" << WideToUTF8(StageKindName(kind)) << "\n";
  oss << "dest=" << WideToUTF8(destPath.wstring()) << "\n";
  oss << "backup=" << WideToUTF8(bak.wstring()) << "\n";
  std::wstring err;
  if (!atomic_write::AtomicWriteUtf8(meta, oss.str(), meta.parent_path(),
                                     WorkspaceEscapeRoot(), &err)) {
    if (outErr)
      *outErr = err;
    return false;
  }
  if (outBackupPath)
    *outBackupPath = bak;
  if (outBackupMetaPath)
    *outBackupMetaPath = meta;
  TrimBackupGenerationsForDestination(kind, destPath, meta);
  return true;
}

bool AtomicWriteVerified(const std::filesystem::path &destPath,
                         std::string_view bytes,
                         std::wstring *outErr = nullptr) {
  if (destPath.empty()) {
    if (outErr)
      *outErr = L"Destination path is empty.";
    return false;
  }
  std::wstring err;
  if (!atomic_write::AtomicWriteBytes(destPath, bytes.data(), bytes.size(),
                                      destPath.parent_path(),
                                      WorkspaceEscapeRoot(), &err)) {
    if (outErr)
      *outErr = err;
    return false;
  }
  auto written = ReadFileBytes(destPath);
  if (!written.has_value() || *written != bytes) {
    if (outErr)
      *outErr = L"Written file verification failed: " + destPath.wstring();
    return false;
  }
  return true;
}

bool DeleteResolvedEmptyClropDestination(const StageMeta &meta,
                                         std::wstring *outErr = nullptr) {
  if (meta.kind != file_output::StagedDiffKind::Clrop ||
      meta.destPath.empty() || meta.targetPath.empty() ||
      !IsSamePath(meta.destPath,
                  DestinationPathFor(file_output::StagedDiffKind::Clrop,
                                     meta.targetPath))) {
    if (outErr)
      *outErr = L"空の注釈データの削除先が不正です。";
    return false;
  }

  const DWORD attributes = GetFileAttributesW(meta.destPath.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return true;
    if (outErr)
      *outErr = L"空の .clrop ファイルを確認できません。";
    return false;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    if (outErr)
      *outErr = L"空の .clrop ファイルの削除先が通常ファイルではありません。";
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(meta.destPath, ec) || ec) {
    if (outErr)
      *outErr = L"空の .clrop ファイルの削除先が通常ファイルではありません。";
    return false;
  }
  if (!std::filesystem::remove(meta.destPath, ec) || ec) {
    if (outErr)
      *outErr = L"空の .clrop ファイルを削除できません。";
    return false;
  }
  return true;
}

void RefreshDirtyUi(HWND owner) {
  if (g_batchSaveUiDepth > 0) {
    g_batchSaveUiPending = true;
    if (owner)
      g_batchSaveUiOwner = owner;
    return;
  }
  RefreshMainWindowUiState(owner);
}

std::vector<StageMeta> LoadAllStageMeta(file_output::StagedDiffKind kind) {
  std::vector<StageMeta> out;
  const auto dir = StageDir(kind);
  std::error_code ec;
  if (dir.empty() || !std::filesystem::exists(dir, ec) || ec)
    return out;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || ec)
      continue;
    const auto path = entry.path();
    const std::wstring filename = path.filename().wstring();
    if (filename.size() < 9 ||
        filename.rfind(L".meta.txt") != filename.size() - 9)
      continue;
    StageMeta meta;
    if (!LoadStageMetaFile(path, &meta))
      continue;
    std::error_code existsEc;
    if (!std::filesystem::exists(meta.stagePath, existsEc) || existsEc)
      continue;
    meta.kind = kind;
    out.push_back(std::move(meta));
  }
  return out;
}

std::vector<StageMeta> LoadAllStageMeta() {
  auto note = LoadAllStageMeta(file_output::StagedDiffKind::Note);
  auto clrop = LoadAllStageMeta(file_output::StagedDiffKind::Clrop);
  note.insert(note.end(), std::make_move_iterator(clrop.begin()),
              std::make_move_iterator(clrop.end()));
  return note;
}

std::optional<StageMeta> FindLatestStageMeta(file_output::StagedDiffKind kind,
                                             const std::wstring &targetPath) {
  const std::wstring targetKey =
      NormalizePathKey(std::filesystem::path(targetPath));
  std::optional<StageMeta> best;
  std::filesystem::file_time_type bestTime{};
  for (auto &meta : LoadAllStageMeta(kind)) {
    if (NormalizePathKey(std::filesystem::path(meta.targetPath)) != targetKey)
      continue;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(meta.stagePath, ec);
    const uint64_t rev = meta.revision.value_or(0);
    const uint64_t bestRev = best.has_value() ? best->revision.value_or(0) : 0;
    if (!best.has_value() || rev > bestRev ||
        (rev == bestRev && !ec && mtime > bestTime)) {
      best = meta;
      if (!ec)
        bestTime = mtime;
    }
  }
  return best;
}

std::vector<AnnotJournalSegment>
ListAnnotJournalSegmentsForTarget(const std::wstring &targetPath) {
  std::vector<AnnotJournalSegment> out;
  const auto dir = AnnotJournalDir();
  const std::wstring targetHash = TargetHash(targetPath);
  std::error_code ec;
  if (dir.empty() || !std::filesystem::exists(dir, ec) || ec)
    return out;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || ec)
      continue;
    auto parsed = ParseAnnotJournalSegmentPath(entry.path());
    if (!parsed.has_value())
      continue;
    if (parsed->targetHash != targetHash)
      continue;
    out.push_back(std::move(*parsed));
  }
  std::sort(out.begin(), out.end(),
            [](const AnnotJournalSegment &lhs, const AnnotJournalSegment &rhs) {
              if (lhs.baseRevision != rhs.baseRevision)
                return lhs.baseRevision < rhs.baseRevision;
              if (lhs.revision != rhs.revision)
                return lhs.revision < rhs.revision;
              return lhs.path.wstring() < rhs.path.wstring();
            });
  return out;
}

void DiscardAnnotJournalSegmentsForTarget(
    const std::wstring &targetPath,
    std::optional<uint64_t> keepBaseRevision = std::nullopt) {
  for (const auto &seg : ListAnnotJournalSegmentsForTarget(targetPath)) {
    if (keepBaseRevision.has_value() && seg.baseRevision == *keepBaseRevision)
      continue;
    std::error_code ec;
    std::filesystem::remove(seg.path, ec);
  }
}

void DiscardAnnotJournalSegmentsForBase(const std::wstring &targetPath,
                                        uint64_t baseRevision) {
  for (const auto &seg : ListAnnotJournalSegmentsForTarget(targetPath)) {
    if (seg.baseRevision != baseRevision)
      continue;
    std::error_code ec;
    std::filesystem::remove(seg.path, ec);
  }
}

bool LoadResolvedStageAnnotationsForMeta(
    const StageMeta &meta, std::vector<Annotation> *outAnnotations,
    std::wstring *outErr = nullptr) {
  if (outAnnotations)
    outAnnotations->clear();
  if (!outAnnotations || meta.kind != file_output::StagedDiffKind::Clrop)
    return false;
  if (meta.stagePath.empty() || meta.targetPath.empty()) {
    if (outErr)
      *outErr = L"注釈 stage 情報が不正です。";
    return false;
  }

  bool mismatch = false;
  std::wstring err;
  clrop::PdfId loadedId{};
  if (!clrop_bridge::LoadAnnotations(
          meta.stagePath.wstring(), meta.targetPath, *outAnnotations, mismatch,
          &loadedId, err, clrop_bridge::LoadAnnotationsValidation::Strong)) {
    if (outErr) {
      *outErr = err.empty()
                    ? L"注釈 stage checkpoint の読み込みに失敗しました。"
                    : err;
      *outErr += L"\ncheckpoint path: " + meta.stagePath.wstring();
    }
    return false;
  }
  if (mismatch) {
    if (outErr) {
      *outErr =
          L"注釈 stage checkpoint の pdf_id が現在の PDF と一致しません。";
      *outErr += L"\ncheckpoint path: " + meta.stagePath.wstring();
    }
    return false;
  }

  const uint64_t baseRevision = meta.revision.value_or(0);
  for (const auto &seg : ListAnnotJournalSegmentsForTarget(meta.targetPath)) {
    if (seg.baseRevision != baseRevision || seg.revision <= baseRevision)
      continue;
    std::vector<AnnotCommand> commands;
    std::wstring parseErr;
    size_t lastByteCount = 0;
    bool loadedCommands = false;
    bool readFailed = false;
    for (int attempt = 1; attempt <= kAnnotJournalReadMaxAttempts; ++attempt) {
      auto bytes = ReadFileBytes(seg.path);
      if (!bytes.has_value()) {
        readFailed = true;
      } else {
        readFailed = false;
        lastByteCount = bytes->size();
        parseErr.clear();
        if (DeserializeAnnotCommandsJson(*bytes, &commands, &parseErr)) {
          loadedCommands = true;
          break;
        }
      }
      if (attempt < kAnnotJournalReadMaxAttempts) {
        Sleep(kAnnotJournalReadRetryDelayMs * static_cast<DWORD>(attempt));
      }
    }
    if (!loadedCommands) {
      if (outErr) {
        *outErr = readFailed
                      ? L"注釈 stage journal の読み込みに失敗しました。"
                      : (parseErr.empty()
                             ? L"注釈 stage journal の解析に失敗しました。"
                             : parseErr);
        *outErr += L"\njournal path: " + seg.path.wstring();
        *outErr += L"\nbytes: " + std::to_wstring(lastByteCount);
        *outErr +=
            L"\nattempts: " + std::to_wstring(kAnnotJournalReadMaxAttempts);
      }
      return false;
    }
    for (const auto &cmd : commands) {
      ApplyAnnotCommandToList(outAnnotations, cmd);
    }
  }
  return true;
}

void PopulateLatestFlags(std::vector<file_output::StagedDiffEntry> *entries) {
  if (!entries)
    return;
  for (auto &entry : *entries)
    entry.isLatest = false;
  for (auto kind : {file_output::StagedDiffKind::Note,
                    file_output::StagedDiffKind::Clrop}) {
    std::vector<size_t> idxs;
    for (size_t i = 0; i < entries->size(); ++i) {
      if ((*entries)[i].kind == kind)
        idxs.push_back(i);
    }
    std::vector<std::wstring> targets;
    for (size_t idx : idxs) {
      targets.push_back(
          NormalizePathKey(std::filesystem::path((*entries)[idx].targetPath)));
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const auto &targetKey : targets) {
      size_t bestIdx = static_cast<size_t>(-1);
      uint64_t bestRev = 0;
      std::filesystem::file_time_type bestTime{};
      for (size_t idx : idxs) {
        auto &entry = (*entries)[idx];
        if (NormalizePathKey(std::filesystem::path(entry.targetPath)) !=
            targetKey)
          continue;
        std::error_code ec;
        const auto mtime =
            std::filesystem::last_write_time(entry.stagePath, ec);
        const uint64_t rev = entry.revision.value_or(0);
        if (bestIdx == static_cast<size_t>(-1) || rev > bestRev ||
            (rev == bestRev && !ec && mtime > bestTime)) {
          bestIdx = idx;
          bestRev = rev;
          if (!ec)
            bestTime = mtime;
        }
      }
      if (bestIdx != static_cast<size_t>(-1)) {
        (*entries)[bestIdx].isLatest = true;
      }
    }
  }
}

enum class PersistedSnapshotMatch {
  None,
  Original,
  Stage,
};

void ClearCurrentNoteEditorModifiedFlag(const std::wstring &notePath) {
  if (!g_hNoteEdit || notePath.empty() || g_currentNotePath.empty())
    return;
  if (NormalizePathKey(std::filesystem::path(notePath)) !=
      NormalizePathKey(std::filesystem::path(g_currentNotePath))) {
    return;
  }
  SendMessageW(g_hNoteEdit, EM_SETMODIFY, FALSE, 0);
}

bool LoadResolvedStageNoteDataForMeta(const StageMeta &meta,
                                      ResolvedNoteStageData *outData,
                                      std::wstring *outErr = nullptr) {
  if (outData)
    *outData = {};
  if (meta.kind != file_output::StagedDiffKind::Note ||
      meta.stagePath.empty()) {
    if (outErr)
      *outErr = L"note stage 情報が不正です。";
    return false;
  }
  auto snapshotBytes = ReadFileBytes(meta.stagePath);
  if (!snapshotBytes.has_value()) {
    if (outErr)
      *outErr = L"note stage checkpoint の読み込みに失敗しました。";
    return false;
  }

  NoteStageState state;
  if (!LoadNoteStageState(meta.stagePath, &state, outErr)) {
    return false;
  }

  ResolvedNoteStageData data;
  data.snapshotBytes = std::move(*snapshotBytes);
  data.committedBytes = data.snapshotBytes;
  for (const auto &seg : ListNoteJournalSegmentsForStage(meta.stagePath)) {
    if (seg.revision == 0 || seg.revision > state.appliedJournalRevision)
      continue;
    auto segBytes = ReadFileBytes(seg.path);
    if (!segBytes.has_value()) {
      if (outErr)
        *outErr = L"note journal の読み込みに失敗しました。";
      return false;
    }
    data.committedBytes += *segBytes;
  }
  data.state = std::move(state);
  data.resolvedBytes = data.committedBytes;
  data.resolvedBytes += data.state.textTailBytes;
  data.resolvedBytes += BuildBlankTailBytes(data.state.blankTailCount);
  for (const auto &seg : ListNoteEditJournalSegmentsForStage(meta.stagePath)) {
    if (seg.revision == 0 ||
        seg.revision > data.state.appliedEditJournalRevision)
      continue;
    auto segBytes = ReadFileBytes(seg.path);
    if (!segBytes.has_value()) {
      if (outErr)
        *outErr = L"note edit journal の読み込みに失敗しました。";
      return false;
    }
    auto edit = ParseNoteByteEdit(*segBytes, outErr);
    if (!edit.has_value())
      return false;
    if (!ApplyNoteByteEdit(&data.resolvedBytes, *edit, outErr))
      return false;
  }
  if (outData)
    *outData = std::move(data);
  return true;
}

PersistedSnapshotMatch
MatchNoteBytesToPersistedState(const std::wstring &notePath,
                               std::string_view bytes,
                               bool *outHasLatestStage = nullptr) {
  if (outHasLatestStage)
    *outHasLatestStage = false;
  if (notePath.empty())
    return PersistedSnapshotMatch::None;

  const auto latestStage =
      FindLatestStageMeta(file_output::StagedDiffKind::Note, notePath);
  if (latestStage.has_value()) {
    if (outHasLatestStage)
      *outHasLatestStage = true;
    ResolvedNoteStageData resolved;
    if (LoadResolvedStageNoteDataForMeta(*latestStage, &resolved) &&
        resolved.resolvedBytes == bytes) {
      return PersistedSnapshotMatch::Stage;
    }
  }

  auto fileBytes = ReadFileBytes(std::filesystem::path(notePath));
  if (fileBytes.has_value() && *fileBytes == bytes) {
    return PersistedSnapshotMatch::Original;
  }
  return PersistedSnapshotMatch::None;
}

bool DiscardDeferredEmptyNotePersistenceIfNeeded(HWND owner,
                                                 const std::wstring &notePath,
                                                 std::string_view bytes) {
  if (notePath.empty() || !bytes.empty())
    return false;

  const std::wstring noteKey =
      NormalizePathKey(std::filesystem::path(notePath));
  for (const auto &entry : g_noteFiles) {
    if (NormalizePathKey(std::filesystem::path(entry.path)) == noteKey) {
      return false;
    }
  }

  std::error_code existsEc;
  if (std::filesystem::exists(std::filesystem::path(notePath), existsEc) &&
      !existsEc) {
    return false;
  }

  file_output::DiscardOtherStagedNoteFilesFor(notePath, {});
  g_noteDirty = false;
  g_noteNeedsIntegrate = false;
  RecordPersistedNoteEditorLength();
  ResetNoteStageEditTracking();
  ClearCurrentNoteEditorModifiedFlag(notePath);
  RefreshCurrentNoteFileSnapshot();
  file_output::ConfigureAutoStageSaveScheduling(owner);
  RefreshDirtyUi(owner);
  return true;
}

bool LoadAnnotationSnapshotForCompare(const std::filesystem::path &clropPath,
                                      const std::wstring &pdfPath,
                                      std::vector<Annotation> *out) {
  if (!out || clropPath.empty() || pdfPath.empty())
    return false;
  bool mismatch = false;
  std::wstring err;
  out->clear();
  return clrop_bridge::LoadAnnotations(
      clropPath.wstring(), pdfPath, *out, mismatch, nullptr, err,
      clrop_bridge::LoadAnnotationsValidation::None);
}

bool AnnotationCompareNearlyEqual(double a, double b, double epsilon = 1e-6) {
  double scale = std::max({1.0, std::abs(a), std::abs(b)});
  return std::abs(a - b) <= epsilon * scale;
}

bool AnnotationCompareEqual(const Annotation &a, const Annotation &b) {
  if (a.type != b.type)
    return false;
  if (a.pageIndex != b.pageIndex)
    return false;
  if (!AnnotationCompareNearlyEqual(a.x1, b.x1) ||
      !AnnotationCompareNearlyEqual(a.y1, b.y1) ||
      !AnnotationCompareNearlyEqual(a.x2, b.x2) ||
      !AnnotationCompareNearlyEqual(a.y2, b.y2)) {
    return false;
  }
  if (!AnnotationCompareNearlyEqual(a.width, b.width) ||
      !AnnotationCompareNearlyEqual(a.alpha, b.alpha) ||
      !AnnotationCompareNearlyEqual(a.fontPt, b.fontPt)) {
    return false;
  }
  if (a.color != b.color)
    return false;
  if (a.text != b.text || a.textLines != b.textLines)
    return false;
  if (a.fontName != b.fontName)
    return false;
  if (a.writingMode != b.writingMode)
    return false;
  if (a.linkId != b.linkId || a.linkNotePath != b.linkNotePath)
    return false;
  if (a.mathKind != b.mathKind)
    return false;
  if (a.shapeKind != b.shapeKind)
    return false;
  if (a.shapeDrawMode != b.shapeDrawMode)
    return false;
  if (a.arrowHead != b.arrowHead)
    return false;
  if (!AnnotationCompareNearlyEqual(a.shapeRotation, b.shapeRotation))
    return false;
  if (a.path.size() != b.path.size())
    return false;
  for (size_t i = 0; i < a.path.size(); ++i) {
    if (!AnnotationCompareNearlyEqual(a.path[i].x, b.path[i].x) ||
        !AnnotationCompareNearlyEqual(a.path[i].y, b.path[i].y)) {
      return false;
    }
  }
  if (a.dash.size() != b.dash.size())
    return false;
  for (size_t i = 0; i < a.dash.size(); ++i) {
    if (!AnnotationCompareNearlyEqual(a.dash[i], b.dash[i]))
      return false;
  }
  if (a.quads.size() != b.quads.size())
    return false;
  for (size_t i = 0; i < a.quads.size(); ++i) {
    if (!AnnotationCompareNearlyEqual(a.quads[i], b.quads[i]))
      return false;
  }
  return true;
}

bool AnnotationVectorsEqual(const std::vector<Annotation> &lhs,
                            const std::vector<Annotation> &rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (!AnnotationCompareEqual(lhs[i], rhs[i]))
      return false;
  }
  return true;
}

std::optional<NoteByteEdit> ComputeNoteByteEdit(std::string_view before,
                                                std::string_view after) {
  size_t prefix = 0;
  const size_t minSize = std::min(before.size(), after.size());
  while (prefix < minSize && before[prefix] == after[prefix]) {
    ++prefix;
  }
  size_t beforeTail = before.size();
  size_t afterTail = after.size();
  while (beforeTail > prefix && afterTail > prefix &&
         before[beforeTail - 1] == after[afterTail - 1]) {
    --beforeTail;
    --afterTail;
  }
  NoteByteEdit edit;
  edit.start = prefix;
  edit.deletedBytes.assign(before.substr(prefix, beforeTail - prefix));
  edit.insertedBytes.assign(after.substr(prefix, afterTail - prefix));
  if (edit.deletedBytes.empty() && edit.insertedBytes.empty())
    return std::nullopt;
  return edit;
}

bool ApplyNoteByteEdit(std::string *bytes, const NoteByteEdit &edit,
                       std::wstring *outErr) {
  if (!bytes)
    return false;
  if (edit.start > bytes->size() ||
      edit.deletedBytes.size() > bytes->size() - edit.start) {
    if (outErr)
      *outErr = L"note edit journal の範囲が不正です。";
    return false;
  }
  if (bytes->compare(edit.start, edit.deletedBytes.size(), edit.deletedBytes) !=
      0) {
    if (outErr)
      *outErr = L"note edit journal の削除対象が一致しません。";
    return false;
  }
  bytes->replace(edit.start, edit.deletedBytes.size(), edit.insertedBytes);
  return true;
}

std::string SerializeNoteByteEdit(const NoteByteEdit &edit) {
  std::string bytes;
  bytes += "NEDJ1\n";
  bytes += "start=" + std::to_string(edit.start) + "\n";
  bytes += "delete_len=" + std::to_string(edit.deletedBytes.size()) + "\n";
  bytes += "insert_len=" + std::to_string(edit.insertedBytes.size()) + "\n";
  bytes += "--DATA--\n";
  bytes += edit.deletedBytes;
  bytes += edit.insertedBytes;
  return bytes;
}

std::optional<NoteByteEdit> ParseNoteByteEdit(std::string_view bytes,
                                              std::wstring *outErr) {
  static const std::string kMarker = "\n--DATA--\n";
  const size_t markerPos = bytes.find(kMarker);
  if (markerPos == std::string_view::npos) {
    if (outErr)
      *outErr = L"note edit journal の形式が不正です。";
    return std::nullopt;
  }
  std::istringstream iss(std::string(bytes.substr(0, markerPos + 1)));
  std::string line;
  size_t start = 0;
  size_t deleteLen = 0;
  size_t insertLen = 0;
  bool hasStart = false;
  bool hasDeleteLen = false;
  bool hasInsertLen = false;
  if (!std::getline(iss, line) || line != "NEDJ1") {
    if (outErr)
      *outErr = L"note edit journal のヘッダが不正です。";
    return std::nullopt;
  }
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    auto parsed = ParseUint64(UTF8ToWide(value));
    if (!parsed.has_value()) {
      if (outErr)
        *outErr = L"note edit journal の数値が不正です。";
      return std::nullopt;
    }
    if (key == "start") {
      start = static_cast<size_t>(*parsed);
      hasStart = true;
    } else if (key == "delete_len") {
      deleteLen = static_cast<size_t>(*parsed);
      hasDeleteLen = true;
    } else if (key == "insert_len") {
      insertLen = static_cast<size_t>(*parsed);
      hasInsertLen = true;
    }
  }
  const size_t dataStart = markerPos + kMarker.size();
  if (!hasStart || !hasDeleteLen || !hasInsertLen ||
      deleteLen > bytes.size() - dataStart ||
      insertLen > bytes.size() - dataStart - deleteLen) {
    if (outErr)
      *outErr = L"note edit journal のサイズが不正です。";
    return std::nullopt;
  }
  NoteByteEdit edit;
  edit.start = start;
  edit.deletedBytes.assign(bytes.substr(dataStart, deleteLen));
  edit.insertedBytes.assign(bytes.substr(dataStart + deleteLen, insertLen));
  return edit;
}

bool WriteNoteEditJournalSegment(const std::filesystem::path &stagePath,
                                 uint64_t revision, const NoteByteEdit &edit,
                                 std::wstring *outErr = nullptr) {
  if (stagePath.empty()) {
    if (outErr)
      *outErr = L"note edit journal stage path が不正です。";
    return false;
  }
  const auto dir = NoteJournalDir();
  if (!EnsureDir(dir)) {
    if (outErr)
      *outErr = L"note edit journal フォルダを作成できません。";
    return false;
  }
  const std::string bytes = SerializeNoteByteEdit(edit);
  const auto path = BuildNoteEditJournalPath(stagePath, revision);
  std::wstring err;
  if (!atomic_write::AtomicWriteBytes(path, bytes.data(), bytes.size(),
                                      path.parent_path(), WorkspaceEscapeRoot(),
                                      &err)) {
    if (outErr)
      *outErr = err;
    return false;
  }
  return true;
}

bool StageResolvedContentMatchesOriginal(const StageMeta &meta) {
  if (meta.targetPath.empty())
    return false;

  if (meta.kind == file_output::StagedDiffKind::Note) {
    auto originalBytes = ReadFileBytes(std::filesystem::path(meta.targetPath));
    if (!originalBytes.has_value())
      return false;

    ResolvedNoteStageData resolved;
    if (!LoadResolvedStageNoteDataForMeta(meta, &resolved))
      return false;
    return resolved.resolvedBytes == *originalBytes;
  }

  if (meta.kind == file_output::StagedDiffKind::Clrop) {
    std::vector<Annotation> staged;
    if (!LoadResolvedStageAnnotationsForMeta(meta, &staged))
      return false;

    const std::filesystem::path clropPath(
        clrop_bridge::ClropPathForPdf(meta.targetPath));
    std::error_code existsEc;
    const bool clropExists = !clropPath.empty() &&
                             std::filesystem::exists(clropPath, existsEc) &&
                             !existsEc;
    if (!clropExists) {
      return staged.empty();
    }

    std::vector<Annotation> original;
    if (!LoadAnnotationSnapshotForCompare(clropPath, meta.targetPath,
                                          &original))
      return false;
    return AnnotationVectorsEqual(staged, original);
  }

  return false;
}

PersistedSnapshotMatch
MatchAnnotationSnapshotToPersistedState(const std::wstring &pdfPath,
                                        const std::vector<Annotation> &annots,
                                        bool *outHasLatestStage = nullptr) {
  if (outHasLatestStage)
    *outHasLatestStage = false;
  if (pdfPath.empty())
    return PersistedSnapshotMatch::None;

  const auto latestStage =
      FindLatestStageMeta(file_output::StagedDiffKind::Clrop, pdfPath);
  if (latestStage.has_value()) {
    if (outHasLatestStage)
      *outHasLatestStage = true;
    std::vector<Annotation> staged;
    if (LoadResolvedStageAnnotationsForMeta(*latestStage, &staged) &&
        AnnotationVectorsEqual(staged, annots)) {
      return PersistedSnapshotMatch::Stage;
    }
  }

  const std::filesystem::path clropPath(clrop_bridge::ClropPathForPdf(pdfPath));
  std::error_code existsEc;
  const bool clropExists = !clropPath.empty() &&
                           std::filesystem::exists(clropPath, existsEc) &&
                           !existsEc;
  if (!clropExists) {
    return annots.empty() ? PersistedSnapshotMatch::Original
                          : PersistedSnapshotMatch::None;
  }

  std::vector<Annotation> loaded;
  if (LoadAnnotationSnapshotForCompare(clropPath, pdfPath, &loaded) &&
      AnnotationVectorsEqual(loaded, annots)) {
    return PersistedSnapshotMatch::Original;
  }
  return PersistedSnapshotMatch::None;
}

void UpdateCurrentIntegrateFlags() {
  if (!g_currentNotePath.empty()) {
    g_noteNeedsIntegrate =
        FindLatestStageMeta(file_output::StagedDiffKind::Note,
                            g_currentNotePath)
            .has_value();
    if (!g_noteNeedsIntegrate)
      g_noteDirty = false;
  }
  const std::wstring logicalPdfPath = CurrentLogicalPdfPath();
  if (!logicalPdfPath.empty()) {
    g_annotsNeedsIntegrate =
        FindLatestStageMeta(file_output::StagedDiffKind::Clrop, logicalPdfPath)
            .has_value();
    if (!g_annotsNeedsIntegrate)
      g_annotsDirty = false;
  }
}

uint64_t NextRevisionFor(file_output::StagedDiffKind kind,
                         const std::wstring &targetPath) {
  uint64_t best = CurrentEditRevision();
  for (const auto &meta : LoadAllStageMeta(kind)) {
    if (NormalizePathKey(std::filesystem::path(meta.targetPath)) !=
        NormalizePathKey(std::filesystem::path(targetPath))) {
      continue;
    }
    best = std::max(best, meta.revision.value_or(0));
  }
  if (kind == file_output::StagedDiffKind::Clrop) {
    for (const auto &seg : ListAnnotJournalSegmentsForTarget(targetPath)) {
      best = std::max(best, seg.revision);
    }
  }
  return best + 1;
}

std::optional<StageMeta> StageNoteSnapshotWithBytes(
    const std::wstring &notePath, std::string_view bytes,
    const note::SnapshotIdentity *sourceIdentity = nullptr,
    std::wstring *outErr = nullptr) {
  if (notePath.empty()) {
    if (outErr)
      *outErr = L"ノートが開かれていません。";
    return std::nullopt;
  }
  const auto dir = StageDir(file_output::StagedDiffKind::Note);
  if (!EnsureDir(dir)) {
    if (outErr)
      *outErr = L"ノート stage フォルダを作成できません。";
    return std::nullopt;
  }

  StageMeta meta;
  meta.kind = file_output::StagedDiffKind::Note;
  meta.targetPath = notePath;
  meta.destPath = std::filesystem::path(notePath);
  meta.revision = NextRevisionFor(meta.kind, meta.targetPath);
  bool destinationExisted = false;
  uint64_t destinationFingerprint = 0;
  if (!CaptureStageDestinationObservation(meta.destPath, &destinationExisted,
                                          &destinationFingerprint, outErr)) {
    return std::nullopt;
  }
  meta.destinationExistedAtStage = destinationExisted;
  meta.destinationFingerprintAtStage = destinationFingerprint;
  std::string textCoreBytes;
  note::SnapshotIdentity textCoreIdentity;
  const bool textCoreMatches =
      CaptureCurrentNoteTextCoreUtf8(notePath, &textCoreBytes,
                                     &textCoreIdentity) &&
      note::FingerprintSnapshotBytes(textCoreBytes) ==
          note::FingerprintSnapshotBytes(bytes);
  std::wstring identityErr;
  const note::NoteIdentity identity =
      note::ResolveRuntimeNoteIdentityPath(notePath, &identityErr);
  if (!identity.note_id.valid()) {
    if (outErr)
      *outErr = identityErr.empty() ? L"ノートの永続IDを確定できません。"
                                    : identityErr;
    return std::nullopt;
  }
  const uint64_t snapshotFingerprint = note::FingerprintSnapshotBytes(bytes);
  auto applyContentIdentity = [&](const note::SnapshotIdentity &candidate) {
    if (!candidate.valid() || candidate.content_revision == 0 ||
        candidate.note_id != identity.note_id ||
        candidate.content_fingerprint != snapshotFingerprint) {
      return;
    }
    meta.contentRevision = candidate.content_revision;
  };
  if (sourceIdentity) {
    applyContentIdentity(*sourceIdentity);
  }
  if (textCoreMatches) {
    applyContentIdentity(textCoreIdentity);
  }
  if (const auto durableRecord =
          note::FindRuntimeNotePersistenceRecord(identity.note_id)) {
    meta.noteId = identity.note_id.value;
    uint64_t basePersistenceRevision = durableRecord->persistence_revision;
    std::string diskBytes;
    if (ReadFileBytesWin32(meta.destPath, diskBytes)) {
      basePersistenceRevision = note::ObserveRuntimeNoteDiskSnapshot(
          identity.note_id, note::FingerprintSnapshotBytes(diskBytes),
          &identityErr);
      if (basePersistenceRevision == 0 && !identityErr.empty()) {
        if (outErr)
          *outErr = identityErr;
        return std::nullopt;
      }
    }
    if (basePersistenceRevision == UINT64_MAX) {
      if (outErr)
        *outErr = L"ノートの永続化リビジョンが上限に達しました。";
      return std::nullopt;
    }
    meta.basePersistenceRevision = basePersistenceRevision;
    meta.persistenceRevision = basePersistenceRevision + 1;
  }
  meta.noteRestoreView = CaptureCurrentNoteRestoreViewState(
      meta.targetPath, meta.contentRevision.value_or(0));
  meta.stagePath = BuildStagePath(meta.kind, meta.targetPath, *meta.revision);
  meta.metaPath = StageMetaPath(meta.stagePath);

  std::wstring err;
  if (!atomic_write::AtomicWriteBytes(
          meta.stagePath, bytes.data(), bytes.size(),
          meta.stagePath.parent_path(), WorkspaceEscapeRoot(), &err)) {
    if (outErr)
      *outErr = err;
    return std::nullopt;
  }
  if (!WriteStageMetaFile(meta, &err)) {
    RemoveStageAndMeta(meta.stagePath);
    if (outErr)
      *outErr = err;
    return std::nullopt;
  }
  return meta;
}

struct NoteStageAppendPlan {
  StageMeta meta;
  NoteStageState nextState;
  std::string journalAddBytes;
};

struct NoteStageEditPlan {
  StageMeta meta;
  NoteStageState nextState;
  NoteByteEdit edit;
};

bool TryBuildNoteStageAppendPlan(const StageMeta &meta,
                                 std::string_view currentBytes,
                                 NoteStageAppendPlan *outPlan,
                                 std::wstring *outErr = nullptr) {
  if (outPlan)
    *outPlan = {};
  ResolvedNoteStageData resolved;
  if (!LoadResolvedStageNoteDataForMeta(meta, &resolved, outErr)) {
    return false;
  }
  if (currentBytes.size() < resolved.committedBytes.size() ||
      currentBytes.compare(0, resolved.committedBytes.size(),
                           resolved.committedBytes) != 0) {
    return false;
  }

  NoteStageAppendPlan plan;
  plan.meta = meta;
  plan.nextState = resolved.state;

  const std::string_view remainder =
      currentBytes.substr(resolved.committedBytes.size());
  DecomposeNoteRemainder(remainder, &plan.journalAddBytes,
                         &plan.nextState.textTailBytes,
                         &plan.nextState.blankTailCount);
  if (!plan.journalAddBytes.empty()) {
    plan.nextState.appliedJournalRevision =
        NextNoteJournalRevisionForStage(meta.stagePath);
  }
  if (outPlan)
    *outPlan = std::move(plan);
  return true;
}

bool ApplyNoteStageAppendPlan(const NoteStageAppendPlan &plan,
                              std::wstring *outErr = nullptr) {
  if (plan.meta.stagePath.empty()) {
    if (outErr)
      *outErr = L"note stage path が不正です。";
    return false;
  }
  if (!plan.journalAddBytes.empty()) {
    if (!WriteNoteJournalSegment(plan.meta.stagePath,
                                 plan.nextState.appliedJournalRevision,
                                 plan.journalAddBytes, outErr)) {
      return false;
    }
  }
  return SaveNoteStageState(plan.meta.stagePath, plan.nextState, outErr);
}

bool TryBuildNoteStageEditPlan(const StageMeta &meta,
                               std::string_view currentBytes,
                               NoteStageEditPlan *outPlan,
                               std::wstring *outErr = nullptr) {
  if (outPlan)
    *outPlan = {};
  ResolvedNoteStageData resolved;
  if (!LoadResolvedStageNoteDataForMeta(meta, &resolved, outErr)) {
    return false;
  }
  auto edit = ComputeNoteByteEdit(resolved.resolvedBytes, currentBytes);
  if (!edit.has_value())
    return false;

  NoteStageEditPlan plan;
  plan.meta = meta;
  plan.nextState = resolved.state;
  plan.nextState.appliedEditJournalRevision =
      NextNoteEditJournalRevisionForStage(meta.stagePath);
  plan.edit = std::move(*edit);
  if (outPlan)
    *outPlan = std::move(plan);
  return true;
}

bool ApplyNoteStageEditPlan(const NoteStageEditPlan &plan,
                            std::wstring *outErr = nullptr) {
  if (plan.meta.stagePath.empty()) {
    if (outErr)
      *outErr = L"note stage path が不正です。";
    return false;
  }
  if (!WriteNoteEditJournalSegment(plan.meta.stagePath,
                                   plan.nextState.appliedEditJournalRevision,
                                   plan.edit, outErr)) {
    return false;
  }
  return SaveNoteStageState(plan.meta.stagePath, plan.nextState, outErr);
}

std::optional<StageMeta>
StageAnnotationCheckpointWithData(const std::wstring &pdfPath,
                                  const std::vector<Annotation> &src,
                                  std::wstring *outErr = nullptr) {
  if (pdfPath.empty()) {
    if (outErr)
      *outErr = L"PDF が開かれていません。";
    return std::nullopt;
  }
  const auto dir = StageDir(file_output::StagedDiffKind::Clrop);
  if (!EnsureDir(dir)) {
    if (outErr)
      *outErr = L"注釈 stage フォルダを作成できません。";
    return std::nullopt;
  }
  StageMeta meta;
  meta.kind = file_output::StagedDiffKind::Clrop;
  meta.targetPath = pdfPath;
  meta.destPath = std::filesystem::path(clrop_bridge::ClropPathForPdf(pdfPath));
  meta.revision = NextRevisionFor(meta.kind, meta.targetPath);
  bool destinationExisted = false;
  uint64_t destinationFingerprint = 0;
  if (!CaptureStageDestinationObservation(meta.destPath, &destinationExisted,
                                          &destinationFingerprint, outErr)) {
    return std::nullopt;
  }
  meta.destinationExistedAtStage = destinationExisted;
  meta.destinationFingerprintAtStage = destinationFingerprint;
  meta.stagePath = BuildStagePath(meta.kind, meta.targetPath, *meta.revision);
  meta.metaPath = StageMetaPath(meta.stagePath);

  std::wstring err;
  if (!clrop_bridge::SaveAnnotations(meta.stagePath.wstring(), pdfPath, src,
                                     err)) {
    if (outErr)
      *outErr = err;
    return std::nullopt;
  }
  if (!WriteStageMetaFile(meta, &err)) {
    RemoveStageAndMeta(meta.stagePath);
    if (outErr)
      *outErr = err;
    return std::nullopt;
  }
  return meta;
}

bool WriteAnnotJournalSegment(const std::wstring &pdfPath,
                              uint64_t baseRevision,
                              const std::vector<AnnotCommand> &commands,
                              std::wstring *outErr = nullptr) {
  if (pdfPath.empty() || commands.empty()) {
    if (outErr)
      *outErr = L"注釈 journal の内容がありません。";
    return false;
  }
  const auto dir = AnnotJournalDir();
  if (!EnsureDir(dir)) {
    if (outErr)
      *outErr = L"注釈 journal フォルダを作成できません。";
    return false;
  }
  std::string json;
  if (!SerializeAnnotCommandsJson(commands, &json)) {
    if (outErr)
      *outErr = L"注釈 journal のシリアライズに失敗しました。";
    return false;
  }
  const uint64_t revision =
      NextRevisionFor(file_output::StagedDiffKind::Clrop, pdfPath);
  const auto path = BuildAnnotJournalPath(pdfPath, baseRevision, revision);
  std::wstring err;
  if (!atomic_write::AtomicWriteUtf8(path, json, path.parent_path(),
                                     WorkspaceEscapeRoot(), &err)) {
    if (outErr)
      *outErr = err;
    return false;
  }
  return true;
}

bool TryAppendPendingAnnotJournal(const std::wstring &pdfPath,
                                  std::wstring *outErr = nullptr) {
  std::vector<AnnotCommand> pendingCommands;
  const auto latestStage =
      FindLatestStageMeta(file_output::StagedDiffKind::Clrop, pdfPath);
  if (pdfPath.empty() || !latestStage.has_value() ||
      !latestStage->revision.has_value() || g_pdf.editingText ||
      !CollectPendingAnnotStageCommands(pdfPath, &pendingCommands) ||
      pendingCommands.empty()) {
    return false;
  }
  return WriteAnnotJournalSegment(pdfPath, *latestStage->revision,
                                  pendingCommands, outErr);
}

struct NotePersistenceCommitPlan {
  note::NoteId noteId{};
  uint64_t expectedBaseRevision = 0;
  uint64_t committedRevision = 0;
  uint64_t contentFingerprint = 0;
  bool persistenceCommitRequired = false;
  bool destinationAlreadyWritten = false;
};

bool PrepareNotePersistenceCommit(const StageMeta &meta,
                                  std::string_view resolvedBytes,
                                  NotePersistenceCommitPlan *outPlan,
                                  std::wstring *outErr) {
  if (outPlan)
    *outPlan = {};
  std::wstring identityErr;
  const note::NoteIdentity identity =
      note::ResolveRuntimeNoteIdentityPath(meta.targetPath, &identityErr);
  if (!identity.note_id.valid()) {
    if (outErr)
      *outErr = identityErr.empty() ? L"ノートの永続IDを確定できません。"
                                    : identityErr;
    return false;
  }

  const uint64_t resolvedFingerprint =
      note::FingerprintSnapshotBytes(resolvedBytes);
  uint64_t currentRevision = 0;
  uint64_t currentFingerprint = 0;
  std::string diskBytes;
  const bool destinationExists = RegularFileExistsWin32(meta.destPath);
  if (destinationExists) {
    if (!ReadFileBytesWin32(meta.destPath, diskBytes)) {
      if (outErr)
        *outErr = L"統合前のノート原本を読み込めません。";
      return false;
    }
    currentFingerprint = note::FingerprintSnapshotBytes(diskBytes);
  }

  const auto currentRecord =
      note::FindRuntimeNotePersistenceRecord(identity.note_id);
  if (!currentRecord.has_value()) {
    if (meta.noteId.has_value() && *meta.noteId != identity.note_id.value) {
      const auto stageRecord =
          note::FindRuntimeNotePersistenceRecord(note::NoteId{*meta.noteId});
      if (meta.basePersistenceRevision.has_value() ||
          meta.persistenceRevision.has_value() || stageRecord.has_value()) {
        if (outErr)
          *outErr = L"note stage の永続IDが統合先と一致しません。";
        return false;
      }
    }
    if (outPlan) {
      outPlan->noteId = identity.note_id;
      outPlan->contentFingerprint = resolvedFingerprint;
      outPlan->destinationAlreadyWritten =
          destinationExists && currentFingerprint == resolvedFingerprint;
    }
    return true;
  }

  if (meta.noteId.has_value() && *meta.noteId != identity.note_id.value) {
    const auto stageRecord =
        note::FindRuntimeNotePersistenceRecord(note::NoteId{*meta.noteId});
    if (meta.basePersistenceRevision.has_value() ||
        meta.persistenceRevision.has_value() || stageRecord.has_value()) {
      if (outErr)
        *outErr = L"note stage の永続IDが統合先と一致しません。";
      return false;
    }
  }

  if (destinationExists) {
    currentRevision = note::ObserveRuntimeNoteDiskSnapshot(
        identity.note_id, currentFingerprint, &identityErr);
    if (currentRevision == 0 && !identityErr.empty()) {
      if (outErr)
        *outErr = identityErr;
      return false;
    }
  } else {
    currentRevision = currentRecord->persistence_revision;
  }

  const uint64_t expectedBase =
      meta.basePersistenceRevision.value_or(currentRevision);
  uint64_t committed = meta.persistenceRevision.value_or(0);
  if (!meta.persistenceRevision.has_value() && expectedBase != UINT64_MAX) {
    committed = expectedBase + 1;
  }
  const note::NotePersistenceObservation observation{
      identity.note_id,
      destinationExists,
      currentRevision,
      currentFingerprint,
  };
  const note::NotePersistenceCommitIntent intent{
      identity.note_id,
      expectedBase,
      committed,
      resolvedFingerprint,
  };
  const note::NotePersistenceCommitDecision decision =
      note::ResolveNotePersistenceCommit(observation, intent);
  if (decision == note::NotePersistenceCommitDecision::InvalidInput) {
    if (outErr) {
      if (intent.expected_base_revision == UINT64_MAX) {
        *outErr = L"ノートの永続化リビジョンが上限に達しました。";
      } else {
        *outErr = L"note stage の永続化意図または原本観測が不正です。";
      }
    }
    return false;
  }
  if (decision == note::NotePersistenceCommitDecision::Conflict) {
    if (outErr)
      *outErr = L"stage 作成後にノート原本が変更されたため統合を中止しました。";
    return false;
  }

  if (outPlan) {
    outPlan->noteId = identity.note_id;
    outPlan->expectedBaseRevision = expectedBase;
    outPlan->committedRevision = committed;
    outPlan->contentFingerprint = resolvedFingerprint;
    outPlan->persistenceCommitRequired = true;
    outPlan->destinationAlreadyWritten =
        decision == note::NotePersistenceCommitDecision::AlreadyWritten;
  }
  return true;
}

bool IntegrateStageMetaToDestination(HWND owner, const StageMeta &meta,
                                     bool discardOtherIfLatest,
                                     std::wstring *outErr = nullptr,
                                     bool skipBackup = false) {
  const ULONGLONG startTick = preview_trace::TickNow();
  preview_trace::Append(L"IntegrateStage",
                        L"begin dest=" + meta.destPath.wstring());
  PumpSaveScrollMessages(owner);
  if (meta.destPath.empty()) {
    if (outErr)
      *outErr = L"統合先が不正です。";
    TraceSaveFailure(L"IntegrateStage", L"dest_empty", meta.targetPath,
                     meta.stagePath, meta.destPath, outErr ? *outErr : L"");
    return false;
  }
  if (IsUnsupportedPath(meta.destPath)) {
    if (outErr)
      *outErr = L"UNC/デバイスパスには統合できません。";
    TraceSaveFailure(L"IntegrateStage", L"unsupported_path", meta.targetPath,
                     meta.stagePath, meta.destPath, outErr ? *outErr : L"");
    return false;
  }
  std::string bytesToWrite;
  bool deleteEmptyClrop = false;
  if (meta.kind == file_output::StagedDiffKind::Clrop) {
    std::vector<Annotation> resolved;
    const ULONGLONG resolveStartTick = preview_trace::TickNow();
    if (!LoadResolvedStageAnnotationsForMeta(meta, &resolved, outErr)) {
      TraceSaveFailure(L"IntegrateStage", L"resolve_clrop_failed",
                       meta.targetPath, meta.stagePath, meta.destPath,
                       outErr ? *outErr : L"");
      return false;
    }
    preview_trace::Append(L"IntegrateStage",
                          L"resolve_clrop elapsed_ms=" +
                              preview_trace::ElapsedMs(resolveStartTick));
    deleteEmptyClrop = resolved.empty();
    if (!deleteEmptyClrop) {
      std::wstring serializeErr;
      const ULONGLONG serializeStartTick = preview_trace::TickNow();
      if (!clrop_bridge::SerializeAnnotations(meta.targetPath, resolved,
                                              bytesToWrite, serializeErr)) {
        if (outErr)
          *outErr = serializeErr;
        TraceSaveFailure(L"IntegrateStage", L"serialize_clrop_failed",
                         meta.targetPath, meta.stagePath, meta.destPath,
                         serializeErr);
        return false;
      }
      preview_trace::Append(L"IntegrateStage",
                            L"serialize_clrop elapsed_ms=" +
                                preview_trace::ElapsedMs(serializeStartTick));
    }
  } else if (meta.kind == file_output::StagedDiffKind::Note) {
    ResolvedNoteStageData resolved;
    if (!LoadResolvedStageNoteDataForMeta(meta, &resolved, outErr)) {
      TraceSaveFailure(L"IntegrateStage", L"resolve_note_failed",
                       meta.targetPath, meta.stagePath, meta.destPath,
                       outErr ? *outErr : L"");
      return false;
    }
    bytesToWrite = std::move(resolved.resolvedBytes);
  } else {
    auto stageBytes = ReadFileBytes(meta.stagePath);
    if (!stageBytes.has_value()) {
      if (outErr)
        *outErr = L"stage の読み込みに失敗しました。";
      TraceSaveFailure(L"IntegrateStage", L"read_stage_failed", meta.targetPath,
                       meta.stagePath, meta.destPath, outErr ? *outErr : L"");
      return false;
    }
    bytesToWrite = std::move(*stageBytes);
  }
  PumpSaveScrollMessages(owner);

  NotePersistenceCommitPlan notePersistence;
  if (meta.kind == file_output::StagedDiffKind::Note &&
      !PrepareNotePersistenceCommit(meta, bytesToWrite, &notePersistence,
                                    outErr)) {
    TraceSaveFailure(L"IntegrateStage", L"note_revision_conflict",
                     meta.targetPath, meta.stagePath, meta.destPath,
                     outErr ? *outErr : L"");
    return false;
  }

  std::wstring backupErr;
  if (!notePersistence.destinationAlreadyWritten && !skipBackup &&
      !CreateBackupIfNeeded(meta.kind, meta.destPath, nullptr, nullptr,
                            &backupErr)) {
    if (outErr)
      *outErr = backupErr;
    TraceSaveFailure(L"IntegrateStage", L"backup_failed", meta.targetPath,
                     meta.stagePath, meta.destPath, backupErr);
    return false;
  }
  PumpSaveScrollMessages(owner);

  std::wstring writeErr;
  if (!notePersistence.destinationAlreadyWritten) {
    if (!DestinationStillMatchesStageObservation(meta, &writeErr)) {
      if (outErr)
        *outErr = writeErr;
      TraceSaveFailure(L"IntegrateStage", L"destination_generation_conflict",
                       meta.targetPath, meta.stagePath, meta.destPath, writeErr);
      return false;
    }
    const ULONGLONG writeStartTick = preview_trace::TickNow();
    const bool atomicWriteOk = deleteEmptyClrop
        ? DeleteResolvedEmptyClropDestination(meta, &writeErr)
        : AtomicWriteVerified(meta.destPath, bytesToWrite, &writeErr);
    preview_trace::Append(
        L"IntegrateStage",
        std::wstring(deleteEmptyClrop ? L"delete_empty_clrop"
                                      : L"atomic_write_verified") +
            L" elapsed_ms=" + preview_trace::ElapsedMs(writeStartTick));
    if (!atomicWriteOk) {
      if (outErr)
        *outErr = writeErr;
      TraceSaveFailure(L"IntegrateStage",
                       deleteEmptyClrop ? L"delete_empty_clrop_failed"
                                        : L"atomic_write_verify_failed",
                       meta.targetPath, meta.stagePath, meta.destPath,
                       writeErr);
      return false;
    }
  }
  PumpSaveScrollMessages(owner);

  if (meta.kind == file_output::StagedDiffKind::Note &&
      notePersistence.persistenceCommitRequired &&
      !note::CommitRuntimeNotePersistence(
          notePersistence.noteId, notePersistence.expectedBaseRevision,
          notePersistence.committedRevision, notePersistence.contentFingerprint,
          &writeErr)) {
    if (outErr)
      *outErr = writeErr;
    TraceSaveFailure(L"IntegrateStage", L"note_revision_commit_failed",
                     meta.targetPath, meta.stagePath, meta.destPath, writeErr);
    return false;
  }
  if (meta.kind == file_output::StagedDiffKind::Note) {
    RefreshCurrentNotePersistenceIdentity(meta.targetPath);
  }

  const auto latest = FindLatestStageMeta(meta.kind, meta.targetPath);
  const bool integratedWasLatest =
      latest.has_value() && IsSamePath(latest->stagePath, meta.stagePath);
  RemoveStageAndMeta(meta.stagePath);
  if (meta.kind == file_output::StagedDiffKind::Clrop) {
    DiscardAnnotJournalSegmentsForBase(meta.targetPath,
                                       meta.revision.value_or(0));
  } else if (meta.kind == file_output::StagedDiffKind::Note) {
    DiscardNoteStageArtifactsForStage(meta.stagePath);
  }
  if (discardOtherIfLatest && integratedWasLatest) {
    if (meta.kind == file_output::StagedDiffKind::Note) {
      file_output::DiscardOtherStagedNoteFilesFor(meta.targetPath, {});
    } else {
      file_output::DiscardOtherStagedClropFilesForPdf(meta.targetPath, {});
    }
  }
  UpdateCurrentIntegrateFlags();
  if (meta.kind == file_output::StagedDiffKind::Clrop) {
    InvalidateAnnotHistoryForPath(meta.destPath.wstring());
  } else if (meta.kind == file_output::StagedDiffKind::Note &&
             NormalizePathKey(meta.targetPath) ==
                 NormalizePathKey(g_currentNotePath)) {
    AddIntegratedCurrentNoteToListIfNeeded(owner, meta.targetPath);
    RefreshCurrentNoteFileSnapshot();
  }
  RefreshDirtyUi(owner);
  preview_trace::Append(L"IntegrateStage",
                        L"end elapsed_ms=" +
                            preview_trace::ElapsedMs(startTick));
  return true;
}

struct BackgroundSaveWorkerResult {
  bool ok = true;
  std::wstring error;
  std::vector<file_output::StagedDiffEntry> integrated;
  uint64_t snapshotRevision = 0;
  ULONGLONG startTick = 0;
};

StageMeta StageMetaFromEntry(const file_output::StagedDiffEntry &entry) {
  const std::filesystem::path metaPath =
      entry.metaPath.empty() ? StageMetaPath(entry.stagePath) : entry.metaPath;
  StageMeta persisted;
  if (LoadStageMetaFile(metaPath, &persisted) &&
      IsSamePath(persisted.stagePath, entry.stagePath)) {
    return persisted;
  }
  if (RegularFileExistsWin32(metaPath))
    return {};
  StageMeta meta;
  meta.kind = entry.kind;
  meta.stagePath = entry.stagePath;
  meta.metaPath = metaPath;
  meta.destPath = entry.destPath;
  meta.targetPath = entry.targetPath;
  meta.revision = entry.revision;
  return meta;
}

bool WriteStageMetaToDestinationOnWorker(const StageMeta &meta,
                                         std::wstring *outErr = nullptr) {
  if (meta.destPath.empty()) {
    if (outErr)
      *outErr = L"統合先が不正です。";
    TraceSaveFailure(L"BackgroundSave", L"dest_empty", meta.targetPath,
                     meta.stagePath, meta.destPath, outErr ? *outErr : L"");
    return false;
  }
  if (IsUnsupportedPath(meta.destPath)) {
    if (outErr)
      *outErr = L"UNC/デバイスパスには統合できません。";
    TraceSaveFailure(L"BackgroundSave", L"unsupported_path", meta.targetPath,
                     meta.stagePath, meta.destPath, outErr ? *outErr : L"");
    return false;
  }

  std::string bytesToWrite;
  bool deleteEmptyClrop = false;
  if (meta.kind == file_output::StagedDiffKind::Clrop) {
    std::vector<Annotation> resolved;
    if (!LoadResolvedStageAnnotationsForMeta(meta, &resolved, outErr)) {
      TraceSaveFailure(L"BackgroundSave", L"resolve_clrop_failed",
                       meta.targetPath, meta.stagePath, meta.destPath,
                       outErr ? *outErr : L"");
      return false;
    }
    deleteEmptyClrop = resolved.empty();
    if (!deleteEmptyClrop) {
      std::wstring serializeErr;
      if (!clrop_bridge::SerializeAnnotations(meta.targetPath, resolved,
                                              bytesToWrite, serializeErr)) {
        if (outErr)
          *outErr = serializeErr;
        TraceSaveFailure(L"BackgroundSave", L"serialize_clrop_failed",
                         meta.targetPath, meta.stagePath, meta.destPath,
                         serializeErr);
        return false;
      }
    }
  } else if (meta.kind == file_output::StagedDiffKind::Note) {
    ResolvedNoteStageData resolved;
    if (!LoadResolvedStageNoteDataForMeta(meta, &resolved, outErr)) {
      TraceSaveFailure(L"BackgroundSave", L"resolve_note_failed",
                       meta.targetPath, meta.stagePath, meta.destPath,
                       outErr ? *outErr : L"");
      return false;
    }
    bytesToWrite = std::move(resolved.resolvedBytes);
  } else {
    auto stageBytes = ReadFileBytes(meta.stagePath);
    if (!stageBytes.has_value()) {
      if (outErr)
        *outErr = L"stage の読み込みに失敗しました。";
      TraceSaveFailure(L"BackgroundSave", L"read_stage_failed", meta.targetPath,
                       meta.stagePath, meta.destPath, outErr ? *outErr : L"");
      return false;
    }
    bytesToWrite = std::move(*stageBytes);
  }

  NotePersistenceCommitPlan notePersistence;
  if (meta.kind == file_output::StagedDiffKind::Note &&
      !PrepareNotePersistenceCommit(meta, bytesToWrite, &notePersistence,
                                    outErr)) {
    TraceSaveFailure(L"BackgroundSave", L"note_revision_conflict",
                     meta.targetPath, meta.stagePath, meta.destPath,
                     outErr ? *outErr : L"");
    return false;
  }

  std::wstring backupErr;
  if (!notePersistence.destinationAlreadyWritten &&
      !CreateBackupIfNeeded(meta.kind, meta.destPath, nullptr, nullptr,
                            &backupErr)) {
    if (outErr)
      *outErr = backupErr;
    TraceSaveFailure(L"BackgroundSave", L"backup_failed", meta.targetPath,
                     meta.stagePath, meta.destPath, backupErr);
    return false;
  }

  std::wstring writeErr;
  if (!notePersistence.destinationAlreadyWritten) {
    if (!DestinationStillMatchesStageObservation(meta, &writeErr)) {
      if (outErr)
        *outErr = writeErr;
      TraceSaveFailure(L"BackgroundSave", L"destination_generation_conflict",
                       meta.targetPath, meta.stagePath, meta.destPath, writeErr);
      return false;
    }
    const bool writeOk = deleteEmptyClrop
        ? DeleteResolvedEmptyClropDestination(meta, &writeErr)
        : AtomicWriteVerified(meta.destPath, bytesToWrite, &writeErr);
    if (!writeOk) {
      if (outErr)
        *outErr = writeErr;
      TraceSaveFailure(L"BackgroundSave",
                       deleteEmptyClrop ? L"delete_empty_clrop_failed"
                                        : L"atomic_write_verify_failed",
                       meta.targetPath, meta.stagePath, meta.destPath, writeErr);
      return false;
    }
  }
  if (meta.kind == file_output::StagedDiffKind::Note &&
      notePersistence.persistenceCommitRequired &&
      !note::CommitRuntimeNotePersistence(
          notePersistence.noteId, notePersistence.expectedBaseRevision,
          notePersistence.committedRevision, notePersistence.contentFingerprint,
          &writeErr)) {
    if (outErr)
      *outErr = writeErr;
    TraceSaveFailure(L"BackgroundSave", L"note_revision_commit_failed",
                     meta.targetPath, meta.stagePath, meta.destPath, writeErr);
    return false;
  }
  return true;
}

void CleanupIntegratedStageOnUiThread(
    const file_output::StagedDiffEntry &entry) {
  const StageMeta meta = StageMetaFromEntry(entry);
  const auto latest = FindLatestStageMeta(meta.kind, meta.targetPath);
  const bool integratedWasLatest =
      latest.has_value() && IsSamePath(latest->stagePath, meta.stagePath);

  RemoveStageAndMeta(meta.stagePath);
  if (meta.kind == file_output::StagedDiffKind::Clrop) {
    DiscardAnnotJournalSegmentsForBase(meta.targetPath,
                                       meta.revision.value_or(0));
  } else if (meta.kind == file_output::StagedDiffKind::Note) {
    DiscardNoteStageArtifactsForStage(meta.stagePath);
  }
  if (integratedWasLatest) {
    if (meta.kind == file_output::StagedDiffKind::Note) {
      file_output::DiscardOtherStagedNoteFilesFor(meta.targetPath, {});
    } else {
      file_output::DiscardOtherStagedClropFilesForPdf(meta.targetPath, {});
    }
  }
  if (meta.kind == file_output::StagedDiffKind::Clrop) {
    InvalidateAnnotHistoryForPath(meta.destPath.wstring());
  } else if (meta.kind == file_output::StagedDiffKind::Note) {
    AddIntegratedCurrentNoteToListIfNeeded(g_hMainWnd, meta.targetPath);
    RefreshCurrentNotePersistenceIdentity(meta.targetPath);
  }
}

bool StageDeferredCurrentNoteForExplicitSave(HWND owner) {
  if (!g_currentNotePath.empty())
    return true;
  if (g_currentSessionPath.empty())
    return true;
  if (IsNoteSaveBlockedByIme()) {
    TraceSaveFailure(L"StageSave", L"deferred_note_ime_blocked",
                     g_currentNotePath);
    ShowImeBlockedSaveNotice(owner, true);
    return false;
  }
  if (!EnsureCurrentNotePathForStageImpl(owner)) {
    TraceSaveFailure(L"StageSave", L"deferred_note_explicit_path_failed",
                     g_currentNotePath);
    return false;
  }

  note::SnapshotIdentity editorIdentity;
  const std::string editorBytes = CaptureCurrentNoteBytes(&editorIdentity);
  if (DiscardDeferredEmptyNotePersistenceIfNeeded(owner, g_currentNotePath,
                                                  editorBytes)) {
    return true;
  }
  std::wstring err;
  bool hasLatestStage = false;
  const PersistedSnapshotMatch match = MatchNoteBytesToPersistedState(
      g_currentNotePath, editorBytes, &hasLatestStage);
  if (match != PersistedSnapshotMatch::None) {
    if (match == PersistedSnapshotMatch::Stage) {
      const auto latest = FindLatestStageMeta(file_output::StagedDiffKind::Note,
                                              g_currentNotePath);
      if (latest.has_value() &&
          !UpdateStageMetaContentIdentity(*latest, editorIdentity, editorBytes,
                                          &err)) {
        TraceSaveFailure(L"StageSave", L"deferred_note_identity_commit_failed",
                         g_currentNotePath, latest->stagePath, latest->destPath,
                         err);
        ShowStageMessageDialog(
            owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
        return false;
      }
    }
    g_noteDirty = false;
    g_noteNeedsIntegrate = hasLatestStage;
    RecordPersistedNoteEditorLength();
    ResetNoteStageEditTracking();
    ClearCurrentNoteEditorModifiedFlag(g_currentNotePath);
    file_output::NotifyAutoIntegrateStageSaved(owner);
    file_output::ConfigureAutoStageSaveScheduling(owner);
    RefreshDirtyUi(owner);
    return true;
  }

  auto meta = StageNoteSnapshotWithBytes(g_currentNotePath, editorBytes,
                                         &editorIdentity, &err);
  if (!meta.has_value()) {
    TraceSaveFailure(L"StageSave", L"deferred_note_explicit_checkpoint_failed",
                     g_currentNotePath, {}, {}, err);
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
    return false;
  }
  file_output::DiscardOtherStagedNoteFilesFor(g_currentNotePath,
                                              meta->stagePath);
  g_noteDirty = false;
  g_noteNeedsIntegrate = true;
  RecordPersistedNoteEditorLength();
  ResetNoteStageEditTracking();
  ClearCurrentNoteEditorModifiedFlag(g_currentNotePath);
  file_output::NotifyAutoIntegrateStageSaved(owner);
  file_output::ConfigureAutoStageSaveScheduling(owner);
  RefreshDirtyUi(owner);
  return true;
}

} // namespace

namespace file_output {

bool EnsureCurrentNotePathForStage(HWND owner) {
  return EnsureCurrentNotePathForStageImpl(owner);
}

bool SaveNoteFile(HWND owner) {
  SaveOperationGuard guard;
  ScopedBatchSaveUiRefresh batchUi(owner);
  if (g_currentNotePath.empty()) {
    ShowStageSoftNotice(owner,
                        IsEnglishUi() ? L"No note is open."
                                      : L"ノートが開かれていません。",
                        SoftNoticeKind::Warning);
    return false;
  }
  if (IsNoteSaveBlockedByIme()) {
    ShowImeBlockedSaveNotice(owner, true);
    return false;
  }
  if (!SaveNoteIfDirty(owner)) {
    return false;
  }

  const auto latestStage =
      FindLatestStageMeta(StagedDiffKind::Note, g_currentNotePath);
  if (!latestStage.has_value()) {
    g_noteDirty = false;
    g_noteNeedsIntegrate = false;
    RecordPersistedNoteEditorLength();
    ResetNoteStageEditTracking();
    ClearCurrentNoteEditorModifiedFlag(g_currentNotePath);
    RefreshCurrentNoteFileSnapshot();
    RefreshDirtyUi(owner);
    return true;
  }

  std::wstring err;
  if (!IntegrateStageMetaToDestination(owner, *latestStage,
                                       /*discardOtherIfLatest=*/true, &err)) {
    ShowStageMessageDialog(owner, IsEnglishUi() ? L"Save note" : L"ノート保存",
                           err);
    return false;
  }
  g_noteDirty = false;
  g_noteNeedsIntegrate = false;
  RecordPersistedNoteEditorLength();
  ResetNoteStageEditTracking();
  ClearCurrentNoteEditorModifiedFlag(g_currentNotePath);
  RefreshCurrentNoteFileSnapshot();
  RefreshDirtyUi(owner);
  return true;
}

bool SaveNoteIfDirty(HWND owner) {
  const ULONGLONG saveStartTick = preview_trace::TickNow();
  SaveOperationGuard guard;
  if (g_currentNotePath.empty() && !g_noteDirty)
    return true;
  if (IsNoteSaveBlockedByIme()) {
    TraceSaveFailure(L"StageSave", L"note_ime_blocked", g_currentNotePath);
    ShowImeBlockedSaveNotice(owner, true);
    return false;
  }
  if (g_currentNotePath.empty() && !EnsureCurrentNotePathForStageImpl(owner)) {
    TraceSaveFailure(L"StageSave", L"deferred_note_path_failed",
                     g_currentNotePath);
    return false;
  }
  if (!g_noteDirty) {
    if (g_noteNeedsIntegrate) {
      const std::string editorBytes = CaptureCurrentNoteBytes();
      if (DiscardDeferredEmptyNotePersistenceIfNeeded(owner, g_currentNotePath,
                                                      editorBytes)) {
        return true;
      }
      auto currentBytes =
          ReadFileBytes(std::filesystem::path(g_currentNotePath));
      if (currentBytes.has_value() && *currentBytes == editorBytes) {
        auto latestStage = FindLatestStageMeta(
            file_output::StagedDiffKind::Note, g_currentNotePath);
        if (latestStage.has_value()) {
          DiscardStagedDiff(latestStage->stagePath);
        }
        DiscardRedundantStagedNoteFilesMatchingOriginal(g_currentNotePath);
        g_noteNeedsIntegrate =
            FindLatestStageMeta(file_output::StagedDiffKind::Note,
                                g_currentNotePath)
                .has_value();
        ClearCurrentNoteEditorModifiedFlag(g_currentNotePath);
      }
    }
    ConfigureAutoStageSaveScheduling(owner);
    return true;
  }
  auto prepared = PrepareNoteStageSnapshotFromCurrentState();
  if (!prepared.has_value()) {
    TraceSaveFailure(L"StageSave", L"prepare_note_snapshot_failed",
                     g_currentNotePath);
    return false;
  }
  if (DiscardDeferredEmptyNotePersistenceIfNeeded(owner, prepared->targetPath,
                                                  prepared->bytes)) {
    return true;
  }

  std::wstring err;
  bool hasLatestStage = false;
  const PersistedSnapshotMatch match = MatchNoteBytesToPersistedState(
      prepared->targetPath, prepared->bytes, &hasLatestStage);
  if (match != PersistedSnapshotMatch::None) {
    if (match == PersistedSnapshotMatch::Stage) {
      const auto latest = FindLatestStageMeta(file_output::StagedDiffKind::Note,
                                              prepared->targetPath);
      if (latest.has_value() &&
          !UpdateStageMetaContentIdentity(*latest, prepared->identity,
                                          prepared->bytes, &err)) {
        TraceSaveFailure(L"StageSave", L"note_identity_commit_failed",
                         prepared->targetPath, latest->stagePath,
                         latest->destPath, err);
        ShowStageMessageDialog(
            owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
        return false;
      }
    }
    if (match == PersistedSnapshotMatch::Original) {
      if (hasLatestStage && g_noteNeedsIntegrate) {
        auto latestStage = FindLatestStageMeta(
            file_output::StagedDiffKind::Note, prepared->targetPath);
        if (latestStage.has_value()) {
          DiscardStagedDiff(latestStage->stagePath);
        }
      }
      DiscardRedundantStagedNoteFilesMatchingOriginal(prepared->targetPath);
      hasLatestStage = FindLatestStageMeta(file_output::StagedDiffKind::Note,
                                           prepared->targetPath)
                           .has_value();
    }
    g_noteDirty = false;
    g_noteNeedsIntegrate = hasLatestStage;
    RecordPersistedNoteEditorLength();
    ResetNoteStageEditTracking();
    ClearCurrentNoteEditorModifiedFlag(prepared->targetPath);
    NotifyAutoIntegrateStageSaved(owner);
    ConfigureAutoStageSaveScheduling(owner);
    RefreshDirtyUi(owner);
    return true;
  }

  const auto latestStage = FindLatestStageMeta(
      file_output::StagedDiffKind::Note, prepared->targetPath);
  if (latestStage.has_value()) {
    NoteStageEditPlan editPlan;
    if (TryBuildNoteStageEditPlan(*latestStage, prepared->bytes, &editPlan) &&
        ApplyNoteStageEditPlan(editPlan, &err)) {
      if (!UpdateStageMetaContentIdentity(editPlan.meta, prepared->identity,
                                          prepared->bytes, &err)) {
        TraceSaveFailure(L"StageSave", L"note_edit_identity_commit_failed",
                         prepared->targetPath, latestStage->stagePath,
                         latestStage->destPath, err);
        ShowStageMessageDialog(
            owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
        return false;
      }
      g_noteDirty = false;
      g_noteNeedsIntegrate = true;
      RecordPersistedNoteEditorLength();
      ResetNoteStageEditTracking();
      ClearCurrentNoteEditorModifiedFlag(prepared->targetPath);
      NotifyAutoIntegrateStageSaved(owner);
      ConfigureAutoStageSaveScheduling(owner);
      RefreshDirtyUi(owner);
      return true;
    }
    if (!err.empty()) {
      TraceSaveFailure(L"StageSave", L"note_edit_journal_failed",
                       prepared->targetPath, latestStage->stagePath,
                       latestStage->destPath, err);
      ShowStageMessageDialog(
          owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
      return false;
    }

    NoteStageAppendPlan plan;
    if (TryBuildNoteStageAppendPlan(*latestStage, prepared->bytes, &plan) &&
        ApplyNoteStageAppendPlan(plan, &err)) {
      if (!UpdateStageMetaContentIdentity(plan.meta, prepared->identity,
                                          prepared->bytes, &err)) {
        TraceSaveFailure(L"StageSave", L"note_append_identity_commit_failed",
                         prepared->targetPath, latestStage->stagePath,
                         latestStage->destPath, err);
        ShowStageMessageDialog(
            owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
        return false;
      }
      g_noteDirty = false;
      g_noteNeedsIntegrate = true;
      RecordPersistedNoteEditorLength();
      ResetNoteStageEditTracking();
      ClearCurrentNoteEditorModifiedFlag(prepared->targetPath);
      NotifyAutoIntegrateStageSaved(owner);
      ConfigureAutoStageSaveScheduling(owner);
      RefreshDirtyUi(owner);
      return true;
    }
    if (!err.empty()) {
      TraceSaveFailure(L"StageSave", L"note_append_failed",
                       prepared->targetPath, latestStage->stagePath,
                       latestStage->destPath, err);
      ShowStageMessageDialog(
          owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
      return false;
    }
  }

  auto meta = StageNoteSnapshotWithBytes(prepared->targetPath, prepared->bytes,
                                         &prepared->identity, &err);
  if (!meta.has_value()) {
    TraceSaveFailure(L"StageSave", L"note_checkpoint_failed",
                     prepared->targetPath, {}, {}, err);
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Stage note" : L"ノート stage 保存", err);
    return false;
  }
  DiscardOtherStagedNoteFilesFor(prepared->targetPath, meta->stagePath);
  g_noteDirty = false;
  g_noteNeedsIntegrate = true;
  RecordPersistedNoteEditorLength();
  ResetNoteStageEditTracking();
  ClearCurrentNoteEditorModifiedFlag(prepared->targetPath);
  NotifyAutoIntegrateStageSaved(owner);
  ConfigureAutoStageSaveScheduling(owner);
  RefreshDirtyUi(owner);
  preview_trace::Append(L"StageSave",
                        L"note_end elapsed_ms=" +
                            preview_trace::ElapsedMs(saveStartTick));
  return true;
}

bool SaveAnnotationsIfDirty(HWND owner) {
  const ULONGLONG saveStartTick = preview_trace::TickNow();
  SaveOperationGuard guard;
  const std::wstring pdfPath = CurrentLogicalPdfPath();
  TraceAnnotStageTiming(L"begin dirty=" + preview_trace::Bool(g_annotsDirty) +
                            L" editingText=" +
                            preview_trace::Bool(g_pdf.editingText),
                        saveStartTick, saveStartTick, pdfPath, g_annots.size());
  if (pdfPath.empty()) {
    TraceAnnotStageTiming(L"skip=no_pdf_path", saveStartTick, saveStartTick,
                          pdfPath, g_annots.size());
    return true;
  }
  if (IsAnnotationSaveBlockedByIme()) {
    TraceAnnotStageTiming(L"abort=ime_blocked", saveStartTick, saveStartTick,
                          pdfPath, g_annots.size());
    TraceSaveFailure(L"StageSave", L"annot_ime_blocked", pdfPath);
    ShowImeBlockedSaveNotice(owner, false);
    return false;
  }
  if (!g_annotsDirty && !g_pdf.editingText) {
    TraceAnnotStageTiming(L"skip=clean", saveStartTick, saveStartTick, pdfPath,
                          g_annots.size());
    ConfigureAutoStageSaveScheduling(owner);
    return true;
  }

  std::wstring err;
  ULONGLONG stepStartTick = preview_trace::TickNow();
  const bool strongValidated =
      EnsureFastLoadedAnnotationsStrongValidatedBeforeSave(owner, pdfPath,
                                                           &err);
  TraceAnnotStageTiming(L"after_strong_validation ok=" +
                            preview_trace::Bool(strongValidated),
                        stepStartTick, saveStartTick, pdfPath, g_annots.size());
  if (!strongValidated) {
    return false;
  }

  stepStartTick = preview_trace::TickNow();
  const bool journalAppended = TryAppendPendingAnnotJournal(pdfPath, &err);
  TraceAnnotStageTiming(L"after_journal_append appended=" +
                            preview_trace::Bool(journalAppended),
                        stepStartTick, saveStartTick, pdfPath, g_annots.size());
  if (journalAppended) {
    g_annotsDirty = false;
    g_annotsNeedsIntegrate = true;
    g_preparedAnnotStageSnapshot.reset();
    ClearPendingAnnotStageCommands(pdfPath);
    NotifyAutoIntegrateStageSaved(owner);
    ConfigureAutoStageSaveScheduling(owner);
    RefreshDirtyUi(owner);
    TraceAnnotStageTiming(L"end journal_appended", saveStartTick, saveStartTick,
                          pdfPath, g_annots.size());
    return true;
  }
  if (!err.empty()) {
    TraceSaveFailure(L"StageSave", L"annot_journal_append_failed", pdfPath, {},
                     {}, err);
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Stage annotations" : L"注釈 stage 保存", err);
    return false;
  }

  stepStartTick = preview_trace::TickNow();
  auto prepared = ReusePreparedAnnotStageSnapshotIfFresh();
  std::wstring snapshotSource = L"reused";
  if (!prepared.has_value()) {
    snapshotSource = L"prepared";
    prepared = PrepareAnnotStageSnapshotFromCurrentState(&err);
  }
  TraceAnnotStageTiming(L"after_prepare_snapshot source=" + snapshotSource +
                            L" ok=" + preview_trace::Bool(prepared.has_value()),
                        stepStartTick, saveStartTick, pdfPath,
                        prepared.has_value() ? prepared->annotations.size()
                                             : g_annots.size());
  if (!prepared.has_value()) {
    if (!err.empty()) {
      TraceSaveFailure(L"StageSave", L"prepare_annot_snapshot_failed", pdfPath,
                       {}, {}, err);
      ShowStageMessageDialog(
          owner, IsEnglishUi() ? L"Stage annotations" : L"注釈 stage 保存",
          err);
      return false;
    }
    ConfigureAutoStageSaveScheduling(owner);
    return true;
  }

  bool hasLatestStage = false;
  stepStartTick = preview_trace::TickNow();
  const PersistedSnapshotMatch match = MatchAnnotationSnapshotToPersistedState(
      prepared->targetPath, prepared->annotations, &hasLatestStage);
  TraceAnnotStageTiming(L"after_match_persisted match=" +
                            std::to_wstring(static_cast<int>(match)) +
                            L" hasLatestStage=" +
                            preview_trace::Bool(hasLatestStage),
                        stepStartTick, saveStartTick, prepared->targetPath,
                        prepared->annotations.size());
  if (match != PersistedSnapshotMatch::None) {
    if (match == PersistedSnapshotMatch::Original) {
      if (hasLatestStage && g_annotsNeedsIntegrate) {
        auto latestStage = FindLatestStageMeta(
            file_output::StagedDiffKind::Clrop, prepared->targetPath);
        if (latestStage.has_value()) {
          DiscardStagedDiff(latestStage->stagePath);
        }
      }
      DiscardRedundantStagedClropFilesMatchingOriginal(prepared->targetPath);
      hasLatestStage = FindLatestStageMeta(file_output::StagedDiffKind::Clrop,
                                           prepared->targetPath)
                           .has_value();
      ClearPendingAnnotStageCommands(prepared->targetPath);
    }
    g_annotsDirty = false;
    g_annotsNeedsIntegrate = hasLatestStage;
    g_preparedAnnotStageSnapshot.reset();
    NotifyAutoIntegrateStageSaved(owner);
    ConfigureAutoStageSaveScheduling(owner);
    RefreshDirtyUi(owner);
    TraceAnnotStageTiming(L"end matched_persisted", saveStartTick,
                          saveStartTick, prepared->targetPath,
                          prepared->annotations.size());
    return true;
  }

  stepStartTick = preview_trace::TickNow();
  auto meta = StageAnnotationCheckpointWithData(prepared->targetPath,
                                                prepared->annotations, &err);
  TraceAnnotStageTiming(L"after_checkpoint ok=" +
                            preview_trace::Bool(meta.has_value()),
                        stepStartTick, saveStartTick, prepared->targetPath,
                        prepared->annotations.size());
  if (!meta.has_value()) {
    TraceSaveFailure(L"StageSave", L"annot_checkpoint_failed",
                     prepared->targetPath, {}, {}, err);
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Stage annotations" : L"注釈 stage 保存", err);
    return false;
  }
  stepStartTick = preview_trace::TickNow();
  DiscardAnnotJournalSegmentsForTarget(prepared->targetPath);
  TraceAnnotStageTiming(L"after_discard_journal", stepStartTick, saveStartTick,
                        prepared->targetPath, prepared->annotations.size());
  g_annotsDirty = false;
  g_annotsNeedsIntegrate = true;
  g_preparedAnnotStageSnapshot.reset();
  ClearPendingAnnotStageCommands(prepared->targetPath);
  NotifyAutoIntegrateStageSaved(owner);
  ConfigureAutoStageSaveScheduling(owner);
  RefreshDirtyUi(owner);
  TraceAnnotStageTiming(L"end checkpoint_saved", saveStartTick, saveStartTick,
                        prepared->targetPath, prepared->annotations.size());
  preview_trace::Append(L"StageSave",
                        L"annot_end elapsed_ms=" +
                            preview_trace::ElapsedMs(saveStartTick));
  return true;
}

void ConfigureNoteStageSaveScheduling(HWND owner) {
  HWND target = NoticeOwner(owner);
  if (!target)
    return;

  if (!g_noteDirty ||
      (g_currentNotePath.empty() && g_currentSessionPath.empty())) {
    ResetNoteStageEditTracking();
    KillTimer(target, kNoteStageSaveTimerId);
    return;
  }

  const UINT delayMs = ComputeNoteStageDelayMs();
  if (delayMs > 0) {
    SetTimer(target, kNoteStageSaveTimerId, delayMs, nullptr);
    return;
  }
  KillTimer(target, kNoteStageSaveTimerId);
}

void ConfigureAutoStageSaveScheduling(HWND owner) {
  HWND target = NoticeOwner(owner);
  if (!target)
    return;

  ConfigureNoteStageSaveScheduling(target);

  if (!HasPendingAutoStageWork() || !UsesAutoStageTimerMode()) {
    ResetAutoStageEditTracking();
    KillTimer(target, kAutoSaveTimerId);
    KillTimer(target, kAutoSaveExecuteTimerId);
    ClearPreparedAutoStageSnapshots();
    return;
  }

  const UINT delayMs = ComputeAutoStageDelayMs();
  if (delayMs > 0) {
    SetTimer(target, kAutoSaveTimerId, delayMs, nullptr);
    return;
  }
  KillTimer(target, kAutoSaveTimerId);
}

void NotifyNoteStageEdit(HWND owner) {
  if (HWND target = NoticeOwner(owner))
    KillTimer(target, kAutoIntegrateTimerId);
  if (g_currentNotePath.empty() && g_currentSessionPath.empty())
    return;
  g_noteStageLastEditTick = GetTickCount64();
  if (!g_noteStagePersistedCharCountKnown && g_hNoteEdit) {
    g_noteStagePersistedCharCount = CurrentNoteEditorCharCount();
    g_noteStagePersistedCharCountKnown = true;
  }
  ConfigureAutoStageSaveScheduling(owner);
}

void ResetNoteStageSaveTracking(HWND owner) {
  RecordPersistedNoteEditorLength();
  ResetNoteStageEditTracking();
  ConfigureAutoStageSaveScheduling(owner);
}

void NotifyAutoStageSaveStep(HWND owner) {
  if (HWND target = NoticeOwner(owner))
    KillTimer(target, kAutoIntegrateTimerId);
  if (!UsesAutoStageTimerMode())
    return;
  NoteAutoStageEditActivity();
  ConfigureAutoStageSaveScheduling(owner);
}

void ScheduleAutoStageSaveRetry(HWND owner) {
  HWND target = NoticeOwner(owner);
  if (!target)
    return;
  KillTimer(target, kAutoSaveExecuteTimerId);
  SetTimer(target, kAutoSaveTimerId, kAutoStageRetryDelayMs, nullptr);
}

void HandleNoteStageSaveTimer(HWND owner) {
  HWND target = NoticeOwner(owner);
  if (target)
    KillTimer(target, kNoteStageSaveTimerId);
  if (!g_noteDirty ||
      (g_currentNotePath.empty() && g_currentSessionPath.empty())) {
    ConfigureAutoStageSaveScheduling(owner);
    return;
  }
  if (IsNoteImeComposing() || IsNoteTyping()) {
    ConfigureAutoStageSaveScheduling(owner);
    return;
  }
  if (!file_output::SaveNoteIfDirty(owner) && target) {
    TraceSaveFailure(L"AutoStage", L"note_timer_save_failed_retry",
                     g_currentNotePath);
    SetTimer(target, kNoteStageSaveTimerId, kAutoStageRetryDelayMs, nullptr);
  }
}

void HandleAutoStageSaveTimer(HWND owner) {
  HWND target = NoticeOwner(owner);
  if (target)
    KillTimer(target, kAutoSaveTimerId);
  if (!HasPendingAutoStageWork()) {
    ConfigureAutoStageSaveScheduling(owner);
    return;
  }
  if (ShouldDeferBackgroundSaveForActiveInput()) {
    if (target)
      SetTimer(target, kAutoSaveTimerId, kAutoStageBlockedPollDelayMs, nullptr);
    return;
  }
  if (!PrepareDeferredAutoStageSnapshots(owner)) {
    TraceSaveFailure(L"AutoStage", L"prepare_deferred_snapshot_failed",
                     CurrentLogicalPdfPath());
    ScheduleAutoStageSaveRetry(owner);
    return;
  }
  if (target)
    SetTimer(target, kAutoSaveExecuteTimerId, kAutoStageDeferredWriteDelayMs,
             nullptr);
}

void HandleDeferredAutoStageSaveTimer(HWND owner) {
  HWND target = NoticeOwner(owner ? owner : g_preparedAutoStageOwner);
  if (target)
    KillTimer(target, kAutoSaveExecuteTimerId);
  if (ShouldDeferBackgroundSaveForActiveInput()) {
    if (target)
      SetTimer(target, kAutoSaveExecuteTimerId, kAutoStageBlockedPollDelayMs,
               nullptr);
    return;
  }
  if (HasPendingAutoStageWork() &&
      CurrentEditRevision() != g_preparedAutoStageRevision) {
    if (!PrepareDeferredAutoStageSnapshots(target)) {
      TraceSaveFailure(L"AutoStage", L"refresh_deferred_snapshot_failed",
                       CurrentLogicalPdfPath());
      ScheduleAutoStageSaveRetry(target);
      return;
    }
    if (target)
      SetTimer(target, kAutoSaveExecuteTimerId, kAutoStageDeferredWriteDelayMs,
               nullptr);
    return;
  }
  if (!g_preparedAnnotStageSnapshot.has_value()) {
    if (g_annotsDirty) {
      if (!file_output::SaveAnnotationsIfDirty(target)) {
        TraceSaveFailure(L"AutoStage", L"direct_annot_save_failed_retry",
                         CurrentLogicalPdfPath());
        ScheduleAutoStageSaveRetry(target);
      }
      return;
    }
    ConfigureAutoStageSaveScheduling(target);
    return;
  }

  SaveOperationGuard guard;
  bool ok = true;
  bool uiNeedsRefresh = false;

  if (ok && g_preparedAnnotStageSnapshot.has_value()) {
    std::wstring err;
    if (!EnsureFastLoadedAnnotationsStrongValidatedBeforeSave(
            target, g_preparedAnnotStageSnapshot->targetPath, &err)) {
      ok = false;
    }
  }

  if (ok && g_preparedAnnotStageSnapshot.has_value()) {
    std::wstring err;
    bool hasLatestStage = false;
    const PersistedSnapshotMatch match =
        MatchAnnotationSnapshotToPersistedState(
            g_preparedAnnotStageSnapshot->targetPath,
            g_preparedAnnotStageSnapshot->annotations, &hasLatestStage);
    if (match != PersistedSnapshotMatch::None) {
      if (match == PersistedSnapshotMatch::Original) {
        if (hasLatestStage && g_annotsNeedsIntegrate) {
          auto latestStage =
              FindLatestStageMeta(file_output::StagedDiffKind::Clrop,
                                  g_preparedAnnotStageSnapshot->targetPath);
          if (latestStage.has_value()) {
            DiscardStagedDiff(latestStage->stagePath);
          }
        }
        DiscardRedundantStagedClropFilesMatchingOriginal(
            g_preparedAnnotStageSnapshot->targetPath);
        hasLatestStage =
            FindLatestStageMeta(file_output::StagedDiffKind::Clrop,
                                g_preparedAnnotStageSnapshot->targetPath)
                .has_value();
        ClearPendingAnnotStageCommands(
            g_preparedAnnotStageSnapshot->targetPath);
      }
      if (CurrentLogicalPdfPath() == g_preparedAnnotStageSnapshot->targetPath) {
        g_annotsDirty = false;
        g_annotsNeedsIntegrate = hasLatestStage;
        uiNeedsRefresh = true;
      }
    } else {
      auto meta = StageAnnotationCheckpointWithData(
          g_preparedAnnotStageSnapshot->targetPath,
          g_preparedAnnotStageSnapshot->annotations, &err);
      if (!meta.has_value()) {
        TraceSaveFailure(L"AutoStage", L"deferred_annot_checkpoint_failed",
                         g_preparedAnnotStageSnapshot->targetPath, {}, {}, err);
        ShowStageMessageDialog(
            target, IsEnglishUi() ? L"Stage annotations" : L"注釈 stage 保存",
            err);
        ok = false;
      } else {
        DiscardAnnotJournalSegmentsForTarget(
            g_preparedAnnotStageSnapshot->targetPath);
        ClearPendingAnnotStageCommands(
            g_preparedAnnotStageSnapshot->targetPath);
      }
      if (ok &&
          CurrentLogicalPdfPath() == g_preparedAnnotStageSnapshot->targetPath) {
        g_annotsDirty = false;
        g_annotsNeedsIntegrate = true;
        uiNeedsRefresh = true;
      }
    }
  }

  if (!ok) {
    TraceSaveFailure(L"AutoStage", L"deferred_write_failed_retry",
                     CurrentLogicalPdfPath());
    ScheduleAutoStageSaveRetry(target);
    return;
  }

  ClearPreparedAutoStageSnapshots();
  NotifyAutoIntegrateStageSaved(target);
  ConfigureAutoStageSaveScheduling(target);
  if (uiNeedsRefresh) {
    RefreshDirtyUi(target);
  }
}

bool ShouldAutoIntegrateOnSwitchOrExit() { return false; }

bool PrepareStagedDiffsForSwitch(HWND owner) {
  return file_output::SaveNoteIfDirty(owner) &&
         file_output::SaveAnnotationsIfDirty(owner);
}

bool MaybeAutoIntegrateOnSwitchOrExit(HWND owner) {
  if (!file_output::SaveNoteIfDirty(owner))
    return false;
  if (!file_output::SaveAnnotationsIfDirty(owner))
    return false;
  return true;
}

void ScheduleIntegrateAfterSwitch(HWND owner) {
  ConfigureAutoIntegrateScheduling(owner);
}

static UINT AutoIntegrateDelayMs() {
  int seconds = g_config.autoIntegrateSeconds;
  if (seconds == kAutoIntegrateModeCustom) {
    seconds = std::clamp(g_config.autoIntegrateCustomMinutes,
                         kAutoIntegrateCustomMinutesMin,
                         kAutoIntegrateCustomMinutesMax) *
              60;
  }
  if (seconds <= 0)
    return 0;
  constexpr int kMaxDelaySeconds = static_cast<int>(UINT_MAX / 1000);
  return static_cast<UINT>(std::min(seconds, kMaxDelaySeconds) * 1000);
}

void ConfigureAutoIntegrateScheduling(HWND owner) {
  HWND target = NoticeOwner(owner);
  if (!target)
    return;
  KillTimer(target, kAutoIntegrateTimerId);
  const UINT delayMs = AutoIntegrateDelayMs();
  if (delayMs == 0)
    return;
  if (!g_noteNeedsIntegrate && !g_annotsNeedsIntegrate)
    return;
  SetTimer(target, kAutoIntegrateTimerId, delayMs, nullptr);
}

void NotifyAutoIntegrateStageSaved(HWND owner) {
  ConfigureAutoIntegrateScheduling(owner);
}

void HandleAutoIntegrateTimer(HWND owner) {
  HWND target = NoticeOwner(owner);
  if (!target)
    return;
  KillTimer(target, kAutoIntegrateTimerId);
  if (AutoIntegrateDelayMs() == 0)
    return;
  if (ShouldDeferBackgroundSaveForActiveInput()) {
    ConfigureAutoIntegrateScheduling(target);
    return;
  }
  if (!g_noteNeedsIntegrate && !g_annotsNeedsIntegrate && !HasAnyStagedDiffs())
    return;
  if (StartBackgroundSaveAndIntegrateTransaction(target) ==
      SaveTransactionStartResult::Failed) {
    ConfigureAutoIntegrateScheduling(target);
  }
}

bool ShouldDeferBackgroundSaveForActiveInput() {
  return IsAutoStageTimingBlocked();
}

bool IntegrateStagedNoteAndAnnotations(HWND owner) {
  const ULONGLONG startTick = preview_trace::TickNow();
  ScopedBatchSaveUiRefresh batchUi(owner);
  preview_trace::Append(
      L"IntegrateStage",
      L"begin noteDirty=" + preview_trace::Bool(g_noteDirty) +
          L" noteNeedsIntegrate=" + preview_trace::Bool(g_noteNeedsIntegrate) +
          L" annotsDirty=" + preview_trace::Bool(g_annotsDirty) +
          L" annotsNeedsIntegrate=" +
          preview_trace::Bool(g_annotsNeedsIntegrate));
  PumpSaveScrollMessages(owner);
  if (!file_output::SaveNoteIfDirty(owner)) {
    TraceSaveFailure(L"IntegrateStage", L"pre_stage_note_failed",
                     g_currentNotePath);
    return false;
  }
  PumpSaveScrollMessages(owner);
  preview_trace::Append(L"IntegrateStage",
                        L"after_stage_note elapsed_ms=" +
                            preview_trace::ElapsedMs(startTick));
  if (!file_output::SaveAnnotationsIfDirty(owner)) {
    TraceSaveFailure(L"IntegrateStage", L"pre_stage_annot_failed",
                     CurrentLogicalPdfPath());
    return false;
  }
  PumpSaveScrollMessages(owner);
  preview_trace::Append(L"IntegrateStage",
                        L"after_stage_annotations elapsed_ms=" +
                            preview_trace::ElapsedMs(startTick));
  bool ok = true;
  auto entries = ListStagedDiffEntries();
  size_t latestCount = 0;
  size_t integratedCount = 0;
  for (const auto &entry : entries) {
    if (entry.isLatest)
      ++latestCount;
  }
  preview_trace::Append(L"IntegrateStage",
                        L"after_list entries=" +
                            std::to_wstring(entries.size()) + L" latest=" +
                            std::to_wstring(latestCount) + L" elapsed_ms=" +
                            preview_trace::ElapsedMs(startTick));
  for (const auto &entry : entries) {
    if (!entry.isLatest)
      continue;
    PumpSaveScrollMessages(owner);
    if (!IntegrateStagedDiff(owner, entry.stagePath)) {
      TraceSaveFailure(L"IntegrateStage", L"entry_failed", entry.targetPath,
                       entry.stagePath, entry.destPath);
      ok = false;
      break;
    }
    ++integratedCount;
    PumpSaveScrollMessages(owner);
  }
  ConfigureAutoIntegrateScheduling(owner);
  preview_trace::Append(L"IntegrateStage",
                        L"end ok=" + preview_trace::Bool(ok) + L" integrated=" +
                            std::to_wstring(integratedCount) + L" latest=" +
                            std::to_wstring(latestCount) + L" elapsed_ms=" +
                            preview_trace::ElapsedMs(startTick));
  return ok;
}

bool RunSaveAndIntegrateTransaction(HWND owner) {
  const ULONGLONG startTick = preview_trace::TickNow();
  uint64_t snapshotRevision = 0;
  if (!TryBeginSaveTransaction(&snapshotRevision)) {
    ShowStageSoftNotice(owner,
                        IsEnglishUi()
                            ? L"Another save transaction is already running."
                            : L"別の保存トランザクションが進行中です。",
                        SoftNoticeKind::Warning);
    return false;
  }

  bool ok = true;
  int iteration = 0;
  ScopedSaveTransactionEnd transactionEnd(owner);
  SaveOperationGuard guard;
  if (HWND statusOwner = owner ? owner : g_hMainWnd) {
    UpdateToolbarUI(statusOwner);
    RefreshStatusDisplay(statusOwner);
    RedrawWindow(statusOwner, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME | RDW_NOCHILDREN);
    UpdateWindow(statusOwner);
    GdiFlush();
  }
  PumpSaveScrollMessages(owner);
  ScopedBatchSaveUiRefresh batchUi(owner);
  for (;;) {
    ++iteration;
    PumpSaveScrollMessages(owner);
    if (!StageDeferredCurrentNoteForExplicitSave(owner)) {
      ok = false;
      TraceSaveFailure(L"SaveTransaction",
                       L"deferred_note_explicit_stage_failed",
                       g_currentNotePath);
      break;
    }
    ok = IntegrateStagedNoteAndAnnotations(owner);
    PumpSaveScrollMessages(owner);
    preview_trace::Append(L"SaveTransaction",
                          L"iteration=" + std::to_wstring(iteration) + L" ok=" +
                              preview_trace::Bool(ok) + L" elapsed_ms=" +
                              preview_trace::ElapsedMs(startTick));
    if (!ok) {
      TraceSaveFailure(L"SaveTransaction", L"iteration_failed",
                       CurrentLogicalPdfPath());
      break;
    }
    if (!ShouldRunQueuedSaveTransaction(snapshotRevision))
      break;
    snapshotRevision = CurrentEditRevision();
  }
  preview_trace::Append(L"SaveTransaction",
                        L"end ok=" + preview_trace::Bool(ok) + L" iterations=" +
                            std::to_wstring(iteration) + L" elapsed_ms=" +
                            preview_trace::ElapsedMs(startTick));
  return ok;
}

SaveTransactionStartResult
StartBackgroundSaveAndIntegrateTransaction(HWND owner) {
  const ULONGLONG startTick = preview_trace::TickNow();
  uint64_t snapshotRevision = 0;
  if (!TryBeginSaveTransaction(&snapshotRevision)) {
    RequestQueuedSaveTransaction();
    ShowStageSoftNotice(
        owner,
        IsEnglishUi() ? L"Save is already running. The request was queued."
                      : L"保存処理が進行中です。保存要求を予約しました。",
        SoftNoticeKind::Info);
    return SaveTransactionStartResult::Failed;
  }

  EnterSaveOperation();
  auto finishNow = [&]() {
    EndSaveTransaction();
    LeaveSaveOperation();
    RefreshSaveTransactionUi(owner);
  };

  preview_trace::Append(
      L"BackgroundSave",
      L"begin noteDirty=" + preview_trace::Bool(g_noteDirty) +
          L" noteNeedsIntegrate=" + preview_trace::Bool(g_noteNeedsIntegrate) +
          L" annotsDirty=" + preview_trace::Bool(g_annotsDirty) +
          L" annotsNeedsIntegrate=" +
          preview_trace::Bool(g_annotsNeedsIntegrate));
  RefreshSaveTransactionUi(owner);

  {
    ScopedBatchSaveUiRefresh batchUi(owner);
    if (!StageDeferredCurrentNoteForExplicitSave(owner)) {
      TraceSaveFailure(L"BackgroundSave", L"pre_stage_deferred_note_failed",
                       g_currentNotePath);
      finishNow();
      return SaveTransactionStartResult::Failed;
    }
    if (!file_output::SaveNoteIfDirty(owner)) {
      TraceSaveFailure(L"BackgroundSave", L"pre_stage_note_failed",
                       g_currentNotePath);
      finishNow();
      return SaveTransactionStartResult::Failed;
    }
    if (!file_output::SaveAnnotationsIfDirty(owner)) {
      TraceSaveFailure(L"BackgroundSave", L"pre_stage_annot_failed",
                       CurrentLogicalPdfPath());
      finishNow();
      return SaveTransactionStartResult::Failed;
    }
  }

  auto allEntries = ListStagedDiffEntries();
  std::vector<StagedDiffEntry> latestEntries;
  latestEntries.reserve(allEntries.size());
  for (const auto &entry : allEntries) {
    if (entry.isLatest)
      latestEntries.push_back(entry);
  }
  if (latestEntries.empty()) {
    ConfigureAutoIntegrateScheduling(owner);
    preview_trace::Append(L"BackgroundSave",
                          L"complete_sync_no_entries elapsed_ms=" +
                              preview_trace::ElapsedMs(startTick));
    finishNow();
    return SaveTransactionStartResult::CompletedSynchronously;
  }

  HWND postOwner = owner ? owner : g_hMainWnd;
  auto *result = new BackgroundSaveWorkerResult();
  result->snapshotRevision = snapshotRevision;
  result->startTick = startTick;
  result->integrated.reserve(latestEntries.size());
  const size_t workerEntryCount = latestEntries.size();

  try {
    std::thread([postOwner, entries = std::move(latestEntries),
                 result]() mutable {
      try {
        for (const auto &entry : entries) {
          std::wstring err;
          if (!WriteStageMetaToDestinationOnWorker(StageMetaFromEntry(entry),
                                                   &err)) {
            result->ok = false;
            result->error =
                err.empty()
                    ? (IsEnglishUi() ? L"Background save failed."
                                     : L"バックグラウンド保存に失敗しました。")
                    : err;
            break;
          }
          result->integrated.push_back(entry);
        }
      } catch (const std::exception &ex) {
        result->ok = false;
        result->error = UTF8ToWide(ex.what());
        preview_trace::Append(L"BackgroundSave", L"worker_exception=std");
      } catch (...) {
        result->ok = false;
        result->error =
            IsEnglishUi()
                ? L"Unknown background save error."
                : L"バックグラウンド保存中に不明なエラーが発生しました。";
        preview_trace::Append(L"BackgroundSave", L"worker_exception=unknown");
      }

      if (!PostMessageW(postOwner, kMsgBackgroundSaveComplete, 0,
                        reinterpret_cast<LPARAM>(result))) {
        EndSaveTransaction();
        LeaveSaveOperation();
        delete result;
      }
    }).detach();
  } catch (const std::exception &ex) {
    preview_trace::Append(L"BackgroundSave", L"start_exception=std");
    delete result;
    finishNow();
    return SaveTransactionStartResult::Failed;
  } catch (...) {
    preview_trace::Append(L"BackgroundSave", L"start_exception=unknown");
    delete result;
    finishNow();
    return SaveTransactionStartResult::Failed;
  }

  preview_trace::Append(
      L"BackgroundSave",
      L"worker_started entries=" + std::to_wstring(workerEntryCount) +
          L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
  return SaveTransactionStartResult::Started;
}

BackgroundSaveCompletion
CompleteBackgroundSaveAndIntegrateTransaction(HWND owner, void *rawResult) {
  BackgroundSaveCompletion completion;
  std::unique_ptr<BackgroundSaveWorkerResult> result(
      static_cast<BackgroundSaveWorkerResult *>(rawResult));
  if (!result) {
    completion.error = IsEnglishUi()
                           ? L"Background save completion was invalid."
                           : L"バックグラウンド保存の完了情報が不正です。";
    EndSaveTransaction();
    LeaveSaveOperation();
    RefreshSaveTransactionUi(owner);
    return completion;
  }

  for (const auto &entry : result->integrated) {
    CleanupIntegratedStageOnUiThread(entry);
    if (entry.kind == StagedDiffKind::Note &&
        NormalizePathKey(entry.targetPath) ==
            NormalizePathKey(g_currentNotePath)) {
      RefreshCurrentNoteFileSnapshot();
    }
    ++completion.integratedCount;
  }

  UpdateCurrentIntegrateFlags();
  RefreshDirtyUi(owner);
  ConfigureAutoIntegrateScheduling(owner);
  completion.ok = result->ok;
  completion.error = result->error;

  const bool shouldRunQueued =
      result->ok && ShouldRunQueuedSaveTransaction(result->snapshotRevision);
  EndSaveTransaction();
  LeaveSaveOperation();
  RefreshSaveTransactionUi(owner);

  preview_trace::Append(
      L"BackgroundSave",
      L"complete ok=" + preview_trace::Bool(result->ok) + L" integrated=" +
          std::to_wstring(completion.integratedCount) + L" queued=" +
          preview_trace::Bool(shouldRunQueued) + L" elapsed_ms=" +
          preview_trace::ElapsedMs(result->startTick));

  if (shouldRunQueued) {
    SaveTransactionStartResult restart =
        StartBackgroundSaveAndIntegrateTransaction(owner);
    completion.restarted = (restart == SaveTransactionStartResult::Started);
    if (restart == SaveTransactionStartResult::Failed) {
      completion.ok = false;
      if (completion.error.empty()) {
        completion.error = IsEnglishUi()
                               ? L"Queued save could not be started."
                               : L"予約された保存を開始できませんでした。";
      }
    }
  }
  return completion;
}

bool HasAnyStagedDiffs() { return !ListStagedDiffEntries().empty(); }

std::wstring FormatStagedDiffLocationSummary(size_t maxEntries) {
  const auto entries = ListStagedDiffEntries();
  if (entries.empty())
    return L"";
  if (maxEntries == 0)
    maxEntries = entries.size();

  size_t noteCount = 0;
  size_t clropCount = 0;
  for (const auto &entry : entries) {
    if (entry.kind == StagedDiffKind::Note) {
      ++noteCount;
    } else if (entry.kind == StagedDiffKind::Clrop) {
      ++clropCount;
    }
  }

  std::wstring text;
  if (IsEnglishUi()) {
    text = L"Affected staged diffs: notes " + std::to_wstring(noteCount) +
           L", annotations " + std::to_wstring(clropCount) + L"\n";
  } else {
    text = L"影響する未統合差分: ノート " + std::to_wstring(noteCount) +
           L"件、注釈 " + std::to_wstring(clropCount) + L"件\n";
  }

  const size_t limit = std::min(maxEntries, entries.size());
  for (size_t i = 0; i < limit; ++i) {
    const auto &entry = entries[i];
    text += L" - ";
    text += (entry.kind == StagedDiffKind::Note)
                ? (IsEnglishUi() ? L"Note" : L"ノート")
                : (IsEnglishUi() ? L"Annotations" : L"注釈");
    if (!entry.isLatest) {
      text += IsEnglishUi() ? L" (old)" : L" (旧)";
    }
    if (entry.revision.has_value()) {
      text += L" rev=" + std::to_wstring(*entry.revision);
    }
    text += IsEnglishUi() ? L"\n   Target: " : L"\n   対象: ";
    text += entry.targetPath.empty() ? L"(unknown)" : entry.targetPath;
    text += IsEnglishUi() ? L"\n   Stage: " : L"\n   差分ファイル: ";
    text += entry.stagePath.empty() ? L"(unknown)" : entry.stagePath.wstring();
    if (!entry.destPath.empty() &&
        entry.destPath.wstring() != entry.targetPath) {
      text += IsEnglishUi() ? L"\n   Destination: " : L"\n   統合先: ";
      text += entry.destPath.wstring();
    }
    text += L"\n";
  }
  if (entries.size() > limit) {
    text += IsEnglishUi()
                ? L" - ... and " + std::to_wstring(entries.size() - limit) +
                      L" more\n"
                : L" - ... ほか " + std::to_wstring(entries.size() - limit) +
                      L" 件\n";
  }
  return text;
}

bool HasPendingOrStagedDiffsUnderPath(const std::filesystem::path &root) {
  if (root.empty())
    return false;
  if (g_noteDirty && !g_currentNotePath.empty() &&
      IsPathUnderRoot(std::filesystem::path(g_currentNotePath), root)) {
    return true;
  }
  if ((g_noteDirty || g_noteNeedsIntegrate) && g_currentNotePath.empty() &&
      !g_currentSessionPath.empty() &&
      IsPathUnderRoot(std::filesystem::path(g_currentSessionPath), root)) {
    return true;
  }
  if ((g_noteNeedsIntegrate || g_annotsDirty || g_annotsNeedsIntegrate) &&
      !CurrentLogicalPdfPath().empty() &&
      IsPathUnderRoot(std::filesystem::path(CurrentLogicalPdfPath()), root)) {
    return true;
  }
  if ((g_annotsDirty || g_annotsNeedsIntegrate) &&
      !CurrentLogicalPdfPath().empty()) {
    const auto clropPath = std::filesystem::path(
        clrop_bridge::ClropPathForPdf(CurrentLogicalPdfPath()));
    if (IsPathUnderRoot(clropPath, root))
      return true;
  }
  for (const auto &entry : ListStagedDiffEntries()) {
    if (IsPathUnderRoot(entry.stagePath, root) ||
        IsPathUnderRoot(entry.destPath, root) ||
        IsPathUnderRoot(std::filesystem::path(entry.targetPath), root)) {
      return true;
    }
  }
  return false;
}

bool DiscardAllStagedDiffs(HWND owner) {
  for (const auto &entry : ListStagedDiffEntries()) {
    RemoveStageAndMeta(entry.stagePath);
    if (entry.kind == StagedDiffKind::Note) {
      DiscardNoteStageArtifactsForStage(entry.stagePath);
    }
  }
  const auto noteJournalDir = NoteJournalDir();
  std::error_code ec;
  if (!noteJournalDir.empty() && std::filesystem::exists(noteJournalDir, ec) &&
      !ec) {
    for (const auto &entry :
         std::filesystem::directory_iterator(noteJournalDir, ec)) {
      if (ec)
        break;
      if (!entry.is_regular_file(ec) || ec)
        continue;
      std::filesystem::remove(entry.path(), ec);
      ec.clear();
    }
  }
  const auto noteStateDir = NoteStateDir();
  ec.clear();
  if (!noteStateDir.empty() && std::filesystem::exists(noteStateDir, ec) &&
      !ec) {
    for (const auto &entry :
         std::filesystem::directory_iterator(noteStateDir, ec)) {
      if (ec)
        break;
      if (!entry.is_regular_file(ec) || ec)
        continue;
      std::filesystem::remove(entry.path(), ec);
      ec.clear();
    }
  }
  const auto journalDir = AnnotJournalDir();
  ec.clear();
  if (!journalDir.empty() && std::filesystem::exists(journalDir, ec) && !ec) {
    for (const auto &entry :
         std::filesystem::directory_iterator(journalDir, ec)) {
      if (ec)
        break;
      if (!entry.is_regular_file(ec) || ec)
        continue;
      std::filesystem::remove(entry.path(), ec);
      ec.clear();
    }
  }
  UpdateCurrentIntegrateFlags();
  RefreshDirtyUi(owner);
  ConfigureAutoStageSaveScheduling(owner);
  ConfigureAutoIntegrateScheduling(owner);
  return true;
}

std::vector<StagedDiffEntry> ListStagedDiffEntries() {
  std::vector<StagedDiffEntry> entries;
  for (const auto &meta : LoadAllStageMeta()) {
    StagedDiffEntry entry;
    entry.kind = meta.kind;
    entry.stagePath = meta.stagePath;
    entry.metaPath = meta.metaPath;
    entry.destPath = meta.destPath;
    entry.targetPath = meta.targetPath;
    entry.revision = meta.revision;
    entry.noteRestoreView = meta.noteRestoreView;
    entries.push_back(std::move(entry));
  }
  PopulateLatestFlags(&entries);
  std::sort(entries.begin(), entries.end(),
            [](const StagedDiffEntry &lhs, const StagedDiffEntry &rhs) {
              if (lhs.kind != rhs.kind) {
                return lhs.kind < rhs.kind;
              }
              const std::wstring lhsKey =
                  NormalizePathKey(std::filesystem::path(lhs.targetPath));
              const std::wstring rhsKey =
                  NormalizePathKey(std::filesystem::path(rhs.targetPath));
              if (lhsKey != rhsKey)
                return lhsKey < rhsKey;
              const uint64_t lhsRev = lhs.revision.value_or(0);
              const uint64_t rhsRev = rhs.revision.value_or(0);
              if (lhsRev != rhsRev)
                return lhsRev > rhsRev;
              return lhs.stagePath.wstring() < rhs.stagePath.wstring();
            });
  return entries;
}

bool PromoteStagedDiff(const std::filesystem::path &stagePath) {
  if (stagePath.empty())
    return false;
  StageMeta meta;
  if (!LoadStageMetaFile(StageMetaPath(stagePath), &meta))
    return false;
  if (meta.kind == StagedDiffKind::Clrop) {
    return false;
  }
  meta.stagePath = stagePath;
  meta.metaPath = StageMetaPath(stagePath);
  meta.revision = NextRevisionFor(meta.kind, meta.targetPath);
  std::wstring err;
  return WriteStageMetaFile(meta, &err);
}

bool IntegrateStagedDiff(HWND owner, const std::filesystem::path &stagePath, bool forceSkipBackup) {
  SaveOperationGuard guard;
  if (stagePath.empty())
    return false;
  StageMeta meta;
  if (!LoadStageMetaFile(StageMetaPath(stagePath), &meta)) {
    TraceSaveFailure(L"IntegrateStage", L"load_meta_failed", {}, stagePath);
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Integrate stage" : L"stage 統合",
        IsEnglishUi() ? L"Failed to read stage metadata."
                      : L"stage metadata の読み込みに失敗しました。");
    return false;
  }
  meta.stagePath = stagePath;
  meta.metaPath = StageMetaPath(stagePath);
  std::wstring err;
  
  auto doSkipBackup = [&]() -> bool {
    SilentDialogOptions confirm;
    confirm.title = IsEnglishUi() ? L"Confirmation" : L"確認";
    confirm.message = IsEnglishUi()
                          ? L"Save will be attempted without creating a "
                            L"backup.\nIf the file gets corrupted, you might "
                            L"not be able to restore it.\nAre you sure?"
                          : L"バックアップを作成せずに保存します。\n万が一保"
                            L"存中にファイルが破損した場合、復元できなくなる"
                            L"可能性があります。\nよろしいですか？";
    confirm.kind = SoftNoticeKind::Warning;
    confirm.buttons = SilentDialogButtons::YesNo;
    confirm.defaultResult = SilentDialogResult::No;
    confirm.escapeResult = SilentDialogResult::No;
    
    if (ShowSilentDialog(owner, confirm) == SilentDialogResult::Yes) {
      if (!IntegrateStageMetaToDestination(owner, meta,
                                           /*discardOtherIfLatest=*/true,
                                           &err, /*skipBackup=*/true)) {
        ShowSilentMessageDialog(owner, IsEnglishUi() ? L"Integrate stage" : L"stage 統合",
                                (IsEnglishUi() ? L"Retry also failed:\n"
                                               : L"再試行も失敗しました:\n") +
                                    err,
                                SoftNoticeKind::Error);
        return false;
      }
      return true;
    }
    return false;
  };
  
  if (forceSkipBackup) {
    return doSkipBackup();
  }

  if (!IntegrateStageMetaToDestination(owner, meta,
                                       /*discardOtherIfLatest=*/true, &err)) {
    SilentDialogOptions options;
    options.title = IsEnglishUi() ? L"Integrate stage" : L"stage 統合";
    options.message = err;
    options.kind = SoftNoticeKind::Error;
    options.buttons = SilentDialogButtons::YesNo;
    options.yesLabel = IsEnglishUi() ? L"Close" : L"閉じる";
    options.noLabel = IsEnglishUi() ? L"Try saving without backup"
                                    : L"バックアップ無しで保存を試みる";
    options.defaultResult = SilentDialogResult::Yes;
    options.escapeResult = SilentDialogResult::Yes;

    if (ShowSilentDialog(owner, options) == SilentDialogResult::No) {
      return doSkipBackup();
    }
    return false;
  }
  return true;
}

bool DiscardStagedDiff(const std::filesystem::path &stagePath) {
  if (stagePath.empty())
    return false;
  StageMeta meta;
  if (!LoadStageMetaFile(StageMetaPath(stagePath), &meta)) {
    DiscardNoteStageArtifactsForStage(stagePath);
    return RemoveStageAndMeta(stagePath);
  }
  RemoveStageAndMeta(stagePath);
  if (meta.kind == StagedDiffKind::Clrop) {
    DiscardAnnotJournalSegmentsForBase(meta.targetPath,
                                       meta.revision.value_or(0));
  } else if (meta.kind == StagedDiffKind::Note) {
    DiscardNoteStageArtifactsForStage(stagePath);
  }
  UpdateCurrentIntegrateFlags();
  return true;
}

std::filesystem::path StagedNotePathFor(const std::wstring &notePath) {
  auto latest = FindLatestStageMeta(StagedDiffKind::Note, notePath);
  return latest.has_value() ? latest->stagePath : std::filesystem::path{};
}

std::filesystem::path StagedClropPathForPdf(const std::wstring &pdfPath) {
  auto latest = FindLatestStageMeta(StagedDiffKind::Clrop, pdfPath);
  return latest.has_value() ? latest->stagePath : std::filesystem::path{};
}

std::optional<StagedNoteSnapshotRef>
FindLatestStagedNoteSnapshotFor(const std::wstring &notePath) {
  auto latest = FindLatestStageMeta(StagedDiffKind::Note, notePath);
  if (!latest.has_value())
    return std::nullopt;
  uint64_t noteId = latest->noteId.value_or(0);
  const uint64_t contentRevision = latest->contentRevision.value_or(0);
  uint64_t basePersistenceRevision =
      latest->basePersistenceRevision.value_or(0);
  uint64_t persistenceRevision = latest->persistenceRevision.value_or(0);
  if (!latest->persistenceRevision.has_value()) {
    const note::NoteIdentity identity =
        note::ResolveRuntimeNoteIdentityPath(notePath);
    if (identity.note_id.valid()) {
      noteId = identity.note_id.value;
      if (const auto record =
              note::FindRuntimeNotePersistenceRecord(identity.note_id)) {
        basePersistenceRevision = record->persistence_revision;
        if (basePersistenceRevision != UINT64_MAX) {
          persistenceRevision = basePersistenceRevision + 1;
        }
      }
    }
  }
  return StagedNoteSnapshotRef{
      latest->stagePath,   noteId,
      contentRevision,     basePersistenceRevision,
      persistenceRevision, latest->noteRestoreView,
  };
}

std::filesystem::path
FindLatestStagedNotePathFor(const std::wstring &notePath) {
  const auto latest = FindLatestStagedNoteSnapshotFor(notePath);
  return latest.has_value() ? latest->stagePath : std::filesystem::path{};
}

std::filesystem::path
FindLatestStagedClropPathForPdf(const std::wstring &pdfPath) {
  auto latest = FindLatestStageMeta(StagedDiffKind::Clrop, pdfPath);
  return latest.has_value() ? latest->stagePath : std::filesystem::path{};
}

bool LoadResolvedStagedNoteBytes(const std::wstring &notePath,
                                 const std::filesystem::path &stagePath,
                                 std::string *outBytes, std::wstring *outErr) {
  if (outBytes)
    outBytes->clear();
  StageMeta meta;
  if (!stagePath.empty()) {
    if (!LoadStageMetaFile(StageMetaPath(stagePath), &meta)) {
      if (outErr)
        *outErr = L"stage metadata の読み込みに失敗しました。";
      return false;
    }
    meta.stagePath = stagePath;
    meta.metaPath = StageMetaPath(stagePath);
  } else {
    auto latest = FindLatestStageMeta(StagedDiffKind::Note, notePath);
    if (!latest.has_value()) {
      if (outErr)
        *outErr = L"note stage が見つかりません。";
      return false;
    }
    meta = *latest;
  }
  ResolvedNoteStageData data;
  if (!LoadResolvedStageNoteDataForMeta(meta, &data, outErr)) {
    return false;
  }
  if (outBytes)
    *outBytes = std::move(data.resolvedBytes);
  return true;
}

bool LoadResolvedStagedAnnotations(const std::wstring &pdfPath,
                                   const std::filesystem::path &stagePath,
                                   std::vector<Annotation> *outAnnotations,
                                   std::wstring *outErr) {
  if (outAnnotations)
    outAnnotations->clear();
  StageMeta meta;
  if (!stagePath.empty()) {
    if (!LoadStageMetaFile(StageMetaPath(stagePath), &meta)) {
      if (outErr)
        *outErr = L"stage metadata の読み込みに失敗しました。";
      return false;
    }
    meta.stagePath = stagePath;
    meta.metaPath = StageMetaPath(stagePath);
  } else {
    auto latest = FindLatestStageMeta(StagedDiffKind::Clrop, pdfPath);
    if (!latest.has_value()) {
      if (outErr)
        *outErr = L"注釈 stage が見つかりません。";
      return false;
    }
    meta = *latest;
  }
  return LoadResolvedStageAnnotationsForMeta(meta, outAnnotations, outErr);
}

void DiscardOtherStagedNoteFilesFor(
    const std::wstring &notePath, const std::filesystem::path &keepStagePath) {
  const std::wstring targetKey =
      NormalizePathKey(std::filesystem::path(notePath));
  for (const auto &entry : ListStagedDiffEntries()) {
    if (entry.kind != StagedDiffKind::Note)
      continue;
    if (NormalizePathKey(std::filesystem::path(entry.targetPath)) != targetKey)
      continue;
    if (!keepStagePath.empty() && IsSamePath(entry.stagePath, keepStagePath))
      continue;
    RemoveStageAndMeta(entry.stagePath);
    DiscardNoteStageArtifactsForStage(entry.stagePath);
  }
}

void DiscardRedundantStagedNoteFilesMatchingOriginal(
    const std::wstring &notePath) {
  const std::wstring targetKey =
      NormalizePathKey(std::filesystem::path(notePath));
  for (const auto &entry : ListStagedDiffEntries()) {
    if (entry.kind != StagedDiffKind::Note)
      continue;
    if (NormalizePathKey(std::filesystem::path(entry.targetPath)) != targetKey)
      continue;

    StageMeta meta;
    if (!LoadStageMetaFile(entry.metaPath, &meta))
      continue;
    meta.stagePath = entry.stagePath;
    meta.metaPath = entry.metaPath;
    if (!StageResolvedContentMatchesOriginal(meta))
      continue;

    RemoveStageAndMeta(entry.stagePath);
    DiscardNoteStageArtifactsForStage(entry.stagePath);
  }
}

void DiscardOtherStagedClropFilesForPdf(
    const std::wstring &pdfPath, const std::filesystem::path &keepStagePath) {
  const std::wstring targetKey =
      NormalizePathKey(std::filesystem::path(pdfPath));
  std::optional<uint64_t> keepRevision;
  for (const auto &entry : ListStagedDiffEntries()) {
    if (entry.kind != StagedDiffKind::Clrop)
      continue;
    if (NormalizePathKey(std::filesystem::path(entry.targetPath)) != targetKey)
      continue;
    if (!keepStagePath.empty() && IsSamePath(entry.stagePath, keepStagePath)) {
      keepRevision = entry.revision;
      continue;
    }
    RemoveStageAndMeta(entry.stagePath);
  }
  DiscardAnnotJournalSegmentsForTarget(pdfPath, keepRevision);
}

void DiscardRedundantStagedClropFilesMatchingOriginal(
    const std::wstring &pdfPath) {
  const std::wstring targetKey =
      NormalizePathKey(std::filesystem::path(pdfPath));
  for (const auto &entry : ListStagedDiffEntries()) {
    if (entry.kind != StagedDiffKind::Clrop)
      continue;
    if (NormalizePathKey(std::filesystem::path(entry.targetPath)) != targetKey)
      continue;

    StageMeta meta;
    if (!LoadStageMetaFile(entry.metaPath, &meta))
      continue;
    meta.stagePath = entry.stagePath;
    meta.metaPath = entry.metaPath;
    if (!StageResolvedContentMatchesOriginal(meta))
      continue;

    RemoveStageAndMeta(entry.stagePath);
    DiscardAnnotJournalSegmentsForBase(meta.targetPath,
                                       meta.revision.value_or(0));
  }
}

bool RestoreFromBackupMeta(HWND owner,
                           const std::filesystem::path &backupMetaPath,
                           std::filesystem::path *outDest) {
  SaveOperationGuard guard;
  if (outDest)
    outDest->clear();
  BackupMetaInfo meta;
  if (!LoadBackupMetaFile(backupMetaPath, &meta)) {
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Restore backup" : L"バックアップ復元",
        IsEnglishUi() ? L"Failed to read backup metadata."
                      : L"バックアップ metadata の読み込みに失敗しました。");
    return false;
  }
  auto backupBytes = ReadFileBytes(meta.backupPath);
  if (!backupBytes.has_value()) {
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Restore backup" : L"バックアップ復元",
        IsEnglishUi() ? L"Failed to read backup data."
                      : L"バックアップ本体の読み込みに失敗しました。");
    return false;
  }
  std::wstring err;
  if (!CreateBackupIfNeeded(meta.kind, meta.destPath, nullptr, nullptr, &err)) {
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Restore backup" : L"バックアップ復元", err);
    return false;
  }
  if (!AtomicWriteVerified(meta.destPath, *backupBytes, &err)) {
    ShowStageMessageDialog(
        owner, IsEnglishUi() ? L"Restore backup" : L"バックアップ復元", err);
    return false;
  }
  if (outDest)
    *outDest = meta.destPath;
  return true;
}

bool DeleteBackupMeta(const std::filesystem::path &backupMetaPath,
                      std::wstring *outErr) {
  SaveOperationGuard guard;
  return DeleteBackupMetaFiles(backupMetaPath, outErr);
}

} // namespace file_output
