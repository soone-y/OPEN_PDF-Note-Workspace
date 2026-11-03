#include "recovery/note_write_channel.h"
#include "core/document_source_protection.h"

namespace recovery {
NoteWriteChannel::NoteWriteChannel(const Review &review, std::wstring token)
    : review_(review), token_(std::move(token)) {}
NoteWriteChannel::~NoteWriteChannel() {
  if (request_) CloseHandle(request_);
  if (reply_) CloseHandle(reply_);
}
bool NoteWriteChannel::CreateServer() {
  if (token_.size() != 32 ||
      token_.find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
    return false;
  const auto name = L"Local\\PdfNoteRecovery_" + token_;
  request_ = CreateEventW(nullptr, FALSE, FALSE, (name + L"_write").c_str());
  if (!request_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
  reply_ = CreateEventW(nullptr, FALSE, FALSE, (name + L"_reply").c_str());
  return reply_ && GetLastError() != ERROR_ALREADY_EXISTS;
}
bool NoteWriteChannel::ConnectClient(HANDLE parent) {
  if (!parent || parent == INVALID_HANDLE_VALUE || token_.size() != 32 ||
      token_.find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
    return false;
  const auto name = L"Local\\PdfNoteRecovery_" + token_;
  request_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"_write").c_str());
  reply_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                      (name + L"_reply").c_str());
  parent_ = parent; // Borrowed; caller retains it until this channel is destroyed.
  return request_ && reply_;
}
bool NoteWriteChannel::ServeOnce() {
  if (!request_ || !reply_ || parent_ || sequence_ == UINT64_MAX) return false;
  ++sequence_;
  // Even an unrecordable reply wakes the child. It must then fail closed and
  // use its durable save intent to recognize any already-published bytes.
  (void)file_output::ServeRecoveryNoteWrite(review_, token_, sequence_);
  DWORD error = 0;
  if (review_.isNote && !review_.originalTarget.empty() &&
      !document_source_protection::Acquire(review_.originalTarget, &error))
    return false; // Parent must end the session rather than wait unprotected.
  return SetEvent(reply_) != FALSE;
}
bool NoteWriteChannel::Write(const std::filesystem::path &destination,
                            std::string_view bytes, bool expectedExists,
                            uint64_t expectedFingerprint) {
  if (!parent_ || !request_ || !reply_ || sequence_ == UINT64_MAX) return false;
  ++sequence_;
  if (!file_output::ProtectRecoveryNoteWrite(
          review_, token_, sequence_, destination, bytes, expectedExists,
          expectedFingerprint)) {
    // A request that was never signalled does not consume a server sequence.
    --sequence_;
    return false;
  }
  if (!ResetEvent(reply_) || !SetEvent(request_)) return false;
  const HANDLE handles[] = {parent_, reply_};
  if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
    return false;
  return file_output::ReadRecoveryNoteWriteReply(review_, token_, sequence_);
}
} // namespace recovery
