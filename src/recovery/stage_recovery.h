#pragma once
#include "core/text_encoding.h"
#include "recovery/recovery_model.h"

namespace recovery {
// The parent keeps its editor/undo state and document ownership. A dedicated
// process owns only the recovery projection; it never reads normal setup or
// starts normal commands/timers. Originals and source stages remain protected
// until a verified save and durable recovery receipt have completed.
struct Source {
  std::wstring title;
  std::vector<Annotation> annotations;
  std::string note;
};
struct Artifact {
  std::filesystem::path original, copy;
  std::string hash;
};
struct Candidate {
  std::vector<Annotation> annotations;
  std::string note;
  std::filesystem::path pdf, destination;
  std::string pdfHash, destinationHash;
  bool destinationExists = false, targetConfirmed = false,
       partialAccepted = false;
  bool saved = false;
  text_encoding::Encoding noteEncoding = text_encoding::Encoding::Utf8;
};
struct Review {
  std::filesystem::path primaryStage, directory, originalTarget;
  std::vector<std::filesystem::path> stages;
  std::vector<Artifact> artifacts;
  std::vector<Source> sources;
  Candidate candidate;
  bool isNote = false, incomplete = false, recoverable = false;
};
enum class ReviewExit { Failed = 1, Deferred = 2, Saved = 3 };

// UI thread only. Queue rather than enter a modal flow inside a save guard.
void RequestStageRecovery(HWND owner, const std::filesystem::path &stage);
[[nodiscard]] bool HasPendingStageRecovery();
// Withdraws a queued (not yet opened) request so the caller can let the user
// choose first. Returns the stage that was queued, or empty when none was.
[[nodiscard]] std::filesystem::path TakePendingStageRecovery();
[[nodiscard]] bool IsStageRecoveryProcess();
// Marks this process as the standalone recovery tool (see standalone_recovery).
void EnterStandaloneRecoveryProcess();
// `pdf_note_workspace.exe --recovery [workspace] [--stage <path>] [--wait-pid <pid>]`.
// Runs before normal startup and reads no app setup.
[[nodiscard]] bool TryRunStandaloneRecovery(int *exitCode);
// Named while the standalone tool runs, so the main app does not start on it.
[[nodiscard]] std::wstring StandaloneRecoveryMutexName();
// Starts the standalone tool for this workspace after this process exits.
[[nodiscard]] bool LaunchStandaloneRecoveryAfterExit(
    const std::filesystem::path &workspace, const std::filesystem::path &stage);
// Standalone tool only: writes a recovered note to its original location.
[[nodiscard]] bool WriteStageRecoveryOriginalNote(
    const std::filesystem::path &destination, std::string_view bytes,
    bool expectedExists, uint64_t expectedFingerprint, std::wstring *error);
[[nodiscard]] ReviewExit RunStageRecoveryWindow(Review &review, HWND parent);
} // namespace recovery

namespace file_output {
// Persistence adapters share the normal stage transaction implementation.
// Candidate snapshots are immutable versions plus one atomic current record.
[[nodiscard]] bool PrepareStageRecovery(const std::filesystem::path &stage,
                                        recovery::Review *review);
[[nodiscard]] bool ProtectStageRecovery(const recovery::Review &review,
                                        const recovery::Candidate &candidate);
[[nodiscard]] bool SelectStageRecoveryPdf(recovery::Review &review,
                                          recovery::Candidate &candidate,
                                          const std::filesystem::path &pdf);
[[nodiscard]] bool
SaveStageRecovery(recovery::Review &review, recovery::Candidate &candidate,
                  const std::filesystem::path &newDestination = {});
[[nodiscard]] bool CompleteStageRecovery(recovery::Review &review);
// Standalone tool: verified atomic note write in this process.
[[nodiscard]] bool WriteRecoveryNoteInProcess(
    const std::filesystem::path &destination, std::string_view bytes,
    bool expectedExists, uint64_t expectedFingerprint, std::wstring *error);
} // namespace file_output
