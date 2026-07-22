#include "note/note_transaction.h"

namespace note {

NoteTransactionToken NoteTransactionCore::Begin(
    NoteChangeOrigin origin,
    NoteId ownerNoteId,
    bool waitForUserInputAfterCommit) {
    if (origin == NoteChangeOrigin::None || origin == NoteChangeOrigin::UserEdit) return {};
    if (next_token_ == 0) next_token_ = 1;
    const NoteTransactionToken token{next_token_++};
    frames_.push_back(Frame{token, origin, ownerNoteId, waitForUserInputAfterCommit});
    ++sequence_;
    return token;
}

bool NoteTransactionCore::Commit(NoteTransactionToken token) {
    return End(token, true);
}

bool NoteTransactionCore::Cancel(NoteTransactionToken token) {
    return End(token, false);
}

bool NoteTransactionCore::End(NoteTransactionToken token, bool commit) {
    if (!token.valid() || frames_.empty() ||
        frames_.back().token.value != token.value) {
        return false;
    }
    const Frame frame = frames_.back();
    frames_.pop_back();
    if (commit && frame.wait_for_user_input_after_commit) {
        waiting_for_user_input_ = true;
        waiting_origin_ = frame.origin;
        waiting_owner_note_id_ = frame.owner_note_id;
    }
    ++sequence_;
    return true;
}

bool NoteTransactionCore::ObserveUserInput() {
    if (!frames_.empty() || !waiting_for_user_input_) return false;
    waiting_for_user_input_ = false;
    waiting_origin_ = NoteChangeOrigin::None;
    waiting_owner_note_id_ = {};
    ++sequence_;
    return true;
}

bool NoteTransactionCore::ShouldSuppressChange() const {
    return !frames_.empty() || waiting_for_user_input_;
}

NoteTransactionSnapshot NoteTransactionCore::Snapshot() const {
    NoteTransactionSnapshot snapshot;
    snapshot.suppress_change = ShouldSuppressChange();
    snapshot.program_mutation_active = !frames_.empty();
    snapshot.waiting_for_user_input = waiting_for_user_input_;
    snapshot.depth = frames_.size();
    snapshot.sequence = sequence_;
    if (!frames_.empty()) {
        snapshot.origin = frames_.back().origin;
        snapshot.owner_note_id = frames_.back().owner_note_id;
    } else if (waiting_for_user_input_) {
        snapshot.origin = waiting_origin_;
        snapshot.owner_note_id = waiting_owner_note_id_;
    }
    return snapshot;
}

} // namespace note
