#include "note/note_kernel.h"

#include "note/note_parser.h"

#include <algorithm>
#include <limits>
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

[[nodiscard]] bool IsPlainPendingLineText(std::wstring_view text) {
    return !ContainsNoteStructuralText(text) &&
           text.find_first_of(L"|>") == std::wstring_view::npos;
}

[[nodiscard]] bool IsPendingLocalLineScopeKind(NoteInfluenceScopeKind kind) {
    return kind == NoteInfluenceScopeKind::LocalLine ||
        kind == NoteInfluenceScopeKind::CodeBlockContent ||
        kind == NoteInfluenceScopeKind::ContainerContent ||
        kind == NoteInfluenceScopeKind::InlineMathContent;
}

[[nodiscard]] bool IsLiteralCodeBodyScope(const NoteInfluenceScope& scope) {
    return scope.kind == NoteInfluenceScopeKind::CodeBlockContent;
}

[[nodiscard]] size_t CountLineNonWhitespace(std::wstring_view text) {
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](wchar_t ch) {
        return ch != L' ' && ch != L'\t';
    }));
}

[[nodiscard]] bool TryAdvanceLineEnd(size_t contentEnd,
                                      size_t deletedLength,
                                      size_t insertedLength,
                                      size_t* outEnd) {
    if (!outEnd || deletedLength > contentEnd) return false;
    const size_t retained = contentEnd - deletedLength;
    if (insertedLength > std::numeric_limits<size_t>::max() - retained) return false;
    *outEnd = retained + insertedLength;
    return true;
}

[[nodiscard]] std::optional<NotePendingLocalLineProof> MakePendingLocalLineProof(
    const NoteTextModel& source,
    const NoteInfluenceScope& influence,
    const TextEdit& edit,
    std::wstring_view deletedText) {
    if (!IsPendingLocalLineScopeKind(influence.kind) ||
        !influence.permits_local_dirty_graph || !influence.source_lines.valid ||
        influence.source_lines.first != influence.source_lines.last ||
        source.revision == 0 || source.revision == std::numeric_limits<uint64_t>::max() ||
        influence.source_revision != source.revision ||
        influence.source_lines.first >= source.line_starts.size() ||
        deletedText.size() != edit.deleted_len ||
        edit.start.value > source.raw.size() ||
        edit.deleted_len > source.raw.size() - edit.start.value ||
        std::wstring_view(source.raw).substr(edit.start.value, edit.deleted_len) != deletedText ||
        !IsPlainPendingLineText(deletedText) ||
        !IsPlainPendingLineText(edit.inserted_text)) {
        return std::nullopt;
    }
    const bool literalCodeBody = IsLiteralCodeBodyScope(influence);

    const size_t contentStart = source.line_starts[influence.source_lines.first];
    size_t contentEnd = influence.source_lines.first + 1 < source.line_starts.size()
        ? source.line_starts[influence.source_lines.first + 1]
        : source.raw.size();
    while (contentEnd > contentStart &&
           (source.raw[contentEnd - 1] == L'\r' || source.raw[contentEnd - 1] == L'\n')) {
        --contentEnd;
    }
    size_t firstVisible = contentStart;
    while (firstVisible < contentEnd &&
           (source.raw[firstVisible] == L' ' || source.raw[firstVisible] == L'\t')) {
        ++firstVisible;
    }
    if ((!literalCodeBody && firstVisible == contentEnd) ||
        firstVisible == std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    const size_t protectedPrefixEnd = influence.protected_prefix_end != 0
        ? influence.protected_prefix_end
        : literalCodeBody ? contentStart : firstVisible + 1;
    if (protectedPrefixEnd < contentStart || protectedPrefixEnd > contentEnd) {
        return std::nullopt;
    }
    if (edit.start.value < protectedPrefixEnd || edit.start.value > contentEnd ||
        edit.deleted_len > contentEnd - edit.start.value) {
        return std::nullopt;
    }

    const size_t beforeNonWhitespace = CountLineNonWhitespace(
        std::wstring_view(source.raw).substr(contentStart, contentEnd - contentStart));
    const size_t removedNonWhitespace = CountLineNonWhitespace(deletedText);
    const size_t insertedNonWhitespace = CountLineNonWhitespace(edit.inserted_text);
    if (removedNonWhitespace > beforeNonWhitespace ||
        insertedNonWhitespace > std::numeric_limits<size_t>::max() -
                                      (beforeNonWhitespace - removedNonWhitespace)) {
        return std::nullopt;
    }
    const size_t afterNonWhitespace = beforeNonWhitespace - removedNonWhitespace +
                                      insertedNonWhitespace;
    size_t afterContentEnd = 0;
    if ((!literalCodeBody && afterNonWhitespace == 0) ||
        !TryAdvanceLineEnd(contentEnd, edit.deleted_len, edit.inserted_text.size(),
                           &afterContentEnd)) {
        return std::nullopt;
    }
    NotePendingLocalLineProof proof;
    proof.scope = influence;
    proof.current_source_revision = source.revision + 1;
    proof.content_start = contentStart;
    proof.protected_prefix_end = protectedPrefixEnd;
    proof.content_end = afterContentEnd;
    proof.non_whitespace_count = afterNonWhitespace;
    return proof;
}

[[nodiscard]] std::optional<NotePendingLocalLineProof> AdvancePendingLocalLineProof(
    const NotePendingLocalLineProof& previous,
    uint64_t beforeRevision,
    const TextEdit& edit,
    std::wstring_view deletedText) {
    if (!IsPendingLocalLineScopeKind(previous.scope.kind) ||
        !previous.scope.permits_local_dirty_graph ||
        !previous.scope.source_lines.valid ||
        previous.scope.source_lines.first != previous.scope.source_lines.last ||
        previous.current_source_revision != beforeRevision ||
        beforeRevision == std::numeric_limits<uint64_t>::max() ||
        previous.content_start > previous.protected_prefix_end ||
        previous.protected_prefix_end > previous.content_end ||
        deletedText.size() != edit.deleted_len ||
        edit.start.value < previous.protected_prefix_end ||
        edit.start.value > previous.content_end ||
        edit.deleted_len > previous.content_end - edit.start.value ||
        !IsPlainPendingLineText(deletedText) ||
        !IsPlainPendingLineText(edit.inserted_text)) {
        return std::nullopt;
    }
    const bool literalCodeBody = IsLiteralCodeBodyScope(previous.scope);
    const size_t removedNonWhitespace = CountLineNonWhitespace(deletedText);
    const size_t insertedNonWhitespace = CountLineNonWhitespace(edit.inserted_text);
    if (removedNonWhitespace > previous.non_whitespace_count ||
        insertedNonWhitespace > std::numeric_limits<size_t>::max() -
                                      (previous.non_whitespace_count - removedNonWhitespace)) {
        return std::nullopt;
    }
    const size_t afterNonWhitespace = previous.non_whitespace_count - removedNonWhitespace +
                                      insertedNonWhitespace;
    size_t afterContentEnd = 0;
    if ((!literalCodeBody && afterNonWhitespace == 0) ||
        !TryAdvanceLineEnd(previous.content_end, edit.deleted_len, edit.inserted_text.size(),
                           &afterContentEnd)) {
        return std::nullopt;
    }
    NotePendingLocalLineProof next = previous;
    next.current_source_revision = beforeRevision + 1;
    next.content_end = afterContentEnd;
    return next;
}

[[nodiscard]] bool PendingCodeBodyEditPreservesFenceState(
    const NotePendingLocalLineProof& proof,
    const NoteTextCore& textCore,
    const TextEdit& edit) {
    if (!IsLiteralCodeBodyScope(proof.scope)) return true;
    if (proof.content_start > proof.content_end ||
        proof.content_end > textCore.text_length()) {
        return false;
    }
    const std::wstring currentLine = textCore.CopyRawRange(
        Utf16CodeUnitOffset{proof.content_start}, proof.content_end - proof.content_start);
    return NoteCodeBlockContentEditPreservesFenceState(
        currentLine, Utf16CodeUnitOffset{proof.content_start}, edit);
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

    const NoteDerivedSnapshotIdentity beforeIdentity{
        text_core_.note_id(), text_core_.content_revision()};
    const bool syntaxCurrent = CanReadSyntax();
    const std::wstring deletedText = text_core_.CopyRawRange(edit.start, edit.deleted_len);
    std::optional<NotePendingLocalLineProof> nextLocalLineProof;
    std::optional<NoteDirtyGraph> locallyProvenDirtyGraph;
    const NoteTextModel* beforeSource = nullptr;
    // A current syntax snapshot is already proven to share the canonical
    // sequence.  Reuse it for the pre-edit dirty graph instead of forcing the
    // lazy canonical model to materialize on every ordinary keystroke.
    std::optional<NoteInfluenceScope> influence;
    if (syntaxCurrent) {
        beforeSource = &syntax_source_;
        influence = ResolveNoteInfluenceScope(
            *beforeSource, document_, edit, &influence_index_);
        if (influence.has_value() &&
            IsPendingLocalLineScopeKind(influence->kind)) {
            nextLocalLineProof = MakePendingLocalLineProof(
                *beforeSource, *influence, edit, deletedText);
            // A locally scoped pre-edit row is not itself sufficient to
            // preserve Markdown structure.  For example, deleting an
            // ordinary prefix can expose a list marker.  Without the narrow
            // proof, leave no incremental-syntax scope for this transaction.
            if (!nextLocalLineProof.has_value()) influence.reset();
        }
    } else if (pending_local_line_proof_.has_value()) {
        if (PendingCodeBodyEditPreservesFenceState(
                *pending_local_line_proof_, text_core_, edit)) {
            nextLocalLineProof = AdvancePendingLocalLineProof(
                *pending_local_line_proof_, beforeIdentity.source_revision, edit, deletedText);
        }
        if (nextLocalLineProof.has_value()) {
            influence = nextLocalLineProof->scope;
            locallyProvenDirtyGraph = BuildNoteDirtyGraphForProvenLocalEdit(
                edit, deletedText, renderActive, *influence);
            if (!locallyProvenDirtyGraph.has_value()) {
                nextLocalLineProof.reset();
            } else {
                result.used_pending_local_dirty_proof = true;
            }
        }
    }
    if (!locallyProvenDirtyGraph.has_value()) {
        beforeSource = beforeSource ? beforeSource : &text_core_.model();
        if (!syntaxCurrent && pending_influence_scope_.has_value()) {
            influence = ResolveNoteInfluenceContinuationScope(
                *beforeSource, *pending_influence_scope_, edit);
        }
        if (influence.has_value() &&
            IsPendingLocalLineScopeKind(influence->kind)) {
            nextLocalLineProof = MakePendingLocalLineProof(
                *beforeSource, *influence, edit, deletedText);
            if (!nextLocalLineProof.has_value()) influence.reset();
        }
        result.dirty_graph = BuildNoteDirtyGraph(beforeSource->raw,
                                                 beforeSource->line_starts,
                                                 edit,
                                                 renderActive,
                                                 influence.has_value() ? &*influence : nullptr);
    } else {
        result.dirty_graph = *locallyProvenDirtyGraph;
    }
    result.text_result = text_core_.Apply(edit);
    if (result.text_result != NoteTextApplyResult::Applied) {
        return result;
    }
    if (influence.has_value() && influence->permits_local_dirty_graph) {
        pending_influence_scope_ = *influence;
    } else {
        pending_influence_scope_.reset();
    }

    // A debounce window is a transaction, not a one-edit slot.  Do not force
    // a full parse merely because normal typing or Backspace produced a
    // second canonical edit.  If the transaction cannot be represented by
    // the transitional one-edit parser path, RefreshDerived takes one
    // conservative full snapshot from the current canonical text instead.
    if (!pending_transaction_.Append(beforeIdentity, edit)) {
        force_full_refresh_ = true;
        pending_local_line_proof_.reset();
        pending_influence_scope_.reset();
    } else {
        pending_local_line_proof_ = std::move(nextLocalLineProof);
    }
    if (!pending_dirty_graph_.has_value()) {
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
    const NoteTextPieceSequence::Snapshot textBefore = text_core_.TakeSnapshot();
    const std::wstring removedText = text_core_.CopyRawRange(edit.start, edit.deleted_len);
    NoteKernelApplyResult result = Apply(edit, renderActive);
    if (result.text_result == NoteTextApplyResult::Applied) {
        // Recording failure is deliberately non-destructive: the canonical edit
        // remains applied, but it is never advertised as undoable.
        history_.Record(textBefore, text_core_.TakeSnapshot(), removedText, edit,
                        selectionBefore, selectionAfter, kind, tick);
    }
    return result;
}

std::optional<NoteKernelHistoryResult> LocalNoteKernel::Undo(bool renderActive) {
    const auto replay = history_.PeekUndo();
    if (!replay.has_value()) return std::nullopt;
    const bool currentMatches = replay->expected_snapshot.valid()
        ? text_core_.MatchesSnapshot(replay->expected_snapshot)
        : NoteHistoryReplayMatchesCurrentText(*replay, text_core_.model().raw);
    if (!currentMatches || replay->edit.deleted_len != replay->expected_deleted_text.size() ||
        !text_core_.RangeMatches(replay->edit.start, replay->expected_deleted_text)) {
        return std::nullopt;
    }
    if (!history_.PrepareUndo()) return std::nullopt;
    NoteKernelHistoryResult result;
    result.apply_result = Apply(replay->edit, renderActive);
    if (!result.applied()) return std::nullopt;
    if (!history_.CommitUndo(text_core_.TakeSnapshot())) return std::nullopt;
    return result;
}

std::optional<NoteKernelHistoryResult> LocalNoteKernel::Redo(bool renderActive) {
    const auto replay = history_.PeekRedo();
    if (!replay.has_value()) return std::nullopt;
    const bool currentMatches = replay->expected_snapshot.valid()
        ? text_core_.MatchesSnapshot(replay->expected_snapshot)
        : NoteHistoryReplayMatchesCurrentText(*replay, text_core_.model().raw);
    if (!currentMatches || replay->edit.deleted_len != replay->expected_deleted_text.size() ||
        !text_core_.RangeMatches(replay->edit.start, replay->expected_deleted_text)) {
        return std::nullopt;
    }
    if (!history_.PrepareRedo()) return std::nullopt;
    NoteKernelHistoryResult result;
    result.apply_result = Apply(replay->edit, renderActive);
    if (!result.applied()) return std::nullopt;
    if (!history_.CommitRedo(text_core_.TakeSnapshot())) return std::nullopt;
    return result;
}

void LocalNoteKernel::ClearHistory() {
    history_.Clear();
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
    if (!mustRebuild && pending_transaction_.active()) {
        const std::optional<TextEdit> equivalentEdit =
            pending_transaction_.TryBuildEquivalentEdit();
        if (equivalentEdit.has_value()) {
            std::optional<NoteDirtyGraph> incrementalDirtyGraph = consumedDirtyGraph;
            std::optional<NoteInfluenceScope> incrementalInfluence;
            if (pending_transaction_.size() == 1) {
                incrementalInfluence = pending_influence_scope_;
            } else {
                incrementalInfluence = ResolveNoteInfluenceScope(
                    syntax_source_, document_, *equivalentEdit, &influence_index_);
            }
            if (pending_transaction_.size() != 1) {
                const bool renderActive = incrementalDirtyGraph.has_value() &&
                    incrementalDirtyGraph->render_dirty;
                incrementalDirtyGraph = BuildNoteDirtyGraph(
                    syntax_source_.raw,
                    syntax_source_.line_starts,
                    *equivalentEdit,
                    renderActive,
                    incrementalInfluence.has_value() ? &*incrementalInfluence : nullptr);
            }
            if (TryApplyIncrementalSyntax(
                    *equivalentEdit,
                    incrementalInfluence.has_value() ? &*incrementalInfluence : nullptr)) {
                pending_transaction_.Clear();
                pending_local_line_proof_.reset();
                pending_influence_scope_.reset();
                pending_dirty_graph_.reset();
                force_full_refresh_ = false;
                if (deferred_full_refresh_) {
                    influence_index_ = {};
                } else {
                    RebuildInfluenceIndex();
                }

                NoteKernelRefreshResult result;
                result.kind = deferred_full_refresh_
                    ? NoteKernelRefreshKind::Deferred
                    : NoteKernelRefreshKind::Incremental;
                result.source_identity = document_.source_identity;
                result.consumed_dirty_graph = std::move(incrementalDirtyGraph);
                result.current = CanReadSyntax();
                if (result.current) RefreshSemanticIndex();
                else semantic_index_ = {};
                return result;
            }
        }
    }

    if (!mustRebuild && !pending_transaction_.active() && CanReadSyntax()) {
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
    pending_local_line_proof_.reset();
}

void LocalNoteKernel::DiscardPendingEditAndRequireFullRefresh() {
    if (pending_transaction_.active()) force_full_refresh_ = true;
    pending_transaction_.Clear();
    pending_local_line_proof_.reset();
    pending_influence_scope_.reset();
    pending_dirty_graph_.reset();
}

void LocalNoteKernel::ClearDerived() {
    ResetDerivedState(true);
}

bool LocalNoteKernel::CanReadSyntax() const {
    return text_core_.valid() && syntax_ready_ &&
           !pending_transaction_.active() && !deferred_full_refresh_ &&
           syntax_source_snapshot_.valid() &&
           NoteDocumentMatchesSourceIdentity(
               document_,
               NoteDerivedSnapshotIdentity{
                   text_core_.note_id(), text_core_.content_revision()}) &&
           NoteDocumentMatchesTextModel(document_, syntax_source_) &&
           NoteInfluenceIndexMatchesTextModel(influence_index_, syntax_source_) &&
           syntax_source_.revision == text_core_.content_revision() &&
           text_core_.MatchesSnapshot(syntax_source_snapshot_);
}

bool LocalNoteKernel::CanReadSemantic() const {
    return CanReadSyntax() &&
           SemanticIndexMatchesTextModel(
               semantic_index_, text_core_.note_id(), syntax_source_);
}

bool LocalNoteKernel::TryApplyIncrementalSyntax(const TextEdit& edit,
                                                 const NoteInfluenceScope* influence) {
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

    const size_t retainedSize = syntax_source_.raw.size() - edit.deleted_len;
    if (edit.inserted_text.size() > std::numeric_limits<size_t>::max() - retainedSize) {
        return false;
    }
    const size_t expectedSize = retainedSize + edit.inserted_text.size();
    if (!syntax_source_snapshot_.valid() || text_core_.text_length() != expectedSize ||
        !text_core_.SharesExactRange(
            syntax_source_snapshot_, Utf16CodeUnitOffset{0}, Utf16CodeUnitOffset{0},
            edit.start.value) ||
        !text_core_.RangeMatches(edit.start, edit.inserted_text)) {
        return false;
    }
    const size_t oldSuffixStart = edit.start.value + edit.deleted_len;
    const size_t newSuffixStart = edit.start.value + edit.inserted_text.size();
    const size_t suffixLen = syntax_source_.raw.size() - oldSuffixStart;
    if (suffixLen > 0 &&
        !text_core_.SharesExactRange(syntax_source_snapshot_,
                                     Utf16CodeUnitOffset{oldSuffixStart},
                                     Utf16CodeUnitOffset{newSuffixStart}, suffixLen)) {
        return false;
    }

    const size_t editOldEnd = edit.start.value + edit.deleted_len;
    const std::wstring_view removed = std::wstring_view(syntax_source_.raw).substr(
        edit.start.value, edit.deleted_len);
    const bool lineBreakOnlyEdit =
        (removed.empty() || IsOnlyNoteLineBreakText(removed)) &&
        (edit.inserted_text.empty() ||
         IsOnlyNoteLineBreakText(edit.inserted_text));
    // A line-break-only edit keeps the previous incremental transition: span
    // offsets are advanced so the canonical revision remains coherent, but
    // the result is deliberately unpublished until a full parse repairs
    // block structure.  It has no syntax scope because no local parse shape
    // has been proven, yet it is not permitted to fall through to a normal
    // incremental snapshot either.
    if (!lineBreakOnlyEdit &&
        (!influence || influence->source_revision != syntax_source_.revision ||
         !NoteInfluenceScopeAllowsIncrementalSyntaxPatch(*influence))) {
        return false;
    }
    if (!lineBreakOnlyEdit &&
        (ContainsNoteStructuralText(removed) ||
         ContainsNoteStructuralText(edit.inserted_text))) {
        return false;
    }

    std::optional<size_t> inlineMathContentIndex;
    size_t inlineMathNormalizedStart = 0;
    std::optional<size_t> insertionAtTextEndOwner;
    if (!lineBreakOnlyEdit) {
        if (influence->kind == NoteInfluenceScopeKind::InlineMathContent) {
            if (influence->source_node_index >= document_.math_spans.size()) {
                return false;
            }
            const MathSpan& math = document_.math_spans[influence->source_node_index];
            if (math.kind != MathKind::Inline || !math.diagnostic_ids.empty() ||
                math.content_span.end <= math.content_span.start ||
                math.content_span.end.value > syntax_source_.raw.size() ||
                influence->protected_prefix_end <= math.content_span.start.value ||
                influence->protected_prefix_end > math.content_span.end.value ||
                edit.start.value < influence->protected_prefix_end ||
                edit.start.value < math.content_span.start.value ||
                edit.start.value > math.content_span.end.value ||
                edit.deleted_len > math.content_span.end.value - edit.start.value) {
                return false;
            }
            const std::wstring_view rawMath = std::wstring_view(syntax_source_.raw).substr(
                math.content_span.start.value,
                math.content_span.end.value - math.content_span.start.value);
            if (math.normalized_tex != rawMath) return false;
            inlineMathNormalizedStart = edit.start.value - math.content_span.start.value;
            if (inlineMathNormalizedStart > math.normalized_tex.size() ||
                edit.deleted_len > math.normalized_tex.size() - inlineMathNormalizedStart) {
                return false;
            }
            inlineMathContentIndex = influence->source_node_index;
        }
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
        for (size_t mathIndex = 0; mathIndex < document_.math_spans.size(); ++mathIndex) {
            const MathSpan& span = document_.math_spans[mathIndex];
            if (inlineMathContentIndex.has_value() && mathIndex == *inlineMathContentIndex) {
                continue;
            }
            if (SpanIntersectsEdit(span.span, edit.start.value, editOldEnd) ||
                SpanIntersectsEdit(span.content_span, edit.start.value, editOldEnd)) {
                return false;
            }
        }
    }

    ApplyTextEdit(&syntax_source_, edit);
    // A PendingNoteEditTransaction can represent multiple canonical content
    // revisions with one equivalent source edit.  No intermediate derived
    // snapshot is published, so bind this newly coherent source model to the
    // current canonical revision rather than exposing an artificial one-step
    // revision behind TextCore.
    if (syntax_source_.revision > text_core_.content_revision()) {
        return false;
    }
    if (inlineMathContentIndex.has_value()) {
        MathSpan& math = document_.math_spans[*inlineMathContentIndex];
        math.normalized_tex.replace(
            inlineMathNormalizedStart, edit.deleted_len, edit.inserted_text);
    }
    syntax_source_.revision = text_core_.content_revision();
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
    syntax_source_snapshot_ = text_core_.TakeSnapshot();
    return syntax_source_.revision == text_core_.content_revision() &&
           text_core_.MatchesSnapshot(syntax_source_snapshot_);
}

NoteKernelRefreshResult LocalNoteKernel::RebuildAll(
    std::optional<NoteDirtyGraph> consumedDirtyGraph) {
    syntax_source_ = text_core_.model();
    document_ = content_kind_ == NoteContentKind::TeXSource
        ? ParseTeXMathDocument(syntax_source_)
        : ParseNoteDocument(syntax_source_);
    SetNoteDocumentSourceIdentity(
        &document_,
        NoteDerivedSnapshotIdentity{
            text_core_.note_id(), text_core_.content_revision()});
    syntax_ready_ = true;
    syntax_source_snapshot_ = text_core_.TakeSnapshot();
    RebuildInfluenceIndex();
    deferred_full_refresh_ = false;
    force_full_refresh_ = false;
    pending_transaction_.Clear();
    pending_local_line_proof_.reset();
    pending_influence_scope_.reset();
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
    syntax_source_snapshot_ = {};
    document_ = {};
    influence_index_ = {};
    semantic_index_ = {};
    syntax_ready_ = false;
    deferred_full_refresh_ = false;
    force_full_refresh_ = false;
    if (clearPendingEdit) {
        pending_transaction_.Clear();
        pending_local_line_proof_.reset();
        pending_influence_scope_.reset();
        pending_dirty_graph_.reset();
    }
}

void LocalNoteKernel::RebuildInfluenceIndex() {
    influence_index_ = BuildNoteInfluenceIndex(syntax_source_, document_);
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
