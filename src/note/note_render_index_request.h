#pragma once

#include "note/note_model.h"

#include <cstddef>

namespace note {

// A render-line index may apply an edit without re-comparing an entire text
// suffix only when this request binds the old published cache to the exact
// adjacent canonical revision accepted by NoteTextCore.
struct NoteRenderIndexApplyRequest {
    NoteDerivedSnapshotIdentity before_source{};
    NoteDerivedSnapshotIdentity after_source{};
    TextEdit edit{};
    size_t before_text_length = 0;
    size_t after_text_length = 0;
};

enum class NoteRenderIndexApplyRequestValidation {
    Valid,
    InvalidBeforeIdentity,
    InvalidAfterIdentity,
    OwnerMismatch,
    NonAdjacentRevision,
    InvalidEditRange,
    LengthOverflow,
    LengthMismatch,
};

[[nodiscard]] NoteRenderIndexApplyRequestValidation
ValidateNoteRenderIndexApplyRequest(const NoteRenderIndexApplyRequest& request) noexcept;

} // namespace note
