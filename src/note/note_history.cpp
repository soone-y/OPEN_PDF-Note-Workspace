#include "note/note_history.h"

#include <algorithm>

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

} // namespace

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

    redo_.clear();
    if (!undo_.empty() && CanMerge(undo_.back(), next)) {
        Merge(&undo_.back(), std::move(next));
    } else {
        undo_.push_back(std::move(next));
        if (undo_.size() > kMaxHistoryEntries) {
            undo_.erase(undo_.begin());
        }
    }
    return true;
}

std::optional<NoteHistoryReplay> NoteHistory::PeekUndo() const {
    if (undo_.empty()) return std::nullopt;
    const Entry& entry = undo_.back();
    return NoteHistoryReplay{entry.inverse, entry.selectionBefore};
}

std::optional<NoteHistoryReplay> NoteHistory::PeekRedo() const {
    if (redo_.empty()) return std::nullopt;
    const Entry& entry = redo_.back();
    return NoteHistoryReplay{entry.forward, entry.selectionAfter};
}

bool NoteHistory::CommitUndo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    return true;
}

bool NoteHistory::CommitRedo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
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
}

} // namespace note
