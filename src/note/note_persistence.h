#pragma once

#include "note/note_identity.h"

#include <cstdint>

namespace note {

enum class NotePersistenceCommitDecision : uint8_t {
    ReadyToWrite,
    AlreadyWritten,
    Conflict,
    InvalidInput,
};

struct NotePersistenceObservation {
    NoteId note_id{};
    bool destination_exists = false;
    uint64_t persistence_revision = 0;
    uint64_t content_fingerprint = 0;
};

struct NotePersistenceCommitIntent {
    NoteId note_id{};
    uint64_t expected_base_revision = 0;
    uint64_t committed_revision = 0;
    uint64_t content_fingerprint = 0;
};

// Pure compare-and-swap decision. Filesystem observation and writes stay in adapters.
NotePersistenceCommitDecision ResolveNotePersistenceCommit(
    const NotePersistenceObservation& observation,
    const NotePersistenceCommitIntent& intent);

} // namespace note
