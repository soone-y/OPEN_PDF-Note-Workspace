#pragma once

#include "note/note_render_final_publication.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace note {

// Pure interaction data for the final view adapter. Coordinates are content
// coordinates: the Win32 adapter applies scroll origin and client clipping at
// its edge, never while resolving source/display correspondence.
struct NoteRenderFinalRect {
    int left_px = 0;
    uint64_t top_px = 0;
    int right_px = 0;
    uint64_t bottom_px = 0;
};

struct NoteRenderFinalResolvedLine {
    LineIndex line_index{};
    NoteRenderLineLayoutLocation layout{};
    NoteRenderSourceLinePlan source_line{};
    NoteRenderLinePlacement placement_line{};
};

struct NoteRenderFinalCaretGeometry {
    LineIndex line_index{};
    Utf16CodeUnitOffset source_offset{};
    int x_px = 0;
    uint64_t top_px = 0;
    uint64_t bottom_px = 0;
};

// A soft-wrap source boundary has two valid visual sites. The adapter chooses
// the side matching its input direction instead of borrowing a native-editor
// coordinate from a different presentation owner.
enum class NoteRenderFinalCaretAffinity {
    BeforeVisualWrap,
    AfterVisualWrap,
};

struct NoteRenderFinalHit {
    LineIndex line_index{};
    Utf16CodeUnitOffset source_offset{};
    // An empty structured row resolves its sole source boundary without a
    // glyph run; run_index stays at this sentinel for that successful hit.
    size_t run_index = static_cast<size_t>(-1);
    Span source_span{};
    bool is_link = false;
    // True only for the application's persisted <link=...> style.  This is
    // distinct from Markdown LinkText so the view can dispatch the existing
    // note/PDF jump contract without consulting a stale legacy line cache.
    bool is_legacy_link_id = false;
    std::wstring link_target;
};

enum class NoteRenderFinalInteractionResult {
    Resolved,
    InvalidOutput,
    InvalidPublication,
    OutsidePublishedRange,
    NativeEditorOwner,
    OutsideContent,
    InconsistentPublication,
    AllocationFailure,
};

// Resolves one structured-owned row for paint. Native-owned rows deliberately
// return NativeEditorOwner: a caller cannot turn a partial final publication
// into mixed rendering by reading its placement opportunistically.
[[nodiscard]] NoteRenderFinalInteractionResult ResolveNoteRenderFinalCommittedLine(
    const NoteRenderFinalPublication& publication,
    LineIndex line_index,
    NoteRenderFinalResolvedLine* out) noexcept;

// Shared by structured and hybrid hit testing. Only blank space strictly
// beyond the last visual row's visible extent may address a hidden inline
// suffix at the logical line end. Tables keep their cell-based hit contract.
// No source parsing, allocation or publication mutation is performed.
[[nodiscard]] std::optional<size_t> ResolveNoteRenderTrailingSyntaxBlankHit(
    const NoteRenderSourceLinePlan& source,
    const NoteRenderLinePlacement& placement,
    int content_x_px,
    uint64_t relative_y_px) noexcept;

// Resolves content Y to one structured-owned row in O(log n), then finds the
// closest measured source boundary across that row's runs. A native-owned row
// is returned as NativeEditorOwner so input dispatch uses exactly the same
// owner partition as paint.
[[nodiscard]] NoteRenderFinalInteractionResult HitTestNoteRenderFinalPublication(
    const NoteRenderFinalPublication& publication,
    int content_x_px,
    uint64_t content_y_px,
    NoteRenderFinalHit* out) noexcept;

// Resolves the measured caret geometry for a structured-owned source offset.
// The result is also the only geometry the final adapter may use to place the
// native caret and the IME composition/candidate windows.
[[nodiscard]] NoteRenderFinalInteractionResult ResolveNoteRenderFinalCaret(
    const NoteRenderFinalPublication& publication,
    Utf16CodeUnitOffset source_offset,
    NoteRenderFinalCaretGeometry* out) noexcept;

[[nodiscard]] NoteRenderFinalInteractionResult ResolveNoteRenderFinalCaret(
    const NoteRenderFinalPublication& publication,
    Utf16CodeUnitOffset source_offset,
    NoteRenderFinalCaretAffinity affinity,
    NoteRenderFinalCaretGeometry* out) noexcept;

// Produces one rectangle per intersecting measured run. It never joins runs
// across an anchored/table placement, so selection never claims the blank gap
// between two independently placed display runs. The output is unchanged on
// every failure or native-owned source row.
[[nodiscard]] NoteRenderFinalInteractionResult ResolveNoteRenderFinalSelection(
    const NoteRenderFinalPublication& publication,
    Span source_selection,
    std::vector<NoteRenderFinalRect>* out) noexcept;

// Horizontal scrolling uses the persistent aggregate instead of expanding
// every source/display boundary. The returned extent includes the exact final
// layout's declared horizontal padding.
[[nodiscard]] NoteRenderFinalInteractionResult ResolveNoteRenderFinalHorizontalExtent(
    const NoteRenderFinalPublication& publication,
    uint64_t* out_extent_px) noexcept;

} // namespace note
