#pragma once

#include <cstddef>
#include <string_view>

namespace note {

enum class NotePresentationFrameAction {
    RawFallback,
    RenderCurrent,
    ReuseCommittedLayout,
    CommitBeforePaint,
};

struct NotePresentationFrameState {
    bool render_active = false;
    bool render_cache_present = false;
    bool render_cache_current = false;
    bool edit_pending = false;
    bool line_count_may_change = false;
    bool ime_composing = false;
};

NotePresentationFrameAction ResolveNotePresentationFrameAction(
    const NotePresentationFrameState& state);

// GetWindowTextW exposes RichEdit hard line breaks as CRLF, while edit
// positions and the renderer use one indexed character per hard line break.
size_t RichEditWindowTextLengthForIndexedText(std::wstring_view indexed_text);

struct NoteDerivedRefreshRequest {
    bool render_active = false;
    bool math_pane_pinned = false;
    bool semantic_pane_active = false;
};

struct NoteDerivedRefreshPlan {
    bool refresh_syntax = false;
    bool refresh_render_plan = false;
    bool refresh_assist = true;

    bool lightweight() const {
        return !refresh_syntax && !refresh_render_plan;
    }
};

NoteDerivedRefreshPlan ResolveNoteDerivedRefreshPlan(
    const NoteDerivedRefreshRequest& request);

} // namespace note
