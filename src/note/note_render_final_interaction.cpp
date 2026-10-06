#include "note/note_render_final_interaction.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool IsStructuredPublishedLine(const NoteRenderFinalPublication& publication,
                                              LineIndex line_index,
                                              NoteRenderFinalInteractionResult* out_result) noexcept {
    if (!publication.valid() || !publication.layout() || !publication.placement() ||
        !publication.source_plan() || !publication.owner_plan() ||
        publication.owner_plan()->frame().frame_kind != NotePresentationFrameKind::DrawCommitted) {
        if (out_result) *out_result = NoteRenderFinalInteractionResult::InvalidPublication;
        return false;
    }
    const std::vector<NotePresentationOwnerRange>& ranges = publication.owner_plan()->owner_ranges();
    for (const NotePresentationOwnerRange& range : ranges) {
        if (line_index < range.lines.first || line_index >= range.lines.last_exclusive) continue;
        if (range.owner == NotePresentationLineOwner::NativeEditor) {
            if (out_result) *out_result = NoteRenderFinalInteractionResult::NativeEditorOwner;
            return false;
        }
        if (range.owner == NotePresentationLineOwner::CommittedPlacement) {
            if (out_result) *out_result = NoteRenderFinalInteractionResult::Resolved;
            return true;
        }
        if (out_result) *out_result = NoteRenderFinalInteractionResult::InconsistentPublication;
        return false;
    }
    if (out_result) *out_result = NoteRenderFinalInteractionResult::OutsidePublishedRange;
    return false;
}

[[nodiscard]] bool RunContainsCaret(const NoteRenderRunPlacement& run,
                                    Utf16CodeUnitOffset source_offset) noexcept {
    return run.source_span.start <= source_offset && source_offset <= run.source_span.end &&
           !run.boundaries.empty();
}

[[nodiscard]] const NoteRenderRunPlacementFragment* FragmentForBoundary(
    const NoteRenderRunPlacement& run,
    const NoteRenderPlacementBoundary& boundary) noexcept {
    return boundary.fragment_index < run.fragments.size()
        ? &run.fragments[boundary.fragment_index] : nullptr;
}

[[nodiscard]] bool FragmentContainsY(const NoteRenderRunPlacementFragment& fragment,
                                     uint64_t relative_y_px) noexcept {
    return relative_y_px >= fragment.top_offset_px &&
           relative_y_px - fragment.top_offset_px < fragment.height_px;
}

[[nodiscard]] const NoteRenderPlacementBoundary* BoundaryAtOrBefore(
    const NoteRenderRunPlacement& run,
    Utf16CodeUnitOffset source_offset,
    std::optional<uint32_t> fragment_index = std::nullopt,
    NoteRenderFinalCaretAffinity affinity =
        NoteRenderFinalCaretAffinity::AfterVisualWrap) noexcept {
    if (!RunContainsCaret(run, source_offset)) return nullptr;
    const NoteRenderPlacementBoundary* selected = nullptr;
    for (const NoteRenderPlacementBoundary& boundary : run.boundaries) {
        if (fragment_index.has_value() && boundary.fragment_index != *fragment_index) continue;
        if (boundary.source_offset > source_offset) break;
        if (!selected || boundary.source_offset > selected->source_offset ||
            (boundary.source_offset == selected->source_offset &&
             affinity == NoteRenderFinalCaretAffinity::AfterVisualWrap)) {
            selected = &boundary;
        }
    }
    return selected;
}

[[nodiscard]] bool RangesIntersect(Span lhs, Span rhs) noexcept {
    return lhs.start < rhs.end && rhs.start < lhs.end;
}

[[nodiscard]] NoteRenderFinalInteractionResult ResolveLineUnchecked(
    const NoteRenderFinalPublication& publication,
    LineIndex line_index,
    NoteRenderFinalResolvedLine* out) noexcept {
    if (!out) return NoteRenderFinalInteractionResult::InvalidOutput;
    NoteRenderFinalInteractionResult owner_result;
    if (!IsStructuredPublishedLine(publication, line_index, &owner_result)) return owner_result;
    const std::optional<NoteRenderLineLayoutLocation> layout =
        NoteRenderLineLayoutMap::LineAt(publication.layout()->line_layouts(), line_index);
    if (!layout.has_value() || layout->line_index != line_index ||
        layout->bottom_px <= layout->top_px) {
        return NoteRenderFinalInteractionResult::InconsistentPublication;
    }
    try {
        NoteRenderFinalResolvedLine candidate;
        candidate.line_index = line_index;
        candidate.layout = *layout;
        if (!publication.source_plan()->ResolveLine(line_index, &candidate.source_line) ||
            !publication.placement()->ResolveLine(line_index, &candidate.placement_line) ||
            candidate.source_line.runs.size() != candidate.placement_line.runs.size()) {
            return NoteRenderFinalInteractionResult::InconsistentPublication;
        }
        *out = std::move(candidate);
        return NoteRenderFinalInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalInteractionResult::InconsistentPublication;
    }
}

[[nodiscard]] bool BuildRunSelectionRects(const NoteRenderRunPlacement& placement,
                                          Span selection,
                                          uint64_t line_top_px,
                                          std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out || !RangesIntersect(placement.source_span, selection) ||
        placement.boundaries.empty() || placement.fragments.empty()) {
        return false;
    }
    for (size_t index = 0; index < placement.fragments.size(); ++index) {
        const NoteRenderRunPlacementFragment& fragment = placement.fragments[index];
        if (!RangesIntersect(fragment.source_span, selection) || fragment.height_px == 0 ||
            line_top_px > std::numeric_limits<uint64_t>::max() - fragment.top_offset_px) {
            continue;
        }
        const Utf16CodeUnitOffset first_offset{
            std::max(fragment.source_span.start.value, selection.start.value)};
        const Utf16CodeUnitOffset last_offset{
            std::min(fragment.source_span.end.value, selection.end.value)};
        const NoteRenderPlacementBoundary* first = BoundaryAtOrBefore(
            placement, first_offset, static_cast<uint32_t>(index),
            NoteRenderFinalCaretAffinity::BeforeVisualWrap);
        const NoteRenderPlacementBoundary* last = BoundaryAtOrBefore(
            placement, last_offset, static_cast<uint32_t>(index),
            NoteRenderFinalCaretAffinity::AfterVisualWrap);
        if (!first || !last) return false;
        int left = std::min(first->x_px, last->x_px);
        int right = std::max(first->x_px, last->x_px);
        // A one-code-unit or a hidden-delimiter selection can have coincident
        // visual boundaries. Keep it visible without overflowing coordinates.
        if (left == right) {
            if (right < std::numeric_limits<int>::max()) ++right;
            else if (left > std::numeric_limits<int>::min()) --left;
            else return false;
        }
        const uint64_t top_px = line_top_px + fragment.top_offset_px;
        if (top_px > std::numeric_limits<uint64_t>::max() - fragment.height_px) return false;
        out->push_back({left, top_px, right, top_px + fragment.height_px});
    }
    return true;
}

} // namespace

NoteRenderFinalInteractionResult ResolveNoteRenderFinalCommittedLine(
    const NoteRenderFinalPublication& publication,
    LineIndex line_index,
    NoteRenderFinalResolvedLine* out) noexcept {
    return ResolveLineUnchecked(publication, line_index, out);
}

NoteRenderFinalInteractionResult HitTestNoteRenderFinalPublication(
    const NoteRenderFinalPublication& publication,
    int content_x_px,
    uint64_t content_y_px,
    NoteRenderFinalHit* out) noexcept {
    if (!out) return NoteRenderFinalInteractionResult::InvalidOutput;
    if (!publication.valid() || !publication.layout()) {
        return NoteRenderFinalInteractionResult::InvalidPublication;
    }
    const std::optional<NoteRenderLineLayoutLocation> layout =
        NoteRenderLineLayoutMap::LineContainingY(
            publication.layout()->line_layouts(), content_y_px);
    if (!layout.has_value()) return NoteRenderFinalInteractionResult::OutsideContent;

    NoteRenderFinalResolvedLine line;
    const NoteRenderFinalInteractionResult line_result =
        ResolveLineUnchecked(publication, layout->line_index, &line);
    if (line_result != NoteRenderFinalInteractionResult::Resolved) return line_result;

    // Empty source rows still own a measured line box, but have no glyph run.
    // Use that same box for hit and caret instead of falling back to native
    // coordinates (or inventing a source character).
    if (line.source_line.content_span.start == line.source_line.content_span.end &&
        line.placement_line.runs.empty()) {
        *out = {};
        out->line_index = line.line_index;
        out->source_offset = line.source_line.content_span.start;
        out->source_span = line.source_line.content_span;
        return NoteRenderFinalInteractionResult::Resolved;
    }

    const uint64_t relative_y_px = content_y_px - line.layout.top_px;
    size_t best_run = static_cast<size_t>(-1);
    const NoteRenderPlacementBoundary* best_boundary = nullptr;
    int64_t best_distance = std::numeric_limits<int64_t>::max();
    bool best_hidden = true;
    for (size_t index = 0; index < line.placement_line.runs.size(); ++index) {
        const NoteRenderRunPlacement& placement = line.placement_line.runs[index];
        const bool hidden = line.source_line.runs[index].kind == NoteRenderSourceRunKind::HiddenSyntax;
        if (placement.boundaries.empty()) return NoteRenderFinalInteractionResult::InconsistentPublication;
        for (const NoteRenderPlacementBoundary& boundary : placement.boundaries) {
            const NoteRenderRunPlacementFragment* fragment =
                FragmentForBoundary(placement, boundary);
            if (!fragment) return NoteRenderFinalInteractionResult::InconsistentPublication;
            if (!FragmentContainsY(*fragment, relative_y_px)) continue;
            const int64_t distance = std::llabs(
                static_cast<int64_t>(boundary.x_px) - static_cast<int64_t>(content_x_px));
            // Match presentation hit testing: hidden delimiters have no
            // clickable text when a displayed run owns this visual row.
            if (!best_boundary || (best_hidden && !hidden) ||
                (best_hidden == hidden && distance < best_distance)) {
                best_run = index;
                best_boundary = &boundary;
                best_distance = distance;
                best_hidden = hidden;
            }
        }
    }
    if (!best_boundary || best_run >= line.source_line.runs.size()) {
        return NoteRenderFinalInteractionResult::OutsideContent;
    }
    try {
        const NoteRenderSourceRun& source_run = line.source_line.runs[best_run];
        const NoteRenderRunPlacement& placement_run = line.placement_line.runs[best_run];
        if (source_run.source_span.start != placement_run.source_span.start ||
            source_run.source_span.end != placement_run.source_span.end ||
            best_boundary->source_offset < source_run.source_span.start ||
            best_boundary->source_offset > source_run.source_span.end) {
            return NoteRenderFinalInteractionResult::InconsistentPublication;
        }
        NoteRenderFinalHit candidate;
        candidate.line_index = line.line_index;
        candidate.source_offset = best_boundary->source_offset;
        candidate.run_index = best_run;
        candidate.source_span = source_run.source_span;
        candidate.is_link = source_run.kind == NoteRenderSourceRunKind::LinkText &&
                            !source_run.link_target.empty();
        if (candidate.is_link) candidate.link_target = source_run.link_target;
        for (const NoteRenderSourceStyleAttribute& style : source_run.styles) {
            if (style.kind == StyleKind::LinkId && !style.value.empty()) {
                candidate.is_link = true;
                candidate.is_legacy_link_id = true;
                candidate.link_target = style.value;
                break;
            }
        }
        *out = std::move(candidate);
        return NoteRenderFinalInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalInteractionResult::InconsistentPublication;
    }
}

NoteRenderFinalInteractionResult ResolveNoteRenderFinalCaret(
    const NoteRenderFinalPublication& publication,
    Utf16CodeUnitOffset source_offset,
    NoteRenderFinalCaretGeometry* out) noexcept {
    return ResolveNoteRenderFinalCaret(
        publication, source_offset, NoteRenderFinalCaretAffinity::AfterVisualWrap, out);
}

NoteRenderFinalInteractionResult ResolveNoteRenderFinalCaret(
    const NoteRenderFinalPublication& publication,
    Utf16CodeUnitOffset source_offset,
    NoteRenderFinalCaretAffinity affinity,
    NoteRenderFinalCaretGeometry* out) noexcept {
    if (!out) return NoteRenderFinalInteractionResult::InvalidOutput;
    if (!publication.valid() || !publication.syntax()) {
        return NoteRenderFinalInteractionResult::InvalidPublication;
    }
    const auto location = NoteSourceLineMap::FindByOffset(
        publication.syntax()->source_line_map(), source_offset);
    if (!location.has_value()) return NoteRenderFinalInteractionResult::OutsideContent;

    NoteRenderFinalResolvedLine line;
    const NoteRenderFinalInteractionResult line_result =
        ResolveLineUnchecked(publication, location->line_index, &line);
    if (line_result != NoteRenderFinalInteractionResult::Resolved) return line_result;

    if (line.source_line.content_span.start == line.source_line.content_span.end &&
        line.placement_line.runs.empty() && source_offset == line.source_line.content_span.start) {
        *out = {line.line_index, source_offset, 0, line.layout.top_px, line.layout.bottom_px};
        return NoteRenderFinalInteractionResult::Resolved;
    }

    const NoteRenderPlacementBoundary* selected = nullptr;
    const NoteRenderRunPlacementFragment* selected_fragment = nullptr;
    for (const NoteRenderRunPlacement& placement : line.placement_line.runs) {
        const NoteRenderPlacementBoundary* candidate =
            BoundaryAtOrBefore(placement, source_offset, std::nullopt, affinity);
        if (!candidate) continue;
        const NoteRenderRunPlacementFragment* fragment = FragmentForBoundary(placement, *candidate);
        if (!fragment) return NoteRenderFinalInteractionResult::InconsistentPublication;
        if (!selected || candidate->source_offset > selected->source_offset ||
            (candidate->source_offset == selected->source_offset &&
             affinity == NoteRenderFinalCaretAffinity::AfterVisualWrap &&
             fragment->top_offset_px >= selected_fragment->top_offset_px)) {
            selected = candidate;
            selected_fragment = fragment;
        }
    }
    if (!selected || !selected_fragment ||
        line.layout.top_px > std::numeric_limits<uint64_t>::max() -
            selected_fragment->top_offset_px) {
        return NoteRenderFinalInteractionResult::OutsideContent;
    }
    const uint64_t top_px = line.layout.top_px + selected_fragment->top_offset_px;
    if (top_px > std::numeric_limits<uint64_t>::max() - selected_fragment->height_px) {
        return NoteRenderFinalInteractionResult::InconsistentPublication;
    }
    *out = {line.line_index, selected->source_offset, selected->x_px,
            top_px, top_px + selected_fragment->height_px};
    return NoteRenderFinalInteractionResult::Resolved;
}

NoteRenderFinalInteractionResult ResolveNoteRenderFinalSelection(
    const NoteRenderFinalPublication& publication,
    Span source_selection,
    std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out) return NoteRenderFinalInteractionResult::InvalidOutput;
    if (!publication.valid() || !publication.syntax()) {
        return NoteRenderFinalInteractionResult::InvalidPublication;
    }
    if (source_selection.end <= source_selection.start) {
        try {
            std::vector<NoteRenderFinalRect> empty;
            *out = std::move(empty);
            return NoteRenderFinalInteractionResult::Resolved;
        } catch (const std::bad_alloc&) {
            return NoteRenderFinalInteractionResult::AllocationFailure;
        }
    }
    const auto first = NoteSourceLineMap::FindByOffset(
        publication.syntax()->source_line_map(), source_selection.start);
    const auto last = NoteSourceLineMap::FindByOffset(
        publication.syntax()->source_line_map(), {source_selection.end.value - 1});
    if (!first.has_value() || !last.has_value() || last->line_index < first->line_index) {
        return NoteRenderFinalInteractionResult::OutsideContent;
    }
    try {
        std::vector<NoteRenderFinalRect> candidate;
        for (size_t value = first->line_index.value; value <= last->line_index.value; ++value) {
            NoteRenderFinalResolvedLine line;
            const NoteRenderFinalInteractionResult line_result =
                ResolveLineUnchecked(publication, {value}, &line);
            if (line_result != NoteRenderFinalInteractionResult::Resolved) return line_result;
            for (const NoteRenderRunPlacement& placement : line.placement_line.runs) {
                if (RangesIntersect(placement.source_span, source_selection) &&
                    !BuildRunSelectionRects(placement, source_selection,
                                            line.layout.top_px, &candidate)) {
                    return NoteRenderFinalInteractionResult::InconsistentPublication;
                }
            }
            if (value == std::numeric_limits<size_t>::max()) {
                return NoteRenderFinalInteractionResult::InconsistentPublication;
            }
        }
        *out = std::move(candidate);
        return NoteRenderFinalInteractionResult::Resolved;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalInteractionResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalInteractionResult::InconsistentPublication;
    }
}

NoteRenderFinalInteractionResult ResolveNoteRenderFinalHorizontalExtent(
    const NoteRenderFinalPublication& publication,
    uint64_t* out_extent_px) noexcept {
    if (!out_extent_px) return NoteRenderFinalInteractionResult::InvalidOutput;
    if (!publication.valid() || !publication.layout()) {
        return NoteRenderFinalInteractionResult::InvalidPublication;
    }
    const uint64_t extent = publication.layout()->line_layouts().max_inline_extent_px();
    const uint64_t padding = publication.layout()->layout_key().horizontal_padding_px;
    if (padding > std::numeric_limits<uint64_t>::max() - extent) {
        return NoteRenderFinalInteractionResult::InconsistentPublication;
    }
    *out_extent_px = extent + padding;
    return NoteRenderFinalInteractionResult::Resolved;
}

} // namespace note
