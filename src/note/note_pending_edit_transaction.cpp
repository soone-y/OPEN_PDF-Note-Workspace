#include "note/note_pending_edit_transaction.h"

#include <limits>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool TryAdd(size_t left, size_t right, size_t* out) {
    if (!out || right > std::numeric_limits<size_t>::max() - left) return false;
    *out = left + right;
    return true;
}

[[nodiscard]] bool IsNoOp(const TextEdit& edit) {
    return edit.deleted_len == 0 && edit.inserted_text.empty();
}

} // namespace

bool PendingNoteEditTransaction::Append(NoteDerivedSnapshotIdentity before,
                                        TextEdit edit) {
    if (!before.note_id.valid() || IsNoOp(edit) ||
        before.source_revision == std::numeric_limits<uint64_t>::max()) {
        return false;
    }

    if (!active()) {
        base_identity_ = before;
    } else if (!current_identity_.has_value() ||
               current_identity_->note_id != before.note_id ||
               current_identity_->source_revision != before.source_revision) {
        return false;
    }

    edits_.push_back(AppliedNoteEdit{before.source_revision, std::move(edit)});
    current_identity_ = NoteDerivedSnapshotIdentity{
        before.note_id, before.source_revision + 1};
    return true;
}

void PendingNoteEditTransaction::Clear() {
    base_identity_.reset();
    current_identity_.reset();
    edits_.clear();
}

std::optional<TextEdit> PendingNoteEditTransaction::TryBuildEquivalentEdit() const {
    if (edits_.empty()) return std::nullopt;

    TextEdit combined = edits_.front().edit;
    if (IsNoOp(combined)) return std::nullopt;

    for (size_t index = 1; index < edits_.size(); ++index) {
        const TextEdit& next = edits_[index].edit;
        if (IsNoOp(next)) return std::nullopt;

        size_t combinedInsertedEnd = 0;
        size_t nextDeletedEnd = 0;
        const bool canAppendInsertion =
            next.deleted_len == 0 &&
            TryAdd(combined.start.value, combined.inserted_text.size(), &combinedInsertedEnd) &&
            next.start.value == combinedInsertedEnd;
        if (canAppendInsertion) {
            combined.inserted_text += next.inserted_text;
            continue;
        }

        const bool bothPureDeletions = combined.inserted_text.empty() &&
            next.inserted_text.empty();
        if (!bothPureDeletions ||
            !TryAdd(next.start.value, next.deleted_len, &nextDeletedEnd)) {
            return std::nullopt;
        }

        if (nextDeletedEnd == combined.start.value) {
            if (next.deleted_len > std::numeric_limits<size_t>::max() - combined.deleted_len) {
                return std::nullopt;
            }
            combined.start = next.start;
            combined.deleted_len += next.deleted_len;
            continue;
        }
        if (next.start == combined.start) {
            if (next.deleted_len > std::numeric_limits<size_t>::max() - combined.deleted_len) {
                return std::nullopt;
            }
            combined.deleted_len += next.deleted_len;
            continue;
        }
        return std::nullopt;
    }
    return combined;
}

} // namespace note
