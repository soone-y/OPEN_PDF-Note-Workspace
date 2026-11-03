#include "note/note_presentation.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace note {

NoteInteractionControlAvailability ResolveNoteInteractionControlAvailability(
    bool renderActive, bool vimEnabled) noexcept {
    return {!renderActive, renderActive && vimEnabled, vimEnabled};
}

NoteImePresentationPhase AdvanceNoteImePresentationPhase(
    NoteImePresentationPhase phase,
    NoteImePresentationEvent event) noexcept {
    switch (event) {
    case NoteImePresentationEvent::StartComposition:
        return NoteImePresentationPhase::Preedit;
    case NoteImePresentationEvent::ResultCommitted:
        return NoteImePresentationPhase::ResultCommittedAwaitingEnd;
    case NoteImePresentationEvent::CancelComposition:
        return NoteImePresentationPhase::CancelledAwaitingEnd;
    case NoteImePresentationEvent::EndComposition:
    case NoteImePresentationEvent::FocusLost:
        return NoteImePresentationPhase::Idle;
    case NoteImePresentationEvent::ObservedNoCompositionText:
        // An empty preedit is not an end/result event: the IME may still own
        // a selected replacement. Accepting that temporary deletion into the
        // canonical document would corrupt history before commit/cancel.
        return phase;
    case NoteImePresentationEvent::ObservedCompositionText:
        // GCS_RESULTSTR is an acknowledgement that the source text is no
        // longer provisional. Some IMEs retain GCS_COMPSTR until END; that
        // residue must not revive Preedit or suppress the final render pass.
        // A flag-free cancellation has the same terminal ownership rule.
        return phase == NoteImePresentationPhase::ResultCommittedAwaitingEnd ||
               phase == NoteImePresentationPhase::CancelledAwaitingEnd
            ? phase
            : NoteImePresentationPhase::Preedit;
    }
    return NoteImePresentationPhase::Idle;
}

bool NoteImePresentationPhaseHasLivePreedit(
    NoteImePresentationPhase phase) noexcept {
    return phase == NoteImePresentationPhase::Preedit;
}

std::optional<NoteImeCandidatePlacement> ResolveNoteImeCandidatePlacement(
    const NoteImeClientRect& caret,
    const NoteImeClientRect& input_band,
    const NoteImeClientRect& viewport,
    int gap_px) noexcept {
    if (gap_px < 0 || viewport.right <= viewport.left || viewport.bottom <= viewport.top ||
        caret.right <= caret.left || caret.bottom <= caret.top ||
        caret.top >= viewport.bottom || caret.bottom <= viewport.top ||
        input_band.top > caret.top || input_band.bottom < caret.bottom) return std::nullopt;
    const auto pixel = [](int64_t value) noexcept {
        return static_cast<int>(std::clamp<int64_t>(
            value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    };
    NoteImeCandidatePlacement result;
    result.exclusion = {viewport.left,
        pixel(static_cast<int64_t>(std::max(viewport.top, input_band.top)) - gap_px),
        viewport.right,
        pixel(static_cast<int64_t>(std::min(viewport.bottom, input_band.bottom)) + gap_px)};
    result.anchor_x = std::clamp(caret.left, viewport.left, viewport.right - 1);
    // CFS_EXCLUDE takes a point of interest (the caret), NOT the candidate
    // window's upper-left corner. Keep that point inside the visible client
    // even at its last row; only the exclusion retains clearance beyond it.
    // IMM owns the candidate size and above/below choice at monitor edges.
    result.anchor_y = std::clamp(caret.top, viewport.top, viewport.bottom - 1);
    return result;
}

NotePresentationLineSurface ResolveNotePresentationLineSurface(
    bool native_raw_requested,
    bool raw_surface_suppressed,
    bool stale_editing_line) {
    // Native raw is the highest-priority owner because RichEdit supplies its
    // caret, selection, IME, and soft-wrap behavior on that surface.
    if (native_raw_requested && !raw_surface_suppressed) {
        return NotePresentationLineSurface::NativeRaw;
    }
    if (stale_editing_line) {
        return NotePresentationLineSurface::OverlayRaw;
    }
    return NotePresentationLineSurface::Structured;
}

bool NotePresentationSurfaceUsesNativeLineMetrics(
    NotePresentationLineSurface surface) noexcept {
    return surface == NotePresentationLineSurface::NativeRaw;
}

bool NoteSelectionTouchesSpan(Span span,
                              size_t selection_start,
                              size_t selection_end) noexcept {
    if (span.end.value <= span.start.value) return false;
    if (selection_end < selection_start) std::swap(selection_start, selection_end);
    if (selection_start == selection_end) {
        return span.start.value <= selection_start && selection_start < span.end.value;
    }
    return selection_start < span.end.value && selection_end > span.start.value;
}

bool NotePointerMovedCaretOwnsRawLine(bool pointer_moved_caret,
                                      bool editor_has_focus,
                                      bool normal_mode_active,
                                      bool has_selection,
                                      int line,
                                      int caret_line) noexcept {
    return pointer_moved_caret && editor_has_focus && !normal_mode_active &&
        !has_selection && line >= 0 && line == caret_line;
}

bool NoteEditingSelectionOwnsRawLine(bool has_selection,
                                     int selection_start_line,
                                     int selection_end_line,
                                     int caret_line,
                                     int line) noexcept {
    if (line < 0) return false;
    if (!has_selection) return line == caret_line;
    if (selection_end_line < selection_start_line) {
        std::swap(selection_start_line, selection_end_line);
    }
    return line >= selection_start_line && line <= selection_end_line;
}

bool NoteNormalCaretPreferenceOwnsRawLine(bool normal_mode_active,
                                          bool normal_visual_mode,
                                          bool show_caret_line_raw,
                                          int line,
                                          int caret_line) noexcept {
    return normal_mode_active && !normal_visual_mode && show_caret_line_raw &&
        line >= 0 && line == caret_line;
}

bool NoteMathRenderEnabledForRoute(bool render_active,
                                   bool math_enabled,
                                   bool tex_math_only_route) noexcept {
    return render_active && (math_enabled || tex_math_only_route);
}

NoteTableLayout ResolveNoteTableLayout(const NoteTableLayoutInput& input) {
    NoteTableLayout out;
    constexpr size_t kMaximumColumns = 256;
    if (input.column_count == 0 || input.column_count > kMaximumColumns ||
        input.minimum_content_width_px < 0 || input.cell_horizontal_padding_px < 0 ||
        input.border_width_px <= 0) {
        return out;
    }

    std::vector<int> contentWidths(input.column_count, input.minimum_content_width_px);
    for (const NoteTableColumnMeasure& measure : input.cell_measures) {
        if (measure.column_index >= input.column_count || measure.content_width_px < 0) {
            return {};
        }
        contentWidths[measure.column_index] = std::max(
            contentWidths[measure.column_index], measure.content_width_px);
    }

    const int64_t padding = static_cast<int64_t>(input.cell_horizontal_padding_px);
    const int64_t border = static_cast<int64_t>(input.border_width_px);
    int64_t cursor = border; // Leading outer border.
    out.columns.reserve(input.column_count);
    for (int contentWidth : contentWidths) {
        const int64_t width = static_cast<int64_t>(contentWidth);
        const int64_t contentX = cursor + padding;
        const int64_t rightBorder = contentX + width + padding;
        const int64_t nextCursor = rightBorder + border;
        if (contentX > std::numeric_limits<int>::max() ||
            rightBorder > std::numeric_limits<int>::max() ||
            nextCursor > std::numeric_limits<int>::max()) {
            return {};
        }
        out.columns.push_back(NoteTableColumnLayout{
            static_cast<int>(cursor - border),
            static_cast<int>(contentX),
            contentWidth,
            static_cast<int>(rightBorder),
        });
        cursor = nextCursor;
    }
    out.total_width_px = static_cast<int>(cursor);
    out.valid = true;
    return out;
}

int ResolveNoteTableCellContentOffset(const NoteTableColumnLayout& column,
                                      int content_width_px,
                                      NoteTableCellAlignment alignment) noexcept {
    if (column.content_width_px <= 0 || content_width_px <= 0 ||
        content_width_px >= column.content_width_px) {
        return 0;
    }
    const int remaining = column.content_width_px - content_width_px;
    switch (alignment) {
    case NoteTableCellAlignment::Center:
        return remaining / 2;
    case NoteTableCellAlignment::Right:
        return remaining;
    case NoteTableCellAlignment::Left:
    default:
        return 0;
    }
}

int ResolveNoteTableDividerHeightPx(int border_width_px) noexcept {
    return border_width_px > 0 ? border_width_px : 0;
}

size_t RichEditWindowTextLengthForIndexedText(std::wstring_view indexed_text) {
    size_t line_breaks = 0;
    for (const wchar_t ch : indexed_text) {
        if (ch == L'\n') ++line_breaks;
    }
    return indexed_text.size() + line_breaks;
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
