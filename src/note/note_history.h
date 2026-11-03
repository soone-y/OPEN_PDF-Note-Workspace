#pragma once

#include "note/note_model.h"
#include "note/note_text_piece_sequence.h"
#include "note/note_text_boundaries.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace note {

// UTF-16 offsets describe the selection at an edit boundary. They are used to
// decide whether adjacent edits can coalesce; replay must not teleport the UI
// back to these historic view positions.
struct NoteTextSelection {
    Utf16CodeUnitOffset anchor{};
    Utf16CodeUnitOffset caret{};
};

enum class NoteHistoryOperationKind {
    Typing,
    DeleteBackward,
    DeleteForward,
    Replace,
    Other,
};

struct NoteHistoryReplay {
    TextEdit edit;
    // The canonical text that must still occupy edit.start..edit.deleted_len
    // immediately before replay.  A matching range is a precondition, not a
    // best-effort hint: replaying against different text would make an undo
    // entry mutate unrelated user content.
    std::wstring expected_deleted_text;
    // Replay requires its immutable canonical snapshot, so stale edits are
    // rejected without materializing the current document.
    NoteTextPieceSequence::Snapshot expected_snapshot;
};

// Per-NoteId, UI-independent undo/redo history. The caller applies the
// returned replay through the canonical text core and commits it only on
// success, so a failed replay can never silently discard history.
class NoteHistory {
public:
    void Clear();
    [[nodiscard]] bool CanUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool CanRedo() const { return !redo_.empty(); }

    bool Record(const NoteTextPieceSequence::Snapshot& before,
                const NoteTextPieceSequence::Snapshot& after,
                std::wstring removedText,
                const TextEdit& forward,
                NoteTextSelection selectionBefore,
                NoteTextSelection selectionAfter,
                NoteHistoryOperationKind kind,
                uint64_t tick);

    [[nodiscard]] std::optional<NoteHistoryReplay> PeekUndo() const;
    [[nodiscard]] std::optional<NoteHistoryReplay> PeekRedo() const;
    // Reserve the opposite stack before mutating canonical text.  Once this
    // succeeds, Commit* can move an entry without allocating after the edit.
    [[nodiscard]] bool PrepareUndo();
    [[nodiscard]] bool PrepareRedo();
    bool CommitUndo(const NoteTextPieceSequence::Snapshot& currentSnapshot);
    bool CommitRedo(const NoteTextPieceSequence::Snapshot& currentSnapshot);

private:
    struct Entry {
        TextEdit forward;
        TextEdit inverse;
        NoteTextSelection selectionBefore;
        NoteTextSelection selectionAfter;
        NoteHistoryOperationKind kind = NoteHistoryOperationKind::Other;
        std::optional<TextUnitClass> unit_class;
        uint64_t tick = 0;
        NoteTextPieceSequence::Snapshot before_snapshot;
        NoteTextPieceSequence::Snapshot after_snapshot;
    };

    static bool CanMerge(const Entry& previous, const Entry& next);
    static void Merge(Entry* previous, Entry next);
    bool Store(Entry next);

    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};

} // namespace note
