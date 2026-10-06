#include "note/note_render_final_publication.h"

#include <exception>
#include <new>
#include <utility>

namespace note {
namespace {

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

[[nodiscard]] bool HasExactPublicationChain(
    const NoteSyntaxSnapshot& syntax,
    const NoteRenderSourcePlan& sourcePlan,
    const NoteRenderLayoutSnapshot& layout,
    const NoteRenderPlacementSnapshot& placement,
    const NotePresentationOwnerPlan& ownerPlan) noexcept {
    return sourcePlan.syntax().get() == &syntax && layout.syntax().get() == &syntax &&
           placement.source_plan().get() == &sourcePlan && placement.layout().get() == &layout &&
           ownerPlan.placement().get() == &placement;
}

[[nodiscard]] bool HasExactCheckpoint(
    const NoteParserCheckpointIndex& checkpoint,
    const NoteSyntaxSnapshot& syntax) noexcept {
    return checkpoint.valid && checkpoint.source_identity == syntax.source_identity() &&
           NoteTextPieceSequence::SameSnapshotIdentity(
               checkpoint.canonical_source_root, syntax.source_root()) &&
           NoteSourceLineMap::SameSnapshotIdentity(
               checkpoint.source_line_map, syntax.source_line_map()) &&
           checkpoint.line_count() == syntax.source_line_map().line_count();
}

} // namespace

NoteRenderFinalPublicationBuildResult NoteRenderFinalPublication::Build(
    const NotePresentationOwnerInput& ownerInput,
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey,
    std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
    std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
    std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept {
    return BuildImpl(ownerInput, textCore, layoutKey, std::move(sourcePlan),
                     std::move(layout), std::move(placement), nullptr, out);
}

NoteRenderFinalPublicationBuildResult NoteRenderFinalPublication::BuildWithCheckpoint(
    const NotePresentationOwnerInput& ownerInput,
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey,
    std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
    std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
    const NoteParserCheckpointIndex& checkpoint,
    std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept {
    return BuildImpl(ownerInput, textCore, layoutKey, std::move(sourcePlan),
                     std::move(layout), std::move(placement), &checkpoint, out);
}

NoteRenderFinalPublicationBuildResult NoteRenderFinalPublication::BuildImpl(
    const NotePresentationOwnerInput& ownerInput,
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey,
    std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
    std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
    const NoteParserCheckpointIndex* checkpoint,
    std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept {
    if (!out) return NoteRenderFinalPublicationBuildResult::InvalidOutput;
    if (!textCore.valid()) return NoteRenderFinalPublicationBuildResult::InvalidTextCore;
    if (!sourcePlan || !sourcePlan->valid()) {
        return NoteRenderFinalPublicationBuildResult::InvalidSourcePlan;
    }
    if (!layout || !layout->valid()) {
        return NoteRenderFinalPublicationBuildResult::InvalidLayoutSnapshot;
    }
    if (!placement || !placement->valid()) {
        return NoteRenderFinalPublicationBuildResult::InvalidPlacementSnapshot;
    }
    const std::shared_ptr<const NoteSyntaxSnapshot>& syntax = sourcePlan->syntax();
    if (!syntax || !syntax->valid()) {
        return NoteRenderFinalPublicationBuildResult::InvalidSyntaxSnapshot;
    }
    if (checkpoint && !HasExactCheckpoint(*checkpoint, *syntax)) {
        return NoteRenderFinalPublicationBuildResult::InvalidSyntaxSnapshot;
    }
    if (layout->syntax() != syntax) {
        return NoteRenderFinalPublicationBuildResult::SourceLayoutIdentityMismatch;
    }
    if (placement->source_plan() != sourcePlan || placement->layout() != layout) {
        return NoteRenderFinalPublicationBuildResult::PlacementIdentityMismatch;
    }
    if (!syntax->MatchesTextCore(textCore) || !sourcePlan->Matches(textCore) ||
        !layout->Matches(textCore, layoutKey) || !placement->Matches(textCore, layoutKey)) {
        return NoteRenderFinalPublicationBuildResult::StaleCanonicalRevision;
    }

    try {
        std::shared_ptr<const NotePresentationOwnerPlan> ownerPlan;
        const NotePresentationOwnerPlanBuildResult ownerResult =
            NotePresentationOwnerPlan::Build(ownerInput, textCore, layoutKey, placement, &ownerPlan);
        if (ownerResult == NotePresentationOwnerPlanBuildResult::AllocationFailure) {
            return NoteRenderFinalPublicationBuildResult::AllocationFailure;
        }
        if (ownerResult != NotePresentationOwnerPlanBuildResult::Built || !ownerPlan ||
            !ownerPlan->valid() ||
            ownerPlan->frame().frame_kind != NotePresentationFrameKind::DrawCommitted) {
            return NoteRenderFinalPublicationBuildResult::RequiresNativeFallback;
        }
        if (!HasExactPublicationChain(*syntax, *sourcePlan, *layout, *placement, *ownerPlan) ||
            !ownerPlan->Matches(textCore, layoutKey) ||
            !IsExactOwnerPartition(ownerPlan->owner_ranges(), ownerInput.visible_lines)) {
            return NoteRenderFinalPublicationBuildResult::OwnerPartitionMismatch;
        }

        std::shared_ptr<NoteRenderFinalPublication> candidate(new NoteRenderFinalPublication());
        candidate->syntax_ = std::move(syntax);
        candidate->source_plan_ = std::move(sourcePlan);
        candidate->layout_ = std::move(layout);
        candidate->placement_ = std::move(placement);
        candidate->owner_plan_ = std::move(ownerPlan);
        if (checkpoint) {
            candidate->continuity_checkpoint_ = *checkpoint;
            candidate->has_continuity_checkpoint_ = true;
        }
        candidate->valid_ = true;
        *out = std::move(candidate);
        return NoteRenderFinalPublicationBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalPublicationBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalPublicationBuildResult::AllocationFailure;
    }
}

bool NoteRenderFinalPublication::Matches(
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey) const noexcept {
    return valid_ && syntax_ && source_plan_ && layout_ && placement_ && owner_plan_ &&
           syntax_->MatchesTextCore(textCore) && source_plan_->Matches(textCore) &&
           layout_->Matches(textCore, layoutKey) && placement_->Matches(textCore, layoutKey) &&
           owner_plan_->Matches(textCore, layoutKey) &&
           (!has_continuity_checkpoint_ || HasExactCheckpoint(continuity_checkpoint_, *syntax_)) &&
           HasExactPublicationChain(*syntax_, *source_plan_, *layout_, *placement_, *owner_plan_);
}

} // namespace note
