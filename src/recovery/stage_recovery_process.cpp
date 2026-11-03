#include "app/startup_instance.h"
#include "bridge/view_bridge.h"
#include "clrop/hash.h"
#include "core/app_core.h"
#include "core/atomic_write.h"
#include "core/document_source_protection.h"
#include "core/localization.h"
#include "core/private_fonts.h"
#include "core/ui_notify.h"
#include "diagnostics/write_checks.h"
#include "file_output/file_output.h"
#include "note/note_identity_store.h"
#include "pdf_view/pdf_view.h"
#include "recovery/stage_recovery.h"
#include "recovery/note_write_channel.h"
#include "ui/core/main_window_api.h"
#include "ui/window_corners.h"
#include "workspace/workspace_actions.h"
#include "workspace/workspace_write_lock.h"
#include <map>
#include <memory>
#include <objbase.h>
#include <shellapi.h>

namespace recovery {
namespace {
std::filesystem::path pending;
bool active = false;
bool recoveryProcess = false;
NoteWriteChannel *noteWriter = nullptr;
std::wstring T(const wchar_t *id) {
  return localization::Text(std::wstring(L"recovery.app.") + id);
}
bool Same(const std::filesystem::path &a, const std::filesystem::path &b) {
  return !a.empty() && !b.empty() &&
         document_source_protection::Key(a) ==
             document_source_protection::Key(b);
}
struct Handle {
  HANDLE value = nullptr;
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
std::wstring Executable() {
  std::wstring path(32768, L'\0');
  const auto n =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (n == 0 || n >= path.size())
    return {};
  path.resize(n);
  return path;
}
std::wstring Quote(const std::wstring &value) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t ch : value) {
    if (ch == L'\\') {
      ++slashes;
      continue;
    }
    if (ch == L'\"') {
      result.append(slashes * 2 + 1, L'\\');
      result += ch;
    } else {
      result.append(slashes, L'\\');
      result += ch;
    }
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  return result + L'\"';
}
std::wstring Token() {
  GUID id{};
  if (FAILED(CoCreateGuid(&id)))
    return {};
  wchar_t text[40]{};
  if (!StringFromGUID2(id, text, 40))
    return {};
  std::wstring token;
  for (wchar_t ch : std::wstring(text))
    if (iswxdigit(ch))
      token += static_cast<wchar_t>(towlower(ch));
  return token;
}
std::filesystem::path RequestPath(const std::filesystem::path &workspace,
                                  const std::wstring &token) {
  return workspace / L"__pdf_note_workspace__" / L"__tmp__" / L"recovery" /
         (L"launch_" + token + L".bin");
}
std::string RequestBytes(const std::filesystem::path &stage, DWORD pid,
                         HWND owner) {
  return "recovery-launch-v1\n" + std::to_string(pid) + "\n" +
         std::to_string(reinterpret_cast<uintptr_t>(owner)) + "\n" +
         stage.generic_u8string();
}
std::vector<wchar_t> Environment(const std::filesystem::path &directory) {
  struct Less {
    bool operator()(const std::wstring &a, const std::wstring &b) const {
      return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
  };
  std::map<std::wstring, std::wstring, Less> values;
  LPWCH block = GetEnvironmentStringsW();
  if (!block)
    return {};
  for (const wchar_t *at = block; *at; at += wcslen(at) + 1) {
    const std::wstring line(at);
    const auto separator = line.find(L'=', line[0] == L'=' ? 1 : 0);
    if (separator == std::wstring::npos)
      continue;
    const auto key = line.substr(0, separator);
    if (key.size() >= 9 && _wcsnicmp(key.c_str(), L"PDF_NOTE_", 9) == 0)
      continue;
    values[key] = line.substr(separator + 1);
  }
  FreeEnvironmentStringsW(block);
  for (const auto *name :
       {L"TEMP", L"TMP", L"APPDATA", L"LOCALAPPDATA", L"USERPROFILE", L"HOME"})
    values[name] = directory.wstring();
  std::vector<wchar_t> result;
  for (const auto &entry : values) {
    const auto line = entry.first + L"=" + entry.second;
    result.insert(result.end(), line.begin(), line.end());
    result.push_back(0);
  }
  result.push_back(0);
  return result;
}
struct ModalGate {
  std::vector<HWND> enabled;
  explicit ModalGate(HWND owner) {
    // Collect before disabling; allocation failure cannot leave an HWND
    // disabled after a failed constructor.
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
          auto *self = reinterpret_cast<ModalGate *>(parameter);
          DWORD pid = 0;
          GetWindowThreadProcessId(window, &pid);
          if (pid == GetCurrentProcessId() && IsWindowEnabled(window))
            self->enabled.push_back(window);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
    if (owner &&
        std::find(enabled.begin(), enabled.end(), owner) == enabled.end() &&
        IsWindowEnabled(owner))
      enabled.push_back(owner);
    active = true;
    for (HWND window : enabled)
      EnableWindow(window, FALSE);
  }
  ~ModalGate() {
    for (HWND window : enabled)
      if (IsWindow(window))
        EnableWindow(window, TRUE);
    active = false;
  }
};
void RefreshSaved(HWND owner, const Review &review, bool wasCurrentNote) {
  if (!review.candidate.saved)
    return;
  const bool exists =
      GetFileAttributesW(
          ToExtendedWin32PathIfAbsoluteLocal(review.candidate.destination)
              .c_str()) != INVALID_FILE_ATTRIBUTES;
  if (exists != review.candidate.destinationExists)
    return;
  std::string hash;
  auto native = review.candidate.destination;
  native.make_preferred();
  if (review.candidate.destinationExists &&
      (!clrop::ComputeFileSha256(native, &hash) ||
       hash != review.candidate.destinationHash))
    return;
  if (!review.isNote) {
    auto pdf = review.candidate.pdf;
    pdf.make_preferred();
    if (!clrop::ComputeFileSha256(pdf, &hash) ||
        hash != review.candidate.pdfHash)
      return;
  }
  if (review.isNote && wasCurrentNote) {
    std::wstring error;
    if (!note::ConfigureRuntimeNoteIdentityStore(g_workspaceRoot, &error)) {
      ShowSoftNotice(owner, T(L"refresh_failed"), SoftNoticeKind::Warning);
      return;
    }
    // The pre-recovery editor checkpoint and the recovery snapshots remain
    // durable. Reopen the chosen final document, never the stale cache.
    ClearNoteEditorSilently(owner);
    if (!LoadNoteFile(owner, review.candidate.destination.wstring())) {
      ShowSoftNotice(owner, T(L"refresh_failed"), SoftNoticeKind::Warning);
    }
  } else if (!review.isNote &&
             Same(review.originalTarget, CurrentLogicalPdfPath())) {
    if (Same(review.candidate.pdf, CurrentLogicalPdfPath())) {
      g_annots = review.candidate.annotations;
      g_annotsDirty = false;
      g_annotsNeedsIntegrate = false;
      ClearPdfAnnotationHistory();
      InvalidateRect(g_hPdfView, nullptr, FALSE);
    } else {
      g_annotsDirty = false;
      g_annotsNeedsIntegrate = false;
      if (!OpenPdfWithAnnotations(owner, review.candidate.pdf.wstring()))
        ShowSoftNotice(owner, T(L"refresh_failed"), SoftNoticeKind::Warning);
    }
  }
  RefreshMainWindowUiState(owner);
}
} // namespace

bool HasActiveStageRecovery() { return active; }
bool HasPendingStageRecovery() { return !pending.empty(); }
bool IsStageRecoveryProcess() { return recoveryProcess; }
void RequestStageRecovery(HWND owner, const std::filesystem::path &stage) {
  if (!owner || !IsWindow(owner) || stage.empty() || active ||
      IsUiAutomationEnabled())
    return;
  if (pending.empty())
    pending = stage;
  PostMessageW(g_hMainWnd ? g_hMainWnd : owner, kMsgStageRecovery, 0, 0);
}
static void HandleRequest(HWND owner) {
  if (active || pending.empty())
    return;
  const auto stage = pending;
  pending.clear();
  if (IsSaveOperationInProgress() || IsSaveTransactionRunning() ||
      HasActiveOfficeConversionJobs()) {
    ShowSoftNotice(owner, T(L"busy"), SoftNoticeKind::Warning);
    return;
  }
  if (!file_output::SaveNoteIfDirty(owner) ||
      !file_output::SaveAnnotationsIfDirty(owner)) {
    ShowSoftNotice(owner, T(L"protect_failed"), SoftNoticeKind::Warning);
    return;
  }
  Review review;
  {
    std::wstring error;
    WorkspaceOperationLock lock(g_workspaceRoot, &error);
    if (!lock.acquired() ||
        !file_output::PrepareStageRecovery(stage, &review)) {
      ShowSoftNotice(owner, T(L"open_failed"), SoftNoticeKind::Warning);
      return;
    }
  }
  const auto token = Token();
  const auto executable = Executable();
  if (token.size() != 32 || executable.empty())
    return;
  const auto request = RequestPath(g_workspaceRoot, token);
  std::wstring error;
  const auto launchBytes = RequestBytes(stage, GetCurrentProcessId(), owner);
  std::string launchRead;
  const auto managed =
      std::filesystem::path(g_workspaceRoot) / L"__pdf_note_workspace__";
  if (!atomic_write::AtomicWriteUtf8(request, launchBytes, managed / L"__tmp__",
                                     managed / L"__escape__", &error) ||
      !ReadFileBytesNoFollowLimitedWin32(request, 128 * 1024, launchRead) ||
      launchRead != launchBytes) {
    ShowSoftNotice(owner, T(L"protect_failed"), SoftNoticeKind::Warning);
    return;
  }
  const auto processDirectory = review.directory / (L"process_" + token);
  if (!atomic_write::EnsureDirectoryExists(processDirectory)) {
    ShowSoftNotice(owner, T(L"open_failed"), SoftNoticeKind::Warning);
    return;
  }
  auto environment = Environment(processDirectory);
  if (environment.empty())
    return;
  auto command = Quote(executable) + L" --stage-recovery " +
                 Quote(g_workspaceRoot) + L" " + Quote(stage.wstring()) + L" " +
                 token + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
                 std::to_wstring(reinterpret_cast<uintptr_t>(owner));
  if (command.size() >= 32767) {
    ShowSoftNotice(owner, T(L"open_failed"), SoftNoticeKind::Warning);
    return;
  }
  const bool wasCurrentNote = review.isNote &&
                             Same(review.originalTarget, g_currentNotePath);
  std::unique_ptr<DocumentOpenLockCandidate> sourceLock;
  if (!review.originalTarget.empty()) {
    sourceLock = std::make_unique<DocumentOpenLockCandidate>(
        review.originalTarget, &error);
    if (!sourceLock->acquired()) {
      ShowSoftNotice(owner, T(L"open_failed"), SoftNoticeKind::Warning);
      return;
    }
  }
  NoteWriteChannel writer(review, token);
  if (!writer.CreateServer()) {
    ShowSoftNotice(owner, T(L"open_failed"), SoftNoticeKind::Warning);
    return;
  }
  SaveOperationGuard operation;
  ModalGate gate(owner);
  Handle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.value ||
      !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits)))
    return;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_SHOWNORMAL;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                      environment.data(), processDirectory.c_str(), &startup,
                      &process)) {
    ShowSoftNotice(owner, T(L"open_failed"), SoftNoticeKind::Warning);
    return;
  }
  Handle child(process.hProcess), thread(process.hThread);
  if (!AssignProcessToJobObject(job.value, child.value) ||
      ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
    TerminateProcess(child.value, 1);
    WaitForSingleObject(child.value, 5000);
    ShowSoftNotice(owner, T(L"open_failed"), SoftNoticeKind::Warning);
    return;
  }
  bool quit = false;
  int quitCode = 0;
  const HANDLE waits[] = {child.value, writer.requestEvent()};
  for (;;) {
    const DWORD wait = MsgWaitForMultipleObjects(2, waits, FALSE,
                                                 INFINITE, QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait == WAIT_OBJECT_0 + 1) {
      if (!writer.ServeOnce()) {
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(child.value, 5000);
        break;
      }
      continue;
    }
    if (wait != WAIT_OBJECT_0 + 2) {
      TerminateJobObject(job.value, 1);
      WaitForSingleObject(child.value, 5000);
      break;
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        quit = true;
        quitCode = static_cast<int>(message.wParam);
        TerminateJobObject(job.value, 1);
        continue;
      }
      // No accelerator, menu, timer, drag/drop, external-open or edit
      // dispatch can bypass disabled HWNDs through this nested loop.
      if (message.message == WM_COMMAND || message.message == WM_NOTIFY ||
          message.message == WM_TIMER || message.message == WM_CLOSE ||
          message.message == WM_COPYDATA || message.message == WM_DROPFILES ||
          (message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST) ||
          (message.message >= WM_MOUSEFIRST && message.message <= WM_MOUSELAST))
        continue;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (sourceLock) {
    DWORD protectionError = 0;
    if (!document_source_protection::Acquire(review.originalTarget,
                                               &protectionError)) {
      // Do not leave an editable, unprotected original after failed reopen.
      // Both the ordinary editor checkpoint and the recovery candidate exist.
      if (Same(review.originalTarget, g_currentNotePath))
        ClearNoteEditorSilently(owner);
      ShowSoftNotice(owner, T(L"refresh_failed"), SoftNoticeKind::Warning);
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(child.value, &code);
  Review finalReview;
  {
    WorkspaceOperationLock lock(g_workspaceRoot, &error);
    if (lock.acquired() &&
        file_output::PrepareStageRecovery(stage, &finalReview))
      RefreshSaved(owner, finalReview, wasCurrentNote);
    else
      ShowSoftNotice(owner, T(L"refresh_failed"), SoftNoticeKind::Warning);
  }
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND window, LPARAM) -> BOOL {
        wchar_t name[128]{};
        GetClassNameW(window, name, 128);
        if (wcscmp(name, L"PdfWorkspaceStageManagerDlg") == 0)
          PostMessageW(window, kMsgStageRecovery, 0, 0);
        return TRUE;
      },
      0);
  if (code != static_cast<DWORD>(ReviewExit::Saved) &&
      code != static_cast<DWORD>(ReviewExit::Deferred))
    ShowSoftNotice(owner, T(L"interrupted"), SoftNoticeKind::Warning);
  if (quit)
    PostQuitMessage(quitCode);
}
void HandleStageRecoveryRequest(HWND owner) {
  try {
    HandleRequest(owner);
  } catch (...) {
    ShowSoftNotice(owner, T(L"interrupted"), SoftNoticeKind::Warning);
  }
}

bool WriteStageRecoveryOriginalNote(const std::filesystem::path &destination,
                                   std::string_view bytes, bool exists,
                                   uint64_t fingerprint, std::wstring *error) {
  if (recoveryProcess && noteWriter &&
      noteWriter->Write(destination, bytes, exists, fingerprint))
    return true;
  if (error) *error = T(L"save_failed");
  return false;
}

bool TryRunStageRecoveryProcess(int *exitCode) {
  int count = 0;
  LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments)
    return false;
  struct Free {
    LPWSTR *p;
    ~Free() { LocalFree(p); }
  } free{arguments};
  if (count < 2 || std::wstring(arguments[1]) != L"--stage-recovery")
    return false;
  if (exitCode)
    *exitCode = 1;
  try {
    if (count != 7)
      return true;
    const std::filesystem::path workspace(arguments[2]), stage(arguments[3]);
    const std::wstring token(arguments[4]);
    if (!workspace.is_absolute() || !write_checks::IsSafeLocalPath(workspace) ||
        token.size() != 32 ||
        token.find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
      return true;
    const std::wstring pidText(arguments[5]), handleText(arguments[6]);
    if (pidText.empty() || handleText.empty() ||
        pidText.find_first_not_of(L"0123456789") != std::wstring::npos ||
        handleText.find_first_not_of(L"0123456789") != std::wstring::npos)
      return true;
    const auto pid = std::stoull(pidText), handle = std::stoull(handleText);
    if (pid == 0 || pid > MAXDWORD || handle > UINTPTR_MAX)
      return true;
    const HWND parent = reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
    DWORD actual = 0;
    GetWindowThreadProcessId(parent, &actual);
    if (actual != pid || IsWindowEnabled(parent))
      return true;
    Handle parentProcess(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                    static_cast<DWORD>(pid)));
    wchar_t parentPath[32768]{};
    DWORD length = 32768;
    if (!parentProcess.value ||
        !QueryFullProcessImageNameW(parentProcess.value, 0, parentPath,
                                    &length) ||
        !Same(parentPath, Executable()))
      return true;
    const auto request = RequestPath(workspace, token);
    std::string bytes;
    if (!write_checks::IsSafeLocalPath(request) ||
        !ReadFileBytesNoFollowLimitedWin32(request, 128 * 1024, bytes) ||
        bytes != RequestBytes(stage, static_cast<DWORD>(pid), parent))
      return true;
    g_workspaceRoot = workspace.wstring();
    g_hInst = GetModuleHandleW(nullptr);
    g_config.autoSaveSeconds = 0;
    recoveryProcess = true;
    std::wstring error;
    WorkspaceOperationLock lock(workspace, &error);
    if (!lock.acquired())
      return true;
    FPDF_InitLibrary();
    FPDF_SetSandBoxPolicy(FPDF_POLICY_MACHINETIME_ACCESS, false);
    LoadLibreOfficePrivateFontsForProcess();
    struct Shutdown {
      ~Shutdown() {
        ClearPdfState();
        ReleaseAllDocumentOpenLocks();
        UnloadLibreOfficePrivateFontsForProcess();
        FPDF_DestroyLibrary();
      }
    } shutdown;
    InstallGlobalBeepFilter();
    const ::ui::ScopedAuxiliaryWindowCorners corners;
    if (!note::ConfigureRuntimeNoteIdentityStore(workspace, &error))
      return true;
    Review review;
    if (!file_output::PrepareStageRecovery(stage, &review))
      return true;
    Handle session(CreateFileW(
        ToExtendedWin32PathIfAbsoluteLocal(review.directory / L"active.lock")
            .c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (session.value == INVALID_HANDLE_VALUE)
      return true;
    if (review.candidate.saved && file_output::CompleteStageRecovery(review)) {
      if (exitCode)
        *exitCode = static_cast<int>(ReviewExit::Saved);
      return true;
    }
    NoteWriteChannel writer(review, token);
    if (!writer.ConnectClient(parentProcess.value)) return true;
    struct Binding {
      explicit Binding(NoteWriteChannel &writer) { noteWriter = &writer; }
      ~Binding() { noteWriter = nullptr; }
    } binding(writer);
    const auto result = RunStageRecoveryWindow(review, parent);
    if (exitCode)
      *exitCode = static_cast<int>(result);
    return true;
  } catch (...) {
    return true;
  }
}
} // namespace recovery
