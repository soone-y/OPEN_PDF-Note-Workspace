#pragma once

#include "note/note_identity.h"

#include <cstdint>

namespace note {

enum class NoteDerivedReadDecision : uint8_t {
    Current,
    Missing,
    OwnerMismatch,
    RevisionMismatch,
    BlockedByPendingEdit,
    BlockedByDeferredRefresh,
};

struct NoteDerivedSnapshotState {
    NoteDerivedSnapshotIdentity source_identity{};
    bool present = false;
};

struct NoteDerivedReadContext {
    NoteId current_note_id{};
    uint64_t content_revision = 0;
    bool pending_content_edit = false;
    bool deferred_refresh = false;
};

NoteDerivedReadDecision ResolveNoteDerivedRead(
    const NoteDerivedSnapshotState& derived,
    const NoteDerivedReadContext& context);
bool CanReadCurrentNoteDerivedSnapshot(
    const NoteDerivedSnapshotState& derived,
    const NoteDerivedReadContext& context);

} // namespace note
