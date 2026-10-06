#pragma once

#include "note/note_presentation.h"
#include "note/note_render_placement_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace note {

struct NotePresentationLineRange {
    LineIndex first{};
    LineIndex last_exclusive{};
};

[[nodiscard]] bool NotePresentationLineRangeIsValid(NotePresentationLineRange range,
                                                     size_t lineCount) noexcept;

enum class NotePresentationLineOwner : uint8_t {
    NativeEditor,
    CommittedPlacement,
};

struct NotePresentationOwnerRange {
    NotePresentationLineRange lines{};
    NotePresentationLineOwner owner = NotePresentationLineOwner::NativeEditor;
};

// Development-time accounting for the atomic-group expansion performed while
// resolving one owner plan.  It records interval-index work rather than the
// document line count, so long-document regressions can prove that a caret
// row does not enumerate unrelated tables, formulas, or containers.
struct NotePresentationOwnerPlanBuildWork {
    size_t atomic_group_queries = 0;
    size_t atomic_group_index_nodes = 0;
};

// This input contains only event-normalized facts. In particular, it does
// not accept a legacy cache-valid flag: candidate identity is verified from
// NoteRenderPlacementSnapshot::Matches before structured ownership is allowed.
struct NotePresentationOwnerInput {
    bool render_active = false;
    bool editor_text_core_current = false;
    bool geometry_may_change = false;
    bool ime_preedit = false;
    bool ime_preedit_can_reuse_committed_layout = false;
    NotePresentationLineRange visible_lines{};
    std::vector<NotePresentationLineRange> requested_editor_lines;
    size_t caret_line = static_cast<size_t>(-1);
};

enum class NotePresentationOwnerPlanBuildResult {
    Built,
    InvalidOutput,
    InvalidVisibleRange,
    InvariantViolation,
    AllocationFailure,
};

// Final line-owner plan. A DrawCommitted plan owns the exact placement
// snapshot it validated; NativeEditor and CommittedPlacement ranges form a
// disjoint, gap-free partition of visible_lines. Table and block-math groups
// are expanded before partitioning, so their geometry is never mixed with a
// raw subset. The final Win32 adapter publishes this plan atomically with
// its source, layout, and placement snapshots; legacy view ownership never
// consumes it as a partial migration artifact.
class NotePresentationOwnerPlan final {
public:
    [[nodiscard]] static NotePresentationOwnerPlanBuildResult Build(
        const NotePresentationOwnerInput& input,
        const NoteTextCore& textCore,
        const NoteRenderLayoutKey& layoutKey,
        std::shared_ptr<const NoteRenderPlacementSnapshot> placement,
        std::shared_ptr<const NotePresentationOwnerPlan>* out,
        NotePresentationOwnerPlanBuildWork* out_work = nullptr) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const NotePresentationPlan& frame() const noexcept { return frame_; }
    [[nodiscard]] const std::vector<NotePresentationOwnerRange>& owner_ranges() const noexcept {
        return owner_ranges_;
    }
    [[nodiscard]] const std::shared_ptr<const NoteRenderPlacementSnapshot>& placement() const noexcept {
        return placement_;
    }
    [[nodiscard]] bool Matches(const NoteTextCore& textCore,
                               const NoteRenderLayoutKey& layoutKey) const noexcept;

private:
    NotePresentationPlan frame_{};
    std::vector<NotePresentationOwnerRange> owner_ranges_;
    std::shared_ptr<const NoteRenderPlacementSnapshot> placement_;
    bool valid_ = false;
};

} // namespace note
