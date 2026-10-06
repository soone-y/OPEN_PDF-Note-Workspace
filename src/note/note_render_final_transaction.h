#pragma once

#include "note/note_render_final_publication.h"

#include <memory>
#include <vector>

namespace note {

// Input to the only complete-build entry point for a final structured frame.
// It deliberately carries no child snapshot or geometry: those values are
// constructed only inside the transaction after its source plan is exact.
struct NoteRenderFinalCompleteBuildInput {
    NoteContentKind content_kind = NoteContentKind::Markdown;
    NoteRenderLayoutKey layout_key{};
    NotePresentationOwnerInput owner_input{};
};

// The Win32 adapter owns GDI measurement, but it returns only immutable value
// data. It must not retain `source_plan`; the transaction validates the
// returned line counts, runs, and boundaries before publication.
struct NoteRenderFinalMeasuredFrame {
    std::vector<NoteRenderLineLayout> line_layouts;
    std::vector<NoteRenderLinePlacement> line_placements;
    std::vector<NoteRenderAtomicGroupPlacement> atomic_group_placements;
};

// Measurement for an already proven local replacement.  The transaction
// supplies the exact row interval; returning any other count rejects the
// candidate instead of silently broadening a local edit into a full frame.
struct NoteRenderFinalMeasuredLineRange {
    std::vector<NoteRenderLineLayout> line_layouts;
    std::vector<NoteRenderLinePlacement> line_placements;
};

class NoteRenderFinalMeasurementProvider {
public:
    virtual ~NoteRenderFinalMeasurementProvider() = default;

    [[nodiscard]] virtual bool Measure(
        const NoteRenderSourcePlan& source_plan,
        const NoteRenderLayoutKey& layout_key,
        NoteRenderFinalMeasuredFrame* out) const noexcept = 0;

    [[nodiscard]] virtual bool MeasureReplacement(
        const NoteRenderSourcePlan& source_plan,
        const NoteRenderLayoutKey& layout_key,
        LineIndex first_line,
        LineIndex last_line_exclusive,
        NoteRenderFinalMeasuredLineRange* out) const noexcept = 0;
};

// A local final transaction cannot choose a different content kind or parser
// configuration: both are inherited from its previous complete publication.
// It carries only the current layout/owner facts and one canonical edit.
struct NoteRenderFinalLocalPatchInput {
    NoteRenderLayoutKey layout_key{};
    NotePresentationOwnerInput owner_input{};
    TextEdit edit{};
};

enum class NoteRenderFinalCompleteBuildResult {
    Built,
    InvalidOutput,
    InvalidTextCore,
    InvalidPreviousPublication,
    InvalidEdit,
    SyntaxBuildFailed,
    SourcePlanBuildFailed,
    LayoutBuildFailed,
    PlacementBuildFailed,
    RequiresNativeFallback,
    PublicationRejected,
    AllocationFailure,
};

// Builds one complete syntax/source-plan/layout/placement/owner publication
// for one immutable NoteTextCore revision. No child candidate is exposed on
// failure, so callers cannot turn a partial build into a migration path.
// The Win32 adapter uses this pure core only through its atomic publication
// boundary; no legacy renderer can observe an individual child candidate.
class NoteRenderFinalTransaction final {
public:
    [[nodiscard]] static NoteRenderFinalCompleteBuildResult BuildComplete(
        const NoteTextCore& textCore,
        const NoteRenderFinalCompleteBuildInput& input,
        const NoteRenderFinalMeasurementProvider& measurement_provider,
        std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept;

    // Replaces a marker-free plain-text row only when the previous complete
    // publication carries the exact checkpoint lineage. Syntax, source plan,
    // layout, placement, and owner are rebuilt into local candidates and are
    // published together. Any unproved edit returns RequiresNativeFallback
    // with `out` unchanged; callers may never reuse one child on its own.
    [[nodiscard]] static NoteRenderFinalCompleteBuildResult BuildLocalPlainTextPatch(
        std::shared_ptr<const NoteRenderFinalPublication> previous,
        const NoteTextCore& current_text_core,
        const NoteRenderFinalLocalPatchInput& input,
        const NoteRenderFinalMeasurementProvider& measurement_provider,
        std::shared_ptr<const NoteRenderFinalPublication>* out) noexcept;
};

} // namespace note
