#include "note/note_syntax_snapshot.h"

#include "note/note_parser.h"
#include "note/note_syntax_lexical.h"
#include "note/note_source_coordinate_transform.h"

#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool SourceCoordinatesMatchLineMap(
    const NoteTextModel& source,
    const NoteSourceLineMap::Snapshot& lineMap) noexcept {
    if (!lineMap.valid() || lineMap.text_length() != source.raw.size() ||
        lineMap.line_count() != source.line_starts.size() ||
        source.line_starts.empty()) {
        return false;
    }

    for (size_t line = 0; line < source.line_starts.size(); ++line) {
        const auto location = NoteSourceLineMap::LineAt(lineMap, {line});
        if (!location.has_value() || location->start.value != source.line_starts[line] ||
            location->content_end < location->start ||
            location->next_start < location->content_end ||
            location->next_start.value > source.raw.size()) {
            return false;
        }
        if (line + 1 < source.line_starts.size() &&
            location->next_start.value != source.line_starts[line + 1]) {
            return false;
        }
    }

    const auto finalLine = NoteSourceLineMap::LineAt(
        lineMap, {source.line_starts.size() - 1});
    return finalLine.has_value() && finalLine->next_start.value == source.raw.size();
}

[[nodiscard]] bool ContainsLocalSyntaxSensitiveText(std::wstring_view text) noexcept {
    return text.find_first_of(L"\r\n`*_~[]()<>#!$\\:|>") != std::wstring_view::npos;
}

// A second, narrower proof for sticky/opaque parser contexts: an interior
// edit of a marker-free plain row cannot open/close a tag, change indentation,
// or expose a block prefix. The text-leaf and unchanged-root proofs are still
// mandatory. Do not clear opaque parser flags or relax general suffix reuse.
[[nodiscard]] bool IsStableInteriorPlainRowEdit(std::wstring_view line, size_t relativeStart) noexcept {
    if (ContainsLocalSyntaxSensitiveText(line)) return false;
    const size_t first = line.find_first_not_of(L" \t");
    if (first == std::wstring_view::npos || relativeStart <= first) return false;
    const wchar_t head = line[first];
    return (head >= L'A' && head <= L'Z') || (head >= L'a' && head <= L'z') || head >= 0x80;
}

// A plain prefix/suffix deletion can turn a completed inline tag pair into
// standalone display math without touching a delimiter. Compare only this
// row's lexical edges; the existing span proof already forbids editing tags
// or their TeX body. Attribute syntax is shared with complete parsing.
[[nodiscard]] bool HasStandaloneNamedMathEdges(std::wstring_view line) noexcept {
    const size_t first = line.find_first_not_of(L" \t");
    if (first == std::wstring_view::npos) return false;
    const size_t last = line.find_last_not_of(L" \t");
    line = line.substr(first, last - first + 1);
    NoteMathOpenTag tag;
    if (!TryParseNoteMathOpenTag(line, {0}, &tag) || !tag.complete || tag.self_closing || !tag.valid_attributes) return false;
    // Explicit inline display does not change when surrounding text is removed.
    if (tag.display == NoteMathTagDisplay::Inline) return false;
    return (line.size() >= 3 && NoteMathCloseTagLength(line.substr(line.size() - 3)) == 3) ||
        (line.size() >= 7 && NoteMathCloseTagLength(line.substr(line.size() - 7)) == 7);
}

} // namespace

NoteSyntaxSnapshotBuildResult NoteSyntaxSnapshot::BuildFromTextCore(
    const NoteTextCore& textCore,
    NoteContentKind contentKind,
    std::shared_ptr<const NoteSyntaxSnapshot>* out) noexcept {
    return BuildFromTextCoreWithCheckpoint(textCore, contentKind, out, nullptr);
}

NoteSyntaxSnapshotBuildResult NoteSyntaxSnapshot::BuildFromTextCoreWithCheckpoint(
    const NoteTextCore& textCore,
    NoteContentKind contentKind,
    std::shared_ptr<const NoteSyntaxSnapshot>* out,
    NoteParserCheckpointIndex* outCheckpoint) noexcept {
    if (!out) return NoteSyntaxSnapshotBuildResult::InvalidOutput;
    if (!textCore.valid()) return NoteSyntaxSnapshotBuildResult::InvalidSource;

    try {
        const NoteTextPieceSequence::Snapshot sourceRoot = textCore.TakeSnapshot();
        const NoteSourceLineMap::Snapshot sourceLineMap = textCore.source_line_map();
        const NoteTextModel source = textCore.model();
        const NoteDerivedSnapshotIdentity sourceIdentity{
            textCore.note_id(), textCore.content_revision()};
        if (!sourceRoot.valid() || !sourceLineMap.valid() ||
            source.raw.size() != sourceRoot.text_length() ||
            source.revision != sourceIdentity.source_revision ||
            !SourceCoordinatesMatchLineMap(source, sourceLineMap) ||
            !textCore.MatchesSnapshot(sourceRoot)) {
            return NoteSyntaxSnapshotBuildResult::InconsistentSourceCoordinates;
        }

        NoteDocument parsed;
        if (contentKind != NoteContentKind::PlainText) {
            parsed = contentKind == NoteContentKind::TeXSource
                ? ParseTeXMathDocument(source)
                : ParseNoteDocument(source);
        } else {
            parsed.meta = source.meta;
        }
        SetNoteDocumentSourceIdentity(&parsed, sourceIdentity);
        if (!NoteDocumentMatchesTextModel(parsed, source)) {
            return NoteSyntaxSnapshotBuildResult::InconsistentSourceCoordinates;
        }
        NoteParserCheckpointIndex checkpoint;
        if (outCheckpoint) {
            checkpoint = BuildNoteParserCheckpointIndex(
                source, parsed, sourceIdentity, sourceRoot, sourceLineMap);
            if (!checkpoint.valid || checkpoint.source_identity != sourceIdentity ||
                !NoteTextPieceSequence::SameSnapshotIdentity(
                    checkpoint.canonical_source_root, sourceRoot) ||
                !NoteSourceLineMap::SameSnapshotIdentity(
                    checkpoint.source_line_map, sourceLineMap)) {
                return NoteSyntaxSnapshotBuildResult::InconsistentSourceCoordinates;
            }
        }
        std::shared_ptr<const NoteSyntaxDocument> persistentDocument;
        if (NoteSyntaxDocument::Build(parsed, sourceLineMap, &persistentDocument) !=
                NoteSyntaxDocumentBuildResult::Built ||
            !persistentDocument || !persistentDocument->valid()) {
            return NoteSyntaxSnapshotBuildResult::InconsistentSourceCoordinates;
        }

        std::shared_ptr<NoteSyntaxSnapshot> candidate(new NoteSyntaxSnapshot());
        candidate->content_kind_ = contentKind;
        candidate->source_identity_ = sourceIdentity;
        candidate->source_root_ = sourceRoot;
        candidate->source_line_map_ = sourceLineMap;
        candidate->syntax_document_ = std::move(persistentDocument);
        candidate->has_structured_syntax_ = contentKind != NoteContentKind::PlainText;

        // The source root can only be published if it still denotes the same
        // canonical document at the end of construction. This is normally a
        // UI-thread no-op, but makes the publication contract explicit for a
        // future worker-built candidate.
        candidate->valid_ = true;
        if (!candidate->MatchesTextCore(textCore)) {
            return NoteSyntaxSnapshotBuildResult::InvalidSource;
        }
        if (outCheckpoint) {
            *outCheckpoint = std::move(checkpoint);
        }
        *out = std::move(candidate);
        return NoteSyntaxSnapshotBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteSyntaxSnapshotBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteSyntaxSnapshotBuildResult::AllocationFailure;
    }
}

NoteSyntaxSnapshotLocalPatchResult NoteSyntaxSnapshot::BuildLocalPlainTextPatch(
    std::shared_ptr<const NoteSyntaxSnapshot> previous,
    const NoteParserCheckpointIndex& previousCheckpoint,
    NoteParserLayoutKey layoutKey,
    const NoteTextCore& currentTextCore,
    const TextEdit& edit,
    std::shared_ptr<const NoteSyntaxSnapshot>* outSnapshot,
    NoteParserCheckpointIndex* outCheckpoint,
    NoteSyntaxSnapshotLocalPatchWork* outWork) noexcept {
    if (!outSnapshot || !outCheckpoint) return NoteSyntaxSnapshotLocalPatchResult::InvalidOutput;
    if (!previous || !previous->valid_ || !previous->has_structured_syntax_ ||
        previous->content_kind_ != NoteContentKind::Markdown || !layoutKey.valid() ||
        !previous->syntax_document_ || !previousCheckpoint.valid ||
        previousCheckpoint.source_identity != previous->source_identity_ ||
        !NoteTextPieceSequence::SameSnapshotIdentity(
            previousCheckpoint.canonical_source_root, previous->source_root_) ||
        !NoteSourceLineMap::SameSnapshotIdentity(
            previousCheckpoint.source_line_map, previous->source_line_map_)) {
        return NoteSyntaxSnapshotLocalPatchResult::InvalidPreviousSnapshot;
    }
    if (!currentTextCore.valid() ||
        (edit.deleted_len == 0 && edit.inserted_text.empty()) ||
        edit.start.value > previous->source_root_.text_length() ||
        edit.deleted_len > previous->source_root_.text_length() - edit.start.value ||
        edit.start.value > std::numeric_limits<size_t>::max() - edit.deleted_len) {
        return NoteSyntaxSnapshotLocalPatchResult::InvalidEdit;
    }
    NoteSourceEditCoordinateMap coordinateMap;
    if (NoteSourceEditCoordinateMap::Build(
            previous->source_root_.text_length(), edit, &coordinateMap) !=
        NoteSourceEditCoordinateMapBuildResult::Built) {
        return NoteSyntaxSnapshotLocalPatchResult::InvalidEdit;
    }
    const std::wstring removed = previous->CopySourceRange(edit.start, edit.deleted_len);
    if (removed.size() != edit.deleted_len || ContainsLocalSyntaxSensitiveText(removed) ||
        ContainsLocalSyntaxSensitiveText(edit.inserted_text) ||
        !previous->syntax_document_->PermitsPlainTextLeafEdit(
            previous->source_line_map_, edit.start, edit.deleted_len)) {
        return NoteSyntaxSnapshotLocalPatchResult::RequiresFullSnapshot;
    }

    try {
        const uint64_t materializationsBefore =
            currentTextCore.model_materialization_count();
        NoteSyntaxSnapshotLocalPatchWork work;
        const NoteTextPieceSequence::Snapshot currentRoot = currentTextCore.TakeSnapshot();
        const NoteSourceLineMap::Snapshot currentLineMap = currentTextCore.source_line_map();
        const NoteDerivedSnapshotIdentity currentIdentity{
            currentTextCore.note_id(), currentTextCore.content_revision()};
        if (!currentRoot.valid() || !currentLineMap.valid() ||
            !currentTextCore.MatchesSnapshot(currentRoot) ||
            currentIdentity.note_id != previous->source_identity_.note_id ||
            previous->source_identity_.source_revision ==
                std::numeric_limits<uint64_t>::max() ||
            currentIdentity.source_revision != previous->source_identity_.source_revision + 1 ||
            !currentTextCore.RangeMatches(
                coordinateMap.new_replacement_span().start, edit.inserted_text) ||
            !currentTextCore.SharesExactRange(
                previous->source_root_, {0}, {0},
                coordinateMap.old_replacement_span().start.value)) {
            return NoteSyntaxSnapshotLocalPatchResult::InvalidCurrentSource;
        }
        const Span oldSuffix{
            coordinateMap.old_replacement_span().end,
            {previous->source_root_.text_length()}};
        if (oldSuffix.end.value > oldSuffix.start.value) {
            Span newSuffix;
            if (!coordinateMap.MapUnchangedSpan(oldSuffix, &newSuffix) ||
                !currentTextCore.SharesExactRange(
                    previous->source_root_, oldSuffix.start, newSuffix.start,
                    oldSuffix.end.value - oldSuffix.start.value)) {
                return NoteSyntaxSnapshotLocalPatchResult::InvalidCurrentSource;
            }
        }
        const auto changedLocation = NoteSourceLineMap::FindByOffset(
            previous->source_line_map_, edit.start);
        if (!changedLocation.has_value()) return NoteSyntaxSnapshotLocalPatchResult::InvalidEdit;
        // Deleting ordinary prefix characters can expose an existing :::
        // fence or make tag math standalone. Prove this edited row before reuse;
        // this copies one row, not the document or its unchanged suffix.
        const std::wstring beforeLine = previous->CopySourceRange(
            changedLocation->start,
            changedLocation->content_end - changedLocation->start);
        const auto afterLocation = NoteSourceLineMap::LineAt(
            currentLineMap, changedLocation->line_index);
        if (!afterLocation.has_value()) return NoteSyntaxSnapshotLocalPatchResult::InvalidEdit;
        const bool needsRowProof = beforeLine.find(L":::") != std::wstring::npos ||
            beforeLine.find(L'<') != std::wstring::npos;
        const bool interiorPlainLeafProof = IsStableInteriorPlainRowEdit(
            beforeLine, edit.start.value - changedLocation->start.value);
        const std::wstring afterLine = !needsRowProof
            ? std::wstring{} : currentTextCore.CopyRawRange(
                afterLocation->start, afterLocation->content_end - afterLocation->start);
        NoteContainerFenceRun fence;
        if (TryParseNoteContainerFenceRun(beforeLine, &fence) ||
            TryParseNoteContainerFenceRun(afterLine, &fence) ||
            (needsRowProof && HasStandaloneNamedMathEdges(beforeLine) != HasStandaloneNamedMathEdges(afterLine))) {
            return NoteSyntaxSnapshotLocalPatchResult::RequiresFullSnapshot;
        }
        std::shared_ptr<NoteSyntaxSnapshot> candidate(new NoteSyntaxSnapshot(*previous));
        candidate->source_identity_ = currentIdentity;
        candidate->source_root_ = currentRoot;
        candidate->source_line_map_ = currentLineMap;
        NoteSyntaxDocumentLocalPatchWork documentPatchWork;
        if (NoteSyntaxDocument::BuildLocalPlainTextPatch(
                previous->syntax_document_, previous->source_line_map_, currentLineMap, edit,
                changedLocation->line_index, &candidate->syntax_document_,
                &documentPatchWork) != NoteSyntaxDocumentLocalPatchResult::Built ||
            !candidate->syntax_document_) {
            return NoteSyntaxSnapshotLocalPatchResult::InconsistentCandidate;
        }
        NoteParserCheckpointIndex rebuiltCheckpoint;
        NoteParserCheckpointLocalPatchWork checkpointPatchWork;
        if (BuildNoteParserCheckpointLocalPlainTextPatch(
                previousCheckpoint, currentIdentity, currentRoot, currentLineMap,
                changedLocation->line_index, &rebuiltCheckpoint, &checkpointPatchWork) !=
            NoteParserCheckpointLocalPatchResult::Built) {
            return NoteSyntaxSnapshotLocalPatchResult::InconsistentCandidate;
        }
        const size_t nextLine = changedLocation->line_index.value + 1;
        if (nextLine < rebuiltCheckpoint.line_count()) {
            if (!NoteParserCheckpointCanReuseSuffix(
                    previousCheckpoint, {nextLine}, rebuiltCheckpoint, {nextLine},
                    currentRoot, layoutKey, layoutKey)) {
                NoteParserLineCheckpoint oldNext, newNext;
                if (!interiorPlainLeafProof ||
                    !previousCheckpoint.ResolveLine({nextLine}, &oldNext) ||
                    !rebuiltCheckpoint.ResolveLine({nextLine}, &newNext) ||
                    oldNext.entry_state != newNext.entry_state || oldNext.exit_state != newNext.exit_state ||
                    oldNext.entry_state.has_bounded_lookbehind_line || oldNext.exit_state.has_bounded_lookbehind_line) {
                    return NoteSyntaxSnapshotLocalPatchResult::FixedPointNotProven;
                }
            }
        }
        NoteParserLineCheckpoint finalCheckpoint;
        if (rebuiltCheckpoint.line_count() != 0 &&
            (!rebuiltCheckpoint.ResolveLine(
                 {rebuiltCheckpoint.line_count() - 1}, &finalCheckpoint) ||
             (!finalCheckpoint.exit_state.supports_suffix_reuse() && !interiorPlainLeafProof))) {
            return NoteSyntaxSnapshotLocalPatchResult::FixedPointNotProven;
        }
        candidate->valid_ = true;
        if (!candidate->MatchesTextCore(currentTextCore)) {
            return NoteSyntaxSnapshotLocalPatchResult::InvalidCurrentSource;
        }
        const uint64_t materializationsAfter =
            currentTextCore.model_materialization_count();
        if (materializationsAfter != materializationsBefore) {
            return NoteSyntaxSnapshotLocalPatchResult::UnexpectedTextCoreMaterialization;
        }
        work.text_core_materializations = materializationsAfter - materializationsBefore;
        work.syntax_document_replacement_payloads = documentPatchWork.replacement_node_payloads;
        work.syntax_document_tail_payload_rewrites = documentPatchWork.tail_node_payload_rewrites;
        work.syntax_document_interval_index_rewrites =
            documentPatchWork.interval_index_payload_rewrites;
        work.checkpoint_lines_built = checkpointPatchWork.replacement_line_payloads;
        *outSnapshot = std::move(candidate);
        *outCheckpoint = std::move(rebuiltCheckpoint);
        if (outWork) *outWork = work;
        return NoteSyntaxSnapshotLocalPatchResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteSyntaxSnapshotLocalPatchResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteSyntaxSnapshotLocalPatchResult::AllocationFailure;
    }
}

std::wstring NoteSyntaxSnapshot::CopySourceRange(Utf16CodeUnitOffset start,
                                                 size_t length) const {
    return NoteTextPieceSequence::CopyRange(source_root_, start, length);
}

bool NoteSyntaxSnapshot::CopyDocumentForCompleteBuild(NoteDocument* out) const noexcept {
    return out && valid_ && syntax_document_ &&
           syntax_document_->CopyDocumentForCompleteBuild(
               source_line_map_, source_identity_, out);
}

bool NoteSyntaxSnapshot::MatchesTextCore(const NoteTextCore& textCore) const noexcept {
    return valid_ && textCore.valid() &&
           source_identity_.note_id == textCore.note_id() &&
           source_identity_.source_revision == textCore.content_revision() &&
           source_root_.valid() && source_line_map_.valid() &&
           syntax_document_ && syntax_document_->valid() &&
           source_root_.text_length() == textCore.text_length() &&
           source_line_map_.text_length() == textCore.text_length() &&
           source_line_map_.line_count() == textCore.logical_line_count() &&
           textCore.MatchesSnapshot(source_root_);
}

} // namespace note
