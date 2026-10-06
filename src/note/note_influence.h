#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace note {

// The smallest source scope whose already-published presentation may depend
// on an edit that does not itself change markup syntax.  This is deliberately
// a description of the *pre-edit* immutable document: callers must discard it
// when that document no longer matches the source text revision.
enum class NoteInfluenceScopeKind {
    Unknown,
    LocalLine,
    InlineLink,
    InlineCode,
    InlineMath,
    // Delimiter-free, diagnostic-free literal text inside an inline math
    // span. The opening delimiter, leading literal content and closing
    // delimiter remain outside the edit; InlineMath owns every other case.
    InlineMathContent,
    CodeBlock,
    // Delimiter-free text inside an already-open code block.  Its source
    // line is local, while CodeBlock remains the conservative owner for a
    // fence marker or any edit that can create one.
    CodeBlockContent,
    // Delimiter-free body text on a list or quote marker row.  The marker,
    // its required whitespace, and the first body character stay immutable;
    // Container remains the conservative owner for nesting or marker edits.
    ContainerContent,
    BlockMath,
    SharedTableGeometry,
    // A list or quote parent was identified from the immutable syntax
    // snapshot.  It records the exact structural owner, but deliberately
    // grants no local-patch proof until parser checkpoints can prove that a
    // marker/depth transition reaches a fixed point.
    Container,
};

struct NoteInfluenceLineRange {
    bool valid = false;
    size_t first = 0;
    size_t last = 0;
};

struct NoteInfluenceScope {
    NoteInfluenceScopeKind kind = NoteInfluenceScopeKind::Unknown;
    // Revision of the source from which this proof was made.  A continuation
    // is valid only for the immediately following canonical text revision.
    uint64_t source_revision = 0;
    NoteInfluenceLineRange source_lines;

    // Index of the immutable syntax node that established this scope, when
    // one is needed to update a node-local derived value. It is meaningful
    // only with the matching source revision; -1 means no node index.
    size_t source_node_index = static_cast<size_t>(-1);

    // Absolute exclusive source offset of syntax that must remain unchanged
    // while carrying this local proof.  Zero means that the scope uses the
    // ordinary leading-character guard.  ContainerContent sets it just after
    // the first body character, so deleting the body into an empty marker can
    // never continue on the local path.
    size_t protected_prefix_end = 0;

    // True only when source revision, edit bounds, source line and enclosing
    // syntax node together prove that BuildNoteDirtyGraph need not materialize
    // an after-text copy merely to locate the affected lines.
    bool permits_local_dirty_graph = false;
};

struct NoteInfluenceIndexEntry {
    NoteInfluenceScopeKind kind = NoteInfluenceScopeKind::Unknown;
    Span span{};
    NoteInfluenceLineRange source_lines;
    // Immutable-document node index for entries that require exact node
    // lookup (currently math). It prevents input handling from scanning all
    // nodes merely to recover the row candidate's owner.
    size_t source_node_index = static_cast<size_t>(-1);
};

// Immutable, revision-bound lookup structure.  Each source row stores only
// entries whose span intersects that row, so resolving an edit never scans
// the document-wide block/inline arrays on the input path.
struct NoteInfluenceIndex {
    uint64_t source_revision = 0;
    std::vector<NoteInfluenceIndexEntry> entries;
    // Compressed sparse row mapping: entries for source row L occupy
    // [line_entry_offsets[L], line_entry_offsets[L + 1]).  This avoids one
    // heap-owning vector per document row in long ordinary notes.
    std::vector<size_t> line_entry_offsets;
    std::vector<size_t> line_entry_indices;
};

[[nodiscard]] NoteInfluenceIndex BuildNoteInfluenceIndex(
    const NoteTextModel& source,
    const NoteDocument& document);

[[nodiscard]] bool NoteInfluenceIndexMatchesTextModel(
    const NoteInfluenceIndex& index,
    const NoteTextModel& source) noexcept;

[[nodiscard]] NoteInfluenceScope ResolveNoteInfluenceScope(
    const NoteTextModel& source,
    const NoteDocument& document,
    const TextEdit& edit,
    const NoteInfluenceIndex* index = nullptr);

// Extends a previously proven plain-text row through a pending transaction.
// This intentionally accepts no containers or inline constructs: changing
// rows, delimiters, or line count revokes the proof and returns Unknown.
[[nodiscard]] NoteInfluenceScope ResolveNoteInfluenceContinuationScope(
    const NoteTextModel& source,
    const NoteInfluenceScope& previous,
    const TextEdit& edit);

// Tests whether a delimiter-free code-body row remains a literal code-body
// row after one bounded edit.  The caller supplies only that source row and
// its absolute start; this function never needs to inspect another row.
[[nodiscard]] bool NoteCodeBlockContentEditPreservesFenceState(
    std::wstring_view beforeLine,
    Utf16CodeUnitOffset lineStart,
    const TextEdit& edit);

// Tests whether an edit remains wholly inside the literal body of a list or
// quote marker row.  It returns the exclusive prefix boundary that must stay
// unchanged for a following pending edit, or zero when the edit could affect
// marker, indentation, nesting, or body existence.
[[nodiscard]] size_t NoteContainerContentProtectedPrefixEnd(
    std::wstring_view beforeLine,
    Utf16CodeUnitOffset lineStart,
    const TextEdit& edit);

// A dirty graph can safely avoid materializing post-edit text for every
// proven scope.  Updating the immutable syntax snapshot in place has a
// stricter contract: only the listed constructs retain their parse shape
// when their delimiter-free content changes.
[[nodiscard]] bool NoteInfluenceScopeAllowsIncrementalSyntaxPatch(
    const NoteInfluenceScope& scope) noexcept;

} // namespace note
