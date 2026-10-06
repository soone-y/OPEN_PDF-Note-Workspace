#pragma once

#include "note/note_render_final_interaction.h"
#include "note/note_render_final_presentation_snapshot.h"
#include "note/note_render_final_presentation_interaction.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <optional>

namespace note {

// Paint-only values. None of these changes source/display geometry, so they
// are deliberately absent from NoteRenderLayoutKey and placement snapshots.
struct NoteRenderFinalGdiPaintTheme {
    COLORREF text = RGB(30, 30, 30);
    // The base canvas colour is required when a legacy style requests a
    // high-contrast foreground/background pair. It is paint-only and never
    // changes placement geometry.
    COLORREF background = RGB(255, 255, 255);
    COLORREF link = RGB(0, 83, 173);
    COLORREF code_surface = RGB(245, 245, 245);
    COLORREF container_surface = RGB(247, 246, 242);
    COLORREF container_border = RGB(176, 174, 166);
    COLORREF inline_code_surface = RGB(238, 241, 244);
    COLORREF quote_bar = RGB(128, 140, 152);
    COLORREF rule = RGB(128, 140, 152);
    COLORREF table_border = RGB(83, 108, 136);
    COLORREF table_header_surface = RGB(237, 244, 252);
    COLORREF selection_surface = RGB(184, 212, 242);
    COLORREF caret = RGB(30, 30, 30);
};

struct NoteRenderFinalGdiPaintOptions {
    int horizontal_scroll_px = 0;
    uint64_t vertical_scroll_px = 0;
    // These are source coordinates, not native control rectangles. The
    // painter resolves them through the same immutable placement as text.
    // Both the structural and hybrid overloads own selection/caret painting;
    // a completed hybrid frame never delegates a RawPaint row to RichEdit.
    std::optional<Span> selection;
    std::optional<Utf16CodeUnitOffset> caret;
    NoteRenderFinalCaretAffinity caret_affinity =
        NoteRenderFinalCaretAffinity::AfterVisualWrap;
    NoteRenderFinalGdiPaintTheme theme{};
    // Borrowed from the immutable frame for this Paint call only. Rectangles
    // are in ordered, nonoverlapping visual bands (same-row clauses may tie).
    const std::vector<NoteImePreeditDecoration>* ime_decorations = nullptr;
};

// Draws only CommittedPlacement ranges from one immutable final publication.
// NativeEditor ranges are intentionally untouched: the caller keeps its raw
// editor as their sole owner. The HDC's existing clip selects the visible
// client area; the adapter supplies scroll origin and source-based selection
// / caret state.  It never consults native editor geometry.
class NoteRenderFinalGdiPainter final {
public:
    [[nodiscard]] static bool Paint(
        HDC hdc,
        const NoteRenderFinalPublication& publication,
        const NoteRenderFinalGdiPaintOptions& options = {}) noexcept;

    // Paints the completed hybrid frame. RawPaint rows, including their
    // selection and caret, are drawn from immutable raw surfaces; no RichEdit
    // glyph pass may be composed underneath it.
    [[nodiscard]] static bool Paint(
        HDC hdc,
        const NoteRenderFinalPresentationSnapshot& presentation,
        const NoteRenderFinalGdiPaintOptions& options = {}) noexcept;
};

} // namespace note
