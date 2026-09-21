#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace note {

enum class NotePresentationFrameAction {
    RawFallback,
    RenderCurrent,
    // A same-logical-line edit keeps the last committed line geometry while
    // the active raw editor line is visible. Structural edits use
    // CommitBeforePaint instead.
    ReuseCommittedLayout,
    CommitBeforePaint,
};

// The frame kind describes the view-level owner. A DrawCommitted frame may
// still expose a bounded EditorLine range for native input, but never mixes
// two text owners for that same line.
enum class NotePresentationFrameKind {
    NativeEditorFallback,
    DrawCommitted,
    CommitThenDraw,
};

enum class NoteCaretPresenter {
    NativeEditor,
    CommittedLayout,
};

enum class NotePresentationReason {
    RenderingDisabled,
    EditorBindingUnavailable,
    StaleImeComposition,
    GeometryMayChange,
    CurrentSnapshot,
    SameLinePendingEdit,
    SnapshotUnavailable,
};

// Exactly one surface owns the visible text for each logical line in a frame.
// NativeRaw leaves the captured RichEdit text visible, OverlayRaw repaints the
// current raw text itself, and Structured paints the committed render cache.
// Keeping this decision explicit prevents an old structured run from being
// drawn over a raw editing line.
enum class NotePresentationLineSurface {
    NativeRaw,
    OverlayRaw,
    Structured,
};

// Native RichEdit drawing is retained only as an input/IME transport.  A
// fallback frame needs every line; a structured frame needs at most the lines
// whose sole presentation owner is NativeRaw.
enum class NoteNativePaintScope {
    AllLines,
    NativeRawLinesOnly,
};

struct NotePresentationFrameState {
    bool render_active = false;
    bool render_cache_present = false;
    bool render_cache_current = false;
    bool edit_pending = false;
    bool line_count_may_change = false;
    bool ime_composing = false;
    // The UI adapter publishes this only after an exact editor-to-TextCore
    // comparison for the current document revision. A missing binding cannot
    // safely reuse canonical line geometry in a hybrid frame.
    bool editor_text_core_current = true;
    // The Win32 adapter proves this only when the live composition is confined
    // to its current logical line and no structural/geometry-sensitive edit
    // has occurred since the committed render snapshot.
    bool ime_composition_can_reuse_committed_layout = false;
};

struct NotePresentationPlan {
    NotePresentationFrameKind frame_kind = NotePresentationFrameKind::NativeEditorFallback;
    NoteCaretPresenter caret_presenter = NoteCaretPresenter::NativeEditor;
    NotePresentationReason reason = NotePresentationReason::RenderingDisabled;
    bool requires_full_repaint_after_commit = false;

    [[nodiscard]] bool uses_committed_layout() const noexcept {
        return frame_kind == NotePresentationFrameKind::DrawCommitted;
    }
};

// Win32-independent geometry for one already-measured logical line. The
// caller owns the boundary storage for the duration of this value's use; the
// layout never copies or mutates it. Boundaries are source offsets in
// nondecreasing order and may have non-monotonic X values for anchored runs.
struct EditorLineBoundary {
    size_t raw = 0;
    int x = 0;
};

struct EditorLineRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct EditorLineCaretGeometry {
    int x = 0;
    int top = 0;
    int bottom = 0;
};

class EditorLineLayout {
public:
    EditorLineLayout(EditorLineRect line_rect,
                     const EditorLineBoundary* boundaries,
                     size_t boundary_count) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    // Returns the conservative enclosing rectangle. Anchored display runs can
    // be non-monotonic in X, so selection rendering that needs disjoint
    // regions must split its source range before calling this primitive.
    [[nodiscard]] std::optional<EditorLineRect> TextRangeRect(
        size_t raw_start, size_t raw_end) const noexcept;
    [[nodiscard]] std::optional<EditorLineCaretGeometry> CaretAt(
        size_t raw_offset) const noexcept;
    [[nodiscard]] std::optional<size_t> HitTest(int client_x, int client_y) const noexcept;
    [[nodiscard]] int MaxInlineExtent() const noexcept;

private:
    [[nodiscard]] size_t BoundaryIndexAtOrBefore(size_t raw_offset) const noexcept;

    EditorLineRect line_rect_{};
    const EditorLineBoundary* boundaries_ = nullptr;
    size_t boundary_count_ = 0;
};

[[nodiscard]] NotePresentationPlan ResolveNotePresentationPlan(
    const NotePresentationFrameState& state) noexcept;

// Transitional adapter for existing call sites. NotePresentationPlan remains
// the single owner of frame semantics; this action is derived from it.
[[nodiscard]] NotePresentationFrameAction ResolveNotePresentationFrameAction(
    const NotePresentationPlan& plan) noexcept;

[[nodiscard]] NotePresentationFrameAction ResolveNotePresentationFrameAction(
    const NotePresentationFrameState& state) noexcept;

NotePresentationLineSurface ResolveNotePresentationLineSurface(
    bool native_raw_requested,
    bool raw_surface_suppressed,
    bool stale_editing_line);

// A NativeRaw surface delegates both painting and vertical line metrics to
// RichEdit. OverlayRaw keeps the committed structured geometry because it is
// drawn into that geometry by the presentation layer.
[[nodiscard]] bool NotePresentationSurfaceUsesNativeLineMetrics(
    NotePresentationLineSurface surface) noexcept;

// Both source spans and non-empty selections are half-open ranges.  A
// collapsed selection is a caret: it touches a span at its start, but not at
// its exclusive end.  This keeps a caret immediately after a rendered block
// from claiming that block's raw editing surface.
[[nodiscard]] bool NoteSelectionTouchesSpan(Span span,
                                            size_t selection_start,
                                            size_t selection_end) noexcept;

// A pointer click that has moved a collapsed editing caret must reveal its
// source line immediately.  Normal-mode and selection gestures keep their
// dedicated presentation owners.
[[nodiscard]] bool NotePointerMovedCaretOwnsRawLine(bool pointer_moved_caret,
                                                     bool editor_has_focus,
                                                     bool normal_mode_active,
                                                     bool has_selection,
                                                     int line,
                                                     int caret_line) noexcept;

// TeX source is a math-only presentation route: when structured rendering is
// enabled it renders completed TeX formulae even if the optional Markdown
// math switch is off. Markdown keeps that switch as an explicit preference.
[[nodiscard]] bool NoteMathRenderEnabledForRoute(bool render_active,
                                                  bool math_enabled,
                                                  bool tex_math_only_route) noexcept;

// The Win32 adapter measures each table cell with the active font, then passes
// only pixel measurements to this pure layout core.  Every structured table
// consumer (paint, selection, caret, hit testing, and horizontal extent) uses
// the same result rather than deriving its own column positions.
struct NoteTableColumnMeasure {
    size_t column_index = 0;
    int content_width_px = 0;
};

struct NoteTableLayoutInput {
    size_t column_count = 0;
    int minimum_content_width_px = 0;
    int cell_horizontal_padding_px = 0;
    int border_width_px = 1;
    std::vector<NoteTableColumnMeasure> cell_measures;
};

struct NoteTableColumnLayout {
    // X offsets relative to the table's left edge.  left_border_x_px is the
    // border before this cell; right_border_x_px is the shared border after it.
    int left_border_x_px = 0;
    int content_x_px = 0;
    int content_width_px = 0;
    int right_border_x_px = 0;
};

struct NoteTableLayout {
    bool valid = false;
    int total_width_px = 0;
    std::vector<NoteTableColumnLayout> columns;
};

// Rejects malformed or unrepresentable dimensions instead of wrapping pixel
// arithmetic.  The caller must use native raw presentation when valid is false.
[[nodiscard]] NoteTableLayout ResolveNoteTableLayout(const NoteTableLayoutInput& input);

enum class NoteTableCellAlignment {
    Left,
    Center,
    Right,
};

// Returns the additional offset inside a validated cell.  Oversized content
// is never shifted outside the cell; the table's measured column is its bound.
[[nodiscard]] int ResolveNoteTableCellContentOffset(const NoteTableColumnLayout& column,
                                                     int content_width_px,
                                                     NoteTableCellAlignment alignment) noexcept;

// Markdown's delimiter row is source syntax, not a visual table row.  Its
// structured height must match one shared grid border; invalid widths reject
// the compact presentation rather than creating an unbounded gap.
[[nodiscard]] int ResolveNoteTableDividerHeightPx(int border_width_px) noexcept;

[[nodiscard]] NoteNativePaintScope ResolveNoteNativePaintScope(
    NotePresentationFrameAction action) noexcept;

// GetWindowTextW exposes RichEdit hard line breaks as CRLF, while edit
// positions and the renderer use one indexed character per hard line break.
size_t RichEditWindowTextLengthForIndexedText(std::wstring_view indexed_text);

// Counts hard logical lines without allocating per-line strings. CRLF is one
// break, as are lone CR and LF characters. An empty document is one line.
[[nodiscard]] size_t CountNoteLogicalLines(std::wstring_view indexed_text) noexcept;

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
