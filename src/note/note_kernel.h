#pragma once

#include "note/note_dirty_graph.h"
#include "note/note_content_kind.h"
#include "note/note_history.h"
#include "note/note_pending_edit_transaction.h"
#include "note/note_semantic_index.h"
#include "note/note_text_core.h"

#include <map>
#include <optional>

namespace note {

enum class NoteKernelRefreshKind {
    Unavailable,
    Unchanged,
    Cleared,
    Incremental,
    Deferred,
    Full,
};

struct NoteKernelApplyResult {
    NoteTextApplyResult text_result = NoteTextApplyResult::InvalidOwner;
    NoteDirtyGraph dirty_graph{};
    // Test/debug observable only: the second and later edit of a pending
    // transaction used the carried local proof rather than materializing the
    // current canonical text to rediscover its source row.
    bool used_pending_local_dirty_proof = false;

    bool applied() const {
        return text_result == NoteTextApplyResult::Applied ||
               text_result == NoteTextApplyResult::NoChange;
    }
};

struct NoteKernelRefreshResult {
    NoteKernelRefreshKind kind = NoteKernelRefreshKind::Unavailable;
    NoteDerivedSnapshotIdentity source_identity{};
    std::optional<NoteDirtyGraph> consumed_dirty_graph;
    bool current = false;
};

struct NoteKernelHistoryResult {
    NoteKernelApplyResult apply_result{};

    bool applied() const { return apply_result.applied(); }
};

// A pending proof is deliberately narrower than NoteInfluenceScope. It is
// valid only after an already-proven ordinary row or a literal code-body,
// container-body, or inline-math-body row. Ordinary rows keep their leading
// whitespace and first visible character immutable; code bodies may become
// empty, but each continuation rechecks that their row is not becoming a
// fence marker. Table, link, delimiter, and diagnostic-bearing math content
// remain outside this proof.
struct NotePendingLocalLineProof {
    NoteInfluenceScope scope;
    uint64_t current_source_revision = 0;
    size_t content_start = 0;
    size_t protected_prefix_end = 0;
    size_t content_end = 0;
    size_t non_whitespace_count = 0;
};

// UI-independent owner of one note's canonical text and all semantic derived state.
class LocalNoteKernel {
public:
    void Reset(NoteId noteId,
               NoteMetadata metadata,
               std::wstring raw,
               uint64_t contentRevision,
               uint64_t persistenceRevision,
               NoteContentKind contentKind);

    [[nodiscard]] NoteKernelApplyResult Apply(const TextEdit& edit, bool renderActive);
    [[nodiscard]] NoteKernelApplyResult ApplyUserEdit(
        const TextEdit& edit,
        NoteTextSelection selectionBefore,
        NoteTextSelection selectionAfter,
        NoteHistoryOperationKind kind,
        uint64_t tick,
        bool renderActive);
    [[nodiscard]] std::optional<NoteKernelHistoryResult> Undo(bool renderActive);
    [[nodiscard]] std::optional<NoteKernelHistoryResult> Redo(bool renderActive);
    void ClearHistory();
    [[nodiscard]] NoteKernelRefreshResult RefreshDerived(bool forceFull = false);
    void RequestFullRefresh();
    void DiscardPendingEditAndRequireFullRefresh();
    void ClearDerived();

    bool valid() const { return text_core_.valid(); }
    bool has_pending_edit() const { return pending_transaction_.active(); }
    bool requires_full_refresh() const { return force_full_refresh_; }
    bool has_deferred_full_refresh() const { return deferred_full_refresh_; }
    bool CanReadSyntax() const;
    bool CanReadSemantic() const;

    NoteContentKind content_kind() const { return content_kind_; }
    NoteTextCore& text_core() { return text_core_; }
    const NoteTextCore& text_core() const { return text_core_; }
    const NoteTextModel& syntax_source() const { return syntax_source_; }
    const NoteDocument& document() const { return document_; }
    const SemanticIndexSnapshot& semantic_index() const { return semantic_index_; }
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }

private:
    bool TryApplyIncrementalSyntax(const TextEdit& edit,
                                   const NoteInfluenceScope* influence);
    [[nodiscard]] NoteKernelRefreshResult RebuildAll(
        std::optional<NoteDirtyGraph> consumedDirtyGraph);
    void ResetDerivedState(bool clearPendingEdit);
    void RebuildInfluenceIndex();
    void RefreshSemanticIndex();

    NoteTextCore text_core_;
    NoteHistory history_;
    NoteContentKind content_kind_ = NoteContentKind::Markdown;
    NoteTextModel syntax_source_;
    NoteTextPieceSequence::Snapshot syntax_source_snapshot_;
    NoteDocument document_;
    NoteInfluenceIndex influence_index_;
    SemanticIndexSnapshot semantic_index_;
    bool syntax_ready_ = false;
    bool deferred_full_refresh_ = false;
    bool force_full_refresh_ = false;
    PendingNoteEditTransaction pending_transaction_;
    std::optional<NotePendingLocalLineProof> pending_local_line_proof_;
    std::optional<NoteInfluenceScope> pending_influence_scope_;
    std::optional<NoteDirtyGraph> pending_dirty_graph_;
};

class LocalNoteKernelRegistry {
public:
    LocalNoteKernel* Reset(NoteId noteId,
                           NoteMetadata metadata,
                           std::wstring raw,
                           uint64_t contentRevision,
                           uint64_t persistenceRevision,
                           NoteContentKind contentKind);
    LocalNoteKernel* Find(NoteId noteId);
    const LocalNoteKernel* Find(NoteId noteId) const;
    LocalNoteKernel* FindForView(const ViewIdentity& viewIdentity);
    const LocalNoteKernel* FindForView(const ViewIdentity& viewIdentity) const;
    bool Forget(NoteId noteId);
    size_t size() const { return kernels_.size(); }

private:
    std::map<uint64_t, LocalNoteKernel> kernels_;
};

} // namespace note
