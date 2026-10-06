#include "note/note_render_final_presentation_snapshot.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool IsExactSurfacePartition(
    const std::vector<NoteRenderFinalSurfaceRange>& ranges,
    size_t lineCount) noexcept {
    if (ranges.empty()) return false;
    size_t cursor = 0;
    for (const NoteRenderFinalSurfaceRange& range : ranges) {
        if (range.lines.first.value != cursor ||
            range.lines.last_exclusive.value <= range.lines.first.value ||
            range.lines.last_exclusive.value > lineCount) {
            return false;
        }
        cursor = range.lines.last_exclusive.value;
    }
    return cursor == lineCount;
}

[[nodiscard]] bool IsExactRawDisplay(
    const NoteRenderFinalRawLineSurface& line,
    const NoteRenderFinalRawTextLine& expected) noexcept {
    if (line.line_index != expected.line_index ||
        line.source_span.start != expected.text_span.start ||
        line.source_span.end != expected.text_span.end ||
        line.display.display_text != expected.text ||
        line.display.boundaries.size() != expected.text.size() + 1 ||
        line.layout.height_px == 0 || line.layout.collapsed_into_atomic_group ||
        line.placement.runs.size() != 1) {
        return false;
    }
    for (size_t index = 0; index < line.display.boundaries.size(); ++index) {
        const NoteRenderFinalDisplayBoundary& boundary = line.display.boundaries[index];
        if (expected.text_span.start.value > std::numeric_limits<size_t>::max() - index ||
            boundary.source_offset.value != expected.text_span.start.value + index ||
            boundary.display_offset != index) {
            return false;
        }
    }
    const NoteRenderRunPlacement& run = line.placement.runs.front();
    if (run.source_span.start != line.source_span.start ||
        run.source_span.end != line.source_span.end || run.fragments.empty() ||
        run.boundaries.empty()) {
        return false;
    }
    Utf16CodeUnitOffset fragmentCursor = line.source_span.start;
    uint64_t previousFragmentBottom = 0;
    uint32_t previousFragmentTop = 0;
    for (const NoteRenderRunPlacementFragment& fragment : run.fragments) {
        if (fragment.source_span.start < line.source_span.start ||
            fragment.source_span.end > line.source_span.end || fragment.height_px == 0 ||
            fragment.source_span.start != fragmentCursor ||
            fragment.source_span.end < fragment.source_span.start ||
            (fragment.source_span.end == fragment.source_span.start && !expected.text.empty()) ||
            (previousFragmentBottom != 0 && fragment.top_offset_px < previousFragmentBottom &&
             (fragment.top_offset_px != previousFragmentTop ||
              static_cast<uint64_t>(fragment.top_offset_px) + fragment.height_px != previousFragmentBottom)) ||
            static_cast<uint64_t>(fragment.top_offset_px) + fragment.height_px > line.layout.height_px ||
            fragment.baseline_offset_px != 0 || fragment.width_px < 0) {
            return false;
        }
        fragmentCursor = fragment.source_span.end;
        previousFragmentTop = fragment.top_offset_px;
        previousFragmentBottom = static_cast<uint64_t>(fragment.top_offset_px) + fragment.height_px;
    }
    if (fragmentCursor != line.source_span.end) return false;
    const NoteRenderPlacementBoundary* previous = nullptr;
    for (const NoteRenderPlacementBoundary& boundary : run.boundaries) {
        if (boundary.source_offset < line.source_span.start ||
            boundary.source_offset > line.source_span.end ||
            boundary.fragment_index >= run.fragments.size() ||
            (previous && (boundary.source_offset < previous->source_offset ||
                           boundary.fragment_index < previous->fragment_index ||
                           (boundary.source_offset == previous->source_offset &&
                            boundary.fragment_index == previous->fragment_index)))) {
            return false;
        }
        const auto& fragment = run.fragments[boundary.fragment_index];
        if (boundary.source_offset < fragment.source_span.start ||
            boundary.source_offset > fragment.source_span.end) return false;
        previous = &boundary;
    }
    return true;
}

[[nodiscard]] bool AppendCanonicalRawTextLine(
    const NoteTextCore& textCore,
    const NoteSourceLineLocation& sourceLine,
    NoteRenderFinalRawTextLine* out) noexcept {
    if (!out) return false;
    try {
        NoteRenderFinalRawTextLine line;
        line.line_index = sourceLine.line_index;
        line.text_span = {sourceLine.start, sourceLine.content_end};
        line.text = textCore.CopyRawRange(sourceLine.start, sourceLine.line.content_length);
        if (line.text.size() != sourceLine.line.content_length) return false;
        *out = std::move(line);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool BuildRawTextLines(
    const NoteTextCore& textCore,
    const std::vector<NoteRenderFinalSurfaceRange>& surfaceRanges,
    const NoteRenderFinalImePreeditPresentation* imePreedit,
    std::vector<NoteRenderFinalRawTextLine>* out) noexcept {
    if (!out) return false;
    if (imePreedit && (!imePreedit->valid() || !imePreedit->Matches(textCore))) return false;
    try {
        std::vector<NoteRenderFinalRawTextLine> candidate;
        bool sawPreeditLine = !imePreedit;
        for (const NoteRenderFinalSurfaceRange& range : surfaceRanges) {
            if (range.surface != NoteRenderFinalSurfaceKind::RawPaint) continue;
            for (size_t line = range.lines.first.value; line < range.lines.last_exclusive.value;
                 ++line) {
                const auto sourceLine = NoteSourceLineMap::LineAt(textCore.source_line_map(), {line});
                if (!sourceLine.has_value()) return false;
                NoteRenderFinalRawTextLine request;
                if (!imePreedit) {
                    if (!AppendCanonicalRawTextLine(textCore, *sourceLine, &request)) return false;
                } else {
                    Utf16CodeUnitOffset editorStart;
                    Utf16CodeUnitOffset editorEnd;
                    if (!imePreedit->MapCanonicalUnchangedBoundary(
                            sourceLine->start, NoteSourceEditBoundarySide::BeforeReplacement,
                            &editorStart) ||
                        !imePreedit->MapCanonicalUnchangedBoundary(
                            sourceLine->content_end,
                            NoteSourceEditBoundarySide::AfterReplacement, &editorEnd) ||
                        editorEnd < editorStart) {
                        return false;
                    }
                    request.line_index = {line};
                    request.text_span = {editorStart, editorEnd};
                    if (request.line_index == imePreedit->line_index()) {
                        request.text = imePreedit->temporary_raw_line();
                        const Span preeditLine = imePreedit->editor_line_span();
                        if (request.text_span.start != preeditLine.start ||
                            request.text_span.end != preeditLine.end) {
                            return false;
                        }
                        sawPreeditLine = true;
                    } else {
                        request.text = textCore.CopyRawRange(
                            sourceLine->start, sourceLine->line.content_length);
                        if (request.text.size() != sourceLine->line.content_length) return false;
                    }
                    if (request.text_span.end.value - request.text_span.start.value !=
                        request.text.size()) {
                        return false;
                    }
                }
                candidate.push_back(std::move(request));
            }
        }
        if (!sawPreeditLine) return false;
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool BuildSurfaceRanges(
    const NotePresentationOwnerPlan& ownerPlan,
    size_t lineCount,
    std::vector<NoteRenderFinalSurfaceRange>* out) noexcept {
    if (!out) return false;
    try {
        std::vector<NoteRenderFinalSurfaceRange> candidate;
        candidate.reserve(ownerPlan.owner_ranges().size());
        for (const NotePresentationOwnerRange& owner : ownerPlan.owner_ranges()) {
            NoteRenderFinalSurfaceKind surface = NoteRenderFinalSurfaceKind::StructuredPaint;
            switch (owner.owner) {
            case NotePresentationLineOwner::CommittedPlacement:
                surface = NoteRenderFinalSurfaceKind::StructuredPaint;
                break;
            case NotePresentationLineOwner::NativeEditor:
                surface = NoteRenderFinalSurfaceKind::RawPaint;
                break;
            default:
                return false;
            }
            candidate.push_back({owner.lines, surface});
        }
        if (!IsExactSurfacePartition(candidate, lineCount)) return false;
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

// Ownership changes must not contract an unchanged editing unit. Keep the
// structured allocation if raw glyphs fit; grow only when raw wrapping needs
// more space. Table/block-math source rows form one unit, not independent
// floors (a block formula's first row already owns its entire graphic).
// Only allocations change: fragments, source boundaries and caret heights
// remain the measured raw geometry. Failure rejects the unpublished frame.
[[nodiscard]] bool PreserveRawAllocationHeights(
    const NoteRenderFinalPublication& publication,
    const std::vector<NoteRenderFinalSurfaceRange>& surfaceRanges,
    std::vector<NoteRenderFinalRawLineSurface>* rawLines) {
    if (!rawLines) return false;
    const auto& structuralLayouts = publication.layout()->line_layouts();
    size_t cursor = 0;
    const auto preserveRow = [&](size_t index) {
        auto& raw = (*rawLines)[index];
        const auto structured = NoteRenderLineLayoutMap::LineAt(
            structuralLayouts, raw.line_index);
        if (!structured || structured->layout.collapsed_into_atomic_group) return false;
        raw.layout.height_px = std::max(raw.layout.height_px, structured->layout.height_px);
        return true;
    };
    for (const auto& range : surfaceRanges) {
        if (range.surface != NoteRenderFinalSurfaceKind::RawPaint) continue;
        const size_t count = range.lines.last_exclusive.value - range.lines.first.value;
        if (cursor > rawLines->size() || count > rawLines->size() - cursor) return false;
        const size_t rangeEnd = cursor + count;
        for (size_t index = cursor; index < rangeEnd; ++index) {
            if ((*rawLines)[index].line_index.value !=
                range.lines.first.value + index - cursor) return false;
        }
        // Query the source interval index once per raw range. Do not copy
        // measured table cells/glyphs or enumerate unrelated document groups.
        std::vector<NoteRenderAtomicGroup> groups;
        if (!publication.source_plan()->CopyAtomicGroupsIntersecting(
                range.lines.first, range.lines.last_exclusive, &groups)) return false;
        for (const auto& group : groups) {
            if (group.kind != NoteRenderAtomicGroupKind::Table &&
                group.kind != NoteRenderAtomicGroupKind::BlockMath) continue;
            if (group.first_line < range.lines.first ||
                group.last_line >= range.lines.last_exclusive ||
                group.last_line < group.first_line) return false;
            while (cursor < rangeEnd && (*rawLines)[cursor].line_index < group.first_line) {
                if (!preserveRow(cursor++)) return false;
            }
            if (cursor == rangeEnd || (*rawLines)[cursor].line_index != group.first_line) {
                return false;
            }
            const size_t groupCount = group.last_line.value - group.first_line.value + 1;
            if (groupCount > rangeEnd - cursor) return false;
            uint64_t rawHeight = 0;
            for (size_t index = cursor; index < cursor + groupCount; ++index) {
                const uint32_t height = (*rawLines)[index].layout.height_px;
                if (height > std::numeric_limits<uint64_t>::max() - rawHeight) return false;
                rawHeight += height;
            }
            const auto first = NoteRenderLineLayoutMap::LineAt(structuralLayouts, group.first_line);
            const auto last = NoteRenderLineLayoutMap::LineAt(structuralLayouts, group.last_line);
            if (!first || !last || last->bottom_px < first->top_px) return false;
            const uint64_t structuredHeight = last->bottom_px - first->top_px;
            if (rawHeight < structuredHeight) {
                auto& lastHeight = (*rawLines)[cursor + groupCount - 1].layout.height_px;
                const uint64_t padding = structuredHeight - rawHeight;
                if (padding > std::numeric_limits<uint32_t>::max() - lastHeight) return false;
                lastHeight += static_cast<uint32_t>(padding);
            }
            cursor += groupCount;
        }
        while (cursor < rangeEnd) {
            if (!preserveRow(cursor++)) return false;
        }
    }
    return cursor == rawLines->size();
}

[[nodiscard]] bool BuildHybridLayouts(
    const NoteRenderLineLayoutMap::Snapshot& structuralLayouts,
    const std::vector<NoteRenderFinalSurfaceRange>& surfaceRanges,
    const std::vector<NoteRenderFinalRawLineSurface>& rawLines,
    NoteRenderLineLayoutMap::Snapshot* out) noexcept {
    if (!out || !structuralLayouts.valid()) return false;
    try {
        NoteRenderLineLayoutMap::Snapshot current = structuralLayouts;
        size_t rawCursor = 0;
        bool hasRawSurface = false;
        for (const NoteRenderFinalSurfaceRange& range : surfaceRanges) {
            if (range.surface != NoteRenderFinalSurfaceKind::RawPaint) continue;
            hasRawSurface = true;
            const size_t count = range.lines.last_exclusive.value - range.lines.first.value;
            if (count == 0 || rawCursor > rawLines.size() || count > rawLines.size() - rawCursor) {
                return false;
            }
            std::vector<NoteRenderLineLayout> replacement;
            replacement.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                const NoteRenderFinalRawLineSurface& raw = rawLines[rawCursor + index];
                if (raw.line_index.value != range.lines.first.value + index) return false;
                replacement.push_back(raw.layout);
            }
            NoteRenderLineLayoutMap::Snapshot replaced;
            if (!NoteRenderLineLayoutMap::Replace(current, range.lines.first,
                                                  range.lines.last_exclusive,
                                                  replacement, &replaced)) {
                return false;
            }
            current = std::move(replaced);
            rawCursor += count;
        }
        if (hasRawSurface != !rawLines.empty() || rawCursor != rawLines.size() ||
            current.line_count() != structuralLayouts.line_count()) {
            return false;
        }
        *out = std::move(current);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

NoteRenderFinalPresentationSnapshotBuildResult NoteRenderFinalPresentationSnapshot::Build(
    const NoteTextCore& textCore,
    std::shared_ptr<const NoteRenderFinalPublication> structuralPublication,
    NotePresentationOwnerInput ownerInput,
    const NoteRenderFinalRawSurfaceMeasurementProvider& rawMeasurementProvider,
    std::shared_ptr<const NoteRenderFinalPresentationSnapshot>* out) noexcept {
    return BuildInternal(textCore, std::move(structuralPublication), std::move(ownerInput),
                         nullptr, rawMeasurementProvider, out);
}

NoteRenderFinalPresentationSnapshotBuildResult
NoteRenderFinalPresentationSnapshot::BuildWithImePreedit(
    const NoteTextCore& textCore,
    std::shared_ptr<const NoteRenderFinalPublication> structuralPublication,
    NotePresentationOwnerInput ownerInput,
    const NoteRenderFinalImePreeditPresentation& imePreedit,
    const NoteRenderFinalRawSurfaceMeasurementProvider& rawMeasurementProvider,
    std::shared_ptr<const NoteRenderFinalPresentationSnapshot>* out) noexcept {
    return BuildInternal(textCore, std::move(structuralPublication), std::move(ownerInput),
                         &imePreedit, rawMeasurementProvider, out);
}

NoteRenderFinalPresentationSnapshotBuildResult
NoteRenderFinalPresentationSnapshot::BuildInternal(
    const NoteTextCore& textCore,
    std::shared_ptr<const NoteRenderFinalPublication> structuralPublication,
    NotePresentationOwnerInput ownerInput,
    const NoteRenderFinalImePreeditPresentation* imePreedit,
    const NoteRenderFinalRawSurfaceMeasurementProvider& rawMeasurementProvider,
    std::shared_ptr<const NoteRenderFinalPresentationSnapshot>* out) noexcept {
    if (!out) return NoteRenderFinalPresentationSnapshotBuildResult::InvalidOutput;
    if (!textCore.valid() || textCore.logical_line_count() == 0) {
        return NoteRenderFinalPresentationSnapshotBuildResult::InvalidTextCore;
    }
    if (!structuralPublication || !structuralPublication->valid() ||
        !structuralPublication->layout() || !structuralPublication->placement() ||
        !structuralPublication->source_plan() ||
        !structuralPublication->Matches(textCore, structuralPublication->layout()->layout_key())) {
        return NoteRenderFinalPresentationSnapshotBuildResult::InvalidStructuralPublication;
    }
    if (imePreedit && (!imePreedit->valid() || !imePreedit->Matches(textCore))) {
        return NoteRenderFinalPresentationSnapshotBuildResult::InvalidImePreedit;
    }
    try {
        // Ownership must cover complete atomic groups even when their first or
        // last source row lies outside the current client clip.  Build it for
        // the whole document; paint later clips this immutable frame.
        ownerInput.visible_lines = {{0}, {textCore.logical_line_count()}};
        std::shared_ptr<const NotePresentationOwnerPlan> ownerPlan;
        if (NotePresentationOwnerPlan::Build(
                ownerInput, textCore, structuralPublication->layout()->layout_key(),
                structuralPublication->placement(), &ownerPlan) !=
                NotePresentationOwnerPlanBuildResult::Built ||
            !ownerPlan || !ownerPlan->valid() ||
            ownerPlan->frame().frame_kind != NotePresentationFrameKind::DrawCommitted ||
            !ownerPlan->Matches(textCore, structuralPublication->layout()->layout_key())) {
            return NoteRenderFinalPresentationSnapshotBuildResult::OwnerPlanBuildFailed;
        }

        std::vector<NoteRenderFinalSurfaceRange> surfaceRanges;
        if (!BuildSurfaceRanges(*ownerPlan, textCore.logical_line_count(), &surfaceRanges)) {
            return NoteRenderFinalPresentationSnapshotBuildResult::OwnerPlanBuildFailed;
        }

        if (imePreedit) {
            const auto preeditSurface = [&]() -> std::optional<NoteRenderFinalSurfaceKind> {
                for (const NoteRenderFinalSurfaceRange& range : surfaceRanges) {
                    if (imePreedit->line_index() >= range.lines.first &&
                        imePreedit->line_index() < range.lines.last_exclusive) {
                        return range.surface;
                    }
                }
                return std::nullopt;
            }();
            if (!preeditSurface.has_value() ||
                *preeditSurface != NoteRenderFinalSurfaceKind::RawPaint) {
                return NoteRenderFinalPresentationSnapshotBuildResult::ImePreeditSurfaceMissing;
            }
        }

        std::vector<NoteRenderFinalRawTextLine> rawTextLines;
        if (!BuildRawTextLines(textCore, surfaceRanges, imePreedit, &rawTextLines)) {
            return imePreedit
                ? NoteRenderFinalPresentationSnapshotBuildResult::ImePreeditSurfaceMissing
                : NoteRenderFinalPresentationSnapshotBuildResult::RawMeasurementFailed;
        }
        std::vector<NoteRenderFinalRawLineSurface> rawLines;
        if (!rawTextLines.empty()) {
            if (!rawMeasurementProvider.MeasureRawTextLines(
                    structuralPublication->layout()->layout_key(), rawTextLines, &rawLines) ||
                rawLines.size() != rawTextLines.size()) {
                return NoteRenderFinalPresentationSnapshotBuildResult::RawMeasurementFailed;
            }
            for (size_t index = 0; index < rawLines.size(); ++index) {
                if (!IsExactRawDisplay(rawLines[index], rawTextLines[index])) {
                    return NoteRenderFinalPresentationSnapshotBuildResult::InvalidRawSurface;
                }
            }
        }

        NoteRenderLineLayoutMap::Snapshot hybridLayouts;
        if (!PreserveRawAllocationHeights(*structuralPublication, surfaceRanges, &rawLines) ||
            !BuildHybridLayouts(
                structuralPublication->layout()->line_layouts(), surfaceRanges, rawLines,
                &hybridLayouts)) {
            return NoteRenderFinalPresentationSnapshotBuildResult::HybridLayoutBuildFailed;
        }

        std::shared_ptr<NoteRenderFinalPresentationSnapshot> candidate(
            new NoteRenderFinalPresentationSnapshot());
        candidate->structural_publication_ = std::move(structuralPublication);
        candidate->owner_plan_ = std::move(ownerPlan);
        candidate->line_layouts_ = std::move(hybridLayouts);
        candidate->surface_ranges_ = std::move(surfaceRanges);
        candidate->raw_lines_ = std::move(rawLines);
        if (imePreedit) candidate->ime_preedit_ = *imePreedit;
        candidate->valid_ = true;
        *out = std::move(candidate);
        return NoteRenderFinalPresentationSnapshotBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPresentationSnapshotBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPresentationSnapshotBuildResult::AllocationFailure;
    }
}

bool NoteRenderFinalPresentationSnapshot::Matches(
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey) const noexcept {
    return valid_ && structural_publication_ && owner_plan_ && line_layouts_.valid() &&
           line_layouts_.line_count() == textCore.logical_line_count() &&
           structural_publication_->Matches(textCore, layoutKey) &&
           owner_plan_->Matches(textCore, layoutKey) &&
           (!ime_preedit_.has_value() || ime_preedit_->Matches(textCore)) &&
           IsExactSurfacePartition(surface_ranges_, textCore.logical_line_count());
}

std::optional<NoteRenderFinalSurfaceKind> NoteRenderFinalPresentationSnapshot::SurfaceAt(
    LineIndex lineIndex) const noexcept {
    if (!valid_) return std::nullopt;
    const auto it = std::upper_bound(
        surface_ranges_.begin(), surface_ranges_.end(), lineIndex.value,
        [](size_t line, const NoteRenderFinalSurfaceRange& range) {
            return line < range.lines.first.value;
        });
    if (it == surface_ranges_.begin()) return std::nullopt;
    const NoteRenderFinalSurfaceRange& range = *std::prev(it);
    if (lineIndex < range.lines.first || lineIndex >= range.lines.last_exclusive) {
        return std::nullopt;
    }
    return range.surface;
}

bool NoteRenderFinalPresentationSnapshot::ResolveRawLine(
    LineIndex lineIndex,
    NoteRenderFinalRawLineSurface* out) const noexcept {
    if (!out || !valid_) return false;
    const auto it = std::lower_bound(
        raw_lines_.begin(), raw_lines_.end(), lineIndex,
        [](const NoteRenderFinalRawLineSurface& line, LineIndex target) {
            return line.line_index < target;
        });
    if (it == raw_lines_.end() || it->line_index != lineIndex) return false;
    try {
        *out = *it;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace note
