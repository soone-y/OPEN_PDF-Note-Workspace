#include "note/note_presentation_owner_plan.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool RangesIntersect(NotePresentationLineRange lhs,
                                   NotePresentationLineRange rhs) noexcept {
    return lhs.first < rhs.last_exclusive && rhs.first < lhs.last_exclusive;
}

void NormalizeRanges(std::vector<NotePresentationLineRange>* ranges) {
    if (!ranges) return;
    std::sort(ranges->begin(), ranges->end(), [](NotePresentationLineRange lhs,
                                                 NotePresentationLineRange rhs) {
        if (lhs.first != rhs.first) return lhs.first < rhs.first;
        return lhs.last_exclusive < rhs.last_exclusive;
    });
    std::vector<NotePresentationLineRange> merged;
    merged.reserve(ranges->size());
    for (const NotePresentationLineRange range : *ranges) {
        if (range.last_exclusive <= range.first) continue;
        if (merged.empty() || merged.back().last_exclusive < range.first) {
            merged.push_back(range);
        } else if (merged.back().last_exclusive < range.last_exclusive) {
            merged.back().last_exclusive = range.last_exclusive;
        }
    }
    *ranges = std::move(merged);
}

void AppendWholeVisibleNativeOwner(std::vector<NotePresentationOwnerRange>* ranges,
                                  NotePresentationLineRange visible) {
    if (!ranges) return;
    ranges->push_back(
        NotePresentationOwnerRange{visible, NotePresentationLineOwner::NativeEditor});
}

// Every visible logical row must be painted by exactly one source.  Keeping
// this as a checked invariant makes it impossible for a raw editor range and
// a committed placement range to overlap, or for an old range to leave a
// visual hole after range coalescing.
[[nodiscard]] bool IsExactOwnerPartition(
    const std::vector<NotePresentationOwnerRange>& ranges,
    NotePresentationLineRange visible) noexcept {
    if (ranges.empty()) return false;
    size_t cursor = visible.first.value;
    for (const NotePresentationOwnerRange& range : ranges) {
        if (range.lines.first.value != cursor ||
            range.lines.last_exclusive.value <= range.lines.first.value ||
            range.lines.last_exclusive.value > visible.last_exclusive.value) {
            return false;
        }
        cursor = range.lines.last_exclusive.value;
    }
    return cursor == visible.last_exclusive.value;
}

[[nodiscard]] bool SameRanges(const std::vector<NotePresentationLineRange>& lhs,
                              const std::vector<NotePresentationLineRange>& rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].first != rhs[index].first ||
            lhs[index].last_exclusive != rhs[index].last_exclusive) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AddQueryWork(NotePresentationOwnerPlanBuildWork* total,
                                const NoteRenderAtomicGroupQueryWork& query) noexcept {
    if (!total) return true;
    if (total->atomic_group_queries == std::numeric_limits<size_t>::max() ||
        query.visited_index_nodes >
            std::numeric_limits<size_t>::max() - total->atomic_group_index_nodes) {
        return false;
    }
    ++total->atomic_group_queries;
    total->atomic_group_index_nodes += query.visited_index_nodes;
    return true;
}

} // namespace

bool NotePresentationLineRangeIsValid(NotePresentationLineRange range,
                                      size_t lineCount) noexcept {
    return range.first.value <= range.last_exclusive.value &&
           range.last_exclusive.value <= lineCount;
}

NotePresentationOwnerPlanBuildResult NotePresentationOwnerPlan::Build(
    const NotePresentationOwnerInput& input,
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey,
    std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
    std::shared_ptr<const NotePresentationOwnerPlan>* out,
    NotePresentationOwnerPlanBuildWork* outWork) noexcept {
    if (!out) return NotePresentationOwnerPlanBuildResult::InvalidOutput;
    if (outWork) *outWork = {};
    const size_t lineCount = textCore.valid() ? textCore.logical_line_count() : 0;
    if (!NotePresentationLineRangeIsValid(input.visible_lines, lineCount) ||
        input.visible_lines.first == input.visible_lines.last_exclusive) {
        return NotePresentationOwnerPlanBuildResult::InvalidVisibleRange;
    }

    try {
        std::shared_ptr<NotePresentationOwnerPlan> candidate(new NotePresentationOwnerPlan());
        const bool placementCurrent = placement && placement->Matches(textCore, layoutKey);
        if (!input.render_active) {
            candidate->frame_.reason = NotePresentationReason::RenderingDisabled;
            AppendWholeVisibleNativeOwner(&candidate->owner_ranges_, input.visible_lines);
        } else if (!input.editor_text_core_current) {
            candidate->frame_.reason = NotePresentationReason::EditorBindingUnavailable;
            AppendWholeVisibleNativeOwner(&candidate->owner_ranges_, input.visible_lines);
        } else if (input.geometry_may_change) {
            candidate->frame_.frame_kind = NotePresentationFrameKind::CommitThenDraw;
            candidate->frame_.reason = NotePresentationReason::GeometryMayChange;
            candidate->frame_.requires_full_repaint_after_commit = true;
            AppendWholeVisibleNativeOwner(&candidate->owner_ranges_, input.visible_lines);
        } else if (input.ime_preedit && !input.ime_preedit_can_reuse_committed_layout) {
            candidate->frame_.reason = NotePresentationReason::StaleImeComposition;
            AppendWholeVisibleNativeOwner(&candidate->owner_ranges_, input.visible_lines);
        } else if (!placementCurrent) {
            candidate->frame_.reason = NotePresentationReason::SnapshotUnavailable;
            AppendWholeVisibleNativeOwner(&candidate->owner_ranges_, input.visible_lines);
        } else {
            std::vector<NotePresentationLineRange> rawRanges;
            rawRanges.reserve(input.requested_editor_lines.size() + 4);
            for (const NotePresentationLineRange requested : input.requested_editor_lines) {
                if (!NotePresentationLineRangeIsValid(requested, lineCount)) {
                    candidate->frame_.reason = NotePresentationReason::SnapshotUnavailable;
                    AppendWholeVisibleNativeOwner(&candidate->owner_ranges_, input.visible_lines);
                    candidate->valid_ = true;
                    *out = std::move(candidate);
                    return NotePresentationOwnerPlanBuildResult::Built;
                }
                if (RangesIntersect(requested, input.visible_lines)) {
                    rawRanges.push_back(NotePresentationLineRange{
                        {std::max(requested.first.value, input.visible_lines.first.value)},
                        {std::min(requested.last_exclusive.value,
                                  input.visible_lines.last_exclusive.value)}});
                }
            }
            NormalizeRanges(&rawRanges);

            // A table or display-math object has one shared geometry. Expand
            // only groups that meet an already requested raw range. Querying
            // the whole visible document here would turn each caret move in a
            // long note into an O(number of groups) scan. Re-run only when a
            // newly expanded range can meet another group; this fixed point is
            // required for future overlapping atomic-group kinds as well.
            bool expanded = false;
            do {
                expanded = false;
                std::vector<NotePresentationLineRange> nextRanges = rawRanges;
                for (const NotePresentationLineRange raw : rawRanges) {
                    std::vector<NoteRenderAtomicGroup> atomicGroups;
                    NoteRenderAtomicGroupQueryWork queryWork;
                    if (!placement->source_plan()->CopyAtomicGroupsIntersecting(
                            raw.first, raw.last_exclusive, &atomicGroups, &queryWork) ||
                        !AddQueryWork(outWork, queryWork)) {
                        candidate->frame_.reason = NotePresentationReason::SnapshotUnavailable;
                        AppendWholeVisibleNativeOwner(&candidate->owner_ranges_, input.visible_lines);
                        candidate->valid_ = true;
                        *out = std::move(candidate);
                        return NotePresentationOwnerPlanBuildResult::Built;
                    }
                    for (const NoteRenderAtomicGroup& group : atomicGroups) {
                        if (group.kind != NoteRenderAtomicGroupKind::Table &&
                            group.kind != NoteRenderAtomicGroupKind::BlockMath) {
                            continue;
                        }
                        if (group.last_line.value == std::numeric_limits<size_t>::max()) continue;
                        const NotePresentationLineRange groupRange{
                            group.first_line, {group.last_line.value + 1}};
                        if (RangesIntersect(groupRange, input.visible_lines)) {
                            nextRanges.push_back(NotePresentationLineRange{
                                {std::max(groupRange.first.value,
                                          input.visible_lines.first.value)},
                                {std::min(groupRange.last_exclusive.value,
                                          input.visible_lines.last_exclusive.value)}});
                        }
                    }
                }
                NormalizeRanges(&nextRanges);
                expanded = !SameRanges(rawRanges, nextRanges);
                rawRanges = std::move(nextRanges);
            } while (expanded);

            candidate->frame_.frame_kind = NotePresentationFrameKind::DrawCommitted;
            candidate->frame_.reason = NotePresentationReason::CurrentSnapshot;
            candidate->frame_.caret_presenter = NoteCaretPresenter::CommittedLayout;
            size_t cursor = input.visible_lines.first.value;
            for (const NotePresentationLineRange raw : rawRanges) {
                if (cursor < raw.first.value) {
                    candidate->owner_ranges_.push_back(NotePresentationOwnerRange{
                        {{cursor}, raw.first}, NotePresentationLineOwner::CommittedPlacement});
                }
                candidate->owner_ranges_.push_back(
                    NotePresentationOwnerRange{raw, NotePresentationLineOwner::NativeEditor});
                cursor = raw.last_exclusive.value;
                if (input.caret_line >= raw.first.value && input.caret_line < raw.last_exclusive.value) {
                    candidate->frame_.caret_presenter = NoteCaretPresenter::NativeEditor;
                }
            }
            if (cursor < input.visible_lines.last_exclusive.value) {
                candidate->owner_ranges_.push_back(NotePresentationOwnerRange{
                    {{cursor}, input.visible_lines.last_exclusive},
                    NotePresentationLineOwner::CommittedPlacement});
            }
            candidate->placement_ = std::move(placement);
        }
        if (!IsExactOwnerPartition(candidate->owner_ranges_, input.visible_lines)) {
            return NotePresentationOwnerPlanBuildResult::InvariantViolation;
        }
        candidate->valid_ = true;
        *out = std::move(candidate);
        return NotePresentationOwnerPlanBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NotePresentationOwnerPlanBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NotePresentationOwnerPlanBuildResult::AllocationFailure;
    }
}

bool NotePresentationOwnerPlan::Matches(const NoteTextCore& textCore,
                                        const NoteRenderLayoutKey& layoutKey) const noexcept {
    return valid_ && frame_.frame_kind == NotePresentationFrameKind::DrawCommitted &&
           placement_ && placement_->Matches(textCore, layoutKey);
}

} // namespace note
