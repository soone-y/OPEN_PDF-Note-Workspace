#include "core/app_core.h"
#include "core/localization.h"
#include "file_output/file_output.h"
#include "recovery/stage_recovery.h"
#include "app/startup_instance.h"

// Recovery runs only in the standalone recovery tool (standalone_recovery.cpp).
// The main app keeps just the request queue: a failed integration names its
// checkpoint here, and the UI decides whether to hand it to the tool.
namespace recovery {
namespace {
std::filesystem::path pending;
bool recoveryProcess = false;
// The standalone tool owns its originals itself (the main app is not
// running), so it writes notes directly.
bool standaloneProcess = false;
} // namespace

bool HasPendingStageRecovery() { return !pending.empty(); }
std::filesystem::path TakePendingStageRecovery() {
  auto stage = std::move(pending);
  pending.clear();
  return stage;
}
bool IsStageRecoveryProcess() { return recoveryProcess; }
void EnterStandaloneRecoveryProcess() {
  recoveryProcess = true;
  standaloneProcess = true;
}
void RequestStageRecovery(HWND owner, const std::filesystem::path &stage) {
  if (!owner || !IsWindow(owner) || stage.empty() || recoveryProcess ||
      IsUiAutomationEnabled())
    return;
  if (pending.empty())
    pending = stage;
  PostMessageW(g_hMainWnd ? g_hMainWnd : owner, kMsgStageRecovery, 0, 0);
}

bool WriteStageRecoveryOriginalNote(const std::filesystem::path &destination,
                                   std::string_view bytes, bool exists,
                                   uint64_t fingerprint, std::wstring *error) {
  if (standaloneProcess)
    return file_output::WriteRecoveryNoteInProcess(destination, bytes, exists,
                                                   fingerprint, error);
  if (error)
    *error = localization::Text(L"recovery.app.save_failed");
  return false;
}

} // namespace recovery
