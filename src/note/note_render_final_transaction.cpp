#include "note/note_render_final_transaction.h"

#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool IsAllocationFailure(
    NoteSyntaxSnapshotBuildResult result) noexcept {
    return result == NoteSyntaxSnapshotBuildResult::AllocationFailure;
}

[[nodiscard]] bool IsAllocationFailure(
    NoteRenderSourcePlanBuildResult result) noexcept {
    return result == NoteRenderSourcePlanBuildResult::AllocationFailure;
}

[[nodiscard]] bool IsAllocationFailure(
    NoteRenderLayoutSnapshotBuildResult result) noexcept {
    return result == NoteRenderLayoutSnapshotBuildResult::AllocationFailure;
}

[[nodiscard]] bool IsAllocationFailure(
    NoteRenderPlacementSnapshotBuildResult result) noexcept {
    return result == NoteRenderPlacementSnapshotBuildResult::AllocationFailure;
}

} // namespace

NoteRenderFinalCompleteBuildResult NoteRenderFinalTransaction::BuildComplete(
    const NoteTextCore& textCore,
    const NoteRenderFinalCompleteBuildInput& input,
    const NoteRenderFinalMeasurementProvider& measurementProvider,
    std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept {
    if (!out) return NoteRenderFinalCompleteBuildResult::InvalidOutput;
    if (!textCore.valid()) return NoteRenderFinalCompleteBuildResult::InvalidTextCore;

    try {
        std::shared_ptr<const NoteSyntaxSnapshot> syntax;
        NoteParserCheckpointIndex checkpoint;
        const NoteSyntaxSnapshotBuildResult syntaxResult =
            NoteSyntaxSnapshot::BuildFromTextCoreWithCheckpoint(
                textCore, input.content_kind, &syntax, &checkpoint);
        if (syntaxResult != NoteSyntaxSnapshotBuildResult::Built || !syntax || !checkpoint.valid) {
            return IsAllocationFailure(syntaxResult)
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::SyntaxBuildFailed;
        }

        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan;
        const NoteRenderSourcePlanBuildResult sourcePlanResult =
            NoteRenderSourcePlan::Build(syntax, &sourcePlan);
        if (sourcePlanResult != NoteRenderSourcePlanBuildResult::Built || !sourcePlan) {
            if (sourcePlanResult == NoteRenderSourcePlanBuildResult::RawOnlyContent) {
                return NoteRenderFinalCompleteBuildResult::RequiresNativeFallback;
            }
            return IsAllocationFailure(sourcePlanResult)
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::SourcePlanBuildFailed;
        }

        NoteRenderFinalMeasuredFrame measuredFrame;
        if (!measurementProvider.Measure(*sourcePlan, input.layout_key, &measuredFrame)) {
            return NoteRenderFinalCompleteBuildResult::PlacementBuildFailed;
        }
        NoteRenderLineLayoutMap::Snapshot lineLayouts;
        if (!NoteRenderLineLayoutMap::Build(measuredFrame.line_layouts, &lineLayouts)) {
            return NoteRenderFinalCompleteBuildResult::LayoutBuildFailed;
        }
        std::shared_ptr<const NoteRenderLayoutSnapshot> layout;
        const NoteRenderLayoutSnapshotBuildResult layoutResult =
            NoteRenderLayoutSnapshot::Build(
                syntax, input.layout_key, std::move(lineLayouts), &layout);
        if (layoutResult != NoteRenderLayoutSnapshotBuildResult::Built || !layout) {
            return IsAllocationFailure(layoutResult)
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::LayoutBuildFailed;
        }

        std::shared_ptr<const NoteRenderPlacementSnapshot> placement;
        const NoteRenderPlacementSnapshotBuildResult placementResult =
            NoteRenderPlacementSnapshot::Build(
                sourcePlan, layout, std::move(measuredFrame.line_placements),
                std::move(measuredFrame.atomic_group_placements), &placement);
        if (placementResult != NoteRenderPlacementSnapshotBuildResult::Built || !placement) {
            return IsAllocationFailure(placementResult)
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::PlacementBuildFailed;
        }

        std::shared_ptr<const NoteRenderFinalPublication> publication;
        const NoteRenderFinalPublicationBuildResult publicationResult =
            NoteRenderFinalPublication::BuildWithCheckpoint(
                input.owner_input, textCore, input.layout_key, std::move(sourcePlan),
                std::move(layout), std::move(placement), checkpoint, &publication);
        if (publicationResult != NoteRenderFinalPublicationBuildResult::Built || !publication) {
            if (publicationResult == NoteRenderFinalPublicationBuildResult::AllocationFailure) {
                return NoteRenderFinalCompleteBuildResult::AllocationFailure;
            }
            return publicationResult == NoteRenderFinalPublicationBuildResult::RequiresNativeFallback
                ? NoteRenderFinalCompleteBuildResult::RequiresNativeFallback
                : NoteRenderFinalCompleteBuildResult::PublicationRejected;
        }
        *out = std::move(publication);
        return NoteRenderFinalCompleteBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalCompleteBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalCompleteBuildResult::AllocationFailure;
    }
}

NoteRenderFinalCompleteBuildResult NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
    std::shared_ptr<const NoteRenderFinalPublication> previous,
    const NoteTextCore& currentTextCore,
    const NoteRenderFinalLocalPatchInput& input,
    const NoteRenderFinalMeasurementProvider& measurementProvider,
    std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept {
    if (!out) return NoteRenderFinalCompleteBuildResult::InvalidOutput;
    if (!currentTextCore.valid()) return NoteRenderFinalCompleteBuildResult::InvalidTextCore;
    if (!previous || !previous->valid() || !previous->has_continuity_checkpoint() ||
        !previous->syntax() || !previous->source_plan() || !previous->layout() ||
        !previous->placement() || !previous->owner_plan()) {
        return NoteRenderFinalCompleteBuildResult::InvalidPreviousPublication;
    }
    if (previous->syntax()->content_kind() != NoteContentKind::Markdown ||
        !input.layout_key.valid() || previous->layout()->layout_key() != input.layout_key) {
        return NoteRenderFinalCompleteBuildResult::RequiresNativeFallback;
    }
    const NoteDerivedSnapshotIdentity previousIdentity = previous->syntax()->source_identity();
    if (!previousIdentity.valid() || previousIdentity.note_id != currentTextCore.note_id() ||
        previousIdentity.source_revision == std::numeric_limits<uint64_t>::max() ||
        currentTextCore.content_revision() != previousIdentity.source_revision + 1) {
        return NoteRenderFinalCompleteBuildResult::InvalidPreviousPublication;
    }
    if (input.edit.deleted_len == 0 && input.edit.inserted_text.empty()) {
        return NoteRenderFinalCompleteBuildResult::InvalidEdit;
    }
    // GeometryMayChange is a pre-measurement barrier.  Do not create a local
    // candidate that an owner plan is required to reject; the whole visible
    // range remains native until the next complete measurement transaction.
    if (input.owner_input.geometry_may_change) {
        return NoteRenderFinalCompleteBuildResult::RequiresNativeFallback;
    }
    const auto changedLocation = NoteSourceLineMap::FindByOffset(
        previous->syntax()->source_line_map(), input.edit.start);
    if (!changedLocation.has_value() ||
        changedLocation->line_index.value == std::numeric_limits<size_t>::max()) {
        return NoteRenderFinalCompleteBuildResult::InvalidEdit;
    }

    try {
        // Layout-key equality was proven above. This nonzero parser proof key
        // therefore represents that exact outer layout identity without
        // introducing a lossy hash from the wider render-layout value.
        constexpr NoteParserLayoutKey kExactOuterLayoutProof{1};
        std::shared_ptr<const NoteSyntaxSnapshot> syntax;
        NoteParserCheckpointIndex checkpoint;
        const NoteSyntaxSnapshotLocalPatchResult syntaxResult =
            NoteSyntaxSnapshot::BuildLocalPlainTextPatch(
                previous->syntax(), previous->continuity_checkpoint_, kExactOuterLayoutProof,
                currentTextCore, input.edit, &syntax, &checkpoint);
        if (syntaxResult != NoteSyntaxSnapshotLocalPatchResult::Built || !syntax ||
            !checkpoint.valid) {
            return syntaxResult == NoteSyntaxSnapshotLocalPatchResult::AllocationFailure
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::RequiresNativeFallback;
        }

        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan;
        const NoteRenderSourcePlanLocalPatchResult sourcePlanResult =
            NoteRenderSourcePlan::BuildLocalPlainTextPatch(
                previous->source_plan(), syntax, currentTextCore, input.edit, &sourcePlan);
        if (sourcePlanResult != NoteRenderSourcePlanLocalPatchResult::Built || !sourcePlan) {
            return sourcePlanResult == NoteRenderSourcePlanLocalPatchResult::AllocationFailure
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::RequiresNativeFallback;
        }

        const LineIndex first = changedLocation->line_index;
        const LineIndex lastExclusive{first.value + 1};
        NoteRenderFinalMeasuredLineRange measuredRange;
        if (!measurementProvider.MeasureReplacement(
                *sourcePlan, input.layout_key, first, lastExclusive, &measuredRange) ||
            measuredRange.line_layouts.size() != 1 ||
            measuredRange.line_placements.size() != 1) {
            return NoteRenderFinalCompleteBuildResult::PlacementBuildFailed;
        }

        std::shared_ptr<const NoteRenderLayoutSnapshot> layout;
        const NoteRenderLayoutSnapshotLocalSpliceResult layoutResult =
            NoteRenderLayoutSnapshot::BuildLocalSplice(
                previous->layout(), syntax, input.layout_key, first, lastExclusive,
                measuredRange.line_layouts, &layout);
        if (layoutResult != NoteRenderLayoutSnapshotLocalSpliceResult::Built || !layout) {
            return layoutResult == NoteRenderLayoutSnapshotLocalSpliceResult::AllocationFailure
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::RequiresNativeFallback;
        }

        std::shared_ptr<const NoteRenderPlacementSnapshot> placement;
        const NoteRenderPlacementSnapshotLocalSpliceResult placementResult =
            NoteRenderPlacementSnapshot::BuildLocalSplice(
                previous->placement(), sourcePlan, layout, first, lastExclusive,
                std::move(measuredRange.line_placements), &placement);
        if (placementResult != NoteRenderPlacementSnapshotLocalSpliceResult::Built || !placement) {
            return placementResult == NoteRenderPlacementSnapshotLocalSpliceResult::AllocationFailure
                ? NoteRenderFinalCompleteBuildResult::AllocationFailure
                : NoteRenderFinalCompleteBuildResult::RequiresNativeFallback;
        }

        std::shared_ptr<const NoteRenderFinalPublication> publication;
        const NoteRenderFinalPublicationBuildResult publicationResult =
            NoteRenderFinalPublication::BuildWithCheckpoint(
                input.owner_input, currentTextCore, input.layout_key, std::move(sourcePlan),
                std::move(layout), std::move(placement), checkpoint, &publication);
        if (publicationResult != NoteRenderFinalPublicationBuildResult::Built || !publication) {
            if (publicationResult == NoteRenderFinalPublicationBuildResult::AllocationFailure) {
                return NoteRenderFinalCompleteBuildResult::AllocationFailure;
            }
            return publicationResult == NoteRenderFinalPublicationBuildResult::RequiresNativeFallback
                ? NoteRenderFinalCompleteBuildResult::RequiresNativeFallback
                : NoteRenderFinalCompleteBuildResult::PublicationRejected;
        }
        *out = std::move(publication);
        return NoteRenderFinalCompleteBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalCompleteBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalCompleteBuildResult::AllocationFailure;
    }
}

} // namespace note
