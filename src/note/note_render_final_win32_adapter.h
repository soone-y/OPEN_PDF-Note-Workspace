#pragma once

#include "note/note_render_final_gdi_measurement_provider.h"
#include "note/note_render_final_gdi_painter.h"
#include "note/note_render_final_presentation_interaction.h"
#include "note/note_render_final_transaction.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace note {

// Message-normalized state for exactly one prospective render-enabled frame.
// The owner is the UI thread. `measurement_dc` is borrowed only while Build
// runs and must have the editor's current base font selected. No HWND, legacy
// cache, RichEdit geometry, or mutable source text is retained by the result.
struct NoteRenderFinalWin32AdapterInput {
    const NoteTextCore* text_core = nullptr;
    std::shared_ptr<const NoteRenderFinalPublication> structural_publication;
    NotePresentationOwnerInput owner_input{};
    std::optional<NoteRenderFinalImePreeditPresentation> ime_preedit;
    Span editor_selection{};
    Utf16CodeUnitOffset editor_caret{};
    NoteRenderFinalCaretAffinity caret_affinity =
        NoteRenderFinalCaretAffinity::AfterVisualWrap;
    HDC measurement_dc = nullptr;
    NoteRenderFinalGdiMeasurementOptions measurement_options{};
};

struct NoteRenderFinalWin32PaintState {
    int horizontal_scroll_px = 0;
    uint64_t vertical_scroll_px = 0;
    // Production uses the Windows caret, positioned from this same frame.
    // Memory-DC tests/exports may instead paint it into the frame. Never use
    // both owners in one visible view.
    bool paint_caret = true;
    NoteRenderFinalGdiPaintTheme theme{};
};

enum class NoteRenderFinalWin32AdapterBuildResult {
    Built,
    RequiresNativeFallback,
    InvalidOutput,
    InvalidInput,
    SelectionOutsideEditorText,
    SnapshotBuildFailed,
    AllocationFailure,
};

// The one completed render-enabled view frame. It owns the immutable hybrid
// snapshot and UI-normalized editor coordinates. The caller may either paint
// it as a whole through this adapter or discard it and paint one whole native
// fallback frame; it cannot expose a partial structured/raw mixture.
class NoteRenderFinalWin32AdapterFrame final {
public:
    [[nodiscard]] static NoteRenderFinalWin32AdapterBuildResult Build(
        const NoteRenderFinalWin32AdapterInput& input,
        std::shared_ptr<const NoteRenderFinalWin32AdapterFrame>* out) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::shared_ptr<const NoteRenderFinalPresentationSnapshot>&
    presentation() const noexcept {
        return presentation_;
    }
    [[nodiscard]] Span editor_selection() const noexcept { return editor_selection_; }
    [[nodiscard]] Utf16CodeUnitOffset editor_caret() const noexcept { return editor_caret_; }
    [[nodiscard]] NoteRenderFinalCaretAffinity caret_affinity() const noexcept {
        return caret_affinity_;
    }
    [[nodiscard]] const std::vector<NoteImePreeditDecoration>& ime_decorations() const noexcept {
        return ime_decorations_;
    }

    // `paint_dc` is borrowed from BeginPaint (or a test memory DC) on the
    // same UI thread that built the frame. Painting never reads the native
    // editor, so successful output has exactly one glyph owner.
    [[nodiscard]] bool Paint(HDC paint_dc,
                             const NoteRenderFinalWin32PaintState& state) const noexcept;
    [[nodiscard]] NoteRenderFinalPresentationInteractionResult HitTest(
        int content_x_px,
        uint64_t content_y_px,
        NoteRenderFinalHit* out) const noexcept;
    [[nodiscard]] NoteRenderFinalPresentationInteractionResult Caret(
        NoteRenderFinalCaretGeometry* out) const noexcept;
    [[nodiscard]] NoteRenderFinalPresentationInteractionResult Selection(
        std::vector<NoteRenderFinalRect>* out) const noexcept;
    [[nodiscard]] NoteRenderFinalPresentationInteractionResult HorizontalExtent(
        uint64_t* out_extent_px) const noexcept;

private:
    std::shared_ptr<const NoteRenderFinalPresentationSnapshot> presentation_;
    Span editor_selection_{};
    std::vector<NoteImePreeditDecoration> ime_decorations_;
    Utf16CodeUnitOffset editor_caret_{};
    NoteRenderFinalCaretAffinity caret_affinity_ =
        NoteRenderFinalCaretAffinity::AfterVisualWrap;
    bool valid_ = false;
};

// Event-normalized input to the final per-view publication boundary.  A
// caller invokes Refresh after an accepted editor change, selection/caret
// change, IME preedit update, or view-layout change -- never from WM_PAINT.
// The returned frame is the only render-enabled object visible to paint,
// hit-test, caret, selection, and IME candidate placement.
struct NoteRenderFinalWin32AdapterStateInput {
    const NoteTextCore* text_core = nullptr;
    NoteContentKind content_kind = NoteContentKind::Markdown;
    NoteRenderLayoutKey layout_key{};
    NotePresentationOwnerInput owner_input{};
    std::optional<NoteRenderFinalImePreeditPresentation> ime_preedit;
    Span editor_selection{};
    Utf16CodeUnitOffset editor_caret{};
    NoteRenderFinalCaretAffinity caret_affinity =
        NoteRenderFinalCaretAffinity::AfterVisualWrap;
    HDC measurement_dc = nullptr;
    NoteRenderFinalGdiMeasurementOptions measurement_options{};
};

enum class NoteRenderFinalWin32AdapterStateRefreshResult {
    // The previous immutable structural publication exactly matches the
    // current canonical text and layout.  Only the presentation frame was
    // rebuilt for new owner/selection/preedit facts.
    PublishedReusedStructural,
    // The accepted editor ticket proved a marker-free, same-line Markdown
    // edit. Syntax, source plan, layout, placement, owner, and presentation
    // were replaced together from one local transaction.
    PublishedLocalPatch,
    // No local proof was available (or it was deliberately rejected), so a
    // complete current publication was measured and published atomically.
    PublishedComplete,
    // No current complete frame is exposed. The caller must paint the entire
    // editor natively for this event, rather than mixing it with an older
    // custom frame.
    RequiresNativeFallback,
    InvalidInput,
    AllocationFailure,
};

#if !defined(NDEBUG)
// Process-local test/debug accounting for the final publication decision. It
// is intentionally not present in release builds, is never persisted, and
// carries no document content. Counts describe decisions, not elapsed time:
// deterministic tests use them to reject a complete transaction for a
// checkpoint-proven same-row edit regardless of host performance.
struct NoteRenderFinalWin32AdapterStateTestWork {
    uint64_t refresh_calls = 0;
    uint64_t structural_reuse_attempts = 0;
    uint64_t local_transaction_attempts = 0;
    uint64_t local_transaction_builds = 0;
    uint64_t complete_transaction_attempts = 0;
    uint64_t complete_transaction_builds = 0;
};
#endif

// Owns the two immutable objects that must cross the view boundary together:
// the current structural publication and its fully resolved hybrid Win32
// frame. A new structural candidate is deliberately private until the frame
// also builds; on any failure `frame()` becomes null, so callers cannot draw
// a stale structured snapshot underneath native content.
class NoteRenderFinalWin32AdapterState final {
public:
    [[nodiscard]] NoteRenderFinalWin32AdapterStateRefreshResult Refresh(
        const NoteRenderFinalWin32AdapterStateInput& input,
        const std::optional<TextEdit>& accepted_local_edit = std::nullopt) noexcept;

    // Withdraws only the public frame at the beginning of an editor/view
    // event. The immediately preceding structural publication remains private
    // so a subsequently accepted exact edit can still prove a local patch.
    // Until Refresh publishes a replacement, the view must use one whole
    // native fallback frame; it must not paint from the retained structure.
    void ClearFrame() noexcept;

    void Clear() noexcept;

    [[nodiscard]] const std::shared_ptr<const NoteRenderFinalWin32AdapterFrame>&
    frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] bool has_frame() const noexcept {
        return frame_ && frame_->valid();
    }

#if !defined(NDEBUG)
    [[nodiscard]] const NoteRenderFinalWin32AdapterStateTestWork& test_work() const noexcept {
        return test_work_;
    }
#endif

private:
    // This is deliberately never exposed to the view. It may describe the
    // immediately preceding revision while a failed refresh is awaiting a
    // native fallback, but `frame_` is cleared in that state.
    std::shared_ptr<const NoteRenderFinalPublication> structural_publication_;
    std::shared_ptr<const NoteRenderFinalWin32AdapterFrame> frame_;
#if !defined(NDEBUG)
    NoteRenderFinalWin32AdapterStateTestWork test_work_{};
#endif
};

} // namespace note
