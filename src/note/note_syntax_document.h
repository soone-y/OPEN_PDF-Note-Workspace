#pragma once

#include "note/note_model.h"
#include "note/note_source_line_map.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace note {

class NoteSyntaxDocumentStorage;
class NoteSyntaxDocumentSpanIndex;

// The final syntax snapshot keeps parser output in source-row-relative,
// persistent payloads.  Absolute offsets are resolved only at an API
// boundary, using the snapshot's exact immutable source-line map.  This
// prevents a same-row edit from rewriting every later node's offset.
enum class NoteSyntaxDocumentNodeKind : unsigned char {
    Block,
    Inline,
    Style,
    Math,
    Diagnostic,
};

enum class NoteSyntaxDocumentBuildResult {
    Built,
    InvalidOutput,
    InvalidSourceLines,
    InconsistentDocument,
    AllocationFailure,
};

enum class NoteSyntaxDocumentLocalPatchResult {
    Built,
    InvalidOutput,
    InvalidPreviousDocument,
    InvalidSourceLines,
    InvalidEdit,
    RequiresFullDocument,
    InconsistentCandidate,
    AllocationFailure,
};

// This is test-only accounting for the final syntax storage.  A local
// same-row patch may replace payloads with endpoints on that row, but it must
// neither copy the untouched document tail nor rewrite its absolute offsets.
struct NoteSyntaxDocumentLocalPatchWork {
    size_t replacement_node_payloads = 0;
    size_t tail_node_payload_rewrites = 0;
    size_t interval_index_payload_rewrites = 0;
};

struct NoteSyntaxDocumentQueryWork {
    size_t visited_index_nodes = 0;
};

// Immutable parser output independent of NoteTextModel.  `CopyDocument...`
// exists only for full-build/differential oracles; final paint and input code
// must resolve only the nodes it consumes.
class NoteSyntaxDocument final {
public:
    [[nodiscard]] static NoteSyntaxDocumentBuildResult Build(
        const NoteDocument& document,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::shared_ptr<const NoteSyntaxDocument>* out) noexcept;

    // Applies a source edit that remains on one logical row.  The caller must
    // separately prove parser fixed point; this type proves only that every
    // changed source anchor and the span index belong to the supplied maps.
    [[nodiscard]] static NoteSyntaxDocumentLocalPatchResult BuildLocalPlainTextPatch(
        std::shared_ptr<const NoteSyntaxDocument> previous,
        const NoteSourceLineMap::Snapshot& beforeSourceLines,
        const NoteSourceLineMap::Snapshot& currentSourceLines,
        const TextEdit& edit,
        LineIndex changedLine,
        std::shared_ptr<const NoteSyntaxDocument>* out,
        NoteSyntaxDocumentLocalPatchWork* outWork = nullptr) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] size_t node_count(NoteSyntaxDocumentNodeKind kind) const noexcept;

    // Uses the persistent interval index.  It is the exact equivalent of the
    // old full-vector predicate, but visits only nodes whose source spans can
    // contain/intersect this edit.
    [[nodiscard]] bool PermitsPlainTextLeafEdit(
        const NoteSourceLineMap::Snapshot& sourceLines,
        Utf16CodeUnitOffset start,
        size_t deletedLength,
        NoteSyntaxDocumentQueryWork* outWork = nullptr) const noexcept;

    [[nodiscard]] bool ResolveBlock(const NoteSourceLineMap::Snapshot& sourceLines,
                                    size_t index,
                                    BlockNode* out) const noexcept;
    [[nodiscard]] bool ResolveInline(const NoteSourceLineMap::Snapshot& sourceLines,
                                     size_t index,
                                     InlineNode* out) const noexcept;
    [[nodiscard]] bool ResolveStyle(const NoteSourceLineMap::Snapshot& sourceLines,
                                    size_t index,
                                    StyleSpan* out) const noexcept;
    [[nodiscard]] bool ResolveMath(const NoteSourceLineMap::Snapshot& sourceLines,
                                   size_t index,
                                   MathSpan* out) const noexcept;
    [[nodiscard]] bool ResolveDiagnostic(const NoteSourceLineMap::Snapshot& sourceLines,
                                         size_t index,
                                         Diagnostic* out) const noexcept;

    // Full materialization is deliberately explicit. It is permitted for a
    // fresh complete derived-plan build, never for a local splice or paint.
    [[nodiscard]] bool CopyDocumentForCompleteBuild(
        const NoteSourceLineMap::Snapshot& sourceLines,
        NoteDerivedSnapshotIdentity sourceIdentity,
        NoteDocument* out) const noexcept;
    // Differential-test spelling for the same complete-build-only adapter.
    [[nodiscard]] bool CopyDocumentForDifferentialTest(
        const NoteSourceLineMap::Snapshot& sourceLines,
        NoteDerivedSnapshotIdentity sourceIdentity,
        NoteDocument* out) const noexcept;

    [[nodiscard]] bool SharesNodePayloadForDifferentialTest(
        const NoteSyntaxDocument& other,
        NoteSyntaxDocumentNodeKind kind,
        size_t index) const noexcept;

private:
    std::shared_ptr<const NoteSyntaxDocumentStorage> storage_;
    std::shared_ptr<const NoteSyntaxDocumentSpanIndex> span_index_;
    std::shared_ptr<const std::vector<std::vector<size_t>>> endpoint_nodes_by_line_;
    NoteSourceLineMap::Snapshot source_line_map_{};
    NoteMetadata metadata_{};
    size_t block_count_ = 0;
    size_t inline_count_ = 0;
    size_t style_count_ = 0;
    size_t math_count_ = 0;
    size_t diagnostic_count_ = 0;
    bool valid_ = false;
};

} // namespace note
