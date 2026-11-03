#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace note {

// Shared applicability for settings dialogs and status-assist commands.
// Disabling a control does not clear its persisted preference.
struct NoteInteractionControlAvailability {
    bool raw_wrap = false;
    bool normal_caret_raw = false;
    bool click_enters_insert = false;
};

[[nodiscard]] NoteInteractionControlAvailability ResolveNoteInteractionControlAvailability(
    bool render_active, bool vim_enabled) noexcept;

// The frame kind describes the view-level owner. A DrawCommitted frame may
// still give a bounded line range to native input, but never mixes two text
// owners for the same line.
enum class NotePresentationFrameKind {
    NativeEditorFallback,
    DrawCommitted,
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
    SnapshotUnavailable,
};

// Win32 may retain GCS_COMPSTR briefly after GCS_RESULTSTR. Rendering must
// distinguish that acknowledged result from an editable preedit: otherwise a
// polling recovery path can incorrectly turn a committed edit back into a
// whole-document native fallback. The adapter owns the Win32 messages; this
// small state machine owns only the presentation meaning of those messages.
enum class NoteImePresentationPhase {
    Idle,
    Preedit,
    ResultCommittedAwaitingEnd,
    CancelledAwaitingEnd,
};

enum class NoteImePresentationEvent {
    StartComposition,
    ResultCommitted,
    CancelComposition,
    EndComposition,
    FocusLost,
    ObservedCompositionText,
    ObservedNoCompositionText,
};

[[nodiscard]] NoteImePresentationPhase AdvanceNoteImePresentationPhase(
    NoteImePresentationPhase phase,
    NoteImePresentationEvent event) noexcept;

[[nodiscard]] bool NoteImePresentationPhaseHasLivePreedit(
    NoteImePresentationPhase phase) noexcept;

// Client-pixel geometry only; IMM handles monitor edges and candidate size.
// Protect the whole visible input band, not the one-pixel caret column.
struct NoteImeClientRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};
struct NoteImeCandidatePlacement {
    NoteImeClientRect exclusion;
    // CFS_EXCLUDE point of interest, not the candidate window origin.
    int anchor_x = 0;
    int anchor_y = 0;
};
[[nodiscard]] std::optional<NoteImeCandidatePlacement> ResolveNoteImeCandidatePlacement(
    const NoteImeClientRect& caret,
    const NoteImeClientRect& input_band,
    const NoteImeClientRect& viewport,
    int gap_px) noexcept;

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

// The owner plan describes what paint may draw. A geometry change retains
// native ownership for the whole visible range; paint does not commit edits.
struct NotePresentationPlan {
    NotePresentationFrameKind frame_kind = NotePresentationFrameKind::NativeEditorFallback;
    NoteCaretPresenter caret_presenter = NoteCaretPresenter::NativeEditor;
    NotePresentationReason reason = NotePresentationReason::RenderingDisabled;
};

// Source/X boundary shared by the view's measured render caches.
// The cache owns storage; X can be non-monotonic for anchored runs.
struct EditorLineBoundary {
    size_t raw = 0;
    int x = 0;
};

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

// Insert-mode editing owns its caret row, or every row intersected by its
// native selection. Callers pass logical line indices captured from one
// editor snapshot.
[[nodiscard]] bool NoteEditingSelectionOwnsRawLine(bool has_selection,
                                                    int selection_start_line,
                                                    int selection_end_line,
                                                    int caret_line,
                                                    int line) noexcept;

// Normal mode is a user preference independent of Insert mode.  When the
// preference is on, only its current non-visual caret row is native raw;
// otherwise normal-mode navigation remains structured.  Visual selections
// retain their dedicated structured selection presenter.
[[nodiscard]] bool NoteNormalCaretPreferenceOwnsRawLine(bool normal_mode_active,
                                                         bool normal_visual_mode,
                                                         bool show_caret_line_raw,
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
