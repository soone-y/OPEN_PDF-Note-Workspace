#include "note/note_presentation.h"

namespace note {

NotePresentationFrameAction ResolveNotePresentationFrameAction(
    const NotePresentationFrameState& state) {
    if (!state.render_active) return NotePresentationFrameAction::RawFallback;
    if (state.ime_composing && !state.render_cache_current) {
        return NotePresentationFrameAction::RawFallback;
    }
    if (state.line_count_may_change) {
        return NotePresentationFrameAction::CommitBeforePaint;
    }
    if (state.render_cache_current) {
        return NotePresentationFrameAction::RenderCurrent;
    }
    if (state.render_cache_present && state.edit_pending) {
        return NotePresentationFrameAction::ReuseCommittedLayout;
    }
    return NotePresentationFrameAction::RawFallback;
}

NoteDerivedRefreshPlan ResolveNoteDerivedRefreshPlan(
    const NoteDerivedRefreshRequest& request) {
    NoteDerivedRefreshPlan plan;
    plan.refresh_render_plan =
        request.render_active || request.math_pane_pinned;
    plan.refresh_syntax =
        plan.refresh_render_plan || request.semantic_pane_active;
    return plan;
}

} // namespace note
