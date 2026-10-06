#pragma once

#include "note/note_source_coordinate_transform.h"
#include "note/note_text_core.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace note {

// Platform-neutral values matching the six IMM composition statuses. These
// describe provisional input, never Markdown/TeX or committed source styles.
enum class NoteImeCharacterAttribute : uint8_t {
    Input,
    TargetConverted,
    Converted,
    TargetNotConverted,
    InputError,
    FixedConverted,
};

struct NoteImePreeditPayload {
    std::wstring text;
    Utf16CodeUnitOffset caret{};
    // Empty metadata means unavailable; use one Input segment in that case.
    // Otherwise one attribute per UTF-16 code unit, and clause boundaries
    // [0, ..., text.size()] in UTF-16 units, not byte offsets.
    std::vector<NoteImeCharacterAttribute> attributes;
    std::vector<Utf16CodeUnitOffset> clauses;
};

struct NoteImePreeditSegment {
    Span editor_span{};
    NoteImeCharacterAttribute attribute = NoteImeCharacterAttribute::Input;
};

enum class NoteRenderFinalImePreeditUpdateResult {
    Updated,
    InactiveSession,
    UpdatedEmptyComposition,
    InvalidComposition,
    AllocationFailure,
};

// UI-thread session anchor, separate from the immutable presentation below.
// A failed IMM read suspends only its temporary payload. A known empty text
// snapshot is still a valid preedit and is not confused with missing data.
// Its canonical replacement and revision survive until result/end/focus loss
// or note switch.
class NoteRenderFinalImePreeditSession final {
public:
    [[nodiscard]] bool Begin(NoteDerivedSnapshotIdentity source, Span replacement) noexcept;
    [[nodiscard]] NoteRenderFinalImePreeditUpdateResult Update(
        std::wstring_view composition, Utf16CodeUnitOffset caret,
        const std::vector<NoteImeCharacterAttribute>& attributes = {},
        const std::vector<Utf16CodeUnitOffset>& clauses = {}) noexcept;
    void Suspend() noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool has_payload() const noexcept { return has_payload_; }
    // START before the first payload is not a failed/suspended IMM read.
    [[nodiscard]] bool awaiting_first_payload() const noexcept { return awaiting_first_payload_; }
    [[nodiscard]] const NoteDerivedSnapshotIdentity& source_identity() const noexcept { return source_identity_; }
    [[nodiscard]] Span canonical_replacement() const noexcept { return canonical_replacement_; }
    [[nodiscard]] const std::wstring& composition_text() const noexcept { return composition_text_; }
    [[nodiscard]] Utf16CodeUnitOffset composition_caret() const noexcept { return composition_caret_; }
    [[nodiscard]] const std::vector<NoteImeCharacterAttribute>& composition_attributes() const noexcept { return composition_attributes_; }
    [[nodiscard]] const std::vector<Utf16CodeUnitOffset>& composition_clauses() const noexcept { return composition_clauses_; }

private:
    NoteDerivedSnapshotIdentity source_identity_{};
    Span canonical_replacement_{};
    std::wstring composition_text_;
    Utf16CodeUnitOffset composition_caret_{};
    std::vector<NoteImeCharacterAttribute> composition_attributes_;
    std::vector<Utf16CodeUnitOffset> composition_clauses_;
    bool active_ = false;
    bool has_payload_ = false;
    bool awaiting_first_payload_ = false;
};

struct NoteImeCommittedContinuation {
    NoteDerivedSnapshotIdentity source_identity{};
    TextEdit committed_edit;
    Span selection_before{};
    Utf16CodeUnitOffset committed_caret{};
};

enum class NoteImeCommittedContinuationResult {
    Prepared,
    InvalidOutput,
    InvalidSession,
    StaleSource,
    InvalidRange,
    ObservedTextMismatch,
    AllocationFailure,
};

// One result+preedit event is observed after native dispatch. Prove that its
// native text is exactly old prefix + result + preedit + old suffix, then
// propose only the result as a canonical edit. No mutation or whole-note
// mirror; failed proof leaves the prior result object and canonical root intact.
[[nodiscard]] NoteImeCommittedContinuationResult PrepareNoteImeCommittedContinuation(
    const NoteTextCore& text_core,
    const NoteRenderFinalImePreeditSession& session,
    std::wstring_view canonical_result,
    std::wstring_view canonical_preedit,
    std::wstring_view observed_editor,
    NoteImeCommittedContinuation* out) noexcept;

// One live IMM composition is not canonical document text. The Win32 adapter
// captures the replacement range from the exact committed TextCore before
// dispatching the first preedit update, then supplies the temporary editor
// selection reported by IMM. Composition containing a line break is rejected
// because it would invalidate the committed source-row partition.
struct NoteRenderFinalImePreeditInput {
    Span canonical_replacement_span{};
    std::wstring composition_text;
    Span editor_selection{};
    std::vector<NoteImeCharacterAttribute> composition_attributes;
    std::vector<Utf16CodeUnitOffset> composition_clauses;
};

enum class NoteRenderFinalImePreeditPresentationBuildResult {
    Built,
    InvalidOutput,
    InvalidTextCore,
    InvalidReplacementRange,
    MultipleLogicalLines,
    ContainsLineBreak,
    InvalidEditorSelection,
    InvalidCompositionMetadata,
    CoordinateMapBuildFailed,
    SourceReadFailed,
    InconsistentTemporaryLine,
    AllocationFailure,
};

// Immutable preedit geometry input for the completed presentation builder.
// It owns only the one temporary raw source row and the exact old/new offset
// relation. It never publishes the composition into NoteTextCore and never
// materializes the complete editor text.
class NoteRenderFinalImePreeditPresentation final {
public:
    [[nodiscard]] static NoteRenderFinalImePreeditPresentationBuildResult Build(
        const NoteTextCore& text_core,
        const NoteRenderFinalImePreeditInput& input,
        NoteRenderFinalImePreeditPresentation* out) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool Matches(const NoteTextCore& text_core) const noexcept;
    [[nodiscard]] const NoteDerivedSnapshotIdentity& source_identity() const noexcept {
        return source_identity_;
    }
    [[nodiscard]] LineIndex line_index() const noexcept { return line_index_; }
    [[nodiscard]] Span canonical_replacement_span() const noexcept {
        return coordinate_map_.old_replacement_span();
    }
    [[nodiscard]] Span editor_replacement_span() const noexcept {
        return coordinate_map_.new_replacement_span();
    }
    [[nodiscard]] Span editor_line_span() const noexcept { return editor_line_span_; }
    [[nodiscard]] Span editor_selection() const noexcept { return editor_selection_; }
    [[nodiscard]] const std::wstring& temporary_raw_line() const noexcept {
        return temporary_raw_line_;
    }
    [[nodiscard]] const NoteSourceEditCoordinateMap& coordinate_map() const noexcept {
        return coordinate_map_;
    }
    [[nodiscard]] const std::vector<NoteImePreeditSegment>& segments() const noexcept {
        return segments_;
    }

    // `false` for a composition-internal offset is intentional: it has no
    // canonical source coordinate. Callers must resolve that offset through
    // the temporary raw surface instead of guessing a committed boundary.
    [[nodiscard]] bool MapEditorUnchangedBoundary(
        Utf16CodeUnitOffset editor_offset,
        NoteSourceEditBoundarySide side,
        Utf16CodeUnitOffset* out_canonical) const noexcept;
    [[nodiscard]] bool MapCanonicalUnchangedBoundary(
        Utf16CodeUnitOffset canonical_offset,
        NoteSourceEditBoundarySide side,
        Utf16CodeUnitOffset* out_editor) const noexcept;

private:
    NoteDerivedSnapshotIdentity source_identity_{};
    NoteSourceEditCoordinateMap coordinate_map_{};
    LineIndex line_index_{};
    Span editor_line_span_{};
    Span editor_selection_{};
    std::wstring temporary_raw_line_;
    std::vector<NoteImePreeditSegment> segments_;
    bool valid_ = false;
};

} // namespace note
