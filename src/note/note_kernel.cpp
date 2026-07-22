#include "note/note_kernel.h"

#include "note/note_parser.h"

#include <algorithm>
#include <utility>

namespace note {
namespace {

bool SpanIntersectsEdit(const Span& span, size_t editStart, size_t editOldEnd) {
    if (editOldEnd == editStart) {
        return span.start.value <= editStart && span.end.value > editStart;
    }
    return span.start.value < editOldEnd && span.end.value > editStart;
}

bool SpanContainsEditRange(const Span& span, size_t editStart, size_t editOldEnd) {
    if (editOldEnd == editStart) {
        return span.start.value <= editStart && span.end.value >= editStart;
    }
    return span.start.value <= editStart && span.end.value >= editOldEnd;
}

void ApplyEditToSpan(Span* span,
                     const TextEdit& edit,
                     bool absorbInsertionAtEnd = false) {
    if (!span) return;
    const size_t editStart = edit.start.value;
    const size_t editOldEnd = edit.start.value + edit.deleted_len;
    const size_t insertedLen = edit.inserted_text.size();
    const ptrdiff_t delta = static_cast<ptrdiff_t>(insertedLen) -
                            static_cast<ptrdiff_t>(edit.deleted_len);

    if (edit.deleted_len == 0) {
        if (span->start.value <= editStart &&
            (span->end.value > editStart ||
             (absorbInsertionAtEnd && span->end.value == editStart))) {
            span->end = {span->end.value + insertedLen};
            return;
        }
        if (span->start.value >= editStart) {
            span->start = {span->start.value + insertedLen};
            span->end = {span->end.value + insertedLen};
        }
        return;
    }

    if (span->end.value <= editStart) return;
    if (span->start.value >= editOldEnd) {
        span->start = {static_cast<size_t>(static_cast<ptrdiff_t>(span->start.value) + delta)};
        span->end = {static_cast<size_t>(static_cast<ptrdiff_t>(span->end.value) + delta)};
        return;
    }

    const size_t newEnd = editStart + insertedLen;
    span->start = {span->start.value < editStart ? span->start.value : editStart};
    span->end = {span->end.value > editOldEnd
        ? static_cast<size_t>(static_cast<ptrdiff_t>(span->end.value) + delta)
        : newEnd};
    if (span->end.value < span->start.value) span->end = span->start;
}

void RecomputeDocumentLocations(NoteDocument* document,
                                const NoteTextModel& model) {
    if (!document) return;
    for (auto& block : document->blocks) {
        block.loc = ResolveLineColumn(model, block.span.start.value);
    }
}

void PruneEmptyDocumentNodes(NoteDocument* document) {
    if (!document) return;
    document->inlines.erase(
        std::remove_if(document->inlines.begin(), document->inlines.end(),
                       [](const InlineNode& node) {
                           return node.span.end <= node.span.start;
                       }),
        document->inlines.end());
    document->style_spans.erase(
        std::remove_if(document->style_spans.begin(), document->style_spans.end(),
                       [](const StyleSpan& span) {
                           return span.span.end <= span.span.start;
                       }),
        document->style_spans.end());
    document->math_spans.erase(
        std::remove_if(document->math_spans.begin(), document->math_spans.end(),
                       [](const MathSpan& span) {
                           return span.span.end <= span.span.start ||
                                  span.content_span.end <= span.content_span.start;
                       }),
        document->math_spans.end());
    document->diagnostics.erase(
        std::remove_if(document->diagnostics.begin(), document->diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.span.end < diagnostic.span.start;
                       }),
        document->diagnostics.end());
}

} // namespace

void LocalNoteKernel::Reset(NoteId noteId,
                            NoteMetadata metadata,
                            std::wstring raw,
                            uint64_t contentRevision,
                            uint64_t persistenceRevision,
                            NoteContentKind contentKind) {
    text_core_.Reset(noteId, std::move(metadata), std::move(raw),
                     contentRevision, persistenceRevision);
    history_.Clear();
    content_kind_ = contentKind;
    ResetDerivedState(true);
}

NoteKernelApplyResult LocalNoteKernel::Apply(const TextEdit& edit,
                                             bool renderActive) {
    NoteKernelApplyResult result;
    if (!text_core_.valid()) return result;

    result.dirty_graph = BuildNoteDirtyGraph(text_core_.model().raw,
                                             text_core_.model().line_starts,
                                             edit,
                                             renderActive);
    result.text_result = text_core_.Apply(edit);
    if (result.text_result != NoteTextApplyResult::Applied) {
        return result;
    }

    if (pending_edit_.has_value()) {
        force_full_refresh_ = true;
        pending_edit_.reset();
        pending_dirty_graph_.reset();
    } else {
        pending_edit_ = edit;
        pending_dirty_graph_ = result.dirty_graph;
    }
    return result;
}

NoteKernelApplyResult LocalNoteKernel::ApplyUserEdit(
    const TextEdit& edit,
    NoteTextSelection selectionBefore,
    NoteTextSelection selectionAfter,
    NoteHistoryOperationKind kind,
    uint64_t tick,
    bool renderActive) {
    const std::wstring textBefore = text_core_.valid() ? text_core_.model().raw : std::wstring{};
    NoteKernelApplyResult result = Apply(edit, renderActive);
    if (result.text_result == NoteTextApplyResult::Applied) {
        // Recording failure is deliberately non-destructive: the canonical edit
        // remains applied, but it is never advertised as undoable.
        history_.Record(textBefore, edit, selectionBefore, selectionAfter, kind, tick);
    }
    return result;
}

std::optional<NoteKernelHistoryResult> LocalNoteKernel::Undo(bool renderActive) {
    const auto replay = history_.PeekUndo();
    if (!replay.has_value()) return std::nullopt;
    NoteKernelHistoryResult result;
    result.selection = replay->selection;
    result.apply_result = Apply(replay->edit, renderActive);
    if (!result.applied()) return std::nullopt;
    if (!history_.CommitUndo()) return std::nullopt;
    return result;
}

std::optional<NoteKernelHistoryResult> LocalNoteKernel::Redo(bool renderActive) {
    const auto replay = history_.PeekRedo();
    if (!replay.has_value()) return std::nullopt;
    NoteKernelHistoryResult result;
    result.selection = replay->selection;
    result.apply_result = Apply(replay->edit, renderActive);
    if (!result.applied()) return std::nullopt;
    if (!history_.CommitRedo()) return std::nullopt;
    return result;
}

NoteKernelRefreshResult LocalNoteKernel::RefreshDerived(bool forceFull) {
    if (!text_core_.valid()) return {};

    std::optional<NoteDirtyGraph> consumedDirtyGraph = pending_dirty_graph_;
    if (content_kind_ == NoteContentKind::PlainText) {
        ResetDerivedState(true);
        NoteKernelRefreshResult result;
        result.kind = NoteKernelRefreshKind::Cleared;
        result.consumed_dirty_graph = std::move(consumedDirtyGraph);
        return result;
    }

    const bool mustRebuild = forceFull || force_full_refresh_ ||
        deferred_full_refresh_ || !syntax_ready_;
    if (!mustRebuild && pending_edit_.has_value()) {
        const TextEdit edit = *pending_edit_;
        if (TryApplyIncrementalSyntax(edit)) {
            pending_edit_.reset();
            pending_dirty_graph_.reset();
            force_full_refresh_ = false;

            NoteKernelRefreshResult result;
            result.kind = deferred_full_refresh_
                ? NoteKernelRefreshKind::Deferred
                : NoteKernelRefreshKind::Incremental;
            result.source_identity = document_.source_identity;
            result.consumed_dirty_graph = std::move(consumedDirtyGraph);
            result.current = CanReadSyntax();
            if (result.current) RefreshSemanticIndex();
            else semantic_index_ = {};
            return result;
        }
    }

    if (!mustRebuild && !pending_edit_.has_value() && CanReadSyntax()) {
        NoteKernelRefreshResult result;
        result.kind = NoteKernelRefreshKind::Unchanged;
        result.source_identity = document_.source_identity;
        result.current = true;
        return result;
    }
    return RebuildAll(std::move(consumedDirtyGraph));
}

void LocalNoteKernel::RequestFullRefresh() {
    force_full_refresh_ = true;
}

void LocalNoteKernel::DiscardPendingEditAndRequireFullRefresh() {
    if (pending_edit_.has_value()) force_full_refresh_ = true;
    pending_edit_.reset();
    pending_dirty_graph_.reset();
}

void LocalNoteKernel::ClearDerived() {
    ResetDerivedState(true);
}

bool LocalNoteKernel::CanReadSyntax() const {
    return text_core_.valid() && syntax_ready_ &&
           !pending_edit_.has_value() && !deferred_full_refresh_ &&
           NoteDocumentMatchesSourceIdentity(
               document_,
               NoteDerivedSnapshotIdentity{
                   text_core_.note_id(), text_core_.content_revision()}) &&
           NoteDocumentMatchesTextModel(document_, syntax_source_) &&
           syntax_source_.revision == text_core_.content_revision() &&
           text_core_.MatchesRaw(syntax_source_.raw);
}

bool LocalNoteKernel::CanReadSemantic() const {
    return CanReadSyntax() &&
           SemanticIndexMatchesTextModel(
               semantic_index_, text_core_.note_id(), syntax_source_);
}

bool LocalNoteKernel::TryApplyIncrementalSyntax(const TextEdit& edit) {
    if (!syntax_ready_ ||
        document_.source_identity.note_id != text_core_.note_id() ||
        !NoteDocumentMatchesTextModel(document_, syntax_source_)) {
        return false;
    }
    if (edit.deleted_len == 0 && edit.inserted_text.empty()) return true;
    if (edit.start.value > syntax_source_.raw.size() ||
        edit.deleted_len > syntax_source_.raw.size() - edit.start.value) {
        return false;
    }

    const std::wstring& text = text_core_.model().raw;
    const size_t expectedSize = syntax_source_.raw.size() - edit.deleted_len +
                                edit.inserted_text.size();
    if (text.size() != expectedSize ||
        text.compare(0, edit.start.value, syntax_source_.raw, 0, edit.start.value) != 0 ||
        text.compare(edit.start.value, edit.inserted_text.size(),
                     edit.inserted_text, 0, edit.inserted_text.size()) != 0) {
        return false;
    }
    const size_t oldSuffixStart = edit.start.value + edit.deleted_len;
    const size_t newSuffixStart = edit.start.value + edit.inserted_text.size();
    const size_t suffixLen = syntax_source_.raw.size() - oldSuffixStart;
    if (suffixLen > 0 &&
        text.compare(newSuffixStart, suffixLen,
                     syntax_source_.raw, oldSuffixStart, suffixLen) != 0) {
        return false;
    }

    const size_t editOldEnd = edit.start.value + edit.deleted_len;
    const std::wstring_view removed = std::wstring_view(syntax_source_.raw).substr(
        edit.start.value, edit.deleted_len);
    const bool lineBreakOnlyEdit =
        (removed.empty() || IsOnlyNoteLineBreakText(removed)) &&
        (edit.inserted_text.empty() ||
         IsOnlyNoteLineBreakText(edit.inserted_text));
    if (!lineBreakOnlyEdit &&
        (ContainsNoteStructuralText(removed) ||
         ContainsNoteStructuralText(edit.inserted_text))) {
        return false;
    }

    std::optional<size_t> insertionAtTextEndOwner;
    if (!lineBreakOnlyEdit) {
        bool coveredByTextInline = false;
        for (size_t index = 0; index < document_.inlines.size(); ++index) {
            const InlineNode& node = document_.inlines[index];
            if (node.kind != InlineKind::Text) continue;
            if (SpanContainsEditRange(node.span, edit.start.value, editOldEnd)) {
                coveredByTextInline = true;
                if (edit.deleted_len == 0 && node.span.end.value == edit.start.value) {
                    insertionAtTextEndOwner = index;
                }
                break;
            }
        }
        if (!coveredByTextInline) return false;
        for (const InlineNode& node : document_.inlines) {
            if (node.kind == InlineKind::Code &&
                SpanIntersectsEdit(node.span, edit.start.value, editOldEnd)) {
                return false;
            }
        }
        for (const MathSpan& span : document_.math_spans) {
            if (SpanIntersectsEdit(span.span, edit.start.value, editOldEnd) ||
                SpanIntersectsEdit(span.content_span, edit.start.value, editOldEnd)) {
                return false;
            }
        }
    }

    ApplyTextEdit(&syntax_source_, edit);
    SetNoteDocumentSourceIdentity(
        &document_,
        NoteDerivedSnapshotIdentity{
            text_core_.note_id(), syntax_source_.revision});

    const Span* insertionOwnerSpan = insertionAtTextEndOwner.has_value()
        ? &document_.inlines[*insertionAtTextEndOwner].span
        : nullptr;
    auto absorbAtEnd = [&](const Span& span) {
        return lineBreakOnlyEdit ||
               (insertionOwnerSpan && span.start <= insertionOwnerSpan->start &&
                span.end == edit.start);
    };
    for (auto& block : document_.blocks) {
        ApplyEditToSpan(&block.span, edit, absorbAtEnd(block.span));
    }
    for (auto& node : document_.inlines) {
        ApplyEditToSpan(&node.span, edit, absorbAtEnd(node.span));
    }
    for (auto& span : document_.style_spans) {
        ApplyEditToSpan(&span.span, edit, absorbAtEnd(span.span));
    }
    for (auto& span : document_.math_spans) {
        ApplyEditToSpan(&span.span, edit);
        ApplyEditToSpan(&span.content_span, edit);
    }
    for (auto& diagnostic : document_.diagnostics) {
        ApplyEditToSpan(&diagnostic.span, edit);
    }
    PruneEmptyDocumentNodes(&document_);
    RecomputeDocumentLocations(&document_, syntax_source_);

    deferred_full_refresh_ = lineBreakOnlyEdit;
    return syntax_source_.revision == text_core_.content_revision() &&
           text_core_.MatchesRaw(syntax_source_.raw);
}

NoteKernelRefreshResult LocalNoteKernel::RebuildAll(
    std::optional<NoteDirtyGraph> consumedDirtyGraph) {
    syntax_source_ = text_core_.model();
    document_ = ParseNoteDocument(syntax_source_);
    SetNoteDocumentSourceIdentity(
        &document_,
        NoteDerivedSnapshotIdentity{
            text_core_.note_id(), text_core_.content_revision()});
    syntax_ready_ = true;
    deferred_full_refresh_ = false;
    force_full_refresh_ = false;
    pending_edit_.reset();
    pending_dirty_graph_.reset();
    RefreshSemanticIndex();

    NoteKernelRefreshResult result;
    result.kind = NoteKernelRefreshKind::Full;
    result.source_identity = document_.source_identity;
    result.consumed_dirty_graph = std::move(consumedDirtyGraph);
    result.current = CanReadSyntax();
    return result;
}

void LocalNoteKernel::ResetDerivedState(bool clearPendingEdit) {
    syntax_source_ = {};
    document_ = {};
    semantic_index_ = {};
    syntax_ready_ = false;
    deferred_full_refresh_ = false;
    force_full_refresh_ = false;
    if (clearPendingEdit) {
        pending_edit_.reset();
        pending_dirty_graph_.reset();
    }
}

void LocalNoteKernel::RefreshSemanticIndex() {
    if (!CanReadSyntax()) {
        semantic_index_ = {};
        return;
    }
    semantic_index_ = BuildSemanticIndexSnapshot(
        text_core_.note_id(), syntax_source_, document_);
}

LocalNoteKernel* LocalNoteKernelRegistry::Reset(
    NoteId noteId,
    NoteMetadata metadata,
    std::wstring raw,
    uint64_t contentRevision,
    uint64_t persistenceRevision,
    NoteContentKind contentKind) {
    if (!noteId.valid()) return nullptr;
    LocalNoteKernel& kernel = kernels_[noteId.value];
    kernel.Reset(noteId, std::move(metadata), std::move(raw), contentRevision,
                 persistenceRevision, contentKind);
    return &kernel;
}

LocalNoteKernel* LocalNoteKernelRegistry::Find(NoteId noteId) {
    if (!noteId.valid()) return nullptr;
    const auto it = kernels_.find(noteId.value);
    return it != kernels_.end() ? &it->second : nullptr;
}

const LocalNoteKernel* LocalNoteKernelRegistry::Find(NoteId noteId) const {
    if (!noteId.valid()) return nullptr;
    const auto it = kernels_.find(noteId.value);
    return it != kernels_.end() ? &it->second : nullptr;
}

LocalNoteKernel* LocalNoteKernelRegistry::FindForView(
    const ViewIdentity& viewIdentity) {
    return viewIdentity.valid() ? Find(viewIdentity.note_id) : nullptr;
}

const LocalNoteKernel* LocalNoteKernelRegistry::FindForView(
    const ViewIdentity& viewIdentity) const {
    return viewIdentity.valid() ? Find(viewIdentity.note_id) : nullptr;
}

bool LocalNoteKernelRegistry::Forget(NoteId noteId) {
    return noteId.valid() && kernels_.erase(noteId.value) != 0;
}

} // namespace note
