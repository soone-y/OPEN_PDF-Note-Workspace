#pragma once

#include "note/note_render_final_display_run.h"
#include "note/note_render_final_ime_preedit_presentation.h"
#include "note/note_render_final_publication.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace note {

// These are paint surfaces, not input controls.  In a render-enabled final
// frame both values are painted by the final compositor; the native editor is
// only an input/IME transport.  NativeEditor is deliberately absent here so
// a completed hybrid frame cannot accidentally delegate one raw row to a
// second coordinate system.
enum class NoteRenderFinalSurfaceKind : uint8_t {
    StructuredPaint,
    RawPaint,
};

struct NoteRenderFinalSurfaceRange {
    NotePresentationLineRange lines{};
    NoteRenderFinalSurfaceKind surface = NoteRenderFinalSurfaceKind::StructuredPaint;
};

// The raw surface is the canonical, untransformed source content for exactly
// one logical source row. `display` maps every source UTF-16 code unit one to
// one; it therefore preserves tabs and Markdown/TeX delimiters while the
// structured side may hide or transform them. Source-line breaks are not
// glyphs and are excluded from source_span.
struct NoteRenderFinalRawLineSurface {
    LineIndex line_index{};
    Span source_span{};
    NoteRenderFinalDisplayRun display{};
    NoteRenderLineLayout layout{};
    NoteRenderLinePlacement placement{};
};

// Text supplied to the raw measurement edge. Its UTF-16 span is in the
// caller's explicitly declared coordinate space: canonical source for a
// normal raw row, temporary editor coordinates for an IME preedit row. The
// provider must never infer or convert that coordinate space.
struct NoteRenderFinalRawTextLine {
    LineIndex line_index{};
    Span text_span{};
    std::wstring text;
};

// Raw measurement is independent from the legacy RichEdit layout. A final
// Win32 measurement provider implements this interface with its selected GDI
// font; pure tests can provide deterministic geometry. The returned vector
// must cover the exact half-open source-row range in order, including empty
// rows, or the completed frame is rejected.
class NoteRenderFinalRawSurfaceMeasurementProvider {
public:
    virtual ~NoteRenderFinalRawSurfaceMeasurementProvider() = default;

    [[nodiscard]] virtual bool MeasureRawLines(
        const NoteTextCore& text_core,
        const NoteRenderLayoutKey& layout_key,
        LineIndex first_line,
        LineIndex last_line_exclusive,
        std::vector<NoteRenderFinalRawLineSurface>* out) const noexcept = 0;

    // Measures explicitly supplied, already-local raw rows. This is the
    // required route for IME preedit; constructing a temporary whole-document
    // TextCore merely to measure one composition line is forbidden.
    [[nodiscard]] virtual bool MeasureRawTextLines(
        const NoteRenderLayoutKey& layout_key,
        const std::vector<NoteRenderFinalRawTextLine>& lines,
        std::vector<NoteRenderFinalRawLineSurface>* out) const noexcept = 0;
};

enum class NoteRenderFinalPresentationSnapshotBuildResult {
    Built,
    InvalidOutput,
    InvalidTextCore,
    InvalidStructuralPublication,
    OwnerPlanBuildFailed,
    RawMeasurementFailed,
    InvalidRawSurface,
    InvalidImePreedit,
    ImePreeditSurfaceMissing,
    HybridLayoutBuildFailed,
    AllocationFailure,
};

// Immutable hybrid presentation for one exact canonical revision and layout
// key.  It takes an already validated structural publication only as semantic
// input and owner-expansion proof. RawPaint allocations preserve the larger of
// structured and measured raw heights: per row normally, per complete table
// or block-math group for atomic ownership. This prevents ownership-only
// contraction without stretching/cropping raw glyphs. The final line-layout
// map replaces each raw range with these allocations; paint, hit test, caret
// and IME all use its single set of aggregate Y coordinates.
class NoteRenderFinalPresentationSnapshot final {
public:
    [[nodiscard]] static NoteRenderFinalPresentationSnapshotBuildResult Build(
        const NoteTextCore& text_core,
        std::shared_ptr<const NoteRenderFinalPublication> structural_publication,
        NotePresentationOwnerInput owner_input,
        const NoteRenderFinalRawSurfaceMeasurementProvider& raw_measurement_provider,
        std::shared_ptr<const NoteRenderFinalPresentationSnapshot>* out) noexcept;

    // Builds one render-enabled IME preedit frame without mutating canonical
    // text. Every RawPaint row is measured in temporary editor coordinates;
    // Structured rows retain canonical coordinates and are translated only by
    // the explicit preedit coordinate map at the interaction edge.
    [[nodiscard]] static NoteRenderFinalPresentationSnapshotBuildResult BuildWithImePreedit(
        const NoteTextCore& text_core,
        std::shared_ptr<const NoteRenderFinalPublication> structural_publication,
        NotePresentationOwnerInput owner_input,
        const NoteRenderFinalImePreeditPresentation& ime_preedit,
        const NoteRenderFinalRawSurfaceMeasurementProvider& raw_measurement_provider,
        std::shared_ptr<const NoteRenderFinalPresentationSnapshot>* out) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::shared_ptr<const NoteRenderFinalPublication>&
    structural_publication() const noexcept {
        return structural_publication_;
    }
    [[nodiscard]] const std::shared_ptr<const NotePresentationOwnerPlan>& owner_plan() const noexcept {
        return owner_plan_;
    }
    [[nodiscard]] const NoteRenderLineLayoutMap::Snapshot& line_layouts() const noexcept {
        return line_layouts_;
    }
    [[nodiscard]] const std::vector<NoteRenderFinalSurfaceRange>& surface_ranges() const noexcept {
        return surface_ranges_;
    }
    [[nodiscard]] bool Matches(const NoteTextCore& text_core,
                               const NoteRenderLayoutKey& layout_key) const noexcept;
    [[nodiscard]] bool has_ime_preedit() const noexcept { return ime_preedit_.has_value(); }
    [[nodiscard]] const NoteRenderFinalImePreeditPresentation* ime_preedit() const noexcept {
        return ime_preedit_.has_value() ? &*ime_preedit_ : nullptr;
    }

    // All source rows have exactly one surface. Structured rows resolve their
    // source/placement from structural_publication(); RawPaint rows resolve
    // their untransformed source/display/placement through this method.
    [[nodiscard]] std::optional<NoteRenderFinalSurfaceKind> SurfaceAt(
        LineIndex line_index) const noexcept;
    [[nodiscard]] bool ResolveRawLine(LineIndex line_index,
                                      NoteRenderFinalRawLineSurface* out) const noexcept;

private:
    [[nodiscard]] static NoteRenderFinalPresentationSnapshotBuildResult BuildInternal(
        const NoteTextCore& text_core,
        std::shared_ptr<const NoteRenderFinalPublication> structural_publication,
        NotePresentationOwnerInput owner_input,
        const NoteRenderFinalImePreeditPresentation* ime_preedit,
        const NoteRenderFinalRawSurfaceMeasurementProvider& raw_measurement_provider,
        std::shared_ptr<const NoteRenderFinalPresentationSnapshot>* out) noexcept;

    std::shared_ptr<const NoteRenderFinalPublication> structural_publication_;
    std::shared_ptr<const NotePresentationOwnerPlan> owner_plan_;
    NoteRenderLineLayoutMap::Snapshot line_layouts_;
    std::vector<NoteRenderFinalSurfaceRange> surface_ranges_;
    std::vector<NoteRenderFinalRawLineSurface> raw_lines_;
    std::optional<NoteRenderFinalImePreeditPresentation> ime_preedit_;
    bool valid_ = false;
};

} // namespace note
