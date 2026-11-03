#pragma once
#include "recovery/stage_recovery.h"

namespace recovery {
// Synchronous local handoff: the child keeps the workspace operation lock;
// only its parent owns and briefly suspends the original's physical handle.
class NoteWriteChannel {
public:
  NoteWriteChannel(const Review &review, std::wstring token);
  ~NoteWriteChannel();
  NoteWriteChannel(const NoteWriteChannel &) = delete;
  NoteWriteChannel &operator=(const NoteWriteChannel &) = delete;
  [[nodiscard]] bool CreateServer();
  [[nodiscard]] bool ConnectClient(HANDLE parent);
  [[nodiscard]] HANDLE requestEvent() const { return request_; }
  [[nodiscard]] bool ServeOnce();
  [[nodiscard]] bool Write(const std::filesystem::path &destination,
                           std::string_view bytes, bool expectedExists,
                           uint64_t expectedFingerprint);
private:
  Review review_;
  std::wstring token_;
  HANDLE request_ = nullptr, reply_ = nullptr, parent_ = nullptr;
  uint64_t sequence_ = 0;
};
} // namespace recovery
