#include "note/note_render_index_request.h"

#include <limits>

namespace note {

NoteRenderIndexApplyRequestValidation
ValidateNoteRenderIndexApplyRequest(const NoteRenderIndexApplyRequest& request) noexcept {
    if (!request.before_source.valid()) {
        return NoteRenderIndexApplyRequestValidation::InvalidBeforeIdentity;
    }
    if (!request.after_source.valid()) {
        return NoteRenderIndexApplyRequestValidation::InvalidAfterIdentity;
    }
    if (request.before_source.note_id != request.after_source.note_id) {
        return NoteRenderIndexApplyRequestValidation::OwnerMismatch;
    }
    if (request.before_source.source_revision == std::numeric_limits<uint64_t>::max() ||
        request.after_source.source_revision != request.before_source.source_revision + 1) {
        return NoteRenderIndexApplyRequestValidation::NonAdjacentRevision;
    }
    if (request.edit.start.value > request.before_text_length ||
        request.edit.deleted_len > request.before_text_length - request.edit.start.value) {
        return NoteRenderIndexApplyRequestValidation::InvalidEditRange;
    }

    const size_t retained_length = request.before_text_length - request.edit.deleted_len;
    if (request.edit.inserted_text.size() >
        std::numeric_limits<size_t>::max() - retained_length) {
        return NoteRenderIndexApplyRequestValidation::LengthOverflow;
    }
    if (retained_length + request.edit.inserted_text.size() != request.after_text_length) {
        return NoteRenderIndexApplyRequestValidation::LengthMismatch;
    }
    return NoteRenderIndexApplyRequestValidation::Valid;
}

} // namespace note
