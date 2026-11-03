#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace note {

// Pending edits remain ordered in their successive canonical coordinate spaces.
// The transaction checks owner/revision continuity before accepting each edit.

// Pure, per-note transaction owned by LocalNoteKernel.  It has no Win32 or
// persistence knowledge; a failed append simply tells the owner to rebuild
// derived state from the current canonical text rather than guessing.
class PendingNoteEditTransaction {
public:
    [[nodiscard]] bool Append(NoteDerivedSnapshotIdentity before,
                              TextEdit edit);
    void Clear();

    [[nodiscard]] bool active() const { return !edits_.empty(); }
    [[nodiscard]] size_t size() const { return edits_.size(); }
    // Returns one exactly equivalent edit only for contiguous typed insertion,
    // backward deletion, or forward deletion.  Other valid edit sequences are
    // deliberately retained as a transaction and must use the conservative
    // refresh path until the checkpoint parser consumes sequences directly.
    [[nodiscard]] std::optional<TextEdit> TryBuildEquivalentEdit() const;

private:
    std::optional<NoteDerivedSnapshotIdentity> current_identity_;
    std::vector<TextEdit> edits_;
};

} // namespace note
