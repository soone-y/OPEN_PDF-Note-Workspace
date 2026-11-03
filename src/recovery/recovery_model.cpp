#include "recovery/recovery_model.h"
#include "core/text_encoding.h"

#include <algorithm>
#include <utility>

namespace recovery {

AnnotationPlan::AnnotationPlan(std::vector<Annotation> initial,
                               AnnotNumericPrecision precision,
                               std::vector<AnnotCommand> remaining,
                               std::optional<annot_stage_replay::State> replayState)
    : candidate_(std::move(initial)), replay_(replayState ? std::move(*replayState)
                                                        : annot_stage_replay::State(candidate_.size(), precision)),
      commands_(std::move(remaining)) {}

void AnnotationPlan::Remember() {
    undo_.push_back({candidate_, replay_, cursor_});
    redo_.clear();
}

const AnnotCommand* AnnotationPlan::pending() const {
    return complete() ? nullptr : &commands_[cursor_];
}

bool AnnotationPlan::Advance() {
    while (!complete()) {
        auto next = candidate_;
        auto replay = replay_;
        if (!replay.Apply(&next, commands_[cursor_])) return false;
        Remember();
        candidate_.swap(next);
        replay_ = std::move(replay);
        ++cursor_;
    }
    return true;
}

bool AnnotationPlan::Resolve(Choice choice) {
    if (complete()) return false;
    auto next = candidate_;
    const auto& cmd = commands_[cursor_];
    if (choice == Choice::Apply) {
        switch (cmd.kind) {
        case AnnotCommandKind::Add:
            if (cmd.after.id.empty() || !annot_stage_replay::IsFinite(cmd.after) ||
                FindAnnotIndexByIdInList(next, cmd.after.id) >= 0) return false;
            next.push_back(cmd.after);
            break;
        case AnnotCommandKind::Remove:
        case AnnotCommandKind::Update: {
            const int index = FindAnnotIndexByIdInList(next, cmd.before.id);
            if (index < 0) return false; // Missing target requires manual editing.
            if (cmd.kind == AnnotCommandKind::Remove) next.erase(next.begin() + index);
            else {
                if (cmd.after.id != cmd.before.id || !annot_stage_replay::IsFinite(cmd.after)) return false;
                next[static_cast<size_t>(index)] = cmd.after;
            }
            break;
        }
        case AnnotCommandKind::ClearAll:
            next.clear();
            break;
        case AnnotCommandKind::Reorder: {
            if (cmd.afterSnapshot.size() != next.size()) return false;
            std::vector<Annotation> ordered;
            for (const auto& requested : cmd.afterSnapshot) {
                const int index = FindAnnotIndexByIdInList(next, requested.id);
                if (index < 0) return false;
                ordered.push_back(next[static_cast<size_t>(index)]);
            }
            if (!annot_stage_replay::ValidList(ordered)) return false;
            next.swap(ordered);
            break;
        }
        default: return false;
        }
    }
    if (!annot_stage_replay::ValidList(next)) return false;
    Remember();
    candidate_.swap(next);
    replay_ = annot_stage_replay::State(candidate_.size(), AnnotNumericPrecision::RoundTrip17);
    ++cursor_;
    return true;
}

bool AnnotationPlan::Replace(const std::vector<Annotation>& annotations) {
    if (!annot_stage_replay::ValidList(annotations)) return false;
    auto next = annotations;
    Remember();
    candidate_.swap(next);
    replay_ = annot_stage_replay::State(candidate_.size(), AnnotNumericPrecision::RoundTrip17);
    return true;
}

bool AnnotationPlan::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back({candidate_, replay_, cursor_});
    auto version = std::move(undo_.back());
    undo_.pop_back();
    candidate_.swap(version.annotations);
    replay_ = std::move(version.replay);
    cursor_ = version.cursor;
    return true;
}

bool AnnotationPlan::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back({candidate_, replay_, cursor_});
    auto version = std::move(redo_.back());
    redo_.pop_back();
    candidate_.swap(version.annotations);
    replay_ = std::move(version.replay);
    cursor_ = version.cursor;
    return true;
}

bool ApplyNoteEdit(std::string* bytes, const NoteEdit& edit) {
    const size_t offset = edit.byteOffset.value;
    if (!bytes || offset > bytes->size() ||
        edit.before.size() > bytes->size() - offset ||
        bytes->compare(offset, edit.before.size(), edit.before) != 0) return false;
    auto next = *bytes;
    next.replace(offset, edit.before.size(), edit.after);
    if (!text_encoding::DecodeUtf8Strict(next)) return false;
    bytes->swap(next);
    return true;
}

bool ResolveNoteEdit(std::string* bytes, const NoteEdit& edit,
                     Utf8ByteOffset chosenByteOffset, Choice choice) {
    if (!bytes) return false;
    if (choice == Choice::Keep) return true;
    NoteEdit positioned = edit;
    positioned.byteOffset = chosenByteOffset;
    // Even explicit relocation never deletes a different piece of text.
    return ApplyNoteEdit(bytes, positioned);
}

} // namespace recovery
