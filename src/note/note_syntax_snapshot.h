#pragma once

#include "note/note_content_kind.h"
#include "note/note_parser_checkpoint.h"
#include "note/note_source_line_map.h"
#include "note/note_syntax_document.h"
#include "note/note_text_core.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace note {

class NoteRenderFinalTransaction;

// A failed construction never publishes a partial source/document/index set.
// Callers must retain their last complete snapshot and choose the documented
// fallback, rather than combining individual fields from different revisions.
enum class NoteSyntaxSnapshotBuildResult {
    Built,
    InvalidOutput,
    InvalidSource,
    InconsistentSourceCoordinates,
    AllocationFailure,
};

// The only accepted incremental final-snapshot route at this stage. It is
// intentionally narrow: unsupported syntax is not approximated, but must use
// BuildFromTextCore for a complete snapshot instead.
enum class NoteSyntaxSnapshotLocalPatchResult {
    Built,
    InvalidOutput,
    InvalidPreviousSnapshot,
    InvalidCurrentSource,
    InvalidEdit,
    RequiresFullSnapshot,
    FixedPointNotProven,
    UnexpectedTextCoreMaterialization,
    InconsistentCandidate,
    AllocationFailure,
};

// Test-only accounting for a persistent syntax splice. No member means
// source/document code units: the final snapshot does not retain either of
// those legacy full-sized objects.
struct NoteSyntaxSnapshotLocalPatchWork {
    uint64_t text_core_materializations = 0;
    size_t syntax_document_replacement_payloads = 0;
    size_t syntax_document_tail_payload_rewrites = 0;
    size_t syntax_document_interval_index_rewrites = 0;
    size_t checkpoint_lines_built = 0;
};

// Complete immutable syntax state for exactly one canonical text root. It
// intentionally owns no NoteTextModel, legacy NoteDocument, legacy semantic
// index, view cache, row identifier, parser checkpoint, or layout key. Parser
// output is converted into NoteSyntaxDocument at full-build time, then every
// local edit operates only on its persistent source-row-relative payloads.
class NoteSyntaxSnapshot final {
public:
    [[nodiscard]] static NoteSyntaxSnapshotBuildResult BuildFromTextCore(
        const NoteTextCore& textCore,
        NoteContentKind contentKind,
        std::shared_ptr<const NoteSyntaxSnapshot>* out) noexcept;

    // Constructs a new immutable snapshot without parsing its unchanged
    // prefix or suffix. The caller supplies a checkpoint belonging to
    // `previous`; a candidate is accepted only when the next unchanged row
    // proves an exact shared canonical range and identical parser state.
    // On every non-Built result both output values are left untouched.
    [[nodiscard]] static NoteSyntaxSnapshotLocalPatchResult BuildLocalPlainTextPatch(
        std::shared_ptr<const NoteSyntaxSnapshot> previous,
        const NoteParserCheckpointIndex& previousCheckpoint,
        NoteParserLayoutKey layoutKey,
        const NoteTextCore& currentTextCore,
        const TextEdit& edit,
        std::shared_ptr<const NoteSyntaxSnapshot>* outSnapshot,
        NoteParserCheckpointIndex* outCheckpoint,
        NoteSyntaxSnapshotLocalPatchWork* outWork = nullptr) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool has_structured_syntax() const noexcept {
        return has_structured_syntax_;
    }
    [[nodiscard]] NoteContentKind content_kind() const noexcept {
        return content_kind_;
    }
    [[nodiscard]] const NoteDerivedSnapshotIdentity& source_identity() const noexcept {
        return source_identity_;
    }
    [[nodiscard]] const NoteTextPieceSequence::Snapshot& source_root() const noexcept {
        return source_root_;
    }
    [[nodiscard]] const NoteSourceLineMap::Snapshot& source_line_map() const noexcept {
        return source_line_map_;
    }
    [[nodiscard]] const std::shared_ptr<const NoteSyntaxDocument>& syntax_document() const noexcept {
        return syntax_document_;
    }
    // A source range is copied only when a final derived component needs that
    // local range. It never exposes a cached whole-document string.
    [[nodiscard]] std::wstring CopySourceRange(Utf16CodeUnitOffset start,
                                               size_t length) const;
    // Complete-build adapter only. It materializes a legacy-shaped document
    // from the persistent syntax payloads for an initial full source-plan
    // build; it is forbidden from local splices and presentation code.
    [[nodiscard]] bool CopyDocumentForCompleteBuild(NoteDocument* out) const noexcept;

    // Root identity, revision, text length and source-line coordinates all
    // participate. A coincidentally equal string from another canonical root
    // can therefore never validate this snapshot.
    [[nodiscard]] bool MatchesTextCore(const NoteTextCore& textCore) const noexcept;

private:
    // The complete final transaction is the only caller permitted to retain
    // a parser checkpoint built from the transient full-parser result.  It
    // receives that immutable checkpoint before the legacy parser document is
    // discarded; no complete-build adapter materializes it again later.
    friend class NoteRenderFinalTransaction;
    [[nodiscard]] static NoteSyntaxSnapshotBuildResult BuildFromTextCoreWithCheckpoint(
        const NoteTextCore& textCore,
        NoteContentKind contentKind,
        std::shared_ptr<const NoteSyntaxSnapshot>* out,
        NoteParserCheckpointIndex* outCheckpoint) noexcept;

    NoteContentKind content_kind_{};
    NoteDerivedSnapshotIdentity source_identity_{};
    NoteTextPieceSequence::Snapshot source_root_{};
    NoteSourceLineMap::Snapshot source_line_map_{};
    std::shared_ptr<const NoteSyntaxDocument> syntax_document_;
    bool has_structured_syntax_ = false;
    bool valid_ = false;
};

} // namespace note
