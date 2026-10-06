#include "note/note_history.h"

#include <algorithm>
#include <limits>

namespace note {
namespace {

bool IsCollapsed(NoteTextSelection selection) {
    return selection.anchor == selection.caret;
}

bool IsInsertion(const TextEdit& edit) {
    return edit.deleted_len == 0 && !edit.inserted_text.empty();
}

bool IsDeletion(const TextEdit& edit) {
    return edit.deleted_len != 0 && edit.inserted_text.empty();
}

bool ContainsLineBreak(const TextEdit& edit) {
    return edit.inserted_text.find_first_of(L"\r\n") != std::wstring::npos;
}

constexpr uint64_t kMergeWindowMs = 1000;
constexpr size_t kMaxHistoryEntries = 4096;

struct TextState {
    core_hash::Sha256Digest fingerprint{};
    size_t length = 0;
};

void AppendCanonicalUtf16(core_hash::Sha256* hasher,
                          size_t* length,
                          std::wstring_view text) {
    if (!hasher || !length) return;
    for (wchar_t codeUnit : text) {
        const std::uint16_t value = static_cast<std::uint16_t>(codeUnit);
        const std::uint8_t bytes[] = {
            static_cast<std::uint8_t>(value & 0xffu),
            static_cast<std::uint8_t>((value >> 8) & 0xffu),
        };
        hasher->Update(bytes, sizeof(bytes));
        ++*length;
    }
}

TextState DescribeText(std::wstring_view text) {
    core_hash::Sha256 hasher;
    TextState state;
    AppendCanonicalUtf16(&hasher, &state.length, text);
    state.fingerprint = hasher.Finalize();
    return state;
}

TextState DescribeAppliedEdit(std::wstring_view before, const TextEdit& edit) {
    core_hash::Sha256 hasher;
    TextState state;
    AppendCanonicalUtf16(&hasher, &state.length, before.substr(0, edit.start.value));
    AppendCanonicalUtf16(&hasher, &state.length, edit.inserted_text);
    AppendCanonicalUtf16(&hasher, &state.length,
                         before.substr(edit.start.value + edit.deleted_len));
    state.fingerprint = hasher.Finalize();
    return state;
}

} // namespace

bool NoteHistoryReplayMatchesCurrentText(const NoteHistoryReplay& replay,
                                         std::wstring_view currentText) {
    if (replay.expected_snapshot.valid()) return false;
    const TextState state = DescribeText(currentText);
    return state.length == replay.expected_content_length &&
           state.fingerprint == replay.expected_content_fingerprint;
}

void NoteHistory::Clear() {
    undo_.clear();
    redo_.clear();
}

bool NoteHistory::Record(std::wstring_view textBefore,
                         const TextEdit& forward,
                         NoteTextSelection selectionBefore,
                         NoteTextSelection selectionAfter,
                         NoteHistoryOperationKind kind,
                         uint64_t tick) {
    if (forward.start.value > textBefore.size() ||
        forward.deleted_len > textBefore.size() - forward.start.value ||
        (forward.deleted_len == 0 && forward.inserted_text.empty())) {
        return false;
    }

    Entry next;
    next.forward = forward;
    next.inverse.start = forward.start;
    next.inverse.deleted_len = forward.inserted_text.size();
    next.inverse.inserted_text.assign(textBefore.substr(forward.start.value, forward.deleted_len));
    next.selectionBefore = selectionBefore;
    next.selectionAfter = selectionAfter;
    next.kind = kind;
    next.tick = tick;
    const TextState beforeState = DescribeText(textBefore);
    const TextState afterState = DescribeAppliedEdit(textBefore, forward);
    next.before_content_fingerprint = beforeState.fingerprint;
    next.before_content_length = beforeState.length;
    next.after_content_fingerprint = afterState.fingerprint;
    next.after_content_length = afterState.length;
    const std::wstring_view affected = forward.inserted_text.empty()
        ? textBefore.substr(forward.start.value, forward.deleted_len)
        : std::wstring_view(forward.inserted_text);
    const std::vector<TextUnit> units = BuildTextUnits(affected);
    if (!units.empty() && units.front().klass != TextUnitClass::LineBreak &&
        std::all_of(units.begin(), units.end(), [&](const TextUnit& unit) {
            return unit.klass == units.front().klass;
        })) {
        next.unit_class = units.front().klass;
    }

    return Store(std::move(next));
}

bool NoteHistory::Record(const NoteTextPieceSequence::Snapshot& before,
                         const NoteTextPieceSequence::Snapshot& after,
                         std::wstring removedText,
                         const TextEdit& forward,
                         NoteTextSelection selectionBefore,
                         NoteTextSelection selectionAfter,
                         NoteHistoryOperationKind kind,
                         uint64_t tick) {
    if (!before.valid() || !after.valid() ||
        forward.start.value > before.text_length() ||
        forward.deleted_len > before.text_length() - forward.start.value ||
        removedText.size() != forward.deleted_len ||
        (forward.deleted_len == 0 && forward.inserted_text.empty()) ||
        forward.inserted_text.size() > std::numeric_limits<size_t>::max() -
                                           (before.text_length() - forward.deleted_len) ||
        after.text_length() != before.text_length() - forward.deleted_len +
                                   forward.inserted_text.size()) {
        return false;
    }

    Entry next;
    next.forward = forward;
    next.inverse.start = forward.start;
    next.inverse.deleted_len = forward.inserted_text.size();
    next.inverse.inserted_text = std::move(removedText);
    next.selectionBefore = selectionBefore;
    next.selectionAfter = selectionAfter;
    next.kind = kind;
    next.tick = tick;
    next.before_snapshot = before;
    next.after_snapshot = after;
    const std::wstring_view affected = forward.inserted_text.empty()
        ? std::wstring_view(next.inverse.inserted_text)
        : std::wstring_view(forward.inserted_text);
    const std::vector<TextUnit> units = BuildTextUnits(affected);
    if (!units.empty() && units.front().klass != TextUnitClass::LineBreak &&
        std::all_of(units.begin(), units.end(), [&](const TextUnit& unit) {
            return unit.klass == units.front().klass;
        })) {
        next.unit_class = units.front().klass;
    }

    return Store(std::move(next));
}

std::optional<NoteHistoryReplay> NoteHistory::PeekUndo() const {
    if (undo_.empty()) return std::nullopt;
    const Entry& entry = undo_.back();
    return NoteHistoryReplay{entry.inverse, entry.forward.inserted_text,
                             entry.after_content_fingerprint,
                             entry.after_content_length,
                             entry.after_snapshot};
}

std::optional<NoteHistoryReplay> NoteHistory::PeekRedo() const {
    if (redo_.empty()) return std::nullopt;
    const Entry& entry = redo_.back();
    return NoteHistoryReplay{entry.forward, entry.inverse.inserted_text,
                             entry.before_content_fingerprint,
                             entry.before_content_length,
                             entry.before_snapshot};
}

bool NoteHistory::PrepareUndo() {
    if (undo_.empty()) return false;
    try {
        if (redo_.size() == redo_.capacity()) redo_.reserve(redo_.size() + 1);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteHistory::PrepareRedo() {
    if (redo_.empty()) return false;
    try {
        if (undo_.size() == undo_.capacity()) undo_.reserve(undo_.size() + 1);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteHistory::CommitUndo(const NoteTextPieceSequence::Snapshot& currentSnapshot) {
    if (undo_.empty() || !currentSnapshot.valid() || redo_.size() == redo_.capacity()) {
        return false;
    }
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    // Applying an inverse constructs a new persistent root even when it
    // reproduces the old text.  The next redo must therefore expect this
    // actual published root, rather than the historical pre-edit root.
    redo_.back().before_snapshot = currentSnapshot;
    // The state reached by this undo is also the post-state of the preceding
    // undo entry.  Keep the adjacent stack boundary in the same published
    // root so a second undo does not reject text that is semantically equal
    // but structurally reconstructed.
    if (!undo_.empty()) undo_.back().after_snapshot = currentSnapshot;
    return true;
}

bool NoteHistory::CommitRedo(const NoteTextPieceSequence::Snapshot& currentSnapshot) {
    if (redo_.empty() || !currentSnapshot.valid() || undo_.size() == undo_.capacity()) {
        return false;
    }
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    // Symmetrically, publish the root produced by replaying forward as the
    // expected state for a following undo.
    undo_.back().after_snapshot = currentSnapshot;
    // The state reached by this redo is the pre-state of the next redo entry.
    // Refresh that adjacent boundary for the same reason as CommitUndo.
    if (!redo_.empty()) redo_.back().before_snapshot = currentSnapshot;
    return true;
}

bool NoteHistory::Store(Entry next) {
    try {
        if (!undo_.empty() && CanMerge(undo_.back(), next)) {
            // Merge a temporary entry first.  A failed allocation then leaves
            // both the prior undo chain and the redo chain intact.
            Entry merged = undo_.back();
            Merge(&merged, std::move(next));
            undo_.back() = std::move(merged);
        } else {
            // A new entry only invalidates redo after it has been stored.
            undo_.push_back(std::move(next));
            if (undo_.size() > kMaxHistoryEntries) {
                undo_.erase(undo_.begin());
            }
        }
    } catch (...) {
        return false;
    }
    redo_.clear();
    return true;
}

bool NoteHistory::CanMerge(const Entry& previous, const Entry& next) {
    if (previous.kind != next.kind || next.tick < previous.tick ||
        next.tick - previous.tick > kMergeWindowMs ||
        ContainsLineBreak(previous.forward) || ContainsLineBreak(previous.inverse) ||
        ContainsLineBreak(next.forward) || ContainsLineBreak(next.inverse) ||
        !previous.unit_class.has_value() || previous.unit_class != next.unit_class ||
        !IsCollapsed(previous.selectionAfter) || !IsCollapsed(next.selectionBefore) ||
        previous.selectionAfter.caret != next.selectionBefore.caret) {
        return false;
    }
    switch (next.kind) {
    case NoteHistoryOperationKind::Typing:
        return IsInsertion(previous.forward) && IsInsertion(next.forward) &&
               previous.forward.start.value + previous.forward.inserted_text.size() == next.forward.start.value;
    case NoteHistoryOperationKind::DeleteBackward:
        return IsDeletion(previous.forward) && IsDeletion(next.forward) &&
               next.forward.start.value + next.forward.deleted_len == previous.forward.start.value;
    case NoteHistoryOperationKind::DeleteForward:
        return IsDeletion(previous.forward) && IsDeletion(next.forward) &&
               previous.forward.start == next.forward.start;
    default:
        return false;
    }
}

void NoteHistory::Merge(Entry* previous, Entry next) {
    if (!previous) return;
    switch (previous->kind) {
    case NoteHistoryOperationKind::Typing:
        previous->forward.inserted_text += next.forward.inserted_text;
        previous->inverse.deleted_len += next.inverse.deleted_len;
        break;
    case NoteHistoryOperationKind::DeleteBackward:
        previous->forward.start = next.forward.start;
        previous->forward.deleted_len += next.forward.deleted_len;
        previous->inverse.start = next.inverse.start;
        previous->inverse.inserted_text = next.inverse.inserted_text + previous->inverse.inserted_text;
        break;
    case NoteHistoryOperationKind::DeleteForward:
        previous->forward.deleted_len += next.forward.deleted_len;
        previous->inverse.inserted_text += next.inverse.inserted_text;
        break;
    default:
        return;
    }
    previous->selectionAfter = next.selectionAfter;
    previous->tick = next.tick;
    previous->after_content_fingerprint = next.after_content_fingerprint;
    previous->after_content_length = next.after_content_length;
    previous->after_snapshot = std::move(next.after_snapshot);
}

} // namespace note
