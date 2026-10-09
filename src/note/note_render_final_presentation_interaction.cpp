#include "note/note_render_final_presentation_interaction.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace note {
namespace {

struct ResolvedPresentationLine {
    LineIndex line_index{};
    NoteRenderFinalSurfaceKind surface = NoteRenderFinalSurfaceKind::StructuredPaint;
    NoteRenderLineLayoutLocation layout{};
    NoteRenderSourceLinePlan structured_source{};
    NoteRenderLinePlacement placement{};
    NoteRenderFinalRawLineSurface raw{};
    bool raw_surface = false;
};

[[nodiscard]] bool IsValidPresentation(
    const NoteRenderFinalPresentationSnapshot& presentation) noexcept {
    const std::shared_ptr<const NoteRenderFinalPublication>& structural =
        presentation.structural_publication();
    return presentation.valid() && structural && structural->valid() && structural->syntax() &&
           structural->source_plan() && structural->placement() &&
           presentation.line_layouts().valid() &&
           presentation.line_layouts().line_count() == structural->source_plan()->line_count();
}

[[nodiscard]] bool RangesIntersect(Span lhs, Span rhs) noexcept {
    return lhs.start < rhs.end && rhs.start < lhs.end;
}

[[nodiscard]] const NoteRenderRunPlacementFragment* FragmentForBoundary(
    const NoteRenderRunPlacement& run,
    const NoteRenderPlacementBoundary& boundary) noexcept {
    return boundary.fragment_index < run.fragments.size()
        ? &run.fragments[boundary.fragment_index] : nullptr;
}

[[nodiscard]] bool FragmentContainsY(const NoteRenderRunPlacementFragment& fragment,
                                     uint64_t relativeY) noexcept {
    return relativeY >= fragment.top_offset_px &&
        relativeY - fragment.top_offset_px < fragment.height_px;
}

[[nodiscard]] const NoteRenderPlacementBoundary* BoundaryAtOrBefore(
    const NoteRenderRunPlacement& run,
    Utf16CodeUnitOffset sourceOffset,
    std::optional<uint32_t> fragmentIndex = std::nullopt,
    NoteRenderFinalCaretAffinity affinity =
        NoteRenderFinalCaretAffinity::AfterVisualWrap) noexcept {
    if (sourceOffset < run.source_span.start || sourceOffset > run.source_span.end ||
        run.boundaries.empty()) {
        return nullptr;
    }
    const NoteRenderPlacementBoundary* selected = nullptr;
    for (const NoteRenderPlacementBoundary& boundary : run.boundaries) {
        if (fragmentIndex.has_value() && boundary.fragment_index != *fragmentIndex) continue;
        if (boundary.source_offset > sourceOffset) break;
        if (!selected || boundary.source_offset > selected->source_offset ||
            (boundary.source_offset == selected->source_offset &&
             affinity == NoteRenderFinalCaretAffinity::AfterVisualWrap)) {
            selected = &boundary;
        }
    }
    return selected;
}

[[nodiscard]] NoteRenderFinalPresentationInteractionResult ResolveLine(
    const NoteRenderFinalPresentationSnapshot& presentation,
    LineIndex lineIndex,
    ResolvedPresentationLine* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation)) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    const auto layout = NoteRenderLineLayoutMap::LineAt(presentation.line_layouts(), lineIndex);
    const auto surface = presentation.SurfaceAt(lineIndex);
    if (!layout.has_value() || !surface.has_value()) {
        return NoteRenderFinalPresentationInteractionResult::OutsidePublishedRange;
    }
    if (layout->bottom_px <= layout->top_px) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
    try {
        ResolvedPresentationLine candidate;
        candidate.line_index = lineIndex;
        candidate.surface = *surface;
        candidate.layout = *layout;
        if (*surface == NoteRenderFinalSurfaceKind::RawPaint) {
            if (!presentation.ResolveRawLine(lineIndex, &candidate.raw) ||
                candidate.raw.placement.runs.size() != 1) {
                return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
            }
            candidate.placement = candidate.raw.placement;
            candidate.raw_surface = true;
        } else {
            const std::shared_ptr<const NoteRenderFinalPublication>& structural =
                presentation.structural_publication();
            if (!structural->source_plan()->ResolveLine(lineIndex, &candidate.structured_source) ||
                !structural->placement()->ResolveLine(lineIndex, &candidate.placement) ||
                candidate.structured_source.runs.size() != candidate.placement.runs.size()) {
                return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
            }
        }
        *out = std::move(candidate);
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPresentationInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
}

[[nodiscard]] bool BuildRunSelectionRects(
    const NoteRenderRunPlacement& placement,
    Span selection,
    uint64_t lineTop,
    std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out || !RangesIntersect(placement.source_span, selection) ||
        placement.boundaries.empty() || placement.fragments.empty()) {
        return true;
    }
    for (size_t index = 0; index < placement.fragments.size(); ++index) {
        const NoteRenderRunPlacementFragment& fragment = placement.fragments[index];
        if (!RangesIntersect(fragment.source_span, selection)) continue;
        if (fragment.height_px == 0 ||
            lineTop > std::numeric_limits<uint64_t>::max() - fragment.top_offset_px) {
            return false;
        }
        const Utf16CodeUnitOffset firstOffset{
            std::max(fragment.source_span.start.value, selection.start.value)};
        const Utf16CodeUnitOffset lastOffset{
            std::min(fragment.source_span.end.value, selection.end.value)};
        const NoteRenderPlacementBoundary* first = BoundaryAtOrBefore(
            placement, firstOffset, static_cast<uint32_t>(index),
            NoteRenderFinalCaretAffinity::BeforeVisualWrap);
        const NoteRenderPlacementBoundary* last = BoundaryAtOrBefore(
            placement, lastOffset, static_cast<uint32_t>(index),
            NoteRenderFinalCaretAffinity::AfterVisualWrap);
        if (!first || !last) return false;
        int left = std::min(first->x_px, last->x_px);
        int right = std::max(first->x_px, last->x_px);
        if (left == right) {
            if (right < std::numeric_limits<int>::max()) ++right;
            else if (left > std::numeric_limits<int>::min()) --left;
            else return false;
        }
        const uint64_t top = lineTop + fragment.top_offset_px;
        if (top > std::numeric_limits<uint64_t>::max() - fragment.height_px) return false;
        out->push_back({left, top, right, top + fragment.height_px});
    }
    return true;
}

[[nodiscard]] bool SourceRunForIndex(const ResolvedPresentationLine& line,
                                     size_t runIndex,
                                     const NoteRenderSourceRun** out) noexcept {
    if (!out || line.raw_surface || runIndex >= line.structured_source.runs.size()) return false;
    *out = &line.structured_source.runs[runIndex];
    return true;
}

struct NavigationBand {
    uint64_t top = 0;
    uint64_t bottom = 0;
};

[[nodiscard]] bool IsHiddenRun(const ResolvedPresentationLine& line, size_t index) noexcept {
    return !line.raw_surface &&
        line.structured_source.runs[index].kind == NoteRenderSourceRunKind::HiddenSyntax;
}

// Glyph rectangles on one visual row can have different tops (inline math,
// font sizes, vertical alignment). Their overlapping bands are one navigation
// row, not several key stops. Touching wrap rows remain distinct. Hidden syntax
// cannot bridge visible rows; it only supplies a syntax-only row's fallback.
// Work is confined to the current/destination logical line, never the document.
[[nodiscard]] std::vector<NavigationBand> BuildNavigationBands(const ResolvedPresentationLine& line) {
    const bool hasVisible = [&]() {
        for (size_t i = 0; i < line.placement.runs.size(); ++i)
            if (!IsHiddenRun(line, i) && !line.placement.runs[i].fragments.empty()) return true;
        return false;
    }();
    std::vector<NavigationBand> bands;
    for (size_t i = 0; i < line.placement.runs.size(); ++i) {
        if (hasVisible && IsHiddenRun(line, i)) continue;
        for (const auto& fragment : line.placement.runs[i].fragments) {
            if (fragment.height_px != 0) bands.push_back({fragment.top_offset_px,
                static_cast<uint64_t>(fragment.top_offset_px) + fragment.height_px});
        }
    }
    std::sort(bands.begin(), bands.end(), [](const auto& a, const auto& b) {
        return a.top < b.top || (a.top == b.top && a.bottom < b.bottom);
    });
    size_t write = 0;
    for (const auto band : bands) {
        if (write && band.top < bands[write - 1].bottom)
            bands[write - 1].bottom = std::max(bands[write - 1].bottom, band.bottom);
        else bands[write++] = band;
    }
    bands.resize(write);
    return bands;
}

[[nodiscard]] size_t NearestNavigationBand(const std::vector<NavigationBand>& bands,
                                           uint64_t y) noexcept {
    size_t best = 0;
    uint64_t distance = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < bands.size(); ++i) {
        const auto& band = bands[i];
        const uint64_t next = y < band.top ? band.top - y
            : y >= band.bottom ? y - band.bottom + 1 : 0;
        if (next < distance) { best = i; distance = next; }
    }
    return best;
}

[[nodiscard]] bool ResolveNavigationBandBoundary(const ResolvedPresentationLine& line,
                                                  NavigationBand band, int x,
                                                  Utf16CodeUnitOffset* out) noexcept {
    const NoteRenderPlacementBoundary* best = nullptr;
    int64_t distance = std::numeric_limits<int64_t>::max();
    bool bestHidden = true;
    for (size_t i = 0; i < line.placement.runs.size(); ++i) {
        const auto& run = line.placement.runs[i];
        const bool hidden = IsHiddenRun(line, i);
        for (const auto& boundary : run.boundaries) {
            const auto* fragment = FragmentForBoundary(run, boundary);
            if (!fragment) return false;
            const uint64_t bottom = static_cast<uint64_t>(fragment->top_offset_px) + fragment->height_px;
            if (fragment->top_offset_px >= band.bottom || bottom <= band.top) continue;
            const int64_t next = std::llabs(static_cast<int64_t>(boundary.x_px) - x);
            if (!best || (bestHidden && !hidden) ||
                (bestHidden == hidden && next < distance)) {
                best = &boundary; bestHidden = hidden; distance = next;
            }
        }
    }
    if (!best) return false;
    *out = best->source_offset;
    return true;
}

[[nodiscard]] NoteRenderFinalPresentationInteractionResult ResolveCaretInLine(
    const ResolvedPresentationLine& line,
    Utf16CodeUnitOffset placementOffset,
    NoteRenderFinalCaretAffinity affinity,
    NoteRenderFinalCaretGeometry* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!line.raw_surface && line.placement.runs.empty() &&
        line.structured_source.content_span.start == line.structured_source.content_span.end &&
        placementOffset == line.structured_source.content_span.start) {
        *out = {line.line_index, placementOffset, 0, line.layout.top_px, line.layout.bottom_px};
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    }
    const NoteRenderPlacementBoundary* selected = nullptr;
    const NoteRenderRunPlacementFragment* selectedFragment = nullptr;
    for (const NoteRenderRunPlacement& run : line.placement.runs) {
        const NoteRenderPlacementBoundary* candidate = BoundaryAtOrBefore(
            run, placementOffset, std::nullopt, affinity);
        if (!candidate) continue;
        const NoteRenderRunPlacementFragment* fragment = FragmentForBoundary(run, *candidate);
        if (!fragment) return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
        if (!selected || candidate->source_offset > selected->source_offset ||
            (candidate->source_offset == selected->source_offset &&
             affinity == NoteRenderFinalCaretAffinity::AfterVisualWrap &&
             fragment->top_offset_px >= selectedFragment->top_offset_px)) {
            selected = candidate;
            selectedFragment = fragment;
        }
    }
    if (!selected || !selectedFragment ||
        line.layout.top_px > std::numeric_limits<uint64_t>::max() -
            selectedFragment->top_offset_px) {
        return NoteRenderFinalPresentationInteractionResult::OutsideContent;
    }
    const uint64_t top = line.layout.top_px + selectedFragment->top_offset_px;
    if (top > std::numeric_limits<uint64_t>::max() - selectedFragment->height_px) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
    *out = {line.line_index, selected->source_offset, selected->x_px,
            top, top + selectedFragment->height_px};
    return NoteRenderFinalPresentationInteractionResult::Resolved;
}

[[nodiscard]] NoteRenderFinalPresentationInteractionResult ResolveEditorCaretLine(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset editorOffset,
    ResolvedPresentationLine* outLine,
    Utf16CodeUnitOffset* outPlacementOffset) noexcept {
    if (!outLine || !outPlacementOffset) {
        return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    }
    const NoteRenderFinalImePreeditPresentation* preedit = presentation.ime_preedit();
    Utf16CodeUnitOffset canonicalOffset = editorOffset;
    if (preedit) {
        const Span editorLine = preedit->editor_line_span();
        if (editorOffset >= editorLine.start && editorOffset <= editorLine.end) {
            const auto resolved = ResolveLine(presentation, preedit->line_index(), outLine);
            if (resolved != NoteRenderFinalPresentationInteractionResult::Resolved) return resolved;
            if (!outLine->raw_surface) {
                return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
            }
            *outPlacementOffset = editorOffset;
            return NoteRenderFinalPresentationInteractionResult::Resolved;
        }
        if (!preedit->MapEditorUnchangedBoundary(
                editorOffset, NoteSourceEditBoundarySide::AfterReplacement, &canonicalOffset)) {
            return NoteRenderFinalPresentationInteractionResult::OutsideContent;
        }
    }
    const auto sourceLocation = NoteSourceLineMap::FindByOffset(
        presentation.structural_publication()->syntax()->source_line_map(), canonicalOffset);
    if (!sourceLocation.has_value()) return NoteRenderFinalPresentationInteractionResult::OutsideContent;
    const auto resolved = ResolveLine(presentation, sourceLocation->line_index, outLine);
    if (resolved != NoteRenderFinalPresentationInteractionResult::Resolved) return resolved;
    *outPlacementOffset = outLine->raw_surface ? editorOffset : canonicalOffset;
    return NoteRenderFinalPresentationInteractionResult::Resolved;
}

[[nodiscard]] bool AppendStructuredSelectionSegment(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Span canonicalSelection,
    std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out || canonicalSelection.end <= canonicalSelection.start) return false;
    const auto first = NoteSourceLineMap::FindByOffset(
        presentation.structural_publication()->syntax()->source_line_map(), canonicalSelection.start);
    const auto last = NoteSourceLineMap::FindByOffset(
        presentation.structural_publication()->syntax()->source_line_map(),
        {canonicalSelection.end.value - 1});
    if (!first.has_value() || !last.has_value() || last->line_index < first->line_index) return false;
    for (size_t lineIndex = first->line_index.value;
         lineIndex <= last->line_index.value; ++lineIndex) {
        const auto surface = presentation.SurfaceAt({lineIndex});
        if (!surface.has_value()) return false;
        if (*surface == NoteRenderFinalSurfaceKind::StructuredPaint) {
            ResolvedPresentationLine line;
            const auto resolved = ResolveLine(presentation, {lineIndex}, &line);
            if (resolved != NoteRenderFinalPresentationInteractionResult::Resolved ||
                line.raw_surface) {
                return false;
            }
            for (const NoteRenderRunPlacement& run : line.placement.runs) {
                if (!BuildRunSelectionRects(run, canonicalSelection, line.layout.top_px, out)) {
                    return false;
                }
            }
        }
        if (lineIndex == std::numeric_limits<size_t>::max()) return false;
    }
    return true;
}

} // namespace

NoteRenderFinalPresentationInteractionResult HitTestNoteRenderFinalPresentation(
    const NoteRenderFinalPresentationSnapshot& presentation,
    int contentX,
    uint64_t contentY,
    NoteRenderFinalHit* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation)) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    const auto layout = NoteRenderLineLayoutMap::LineContainingY(
        presentation.line_layouts(), contentY);
    if (!layout.has_value()) return NoteRenderFinalPresentationInteractionResult::OutsideContent;
    ResolvedPresentationLine line;
    const auto lineResult = ResolveLine(presentation, layout->line_index, &line);
    if (lineResult != NoteRenderFinalPresentationInteractionResult::Resolved) return lineResult;
    // A blank structured row has a line box, but deliberately no source run.
    if (!line.raw_surface && line.placement.runs.empty() &&
        line.structured_source.content_span.start == line.structured_source.content_span.end) {
        *out = {};
        out->line_index = line.line_index;
        out->source_offset = line.structured_source.content_span.start;
        out->source_span = line.structured_source.content_span;
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    }
    const uint64_t relativeY = contentY - line.layout.top_px;
    if (!line.raw_surface) {
        if (const auto suffix = ResolveNoteRenderTrailingSyntaxBlankHit(
                line.structured_source, line.placement, contentX, relativeY)) {
            *out = {};
            out->line_index = line.line_index;
            out->source_offset = line.structured_source.content_span.end;
            out->run_index = *suffix;
            out->source_span = line.structured_source.runs[*suffix].source_span;
            return NoteRenderFinalPresentationInteractionResult::Resolved;
        }
    }
    size_t bestRun = static_cast<size_t>(-1);
    const NoteRenderPlacementBoundary* bestBoundary = nullptr;
    uint64_t bestVerticalDistance = std::numeric_limits<uint64_t>::max();
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    bool bestHidden = true;
    for (size_t runIndex = 0; runIndex < line.placement.runs.size(); ++runIndex) {
        const NoteRenderRunPlacement& run = line.placement.runs[runIndex];
        const bool hidden = !line.raw_surface &&
            line.structured_source.runs[runIndex].kind == NoteRenderSourceRunKind::HiddenSyntax;
        for (const NoteRenderPlacementBoundary& boundary : run.boundaries) {
            const NoteRenderRunPlacementFragment* fragment = FragmentForBoundary(run, boundary);
            if (!fragment) return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
            uint64_t verticalDistance = 0;
            if (!FragmentContainsY(*fragment, relativeY)) {
                if (!line.raw_surface) continue;
                // A raw allocation may keep the taller structured row/group
                // as bottom padding. Click it using the nearest raw visual
                // row, never native-editor coordinates or a following row.
                const uint64_t bottom = static_cast<uint64_t>(fragment->top_offset_px) +
                    fragment->height_px;
                verticalDistance = relativeY < fragment->top_offset_px
                    ? fragment->top_offset_px - relativeY : relativeY - bottom + 1;
            }
            const int64_t distance = std::llabs(
                static_cast<int64_t>(boundary.x_px) - static_cast<int64_t>(contentX));
            // Invisible syntax can share an X coordinate with visible text.
            // It is a fallback for syntax-only rows, never the click owner
            // when this visual row contains a displayed source run.
            if (!bestBoundary || verticalDistance < bestVerticalDistance ||
                (verticalDistance == bestVerticalDistance &&
                 ((bestHidden && !hidden) ||
                  (bestHidden == hidden && distance < bestDistance)))) {
                bestRun = runIndex;
                bestBoundary = &boundary;
                bestDistance = distance;
                bestVerticalDistance = verticalDistance;
                bestHidden = hidden;
            }
        }
    }
    if (!bestBoundary || bestRun >= line.placement.runs.size()) {
        return NoteRenderFinalPresentationInteractionResult::OutsideContent;
    }
    try {
        NoteRenderFinalHit candidate;
        candidate.line_index = line.line_index;
        candidate.source_offset = bestBoundary->source_offset;
        candidate.run_index = bestRun;
        candidate.source_span = line.placement.runs[bestRun].source_span;
        if (!line.raw_surface) {
            const NoteRenderSourceRun* sourceRun = nullptr;
            if (!SourceRunForIndex(line, bestRun, &sourceRun) ||
                sourceRun->source_span.start != candidate.source_span.start ||
                sourceRun->source_span.end != candidate.source_span.end) {
                return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
            }
            candidate.is_link = sourceRun->kind == NoteRenderSourceRunKind::LinkText &&
                !sourceRun->link_target.empty();
            if (candidate.is_link) candidate.link_target = sourceRun->link_target;
            for (const NoteRenderSourceStyleAttribute& style : sourceRun->styles) {
                if (style.kind == StyleKind::LinkId && !style.value.empty()) {
                    candidate.is_link = true;
                    candidate.is_legacy_link_id = true;
                    candidate.link_target = style.value;
                    break;
                }
            }
        }
        *out = std::move(candidate);
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPresentationInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
}

NoteRenderFinalPresentationInteractionResult ResolveNoteRenderFinalPresentationCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset sourceOffset,
    NoteRenderFinalCaretGeometry* out) noexcept {
    return ResolveNoteRenderFinalPresentationCaret(
        presentation, sourceOffset, NoteRenderFinalCaretAffinity::AfterVisualWrap, out);
}

NoteRenderFinalPresentationInteractionResult ResolveNoteRenderFinalPresentationCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset sourceOffset,
    NoteRenderFinalCaretAffinity affinity,
    NoteRenderFinalCaretGeometry* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation)) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    const auto sourceLocation = NoteSourceLineMap::FindByOffset(
        presentation.structural_publication()->syntax()->source_line_map(), sourceOffset);
    if (!sourceLocation.has_value()) return NoteRenderFinalPresentationInteractionResult::OutsideContent;
    ResolvedPresentationLine line;
    const auto lineResult = ResolveLine(presentation, sourceLocation->line_index, &line);
    if (lineResult != NoteRenderFinalPresentationInteractionResult::Resolved) return lineResult;
    return ResolveCaretInLine(line, sourceOffset, affinity, out);
}

NoteRenderFinalPresentationInteractionResult ResolveNoteRenderFinalPresentationSelection(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Span selection,
    std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation)) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    try {
        std::vector<NoteRenderFinalRect> candidate;
        if (selection.end <= selection.start) {
            *out = std::move(candidate);
            return NoteRenderFinalPresentationInteractionResult::Resolved;
        }
        const auto first = NoteSourceLineMap::FindByOffset(
            presentation.structural_publication()->syntax()->source_line_map(), selection.start);
        const auto last = NoteSourceLineMap::FindByOffset(
            presentation.structural_publication()->syntax()->source_line_map(),
            {selection.end.value - 1});
        if (!first.has_value() || !last.has_value() || last->line_index < first->line_index) {
            return NoteRenderFinalPresentationInteractionResult::OutsideContent;
        }
        for (size_t lineIndex = first->line_index.value;
             lineIndex <= last->line_index.value; ++lineIndex) {
            ResolvedPresentationLine line;
            const auto lineResult = ResolveLine(presentation, {lineIndex}, &line);
            if (lineResult != NoteRenderFinalPresentationInteractionResult::Resolved) return lineResult;
            for (const NoteRenderRunPlacement& run : line.placement.runs) {
                if (!BuildRunSelectionRects(run, selection, line.layout.top_px, &candidate)) {
                    return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
                }
            }
            if (lineIndex == std::numeric_limits<size_t>::max()) {
                return NoteRenderFinalPresentationInteractionResult::OutsideContent;
            }
        }
        *out = std::move(candidate);
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPresentationInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
}

NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationHorizontalExtent(
    const NoteRenderFinalPresentationSnapshot& presentation,
    uint64_t* outExtent) noexcept {
    if (!outExtent) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation) || !presentation.structural_publication()->layout()) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    const uint64_t maxExtent = presentation.line_layouts().max_inline_extent_px();
    const uint64_t padding = presentation.structural_publication()->layout()->layout_key()
        .horizontal_padding_px;
    if (maxExtent > std::numeric_limits<uint64_t>::max() - padding) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
    *outExtent = maxExtent + padding;
    return NoteRenderFinalPresentationInteractionResult::Resolved;
}

NoteRenderFinalPresentationInteractionResult
MoveNoteRenderFinalPresentationVertical(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset caretOffset,
    int preferredX,
    NoteRenderFinalVerticalMove move,
    uint64_t pageHeight,
    Utf16CodeUnitOffset* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation) || presentation.ime_preedit()) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    NoteRenderFinalCaretGeometry caret;
    const auto resolved = ResolveNoteRenderFinalPresentationCaret(presentation, caretOffset, &caret);
    if (resolved != NoteRenderFinalPresentationInteractionResult::Resolved) return resolved;
    const uint64_t extent = presentation.line_layouts().total_height_px();
    if (extent == 0 || caret.top_px >= extent) return NoteRenderFinalPresentationInteractionResult::OutsideContent;
    const bool upward = move == NoteRenderFinalVerticalMove::Up ||
                        move == NoteRenderFinalVerticalMove::PageUp;
    const bool page = move == NoteRenderFinalVerticalMove::PageUp ||
                      move == NoteRenderFinalVerticalMove::PageDown;
    try {
        ResolvedPresentationLine line;
        auto lineResult = ResolveLine(presentation, caret.line_index, &line);
        if (lineResult != NoteRenderFinalPresentationInteractionResult::Resolved) return lineResult;
        auto bands = BuildNavigationBands(line);
        const size_t currentBand = NearestNavigationBand(bands, caret.top_px - line.layout.top_px);
        size_t targetBand = currentBand;
        uint64_t targetY = 0;
        bool crossLine = page;
        if (page) {
            const uint64_t distance = std::max<uint64_t>(1, pageHeight);
            targetY = upward ? caret.top_px - std::min(caret.top_px, distance)
                : caret.top_px + std::min(extent - 1 - caret.top_px, distance);
        } else if (!bands.empty() && upward && currentBand > 0) {
            targetBand = currentBand - 1;
        } else if (!bands.empty() && !upward && currentBand + 1 < bands.size()) {
            targetBand = currentBand + 1;
        } else if (upward && line.layout.top_px == 0) {
            *out = {0};
            return NoteRenderFinalPresentationInteractionResult::Resolved;
        } else if (!upward && line.layout.bottom_px >= extent) {
            *out = {presentation.structural_publication()->syntax()->source_root().text_length()};
            return NoteRenderFinalPresentationInteractionResult::Resolved;
        } else {
            crossLine = true;
            targetY = upward ? line.layout.top_px - 1 : line.layout.bottom_px;
        }
        if (crossLine) {
            const auto target = NoteRenderLineLayoutMap::LineContainingY(presentation.line_layouts(), targetY);
            if (!target) return NoteRenderFinalPresentationInteractionResult::OutsideContent;
            lineResult = ResolveLine(presentation, target->line_index, &line);
            if (lineResult != NoteRenderFinalPresentationInteractionResult::Resolved) return lineResult;
            bands = BuildNavigationBands(line);
            targetBand = page ? NearestNavigationBand(bands, targetY - line.layout.top_px)
                : upward && !bands.empty() ? bands.size() - 1 : 0;
        }
        if (bands.empty()) {
            if (!line.raw_surface && line.placement.runs.empty() &&
                line.structured_source.content_span.start == line.structured_source.content_span.end) {
                *out = line.structured_source.content_span.start;
                return NoteRenderFinalPresentationInteractionResult::Resolved;
            }
            return NoteRenderFinalPresentationInteractionResult::OutsideContent;
        }
        Utf16CodeUnitOffset destination;
        if (!ResolveNavigationBandBoundary(line, bands[targetBand], preferredX, &destination))
            return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
        *out = destination;
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPresentationInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
}

NoteRenderFinalPresentationInteractionResult
HitTestNoteRenderFinalPresentationEditor(
    const NoteRenderFinalPresentationSnapshot& presentation,
    int contentX,
    uint64_t contentY,
    NoteRenderFinalHit* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    NoteRenderFinalHit candidate;
    const NoteRenderFinalPresentationInteractionResult result =
        HitTestNoteRenderFinalPresentation(presentation, contentX, contentY, &candidate);
    if (result != NoteRenderFinalPresentationInteractionResult::Resolved) return result;
    const NoteRenderFinalImePreeditPresentation* preedit = presentation.ime_preedit();
    if (!preedit) {
        *out = std::move(candidate);
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    }
    const auto surface = presentation.SurfaceAt(candidate.line_index);
    if (!surface.has_value()) return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    if (*surface == NoteRenderFinalSurfaceKind::RawPaint) {
        *out = std::move(candidate);
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    }
    const Span replaced = preedit->canonical_replacement_span();
    const auto mapBoundary = [&](Utf16CodeUnitOffset canonical,
                                 Utf16CodeUnitOffset* editor) noexcept {
        if (canonical > replaced.start && canonical < replaced.end) return false;
        return preedit->MapCanonicalUnchangedBoundary(
            canonical,
            canonical <= replaced.start ? NoteSourceEditBoundarySide::BeforeReplacement
                                        : NoteSourceEditBoundarySide::AfterReplacement,
            editor);
    };
    NoteRenderFinalHit translated = candidate;
    if (!mapBoundary(candidate.source_offset, &translated.source_offset) ||
        !mapBoundary(candidate.source_span.start, &translated.source_span.start) ||
        !mapBoundary(candidate.source_span.end, &translated.source_span.end)) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
    *out = std::move(translated);
    return NoteRenderFinalPresentationInteractionResult::Resolved;
}

NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationEditorCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset editorOffset,
    NoteRenderFinalCaretGeometry* out) noexcept {
    return ResolveNoteRenderFinalPresentationEditorCaret(
        presentation, editorOffset, NoteRenderFinalCaretAffinity::AfterVisualWrap, out);
}

NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationEditorCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset editorOffset,
    NoteRenderFinalCaretAffinity affinity,
    NoteRenderFinalCaretGeometry* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation)) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    ResolvedPresentationLine line;
    Utf16CodeUnitOffset placementOffset{};
    const auto lineResult = ResolveEditorCaretLine(
        presentation, editorOffset, &line, &placementOffset);
    if (lineResult != NoteRenderFinalPresentationInteractionResult::Resolved) return lineResult;
    return ResolveCaretInLine(line, placementOffset, affinity, out);
}

NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationEditorSelection(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Span editorSelection,
    std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation)) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    const NoteRenderFinalImePreeditPresentation* preedit = presentation.ime_preedit();
    if (!preedit) {
        return ResolveNoteRenderFinalPresentationSelection(presentation, editorSelection, out);
    }
    const NoteSourceEditCoordinateMap& coordinateMap = preedit->coordinate_map();
    if (editorSelection.end < editorSelection.start ||
        editorSelection.end.value > coordinateMap.new_source_length()) {
        return NoteRenderFinalPresentationInteractionResult::OutsideContent;
    }
    try {
        std::vector<NoteRenderFinalRect> candidate;
        if (editorSelection.end == editorSelection.start) {
            *out = std::move(candidate);
            return NoteRenderFinalPresentationInteractionResult::Resolved;
        }

        // Raw rows use temporary editor coordinates directly. This includes
        // the composition row and any atomic group expanded around it.
        for (const NoteRenderFinalSurfaceRange& range : presentation.surface_ranges()) {
            if (range.surface != NoteRenderFinalSurfaceKind::RawPaint) continue;
            for (size_t lineIndex = range.lines.first.value;
                 lineIndex < range.lines.last_exclusive.value; ++lineIndex) {
                NoteRenderFinalRawLineSurface raw;
                const auto layout = NoteRenderLineLayoutMap::LineAt(
                    presentation.line_layouts(), {lineIndex});
                if (!layout.has_value() || !presentation.ResolveRawLine({lineIndex}, &raw) ||
                    raw.placement.runs.size() != 1) {
                    return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
                }
                if (!BuildRunSelectionRects(raw.placement.runs.front(), editorSelection,
                                            layout->top_px, &candidate)) {
                    return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
                }
            }
        }

        const Span replacement = coordinateMap.new_replacement_span();
        const auto appendUnchanged = [&](Span editorSegment) {
            if (editorSegment.end <= editorSegment.start) return true;
            Span canonicalSegment;
            return coordinateMap.MapUnchangedNewSpan(editorSegment, &canonicalSegment) &&
                AppendStructuredSelectionSegment(presentation, canonicalSegment, &candidate);
        };
        const size_t prefixEnd = std::min(editorSelection.end.value, replacement.start.value);
        if (editorSelection.start.value < prefixEnd &&
            !appendUnchanged({editorSelection.start, {prefixEnd}})) {
            return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
        }
        const size_t suffixStart = std::max(editorSelection.start.value, replacement.end.value);
        if (suffixStart < editorSelection.end.value &&
            !appendUnchanged({{suffixStart}, editorSelection.end})) {
            return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
        }
        *out = std::move(candidate);
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPresentationInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
}

NoteRenderFinalPresentationInteractionResult ResolveNoteRenderFinalImePreeditDecorations(
    const NoteRenderFinalPresentationSnapshot& presentation,
    std::vector<NoteImePreeditDecoration>* out) noexcept {
    if (!out) return NoteRenderFinalPresentationInteractionResult::InvalidOutput;
    if (!IsValidPresentation(presentation)) return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    try {
        std::vector<NoteImePreeditDecoration> candidate;
        const auto* preedit = presentation.ime_preedit();
        if (!preedit || preedit->segments().empty()) {
            *out = std::move(candidate);
            return NoteRenderFinalPresentationInteractionResult::Resolved;
        }
        NoteRenderFinalRawLineSurface raw;
        const auto layout = NoteRenderLineLayoutMap::LineAt(presentation.line_layouts(), preedit->line_index());
        if (!layout || !presentation.ResolveRawLine(preedit->line_index(), &raw) || raw.placement.runs.size() != 1) {
            return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
        }
        const auto& run = raw.placement.runs.front();
        // Raw boundaries are validated in source/fragment order at publication.
        // A wrap boundary may occur twice; choose the requested fragment, not
        // an adjacent visual row. Searches are logarithmic, not one line scan
        // per character attribute or clause.
        const auto boundaryAt = [&](Utf16CodeUnitOffset offset, size_t fragment) -> const NoteRenderPlacementBoundary* {
            auto it = std::upper_bound(run.boundaries.begin(), run.boundaries.end(), offset,
                [](Utf16CodeUnitOffset value, const NoteRenderPlacementBoundary& boundary) {
                    return value < boundary.source_offset;
                });
            while (it != run.boundaries.begin()) {
                --it;
                if (it->fragment_index == fragment) return &*it;
                if (it->fragment_index < fragment) break;
            }
            return nullptr;
        };
        // Merge ordered attribute segments with ordered visual fragments.
        // Resolve the row only once. No clause performs a document-wide or
        // repeated raw-surface copy, even for a long preedit with many clauses.
        size_t segmentIndex = 0;
        size_t fragmentIndex = 0;
        size_t lastDecoratedSegment = std::numeric_limits<size_t>::max();
        while (segmentIndex < preedit->segments().size() && fragmentIndex < run.fragments.size()) {
            const auto& segment = preedit->segments()[segmentIndex];
            const auto& fragment = run.fragments[fragmentIndex];
            if (fragment.source_span.end <= segment.editor_span.start) { ++fragmentIndex; continue; }
            if (segment.editor_span.end <= fragment.source_span.start) { ++segmentIndex; continue; }
            const auto* first = boundaryAt({std::max(segment.editor_span.start.value, fragment.source_span.start.value)}, fragmentIndex);
            const auto* last = boundaryAt({std::min(segment.editor_span.end.value, fragment.source_span.end.value)}, fragmentIndex);
            if (!first || !last || layout->top_px > std::numeric_limits<uint64_t>::max() - fragment.top_offset_px) {
                return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
            }
            const uint64_t top = layout->top_px + fragment.top_offset_px;
            if (fragment.height_px == 0 || top > std::numeric_limits<uint64_t>::max() - fragment.height_px) {
                return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
            }
            int left = std::min(first->x_px, last->x_px);
            int right = std::max(first->x_px, last->x_px);
            if (left == right) {
                if (right == std::numeric_limits<int>::max()) --left;
                else ++right;
            }
            if (!candidate.empty() && lastDecoratedSegment == segmentIndex &&
                candidate.back().rect.right_px == left && candidate.back().rect.top_px == top &&
                candidate.back().rect.bottom_px == top + fragment.height_px) {
                // Word fitting can produce adjacent fragments on the same
                // visual row. Do not introduce a fake clause gap between them.
                candidate.back().rect.right_px = right;
            } else {
                candidate.push_back({{left, top, right, top + fragment.height_px}, segment.attribute});
            }
            lastDecoratedSegment = segmentIndex;
            if (segment.editor_span.end <= fragment.source_span.end) ++segmentIndex;
            if (fragment.source_span.end <= segment.editor_span.end) ++fragmentIndex;
        }
        if (segmentIndex != preedit->segments().size()) return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
        *out = std::move(candidate);
        return NoteRenderFinalPresentationInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPresentationInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPresentationInteractionResult::InconsistentPresentation;
    }
}

} // namespace note
