#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace note {

// One pending derived-state refresh may cover multiple canonical TextEdit
// operations.  Each operation remains in its own pre-edit revision so callers
// never infer order from offsets alone.
struct AppliedNoteEdit {
    uint64_t before_content_revision = 0;
    TextEdit edit;
};

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
    [[nodiscard]] const std::vector<AppliedNoteEdit>& edits() const {
        return edits_;
    }
    [[nodiscard]] std::optional<NoteDerivedSnapshotIdentity> base_identity() const {
        return base_identity_;
    }
    [[nodiscard]] std::optional<NoteDerivedSnapshotIdentity> current_identity() const {
        return current_identity_;
    }

    // Returns one exactly equivalent edit only for contiguous typed insertion,
    // backward deletion, or forward deletion.  Other valid edit sequences are
    // deliberately retained as a transaction and must use the conservative
    // refresh path until the checkpoint parser consumes sequences directly.
    [[nodiscard]] std::optional<TextEdit> TryBuildEquivalentEdit() const;

private:
    std::optional<NoteDerivedSnapshotIdentity> base_identity_;
    std::optional<NoteDerivedSnapshotIdentity> current_identity_;
    std::vector<AppliedNoteEdit> edits_;
};

} // namespace note
