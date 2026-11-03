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
[[nodiscard]] bool HasActiveStageRecovery();
[[nodiscard]] bool HasPendingStageRecovery();
[[nodiscard]] bool IsStageRecoveryProcess();
// Original note bytes are published by the parent that owns source protection.
[[nodiscard]] bool WriteStageRecoveryOriginalNote(
    const std::filesystem::path &destination, std::string_view bytes,
    bool expectedExists, uint64_t expectedFingerprint, std::wstring *error);
void HandleStageRecoveryRequest(HWND owner);
// Recognizes only the private recovery invocation before normal startup.
[[nodiscard]] bool TryRunStageRecoveryProcess(int *exitCode);
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
// Private local-process protocol. Records stay inside the validated review root.
[[nodiscard]] bool ProtectRecoveryNoteWrite(
    const recovery::Review &review, const std::wstring &token, uint64_t sequence,
    const std::filesystem::path &destination, std::string_view bytes,
    bool expectedExists, uint64_t expectedFingerprint);
[[nodiscard]] bool ServeRecoveryNoteWrite(
    const recovery::Review &review, const std::wstring &token, uint64_t sequence);
[[nodiscard]] bool ReadRecoveryNoteWriteReply(
    const recovery::Review &review, const std::wstring &token, uint64_t sequence);
} // namespace file_output
