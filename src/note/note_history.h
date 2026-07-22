#pragma once

#include "note/note_model.h"
#include "note/note_text_boundaries.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace note {

// UTF-16 offsets are preserved exactly so undo/redo restores the user's caret
// and selection, not merely the text.
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
    NoteTextSelection selection;
};

// Per-NoteId, UI-independent undo/redo history. The caller applies the
// returned replay through the canonical text core and commits it only on
// success, so a failed replay can never silently discard history.
class NoteHistory {
public:
    void Clear();
    [[nodiscard]] bool CanUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool CanRedo() const { return !redo_.empty(); }

    bool Record(std::wstring_view textBefore,
                const TextEdit& forward,
                NoteTextSelection selectionBefore,
                NoteTextSelection selectionAfter,
                NoteHistoryOperationKind kind,
                uint64_t tick);

    [[nodiscard]] std::optional<NoteHistoryReplay> PeekUndo() const;
    [[nodiscard]] std::optional<NoteHistoryReplay> PeekRedo() const;
    bool CommitUndo();
    bool CommitRedo();

private:
    struct Entry {
        TextEdit forward;
        TextEdit inverse;
        NoteTextSelection selectionBefore;
        NoteTextSelection selectionAfter;
        NoteHistoryOperationKind kind = NoteHistoryOperationKind::Other;
        std::optional<TextUnitClass> unit_class;
        uint64_t tick = 0;
    };

    static bool CanMerge(const Entry& previous, const Entry& next);
    static void Merge(Entry* previous, Entry next);

    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};

} // namespace note
