#include "note/note_source_coordinate_transform.h"

#include <limits>

namespace note {

NoteSourceEditCoordinateMapBuildResult NoteSourceEditCoordinateMap::Build(
    size_t oldSourceLength,
    const TextEdit& edit,
    NoteSourceEditCoordinateMap* out) noexcept {
    if (!out) return NoteSourceEditCoordinateMapBuildResult::InvalidOutput;
    if (edit.start.value > oldSourceLength ||
        edit.deleted_len > oldSourceLength - edit.start.value) {
        return NoteSourceEditCoordinateMapBuildResult::InvalidEditRange;
    }
    const size_t retainedLength = oldSourceLength - edit.deleted_len;
    if (edit.inserted_text.size() > std::numeric_limits<size_t>::max() - retainedLength) {
        return NoteSourceEditCoordinateMapBuildResult::LengthOverflow;
    }

    NoteSourceEditCoordinateMap candidate;
    candidate.old_source_length_ = oldSourceLength;
    candidate.new_source_length_ = retainedLength + edit.inserted_text.size();
    candidate.old_replacement_span_ = {
        edit.start, {edit.start.value + edit.deleted_len}};
    candidate.new_replacement_span_ = {
        edit.start, {edit.start.value + edit.inserted_text.size()}};
    candidate.valid_ = true;
    *out = candidate;
    return NoteSourceEditCoordinateMapBuildResult::Built;
}

bool NoteSourceEditCoordinateMap::MapUnchangedBoundary(
    Utf16CodeUnitOffset oldOffset,
    NoteSourceEditBoundarySide side,
    Utf16CodeUnitOffset* out) const noexcept {
    if (!out || !valid_ || oldOffset.value > old_source_length_) return false;
    const size_t oldStart = old_replacement_span_.start.value;
    const size_t oldEnd = old_replacement_span_.end.value;
    const size_t newStart = new_replacement_span_.start.value;
    const size_t newEnd = new_replacement_span_.end.value;
    if (oldOffset.value < oldStart) {
        *out = oldOffset;
        return true;
    }
    if (oldOffset.value > oldEnd) {
        const size_t suffixOffset = oldOffset.value - oldEnd;
        if (suffixOffset > new_source_length_ - newEnd) return false;
        *out = {newEnd + suffixOffset};
        return true;
    }
    if (oldOffset.value == oldStart || oldOffset.value == oldEnd) {
        *out = {side == NoteSourceEditBoundarySide::BeforeReplacement
                    ? newStart
                    : newEnd};
        return true;
    }
    return false;
}

bool NoteSourceEditCoordinateMap::MapUnchangedNewBoundary(
    Utf16CodeUnitOffset newOffset,
    NoteSourceEditBoundarySide side,
    Utf16CodeUnitOffset* out) const noexcept {
    if (!out || !valid_ || newOffset.value > new_source_length_) return false;
    const size_t oldStart = old_replacement_span_.start.value;
    const size_t oldEnd = old_replacement_span_.end.value;
    const size_t newStart = new_replacement_span_.start.value;
    const size_t newEnd = new_replacement_span_.end.value;
    if (newOffset.value < newStart) {
        *out = newOffset;
        return true;
    }
    if (newOffset.value > newEnd) {
        const size_t suffixOffset = newOffset.value - newEnd;
        if (suffixOffset > old_source_length_ - oldEnd) return false;
        *out = {oldEnd + suffixOffset};
        return true;
    }
    if (newOffset.value == newStart || newOffset.value == newEnd) {
        *out = {side == NoteSourceEditBoundarySide::BeforeReplacement
                    ? oldStart
                    : oldEnd};
        return true;
    }
    return false;
}

bool NoteSourceEditCoordinateMap::MapUnchangedSpan(Span oldSpan, Span* out) const noexcept {
    if (!out || !valid_ || oldSpan.end < oldSpan.start ||
        oldSpan.end.value > old_source_length_ || oldSpan.end == oldSpan.start) {
        return false;
    }
    const size_t oldStart = old_replacement_span_.start.value;
    const size_t oldEnd = old_replacement_span_.end.value;
    if (oldSpan.end.value <= oldStart) {
        *out = oldSpan;
        return true;
    }
    if (oldSpan.start.value >= oldEnd) {
        Utf16CodeUnitOffset mappedStart;
        Utf16CodeUnitOffset mappedEnd;
        if (!MapUnchangedBoundary(
                oldSpan.start, NoteSourceEditBoundarySide::AfterReplacement, &mappedStart) ||
            !MapUnchangedBoundary(
                oldSpan.end, NoteSourceEditBoundarySide::AfterReplacement, &mappedEnd)) {
            return false;
        }
        *out = {mappedStart, mappedEnd};
        return true;
    }
    return false;
}

bool NoteSourceEditCoordinateMap::MapUnchangedNewSpan(Span newSpan, Span* out) const noexcept {
    if (!out || !valid_ || newSpan.end < newSpan.start ||
        newSpan.end.value > new_source_length_ || newSpan.end == newSpan.start) {
        return false;
    }
    const size_t newStart = new_replacement_span_.start.value;
    const size_t newEnd = new_replacement_span_.end.value;
    if (newSpan.end.value <= newStart) {
        *out = newSpan;
        return true;
    }
    if (newSpan.start.value >= newEnd) {
        Utf16CodeUnitOffset mappedStart;
        Utf16CodeUnitOffset mappedEnd;
        if (!MapUnchangedNewBoundary(
                newSpan.start, NoteSourceEditBoundarySide::AfterReplacement, &mappedStart) ||
            !MapUnchangedNewBoundary(
                newSpan.end, NoteSourceEditBoundarySide::AfterReplacement, &mappedEnd)) {
            return false;
        }
        *out = {mappedStart, mappedEnd};
        return true;
    }
    return false;
}

} // namespace note
