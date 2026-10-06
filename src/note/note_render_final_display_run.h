#pragma once

#include "note/note_render_source_plan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace note {

// Maps a canonical source boundary to a UTF-16 boundary in one visible run.
// More than one source boundary may intentionally map to the same display
// boundary: Markdown escapes and math delimiters consume source without
// becoming glyphs. GDI measurement fills the corresponding X coordinates;
// it must not invent a second source/display mapping.
struct NoteRenderFinalDisplayBoundary {
    Utf16CodeUnitOffset source_offset{};
    size_t display_offset = 0;
};

struct NoteRenderFinalDisplayRun {
    std::wstring display_text;
    std::vector<NoteRenderFinalDisplayBoundary> boundaries;
};

enum class NoteRenderFinalDisplayRunBuildResult {
    Built,
    InvalidOutput,
    InvalidSyntax,
    InvalidSourceRun,
    InconsistentSource,
    AllocationFailure,
};

// Builds one visible run strictly from the final syntax snapshot and its
// source-plan run. Text/links/images decode Markdown backslash escapes;
// inline code and TeX bodies remain literal. Math uses display_source_span
// while retaining zero-width boundaries for its source delimiters. This is
// the only source-to-display transformation permitted to the final GDI
// measurement and paint adapters.
[[nodiscard]] NoteRenderFinalDisplayRunBuildResult BuildNoteRenderFinalDisplayRun(
    const NoteSyntaxSnapshot& syntax,
    const NoteRenderSourceRun& source_run,
    NoteRenderFinalDisplayRun* out) noexcept;

} // namespace note
