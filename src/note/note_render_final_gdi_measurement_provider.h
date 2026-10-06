#pragma once

#include "note/note_render_final_presentation_snapshot.h"
#include "note/note_render_final_transaction.h"

#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace note {

// Position an inline math graphic within the visual row that owns it.  This
// is geometry, rather than paint styling: the resulting placement is shared
// by painting, selection and hit testing.
enum class NoteRenderInlineMathVerticalAlignment : uint8_t {
    Top = 0,
    Center = 1,
    Bottom = 2,
};

[[nodiscard]] uint32_t ResolveNoteRenderInlineMathTopOffset(
    uint32_t row_height_px, uint32_t math_height_px,
    NoteRenderInlineMathVerticalAlignment alignment) noexcept;

// Geometry-only settings owned by the final Win32 adapter. Theme colours are
// deliberately absent: they do not alter source/display correspondence and
// must never cause an otherwise current placement to be reused under a
// different layout key.
struct NoteRenderFinalGdiMeasurementOptions {
    uint32_t minimum_left_padding_px = 8;
    uint32_t minimum_right_padding_px = 8;
    uint32_t list_continuation_columns = 2;
    uint32_t quote_gutter_columns = 2;
    uint32_t code_block_inset_columns = 2;
    NoteRenderInlineMathVerticalAlignment inline_math_vertical_alignment =
        NoteRenderInlineMathVerticalAlignment::Bottom;

    [[nodiscard]] bool valid() const noexcept;
};

// Synchronous Win32/GDI measurement for the final publication route. `hdc`
// is borrowed and must remain valid, on its owning UI thread, for the entire
// call to Measure or MeasureReplacement. The provider stores no source plan,
// canonical text, result geometry, or legacy view state; successful calls
// return only immutable value data for transaction validation.
//
// This first measurement unit handles text, inline code, links, headings,
// lists, quotes, code blocks, hidden syntax, tabs, style fonts, soft wraps,
// inline math, display math, and Markdown tables. Display math is one atomic
// graphic whose later source rows are collapsed under its anchor. Tables keep
// their source rows but receive one shared parser-derived grid. It never
// substitutes raw TeX/plain table text as a visually different approximation.
class NoteRenderFinalGdiMeasurementProvider final
    : public NoteRenderFinalMeasurementProvider,
      public NoteRenderFinalRawSurfaceMeasurementProvider {
public:
    explicit NoteRenderFinalGdiMeasurementProvider(
        HDC hdc,
        NoteRenderFinalGdiMeasurementOptions options = {}) noexcept;

    [[nodiscard]] bool Measure(
        const NoteRenderSourcePlan& source_plan,
        const NoteRenderLayoutKey& layout_key,
        NoteRenderFinalMeasuredFrame* out) const noexcept override;

    [[nodiscard]] bool MeasureReplacement(
        const NoteRenderSourcePlan& source_plan,
        const NoteRenderLayoutKey& layout_key,
        LineIndex first_line,
        LineIndex last_line_exclusive,
        NoteRenderFinalMeasuredLineRange* out) const noexcept override;

    // Raw source is measured with the same GDI font, tab policy, wrapping,
    // and horizontal padding as structured content.  It deliberately does
    // not ask RichEdit for line positions or glyphs.
    [[nodiscard]] bool MeasureRawLines(
        const NoteTextCore& text_core,
        const NoteRenderLayoutKey& layout_key,
        LineIndex first_line,
        LineIndex last_line_exclusive,
        std::vector<NoteRenderFinalRawLineSurface>* out) const noexcept override;

    [[nodiscard]] bool MeasureRawTextLines(
        const NoteRenderLayoutKey& layout_key,
        const std::vector<NoteRenderFinalRawTextLine>& lines,
        std::vector<NoteRenderFinalRawLineSurface>* out) const noexcept override;

private:
    HDC hdc_ = nullptr;
    NoteRenderFinalGdiMeasurementOptions options_{};
};

} // namespace note
