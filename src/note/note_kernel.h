#pragma once

#include "note/note_dirty_graph.h"
#include "note/note_history.h"
#include "note/note_semantic_index.h"
#include "note/note_text_core.h"

#include <map>
#include <optional>

namespace note {

enum class NoteContentKind {
    PlainText,
    Markdown,
};

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
    NoteTextSelection selection{};

    bool applied() const { return apply_result.applied(); }
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
    [[nodiscard]] NoteKernelRefreshResult RefreshDerived(bool forceFull = false);
    void RequestFullRefresh();
    void DiscardPendingEditAndRequireFullRefresh();
    void ClearDerived();

    bool valid() const { return text_core_.valid(); }
    bool has_pending_edit() const { return pending_edit_.has_value(); }
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
    bool TryApplyIncrementalSyntax(const TextEdit& edit);
    [[nodiscard]] NoteKernelRefreshResult RebuildAll(
        std::optional<NoteDirtyGraph> consumedDirtyGraph);
    void ResetDerivedState(bool clearPendingEdit);
    void RefreshSemanticIndex();

    NoteTextCore text_core_;
    NoteHistory history_;
    NoteContentKind content_kind_ = NoteContentKind::Markdown;
    NoteTextModel syntax_source_;
    NoteDocument document_;
    SemanticIndexSnapshot semantic_index_;
    bool syntax_ready_ = false;
    bool deferred_full_refresh_ = false;
    bool force_full_refresh_ = false;
    std::optional<TextEdit> pending_edit_;
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
