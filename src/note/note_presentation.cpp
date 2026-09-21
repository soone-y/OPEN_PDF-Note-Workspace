#include "note/note_presentation.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

namespace note {

NotePresentationPlan ResolveNotePresentationPlan(
    const NotePresentationFrameState& state) noexcept {
    NotePresentationPlan plan;
    if (!state.render_active) {
        plan.reason = NotePresentationReason::RenderingDisabled;
        return plan;
    }
    if (!state.editor_text_core_current) {
        plan.reason = NotePresentationReason::EditorBindingUnavailable;
        return plan;
    }
    // Keep the native IME line visible without discarding the rest of the
    // committed frame only after the adapter has proved that its composition
    // is a local, non-structural edit. Any uncertain or geometry-changing IME
    // edit retains one whole-view native owner for this frame.
    if (state.ime_composing && !state.render_cache_current &&
        (!state.ime_composition_can_reuse_committed_layout || state.line_count_may_change)) {
        plan.reason = NotePresentationReason::StaleImeComposition;
        return plan;
    }
    if (state.line_count_may_change) {
        plan.frame_kind = NotePresentationFrameKind::CommitThenDraw;
        plan.reason = NotePresentationReason::GeometryMayChange;
        plan.requires_full_repaint_after_commit = true;
        return plan;
    }
    if (state.render_cache_current) {
        plan.frame_kind = NotePresentationFrameKind::DrawCommitted;
        plan.caret_presenter = NoteCaretPresenter::CommittedLayout;
        plan.reason = NotePresentationReason::CurrentSnapshot;
        return plan;
    }
    if (state.render_cache_present && state.edit_pending) {
        // The active line can be drawn as raw text only while the committed
        // logical-line layout remains valid. The caller marks line-count and
        // derived-layout changes as a paint barrier above.
        plan.frame_kind = NotePresentationFrameKind::DrawCommitted;
        plan.reason = NotePresentationReason::SameLinePendingEdit;
        return plan;
    }
    plan.reason = NotePresentationReason::SnapshotUnavailable;
    return plan;
}

NotePresentationFrameAction ResolveNotePresentationFrameAction(
    const NotePresentationPlan& plan) noexcept {
    switch (plan.frame_kind) {
    case NotePresentationFrameKind::NativeEditorFallback:
        return NotePresentationFrameAction::RawFallback;
    case NotePresentationFrameKind::CommitThenDraw:
        return NotePresentationFrameAction::CommitBeforePaint;
    case NotePresentationFrameKind::DrawCommitted:
        return plan.reason == NotePresentationReason::SameLinePendingEdit
            ? NotePresentationFrameAction::ReuseCommittedLayout
            : NotePresentationFrameAction::RenderCurrent;
    }
    return NotePresentationFrameAction::RawFallback;
}

EditorLineLayout::EditorLineLayout(EditorLineRect line_rect,
                                   const EditorLineBoundary* boundaries,
                                   size_t boundary_count) noexcept
    : line_rect_(line_rect),
      boundaries_(boundaries),
      boundary_count_(boundary_count) {}

bool EditorLineLayout::valid() const noexcept {
    if (!boundaries_ || boundary_count_ == 0 ||
        line_rect_.right < line_rect_.left || line_rect_.bottom <= line_rect_.top) {
        return false;
    }
    for (size_t index = 1; index < boundary_count_; ++index) {
        if (boundaries_[index].raw < boundaries_[index - 1].raw) return false;
    }
    return true;
}

size_t EditorLineLayout::BoundaryIndexAtOrBefore(size_t raw_offset) const noexcept {
    if (!valid()) return 0;
    const auto first_after = std::upper_bound(
        boundaries_, boundaries_ + boundary_count_, raw_offset,
        [](size_t raw, const EditorLineBoundary& boundary) {
            return raw < boundary.raw;
        });
    return first_after == boundaries_
        ? 0
        : static_cast<size_t>(first_after - boundaries_ - 1);
}

std::optional<EditorLineCaretGeometry> EditorLineLayout::CaretAt(
    size_t raw_offset) const noexcept {
    if (!valid()) return std::nullopt;
    const EditorLineBoundary& boundary = boundaries_[BoundaryIndexAtOrBefore(raw_offset)];
    return EditorLineCaretGeometry{boundary.x, line_rect_.top, line_rect_.bottom};
}

std::optional<EditorLineRect> EditorLineLayout::TextRangeRect(
    size_t raw_start, size_t raw_end) const noexcept {
    if (!valid()) return std::nullopt;
    if (raw_end < raw_start) std::swap(raw_start, raw_end);
    const size_t first = BoundaryIndexAtOrBefore(raw_start);
    const size_t last = BoundaryIndexAtOrBefore(raw_end);
    int left = boundaries_[first].x;
    int right = left;
    for (size_t index = first; index <= last; ++index) {
        left = std::min(left, boundaries_[index].x);
        right = std::max(right, boundaries_[index].x);
    }
    if (right == left) {
        if (right < std::numeric_limits<int>::max()) {
            ++right;
        } else if (left > std::numeric_limits<int>::min()) {
            --left;
        }
    }
    return EditorLineRect{left, line_rect_.top, right, line_rect_.bottom};
}

std::optional<size_t> EditorLineLayout::HitTest(int client_x, int client_y) const noexcept {
    if (!valid() || client_y < line_rect_.top || client_y >= line_rect_.bottom) {
        return std::nullopt;
    }
    size_t best = 0;
    int64_t best_distance = std::numeric_limits<int64_t>::max();
    for (size_t index = 0; index < boundary_count_; ++index) {
        const int64_t distance = std::llabs(
            static_cast<int64_t>(boundaries_[index].x) - static_cast<int64_t>(client_x));
        if (distance < best_distance) {
            best = index;
            best_distance = distance;
        }
    }
    return boundaries_[best].raw;
}

int EditorLineLayout::MaxInlineExtent() const noexcept {
    if (!valid()) return line_rect_.left;
    int right = boundaries_[0].x;
    for (size_t index = 1; index < boundary_count_; ++index) {
        right = std::max(right, boundaries_[index].x);
    }
    return right;
}

NotePresentationFrameAction ResolveNotePresentationFrameAction(
    const NotePresentationFrameState& state) noexcept {
    return ResolveNotePresentationFrameAction(ResolveNotePresentationPlan(state));
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

NoteNativePaintScope ResolveNoteNativePaintScope(
    NotePresentationFrameAction action) noexcept {
    switch (action) {
    case NotePresentationFrameAction::RawFallback:
    case NotePresentationFrameAction::CommitBeforePaint:
        return NoteNativePaintScope::AllLines;
    case NotePresentationFrameAction::RenderCurrent:
    case NotePresentationFrameAction::ReuseCommittedLayout:
        return NoteNativePaintScope::NativeRawLinesOnly;
    }
    return NoteNativePaintScope::AllLines;
}

size_t RichEditWindowTextLengthForIndexedText(std::wstring_view indexed_text) {
    size_t line_breaks = 0;
    for (const wchar_t ch : indexed_text) {
        if (ch == L'\n') ++line_breaks;
    }
    return indexed_text.size() + line_breaks;
}

size_t CountNoteLogicalLines(std::wstring_view indexed_text) noexcept {
    size_t line_count = 1;
    for (size_t index = 0; index < indexed_text.size(); ++index) {
        const wchar_t ch = indexed_text[index];
        if (ch == L'\r') {
            ++line_count;
            if (index + 1 < indexed_text.size() && indexed_text[index + 1] == L'\n') {
                ++index;
            }
        } else if (ch == L'\n') {
            ++line_count;
        }
    }
    return line_count;
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
