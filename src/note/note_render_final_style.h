#pragma once

#include "note/note_render_source_plan.h"

#include <cstdint>
#include <string_view>

namespace note {

// Anchoring is a placement instruction, not a painter adjustment.  Keeping
// the parsed value beside the immutable source-run style lets measurement,
// interaction, and painting share the very same X coordinates.
enum class NoteRenderFinalHorizontalAnchor : uint8_t {
    None,
    Left,
    Center,
    Right,
};

// Geometry-affecting subset of a parser-derived style run.  Both the GDI
// measurement and paint edges resolve this value from the same immutable
// source run; neither edge consults the legacy LineCache or reparses a tag.
// Values that do not affect font selection are intentionally kept out of
// this type and remain paint-only.
struct NoteRenderFinalRunFontStyle {
    bool bold = false;
    bool italic = false;
    int font_height_px = 0;
    std::wstring_view font_family{};
    // 1000 is one normal line. The legacy `<m>` syntax stores its requested
    // extra line units as this multiplier, so placement owns the resulting
    // row height rather than native RichEdit paragraph metrics.
    uint32_t line_height_permille = 1000;
    // `<d=N>` is line-level in the legacy syntax: the first visible run on a
    // row decides its additional space-column offset.  The resolver retains
    // the run fact here; measurement applies the line rule once.
    bool has_indent = false;
    int indent_columns = 0;
    // `<d=left|center|right[ offset]>` is run-level.  A contiguous sequence
    // with the same anchor forms one non-flow placement group.
    NoteRenderFinalHorizontalAnchor anchor = NoteRenderFinalHorizontalAnchor::None;
    int anchor_offset_columns = 0;
};

// `base_font_height_px` is the selected view font's actual TEXTMETRIC height,
// not a configured point size.  The result is clamped to the same safe range
// as the legacy markup parser. Invalid or unknown attributes are ignored: the
// parser has already decided which spans are valid semantic style input.
[[nodiscard]] bool ResolveNoteRenderFinalRunFontStyle(
    const NoteRenderSourceLinePlan& line,
    const NoteRenderSourceRun& run,
    int base_font_height_px,
    NoteRenderFinalRunFontStyle* out) noexcept;

} // namespace note
