#pragma once

#include "note/note_model.h"

#include <cstddef>

namespace note {

// A source boundary at an insertion/replacement has two valid meanings:
// immediately before the inserted replacement and immediately after it.
// Callers must choose explicitly instead of silently shifting one half of a
// suffix by the wrong delta.
enum class NoteSourceEditBoundarySide {
    BeforeReplacement,
    AfterReplacement,
};

enum class NoteSourceEditCoordinateMapBuildResult {
    Built,
    InvalidOutput,
    InvalidEditRange,
    LengthOverflow,
};

// Exact coordinate relation for one canonical TextEdit.  It maps only ranges
// that prove they are entirely unchanged: prefix spans remain fixed and
// suffix spans receive one lazy delta.  A range that touches the replaced
// interval is deliberately rejected; it must be rebuilt from the new source.
class NoteSourceEditCoordinateMap final {
public:
    [[nodiscard]] static NoteSourceEditCoordinateMapBuildResult Build(
        size_t oldSourceLength,
        const TextEdit& edit,
        NoteSourceEditCoordinateMap* out) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] size_t old_source_length() const noexcept { return old_source_length_; }
    [[nodiscard]] size_t new_source_length() const noexcept { return new_source_length_; }
    [[nodiscard]] Span old_replacement_span() const noexcept { return old_replacement_span_; }
    [[nodiscard]] Span new_replacement_span() const noexcept { return new_replacement_span_; }

    [[nodiscard]] bool MapUnchangedBoundary(
        Utf16CodeUnitOffset oldOffset,
        NoteSourceEditBoundarySide side,
        Utf16CodeUnitOffset* out) const noexcept;
    // The inverse mapping has the same deliberately narrow contract: a
    // boundary inside the new replacement has no old canonical source
    // position. Final IME presentation uses this to distinguish transient
    // composition text from an unchanged committed prefix or suffix.
    [[nodiscard]] bool MapUnchangedNewBoundary(
        Utf16CodeUnitOffset newOffset,
        NoteSourceEditBoundarySide side,
        Utf16CodeUnitOffset* out) const noexcept;
    [[nodiscard]] bool MapUnchangedSpan(Span oldSpan, Span* out) const noexcept;
    [[nodiscard]] bool MapUnchangedNewSpan(Span newSpan, Span* out) const noexcept;

private:
    size_t old_source_length_ = 0;
    size_t new_source_length_ = 0;
    Span old_replacement_span_{};
    Span new_replacement_span_{};
    bool valid_ = false;
};

} // namespace note
