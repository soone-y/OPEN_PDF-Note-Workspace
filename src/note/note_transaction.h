#pragma once

#include "note/note_identity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace note {

enum class NoteChangeOrigin : uint8_t {
    None,
    UserEdit,
    ProgramLoad,
    ProgramClear,
    PresentationUpdate,
    PersistenceCommit,
};

struct NoteTransactionToken {
    uint64_t value = 0;

    bool valid() const { return value != 0; }
};

struct NoteTransactionSnapshot {
    bool suppress_change = false;
    bool program_mutation_active = false;
    bool waiting_for_user_input = false;
    NoteChangeOrigin origin = NoteChangeOrigin::None;
    NoteId owner_note_id{};
    size_t depth = 0;
    uint64_t sequence = 0;
};

// UI-thread state machine for distinguishing program mutations from user edits.
class NoteTransactionCore {
public:
    NoteTransactionToken Begin(NoteChangeOrigin origin,
                               NoteId ownerNoteId,
                               bool waitForUserInputAfterCommit);
    bool Commit(NoteTransactionToken token);
    bool Cancel(NoteTransactionToken token);
    bool ObserveUserInput();

    NoteTransactionSnapshot Snapshot() const;
    bool ShouldSuppressChange() const;
    bool ProgramMutationActive() const { return !frames_.empty(); }

private:
    struct Frame {
        NoteTransactionToken token{};
        NoteChangeOrigin origin = NoteChangeOrigin::None;
        NoteId owner_note_id{};
        bool wait_for_user_input_after_commit = false;
    };

    bool End(NoteTransactionToken token, bool commit);

    std::vector<Frame> frames_;
    bool waiting_for_user_input_ = false;
    NoteChangeOrigin waiting_origin_ = NoteChangeOrigin::None;
    NoteId waiting_owner_note_id_{};
    uint64_t next_token_ = 1;
    uint64_t sequence_ = 0;
};

} // namespace note
