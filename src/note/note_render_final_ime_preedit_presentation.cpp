#include "note/note_render_final_ime_preedit_presentation.h"
#include "note/note_text_boundaries.h"

#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool ContainsLogicalLineBreak(std::wstring_view text) noexcept {
    return text.find_first_of(L"\r\n") != std::wstring_view::npos;
}

[[nodiscard]] bool IsWithin(Span outer, Span inner) noexcept {
    return outer.start <= inner.start && inner.end <= outer.end;
}

// Bounded by the preedit, never by the complete document. Missing metadata is
// legal; present metadata must describe whole editor text units consistently.
// Allocation errors are translated by the surrounding session/build boundary.
[[nodiscard]] bool ValidCompositionMetadata(
    std::wstring_view text,
    const std::vector<NoteImeCharacterAttribute>& attributes,
    const std::vector<Utf16CodeUnitOffset>& clauses) {
    if (!attributes.empty()) {
        if (attributes.size() != text.size()) return false;
        for (const auto attribute : attributes) {
            if (static_cast<uint8_t>(attribute) >
                static_cast<uint8_t>(NoteImeCharacterAttribute::FixedConverted)) return false;
        }
    }
    if (!clauses.empty()) {
        if (clauses.front().value != 0 || clauses.back().value != text.size()) return false;
        for (size_t index = 1; index < clauses.size(); ++index) {
            if (clauses[index] <= clauses[index - 1]) return false;
        }
    }
    if (attributes.empty() && clauses.empty()) return true;
    const auto units = BuildTextUnits(text);
    size_t clause = clauses.empty() ? 0 : 1;
    for (const auto& unit : units) {
        if (!attributes.empty()) {
            for (size_t index = unit.span.start.value + 1; index < unit.span.end.value; ++index) {
                if (attributes[index] != attributes[unit.span.start.value]) return false;
            }
        }
        if (clause < clauses.size()) {
            if (clauses[clause] < unit.span.end) return false;
            if (clauses[clause] == unit.span.end) ++clause;
        }
    }
    return clauses.empty() || clause == clauses.size();
}

[[nodiscard]] std::vector<NoteImePreeditSegment> BuildCompositionSegments(
    const NoteRenderFinalImePreeditInput& input) {
    std::vector<NoteImePreeditSegment> result;
    size_t first = 0;
    size_t clause = 1;
    while (first < input.composition_text.size()) {
        const auto attribute = input.composition_attributes.empty()
            ? NoteImeCharacterAttribute::Input : input.composition_attributes[first];
        const size_t clauseEnd = input.composition_clauses.empty()
            ? input.composition_text.size() : input.composition_clauses[clause].value;
        size_t end = first + 1;
        while (end < clauseEnd && (input.composition_attributes.empty() ||
                                   input.composition_attributes[end] == attribute)) ++end;
        const size_t origin = input.canonical_replacement_span.start.value;
        result.push_back({{{origin + first}, {origin + end}}, attribute});
        first = end;
        if (first == clauseEnd) ++clause;
    }
    return result;
}

[[nodiscard]] bool CopyExact(const NoteTextCore& text_core,
                             Span span,
                             std::wstring* out) noexcept {
    if (!out || span.end < span.start || span.end.value > text_core.text_length()) return false;
    try {
        std::wstring copied = text_core.CopyRawRange(
            span.start, span.end.value - span.start.value);
        if (copied.size() != span.end.value - span.start.value) return false;
        *out = std::move(copied);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool NoteRenderFinalImePreeditSession::Begin(
    NoteDerivedSnapshotIdentity source, Span replacement) noexcept {
    Reset();
    if (!source.note_id.valid() || replacement.end < replacement.start) return false;
    source_identity_ = source;
    canonical_replacement_ = replacement;
    active_ = true;
    awaiting_first_payload_ = true;
    return true;
}

NoteRenderFinalImePreeditUpdateResult NoteRenderFinalImePreeditSession::Update(
    std::wstring_view composition, Utf16CodeUnitOffset caret,
    const std::vector<NoteImeCharacterAttribute>& attributes,
    const std::vector<Utf16CodeUnitOffset>& clauses) noexcept {
    if (!active_) return NoteRenderFinalImePreeditUpdateResult::InactiveSession;
    if (ContainsLogicalLineBreak(composition) || caret.value > composition.size()) {
        Suspend();
        return NoteRenderFinalImePreeditUpdateResult::InvalidComposition;
    }
    try {
        if (!ValidCompositionMetadata(composition, attributes, clauses)) {
            Suspend();
            return NoteRenderFinalImePreeditUpdateResult::InvalidComposition;
        }
        std::wstring candidate(composition);
        auto candidateAttributes = attributes;
        auto candidateClauses = clauses;
        composition_text_.swap(candidate);
        composition_attributes_.swap(candidateAttributes);
        composition_clauses_.swap(candidateClauses);
        composition_caret_ = caret;
        has_payload_ = true;
        awaiting_first_payload_ = false;
        return composition.empty() ? NoteRenderFinalImePreeditUpdateResult::UpdatedEmptyComposition
                                   : NoteRenderFinalImePreeditUpdateResult::Updated;
    } catch (const std::bad_alloc&) {
        Suspend();
        return NoteRenderFinalImePreeditUpdateResult::AllocationFailure;
    } catch (const std::exception&) {
        Suspend();
        return NoteRenderFinalImePreeditUpdateResult::InvalidComposition;
    }
}

void NoteRenderFinalImePreeditSession::Suspend() noexcept {
    composition_text_.clear();
    composition_attributes_.clear();
    composition_clauses_.clear();
    composition_caret_ = {};
    has_payload_ = false;
    awaiting_first_payload_ = false;
}

void NoteRenderFinalImePreeditSession::Reset() noexcept {
    Suspend();
    source_identity_ = {};
    canonical_replacement_ = {};
    active_ = false;
}

NoteImeCommittedContinuationResult PrepareNoteImeCommittedContinuation(
    const NoteTextCore& textCore,
    const NoteRenderFinalImePreeditSession& session,
    std::wstring_view result,
    std::wstring_view preedit,
    std::wstring_view observed,
    NoteImeCommittedContinuation* out) noexcept {
    if (!out) return NoteImeCommittedContinuationResult::InvalidOutput;
    if (!textCore.valid() || !session.active()) return NoteImeCommittedContinuationResult::InvalidSession;
    if (session.source_identity() != NoteDerivedSnapshotIdentity{textCore.note_id(), textCore.content_revision()}) {
        return NoteImeCommittedContinuationResult::StaleSource;
    }
    const Span replacement = session.canonical_replacement();
    if (replacement.end < replacement.start || replacement.end.value > textCore.text_length() ||
        result.find(L'\r') != std::wstring_view::npos || preedit.find(L'\r') != std::wstring_view::npos) {
        return NoteImeCommittedContinuationResult::InvalidRange;
    }
    const size_t removed = replacement.end.value - replacement.start.value;
    const size_t unchanged = textCore.text_length() - removed;
    if (result.size() > std::numeric_limits<size_t>::max() - unchanged ||
        preedit.size() > std::numeric_limits<size_t>::max() - unchanged - result.size()) {
        return NoteImeCommittedContinuationResult::InvalidRange;
    }
    const size_t resultEnd = replacement.start.value + result.size();
    const size_t suffixStart = resultEnd + preedit.size();
    if (observed.size() != unchanged + result.size() + preedit.size() ||
        observed.substr(replacement.start.value, result.size()) != result ||
        observed.substr(resultEnd, preedit.size()) != preedit ||
        !textCore.RangeMatches({0}, observed.substr(0, replacement.start.value)) ||
        !textCore.RangeMatches(replacement.end, observed.substr(suffixStart))) {
        return NoteImeCommittedContinuationResult::ObservedTextMismatch;
    }
    try {
        NoteImeCommittedContinuation candidate;
        candidate.source_identity = session.source_identity();
        candidate.committed_edit = {replacement.start, removed, std::wstring(result)};
        candidate.selection_before = replacement;
        candidate.committed_caret = {resultEnd};
        *out = std::move(candidate);
        return NoteImeCommittedContinuationResult::Prepared;
    } catch (const std::exception&) {
        return NoteImeCommittedContinuationResult::AllocationFailure;
    }
}

NoteRenderFinalImePreeditPresentationBuildResult
NoteRenderFinalImePreeditPresentation::Build(
    const NoteTextCore& textCore,
    const NoteRenderFinalImePreeditInput& input,
    NoteRenderFinalImePreeditPresentation* out) noexcept {
    if (!out) return NoteRenderFinalImePreeditPresentationBuildResult::InvalidOutput;
    if (!textCore.valid() || textCore.logical_line_count() == 0) {
        return NoteRenderFinalImePreeditPresentationBuildResult::InvalidTextCore;
    }
    const Span replacement = input.canonical_replacement_span;
    if (replacement.end < replacement.start || replacement.end.value > textCore.text_length()) {
        return NoteRenderFinalImePreeditPresentationBuildResult::InvalidReplacementRange;
    }
    if (ContainsLogicalLineBreak(input.composition_text)) {
        return NoteRenderFinalImePreeditPresentationBuildResult::ContainsLineBreak;
    }

    const auto firstLine = NoteSourceLineMap::FindByOffset(
        textCore.source_line_map(), replacement.start);
    const auto lastLine = NoteSourceLineMap::FindByOffset(
        textCore.source_line_map(), replacement.end);
    if (!firstLine.has_value() || !lastLine.has_value()) {
        return NoteRenderFinalImePreeditPresentationBuildResult::InvalidReplacementRange;
    }
    if (firstLine->line_index != lastLine->line_index ||
        replacement.start < firstLine->start || replacement.end > firstLine->content_end) {
        return NoteRenderFinalImePreeditPresentationBuildResult::MultipleLogicalLines;
    }

    try {
        if (!ValidCompositionMetadata(input.composition_text, input.composition_attributes,
                                      input.composition_clauses)) {
            return NoteRenderFinalImePreeditPresentationBuildResult::InvalidCompositionMetadata;
        }
        NoteSourceEditCoordinateMap coordinateMap;
        const TextEdit preeditEdit{
            replacement.start, replacement.end.value - replacement.start.value,
            input.composition_text};
        if (NoteSourceEditCoordinateMap::Build(
                textCore.text_length(), preeditEdit, &coordinateMap) !=
            NoteSourceEditCoordinateMapBuildResult::Built) {
            return NoteRenderFinalImePreeditPresentationBuildResult::CoordinateMapBuildFailed;
        }
        if (!IsWithin(coordinateMap.new_replacement_span(), input.editor_selection)) {
            return NoteRenderFinalImePreeditPresentationBuildResult::InvalidEditorSelection;
        }

        Utf16CodeUnitOffset editorLineStart;
        Utf16CodeUnitOffset editorLineEnd;
        if (!coordinateMap.MapUnchangedBoundary(
                firstLine->start, NoteSourceEditBoundarySide::BeforeReplacement,
                &editorLineStart) ||
            !coordinateMap.MapUnchangedBoundary(
                firstLine->content_end, NoteSourceEditBoundarySide::AfterReplacement,
                &editorLineEnd) ||
            editorLineEnd < editorLineStart) {
            return NoteRenderFinalImePreeditPresentationBuildResult::CoordinateMapBuildFailed;
        }

        std::wstring prefix;
        std::wstring suffix;
        if (!CopyExact(textCore, {firstLine->start, replacement.start}, &prefix) ||
            !CopyExact(textCore, {replacement.end, firstLine->content_end}, &suffix)) {
            return NoteRenderFinalImePreeditPresentationBuildResult::SourceReadFailed;
        }
        if (prefix.size() > std::numeric_limits<size_t>::max() - input.composition_text.size() ||
            prefix.size() + input.composition_text.size() >
                std::numeric_limits<size_t>::max() - suffix.size()) {
            return NoteRenderFinalImePreeditPresentationBuildResult::AllocationFailure;
        }
        std::wstring temporaryRawLine;
        temporaryRawLine.reserve(prefix.size() + input.composition_text.size() + suffix.size());
        temporaryRawLine.append(prefix);
        temporaryRawLine.append(input.composition_text);
        temporaryRawLine.append(suffix);
        if (editorLineEnd.value - editorLineStart.value != temporaryRawLine.size()) {
            return NoteRenderFinalImePreeditPresentationBuildResult::InconsistentTemporaryLine;
        }

        NoteRenderFinalImePreeditPresentation candidate;
        candidate.source_identity_ = {textCore.note_id(), textCore.content_revision()};
        candidate.coordinate_map_ = std::move(coordinateMap);
        candidate.line_index_ = firstLine->line_index;
        candidate.editor_line_span_ = {editorLineStart, editorLineEnd};
        candidate.editor_selection_ = input.editor_selection;
        candidate.temporary_raw_line_ = std::move(temporaryRawLine);
        candidate.segments_ = BuildCompositionSegments(input);
        candidate.valid_ = true;
        *out = std::move(candidate);
        return NoteRenderFinalImePreeditPresentationBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalImePreeditPresentationBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalImePreeditPresentationBuildResult::SourceReadFailed;
    }
}

bool NoteRenderFinalImePreeditPresentation::Matches(const NoteTextCore& textCore) const noexcept {
    return valid_ && textCore.valid() && source_identity_.note_id == textCore.note_id() &&
        source_identity_.source_revision == textCore.content_revision();
}

bool NoteRenderFinalImePreeditPresentation::MapEditorUnchangedBoundary(
    Utf16CodeUnitOffset editorOffset,
    NoteSourceEditBoundarySide side,
    Utf16CodeUnitOffset* outCanonical) const noexcept {
    return valid_ && coordinate_map_.MapUnchangedNewBoundary(editorOffset, side, outCanonical);
}

bool NoteRenderFinalImePreeditPresentation::MapCanonicalUnchangedBoundary(
    Utf16CodeUnitOffset canonicalOffset,
    NoteSourceEditBoundarySide side,
    Utf16CodeUnitOffset* outEditor) const noexcept {
    return valid_ && coordinate_map_.MapUnchangedBoundary(canonicalOffset, side, outEditor);
}

} // namespace note
