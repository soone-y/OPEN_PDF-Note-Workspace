#include "note/note_revision_gate.h"

namespace note {

NoteDerivedReadDecision ResolveNoteDerivedRead(
    const NoteDerivedSnapshotState& derived,
    const NoteDerivedReadContext& context) {
    if (!derived.present) return NoteDerivedReadDecision::Missing;
    if (!derived.source_identity.note_id.valid() ||
        !context.current_note_id.valid() ||
        derived.source_identity.note_id != context.current_note_id) {
        return NoteDerivedReadDecision::OwnerMismatch;
    }
    if (derived.source_identity.source_revision == 0 ||
        context.content_revision == 0 ||
        derived.source_identity.source_revision != context.content_revision) {
        return NoteDerivedReadDecision::RevisionMismatch;
    }
    if (context.pending_content_edit) {
        return NoteDerivedReadDecision::BlockedByPendingEdit;
    }
    if (context.deferred_refresh) {
        return NoteDerivedReadDecision::BlockedByDeferredRefresh;
    }
    return NoteDerivedReadDecision::Current;
}

bool CanReadCurrentNoteDerivedSnapshot(
    const NoteDerivedSnapshotState& derived,
    const NoteDerivedReadContext& context) {
    return ResolveNoteDerivedRead(derived, context) ==
           NoteDerivedReadDecision::Current;
}

} // namespace note
