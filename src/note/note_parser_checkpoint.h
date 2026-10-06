#pragma once

#include "note/note_model.h"
#include "note/note_source_line_map.h"
#include "note/note_text_piece_sequence.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace note {

class NoteParserCheckpointStorage;

// Optional process-local work accounting, never persisted. Regression tests
// bound interval-query work instead of depending on machine-specific timing.
struct NoteParserCheckpointBuildWork {
    size_t indexed_intervals = 0;
    size_t boundary_queries = 0;
    size_t visited_index_nodes = 0;
    size_t matched_intervals = 0;
};

// A presentation configuration is part of a parser/render fixed point.  The
// caller owns its construction from font/DPI/render-mode inputs; zero means
// that no complete layout identity was supplied, and therefore permits no
// suffix reuse.
struct NoteParserLayoutKey {
    uint64_t value = 0;

    [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

[[nodiscard]] bool operator==(NoteParserLayoutKey lhs,
                              NoteParserLayoutKey rhs) noexcept;
[[nodiscard]] bool operator!=(NoteParserLayoutKey lhs,
                              NoteParserLayoutKey rhs) noexcept;

enum class NoteParserTableMode {
    None,
    Header,
    Body,
};

enum class NoteParserContainerKind {
    Quote,
    List,
    ListItem,
    FencedContainer,
};

// This is deliberately a parser-facing description rather than a view cache
// key.  It retains the nesting information which a future local parser needs
// to resume a list/quote row without inferring state from rendered glyphs.
struct NoteParserContainerFrame {
    NoteParserContainerKind kind = NoteParserContainerKind::Quote;
    size_t parent_depth = 0;
    bool ordered = false;
    int start_number = 1;
    bool task_item = false;
    bool task_checked = false;
    size_t fence_marker_count = 0;
    std::wstring info_string;
};

[[nodiscard]] bool operator==(const NoteParserContainerFrame& lhs,
                              const NoteParserContainerFrame& rhs) noexcept;
[[nodiscard]] bool operator!=(const NoteParserContainerFrame& lhs,
                              const NoteParserContainerFrame& rhs) noexcept;

// Complete inherited syntax state at one logical-line boundary.  The
// look-behind flag is included because Setext/table delimiter lines read the
// preceding source row. Its exact canonical identity is checked separately
// by the suffix predicate. States can faithfully describe constructs that
// the local parser has not implemented yet; has_unmodeled_context then keeps
// those snapshots out of the suffix-reuse path instead of guessing.
struct NoteParserCheckpointState {
    bool has_bounded_lookbehind_line = false;

    bool code_fence_open = false;
    wchar_t code_fence_marker = 0;
    size_t code_fence_marker_count = 0;

    bool block_math_open = false;
    MathDelimiter block_math_delimiter = MathDelimiter::Dollar;

    NoteParserTableMode table_mode = NoteParserTableMode::None;
    size_t table_column_count = 0;
    std::vector<NoteParserContainerFrame> container_stack;

    // Heading-derived indentation may span an entire section. It is a
    // modeled inherited style, not an opaque inline delimiter. Keep the
    // ordered values exact so a changed heading/indent cannot falsely
    // establish a suffix fixed point. Explicit tags remain conservative
    // through opaque_legacy_markup.
    std::vector<std::wstring> inherited_indent_values;

    // These flags are deliberately sticky only for the construct which is
    // not yet represented as a resumable parser state.  They make the proof
    // conservative: a false positive costs reuse, while a false negative
    // could reuse stale structured output.
    bool opaque_inline_continuation = false;
    bool opaque_legacy_markup = false;
    bool opaque_block_math_candidate = false;
    bool has_unmodeled_context = false;

    [[nodiscard]] bool supports_suffix_reuse() const noexcept;
};

[[nodiscard]] bool operator==(const NoteParserCheckpointState& lhs,
                              const NoteParserCheckpointState& rhs) noexcept;
[[nodiscard]] bool operator!=(const NoteParserCheckpointState& lhs,
                              const NoteParserCheckpointState& rhs) noexcept;

// `source_line` includes its line-break transport. It gives the fixed-point
// predicate exact old/new ranges to verify with the canonical piece roots;
// shifted absolute offsets therefore never stand in for source identity.
struct NoteParserLineCheckpoint {
    Span source_line{};
    NoteParserCheckpointState entry_state;
    NoteParserCheckpointState exit_state;
};

enum class NoteParserCheckpointLocalPatchResult {
    Built,
    InvalidOutput,
    InvalidPreviousIndex,
    InvalidCurrentSource,
    InvalidChangedLine,
    AllocationFailure,
};

// Test-only accounting for the persistent checkpoint state. Source rows are
// resolved from NoteSourceLineMap, so a marker-free same-row edit changes no
// checkpoint-state payload and never rewrites the unchanged tail.
struct NoteParserCheckpointLocalPatchWork {
    size_t replacement_line_payloads = 0;
    size_t tail_line_payload_rewrites = 0;
};

struct NoteParserCheckpointIndex {
    NoteDerivedSnapshotIdentity source_identity{};
    NoteTextPieceSequence::Snapshot canonical_source_root;
    NoteSourceLineMap::Snapshot source_line_map;
    bool valid = false;

    [[nodiscard]] size_t line_count() const noexcept;
    // Resolves source_line against this revision's persistent source-line map.
    // Paint/input code must not materialize all checkpoint rows.
    [[nodiscard]] bool ResolveLine(
        LineIndex index,
        NoteParserLineCheckpoint* out) const noexcept;
    // Differential-test oracle only; not a final runtime traversal API.
    [[nodiscard]] bool CopyLinesForDifferentialTest(
        std::vector<NoteParserLineCheckpoint>* out) const noexcept;
    [[nodiscard]] bool SharesLinePayloadForDifferentialTest(
        const NoteParserCheckpointIndex& other,
        LineIndex index) const noexcept;
    [[nodiscard]] bool ReplaceLineForDifferentialTest(
        LineIndex index,
        const NoteParserLineCheckpoint& replacement,
        NoteParserCheckpointIndex* out) const noexcept;

private:
    // Opaque immutable state storage. Clients resolve individual rows through
    // the methods above; only construction and test-only replacement may
    // create a new root.
    std::shared_ptr<const NoteParserCheckpointStorage> line_storage_;

    friend NoteParserCheckpointIndex BuildNoteParserCheckpointIndex(
        const NoteTextModel& source,
        const NoteDocument& document,
        NoteDerivedSnapshotIdentity source_identity,
        NoteTextPieceSequence::Snapshot canonical_source_root);
    friend NoteParserCheckpointIndex BuildNoteParserCheckpointIndex(
        const NoteTextModel& source,
        const NoteDocument& document,
        NoteDerivedSnapshotIdentity source_identity,
        NoteTextPieceSequence::Snapshot canonical_source_root,
        const NoteSourceLineMap::Snapshot& source_line_map,
        NoteParserCheckpointBuildWork* out_work);
    friend NoteParserCheckpointLocalPatchResult
    BuildNoteParserCheckpointLocalPlainTextPatch(
        const NoteParserCheckpointIndex& previous,
        NoteDerivedSnapshotIdentity current_identity,
        NoteTextPieceSequence::Snapshot current_canonical_root,
        const NoteSourceLineMap::Snapshot& current_source_line_map,
        LineIndex changed_line,
        NoteParserCheckpointIndex* out,
        NoteParserCheckpointLocalPatchWork* out_work) noexcept;
};

// Builds a pure, revision-bound parser checkpoint snapshot.  It rejects a
// stale NoteDocument, malformed line starts, missing/duplicate identities,
// invalid source spans, or an invalid source owner.  The function has no Win32,
// view-cache, persistence, or global-state dependency.
[[nodiscard]] NoteParserCheckpointIndex BuildNoteParserCheckpointIndex(
    const NoteTextModel& source,
    const NoteDocument& document,
    NoteDerivedSnapshotIdentity source_identity,
    NoteTextPieceSequence::Snapshot canonical_source_root);

// Full construction may receive the exact canonical source-line map already
// published by NoteTextCore/NoteSyntaxSnapshot. This avoids rebuilding a
// coordinate index solely for checkpoint construction.
[[nodiscard]] NoteParserCheckpointIndex BuildNoteParserCheckpointIndex(
    const NoteTextModel& source,
    const NoteDocument& document,
    NoteDerivedSnapshotIdentity source_identity,
    NoteTextPieceSequence::Snapshot canonical_source_root,
    const NoteSourceLineMap::Snapshot& source_line_map,
    NoteParserCheckpointBuildWork* out_work = nullptr);

// Marker-free same-row edits preserve all parser boundary states. This
// publication updates only identity/root/line-map, sharing every immutable
// state payload with `previous`; caller-side syntax proof remains mandatory.
[[nodiscard]] NoteParserCheckpointLocalPatchResult
BuildNoteParserCheckpointLocalPlainTextPatch(
    const NoteParserCheckpointIndex& previous,
    NoteDerivedSnapshotIdentity current_identity,
    NoteTextPieceSequence::Snapshot current_canonical_root,
    const NoteSourceLineMap::Snapshot& current_source_line_map,
    LineIndex changed_line,
    NoteParserCheckpointIndex* out,
    NoteParserCheckpointLocalPatchWork* out_work = nullptr) noexcept;

[[nodiscard]] bool NoteParserCheckpointIndexMatchesTextModel(
    const NoteParserCheckpointIndex& index,
    const NoteTextModel& source,
    NoteDerivedSnapshotIdentity source_identity) noexcept;

// A suffix is reusable only when all three independent conditions hold:
// the current root proves the old/new line ranges (and the bounded
// look-behind row, when present) are exact shared canonical identity, both
// parser boundary states match, and the layout key is unchanged. The line
// result is then deterministic from (entry state, shared source line, layout
// key).
// Any unmodeled state returns false and leaves the caller on its full fallback.
[[nodiscard]] bool NoteParserCheckpointCanReuseSuffix(
    const NoteParserCheckpointIndex& previous,
    LineIndex previous_line,
    const NoteParserCheckpointIndex& rebuilt,
    LineIndex rebuilt_line,
    const NoteTextPieceSequence::Snapshot& current_canonical_root,
    NoteParserLayoutKey previous_layout_key,
    NoteParserLayoutKey rebuilt_layout_key) noexcept;

} // namespace note
