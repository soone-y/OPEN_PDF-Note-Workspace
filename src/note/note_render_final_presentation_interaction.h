#pragma once

#include "note/note_render_final_interaction.h"
#include "note/note_render_final_presentation_snapshot.h"

#include <cstdint>
#include <vector>

namespace note {

enum class NoteRenderFinalPresentationInteractionResult {
    Resolved,
    InvalidOutput,
    InvalidPresentation,
    OutsidePublishedRange,
    OutsideContent,
    InconsistentPresentation,
    AllocationFailure,
};

struct NoteImePreeditDecoration {
    NoteRenderFinalRect rect{};
    NoteImeCharacterAttribute attribute = NoteImeCharacterAttribute::Input;
};

// Resolve once before frame publication, using the same raw placement as
// caret/selection. Paint reads these rectangles, not IMM or text boundaries.
[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalImePreeditDecorations(
    const NoteRenderFinalPresentationSnapshot& presentation,
    std::vector<NoteImePreeditDecoration>* out) noexcept;

// Hybrid counterparts to the structural-publication interaction API.  They
// return the existing neutral geometry/value types, but resolve raw and
// structured surfaces through one hybrid line-layout map.  A completed
// render-enabled frame has no NativeEditorOwner result by design.
[[nodiscard]] NoteRenderFinalPresentationInteractionResult
HitTestNoteRenderFinalPresentation(
    const NoteRenderFinalPresentationSnapshot& presentation,
    int content_x_px,
    uint64_t content_y_px,
    NoteRenderFinalHit* out) noexcept;

[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset source_offset,
    NoteRenderFinalCaretGeometry* out) noexcept;

[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset source_offset,
    NoteRenderFinalCaretAffinity affinity,
    NoteRenderFinalCaretGeometry* out) noexcept;

[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationSelection(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Span source_selection,
    std::vector<NoteRenderFinalRect>* out) noexcept;

[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationHorizontalExtent(
    const NoteRenderFinalPresentationSnapshot& presentation,
    uint64_t* out_extent_px) noexcept;

enum class NoteRenderFinalVerticalMove { Up, Down, PageUp, PageDown };

// Input reads the pre-event presentation. Vertical movement uses measured
// visual rows (including raw wraps), never native-editor line heights. The
// preferred X survives short rows. Page distance is the client height, not a
// guessed number of source lines. Output remains unchanged on failure.
[[nodiscard]] NoteRenderFinalPresentationInteractionResult
MoveNoteRenderFinalPresentationVertical(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset caret_offset,
    int preferred_x_px,
    NoteRenderFinalVerticalMove move,
    uint64_t page_height_px,
    Utf16CodeUnitOffset* out_offset) noexcept;

// IME-facing counterparts. Their offsets are the current RichEdit temporary
// document coordinates during a live preedit (and equal canonical offsets in
// every other frame). Composition-internal offsets are resolved only through
// RawPaint; unchanged structured ranges are translated through the exact
// preedit coordinate map. Adapters use this family for all input events so
// they never infer which coordinate space a frame currently exposes.
[[nodiscard]] NoteRenderFinalPresentationInteractionResult
HitTestNoteRenderFinalPresentationEditor(
    const NoteRenderFinalPresentationSnapshot& presentation,
    int content_x_px,
    uint64_t content_y_px,
    NoteRenderFinalHit* out) noexcept;

[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationEditorCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset editor_offset,
    NoteRenderFinalCaretGeometry* out) noexcept;

[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationEditorCaret(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Utf16CodeUnitOffset editor_offset,
    NoteRenderFinalCaretAffinity affinity,
    NoteRenderFinalCaretGeometry* out) noexcept;

[[nodiscard]] NoteRenderFinalPresentationInteractionResult
ResolveNoteRenderFinalPresentationEditorSelection(
    const NoteRenderFinalPresentationSnapshot& presentation,
    Span editor_selection,
    std::vector<NoteRenderFinalRect>* out) noexcept;

} // namespace note
