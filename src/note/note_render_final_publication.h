#pragma once

#include "note/note_parser_checkpoint.h"
#include "note/note_presentation_owner_plan.h"

#include <memory>

namespace note {

// Results are deliberately specific because a caller must never recover by
// composing a frame from mismatched candidates.  Every non-Built result
// means: retain no structured candidate for this frame and use the complete
// visible native fallback instead.
enum class NoteRenderFinalPublicationBuildResult {
    Built,
    InvalidOutput,
    InvalidTextCore,
    InvalidSourcePlan,
    InvalidLayoutSnapshot,
    InvalidPlacementSnapshot,
    InvalidSyntaxSnapshot,
    SourceLayoutIdentityMismatch,
    PlacementIdentityMismatch,
    StaleCanonicalRevision,
    RequiresNativeFallback,
    OwnerPartitionMismatch,
    AllocationFailure,
};

// The sole immutable structured frame candidate for the final renderer.
// It gives paint, selection, caret, hit-testing, IME anchors and scrolling
// one exact source/layout/placement/owner chain.  This module has no Win32
// or legacy-view dependency; the complete Win32 adapter publishes it
// atomically in place of the retired legacy route.
class NoteRenderFinalPublication final {
public:
    [[nodiscard]] static NoteRenderFinalPublicationBuildResult Build(
        const NotePresentationOwnerInput& ownerInput,
        const NoteTextCore& textCore,
        const NoteRenderLayoutKey& layoutKey,
        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
        std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
        std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
        std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::shared_ptr<const NoteSyntaxSnapshot>& syntax() const noexcept {
        return syntax_;
    }
    [[nodiscard]] const std::shared_ptr<const NoteRenderSourcePlan>& source_plan() const noexcept {
        return source_plan_;
    }
    [[nodiscard]] const std::shared_ptr<const NoteRenderLayoutSnapshot>& layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] const std::shared_ptr<const NoteRenderPlacementSnapshot>& placement() const noexcept {
        return placement_;
    }
    [[nodiscard]] const std::shared_ptr<const NotePresentationOwnerPlan>& owner_plan() const noexcept {
        return owner_plan_;
    }
    // Only publications built by NoteRenderFinalTransaction carry the
    // checkpoint needed to prove a later local patch. Direct component tests
    // may build an identity-checked frame without continuity state, but that
    // frame is not eligible for the final local transaction.
    [[nodiscard]] bool has_continuity_checkpoint() const noexcept {
        return has_continuity_checkpoint_;
    }

    // Identity is checked at the publication boundary rather than left to a
    // paint/input consumer.  A candidate that ceases to match is unusable as
    // a whole; callers must not retain individual child snapshots.
    [[nodiscard]] bool Matches(const NoteTextCore& textCore,
                               const NoteRenderLayoutKey& layoutKey) const noexcept;

private:
    friend class NoteRenderFinalTransaction;

    [[nodiscard]] static NoteRenderFinalPublicationBuildResult BuildWithCheckpoint(
        const NotePresentationOwnerInput& ownerInput,
        const NoteTextCore& textCore,
        const NoteRenderLayoutKey& layoutKey,
        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
        std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
        std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
        const NoteParserCheckpointIndex& checkpoint,
        std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept;
    [[nodiscard]] static NoteRenderFinalPublicationBuildResult BuildImpl(
        const NotePresentationOwnerInput& ownerInput,
        const NoteTextCore& textCore,
        const NoteRenderLayoutKey& layoutKey,
        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
        std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
        std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
        const NoteParserCheckpointIndex* checkpoint,
        std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept;

    std::shared_ptr<const NoteSyntaxSnapshot> syntax_;
    std::shared_ptr<const NoteRenderSourcePlan> source_plan_;
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout_;
    std::shared_ptr<const NoteRenderPlacementSnapshot> placement_;
    std::shared_ptr<const NotePresentationOwnerPlan> owner_plan_;
    NoteParserCheckpointIndex continuity_checkpoint_;
    bool has_continuity_checkpoint_ = false;
    bool valid_ = false;
};

} // namespace note
