#include "note/note_persistence.h"

#include <limits>

namespace note {

NotePersistenceCommitDecision ResolveNotePersistenceCommit(
    const NotePersistenceObservation& observation,
    const NotePersistenceCommitIntent& intent) {
    if (!observation.note_id.valid() ||
        !intent.note_id.valid() ||
        observation.note_id != intent.note_id ||
        intent.expected_base_revision == std::numeric_limits<uint64_t>::max() ||
        intent.committed_revision == 0 ||
        intent.committed_revision != intent.expected_base_revision + 1 ||
        intent.content_fingerprint == 0 ||
        (observation.destination_exists && observation.content_fingerprint == 0) ||
        (!observation.destination_exists && observation.content_fingerprint != 0)) {
        return NotePersistenceCommitDecision::InvalidInput;
    }

    if (observation.destination_exists &&
        observation.persistence_revision == intent.committed_revision &&
        observation.content_fingerprint == intent.content_fingerprint) {
        return NotePersistenceCommitDecision::AlreadyWritten;
    }
    if (observation.persistence_revision == intent.expected_base_revision) {
        return NotePersistenceCommitDecision::ReadyToWrite;
    }
    return NotePersistenceCommitDecision::Conflict;
}

} // namespace note
