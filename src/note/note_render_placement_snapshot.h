#pragma once

#include "note/note_render_layout_snapshot.h"
#include "note/note_render_source_plan.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace note {

class NoteRenderPlacementStorage;
class NoteRenderAtomicGroupPlacementStorage;

// One explicit source/display boundary supplied by the measurement adapter.
// Source offsets are ordered; X may be non-monotonic for an anchored run.
// A boundary on a visual-wrap edge is represented twice: once for the end of
// the preceding fragment and once for the start of the following fragment.
// `fragment_index` preserves that visual affinity; source offset alone cannot.
struct NoteRenderPlacementBoundary {
    Utf16CodeUnitOffset source_offset{};
    int x_px = 0;
    uint32_t fragment_index = 0;
};

// One contiguous visual rectangle of a source run. A logical source row may
// have several fragments after word wrapping, and table/math layout can place
// fragments at non-flow X positions. All paint and interaction geometry must
// resolve through these rectangles rather than treating a logical row as one
// vertical band.
struct NoteRenderRunPlacementFragment {
    Span source_span{};
    int x_px = 0;
    int width_px = 0;
    uint32_t top_offset_px = 0;
    uint32_t height_px = 0;
    // Math graphics preserve their measured baseline here. Text fragments
    // leave it zero because GDI uses the selected font's top alignment.
    int baseline_offset_px = 0;
};

struct NoteRenderRunPlacement {
    Span source_span{};
    int x_px = 0;
    int width_px = 0;
    std::vector<NoteRenderPlacementBoundary> boundaries;
    // An empty vector is normalized at the immutable snapshot boundary to
    // one whole-row fragment. This compact form is valid only for an
    // unwrapped run; adapters that perform wrapping publish explicit data.
    std::vector<NoteRenderRunPlacementFragment> fragments;
};

// Geometry emitted during measurement for non-text structural presentation.
// The final painter consumes these immutable values directly; it must not
// rediscover marker widths, quote gutters, surfaces, or rules from raw source
// text. Theme colours are intentionally not geometry and remain paint-only.
enum class NoteRenderVisualDecorationKind : uint8_t {
    ListMarker,
    TaskCheckbox,
    QuoteBar,
    CodeBlockSurface,
    ContainerSurface,
    ContainerBorder,
    HorizontalRule,
    InlineCodeSurface,
    InlineMathGraphic,
};

struct NoteRenderVisualDecoration {
    NoteRenderVisualDecorationKind kind = NoteRenderVisualDecorationKind::ListMarker;
    Span source_span{};
    int left_px = 0;
    uint32_t top_offset_px = 0;
    int right_px = 0;
    uint32_t bottom_offset_px = 0;
    std::wstring text;
    bool checked = false;
};

struct NoteRenderLinePlacement {
    std::vector<NoteRenderRunPlacement> runs;
    std::vector<NoteRenderVisualDecoration> decorations;
};

// Table geometry is group-local. X values are offsets from the atomic-group
// left edge; Y values are offsets from the atomic-group top.  Keeping shared
// borders here prevents a body row, selection, or painter from independently
// rounding column widths.
struct NoteRenderTableColumnPlacement {
    int left_border_x_px = 0;
    int content_x_px = 0;
    int content_width_px = 0;
    int right_border_x_px = 0;
};

struct NoteRenderTableRowPlacement {
    LineIndex source_line{};
    uint32_t top_offset_px = 0;
    uint32_t height_px = 0;
    bool header = false;
    bool divider = false;
};

// One measured visual group whose geometry cannot be reconstructed from
// independent source rows.  `top_px` is intentionally absent: it is always
// the aggregate top of `first_line` in the exact layout snapshot.  This
// keeps a local height splice from rewriting absolute Y in every following
// group.
//
// Block math contributes its full visual height only through `first_line`;
// every following source line is marked `collapsed_into_atomic_group` by the
// measured layout.  A table keeps its individual visual rows, but records one
// group rectangle so its shared column grid and borders have a single owner.
// The measured frame must contain exactly one placement for every table or
// block-math source-plan atomic group.
struct NoteRenderAtomicGroupPlacement {
    NoteRenderAtomicGroupKind kind = NoteRenderAtomicGroupKind::BlockMath;
    Span source_span{};
    LineIndex first_line{};
    LineIndex last_line{};
    int x_px = 0;
    int width_px = 0;
    uint32_t height_px = 0;
    std::vector<NoteRenderTableColumnPlacement> table_columns;
    std::vector<NoteRenderTableRowPlacement> table_rows;
    // Block math only: baseline relative to the atomic group's top. Tables
    // keep this zero and carry their row geometry instead.
    int math_baseline_offset_px = 0;
};

enum class NoteRenderPlacementSnapshotBuildResult {
    Built,
    InvalidOutput,
    InvalidSourcePlan,
    InvalidLayoutSnapshot,
    SourceLayoutIdentityMismatch,
    LineCountMismatch,
    RunCountMismatch,
    BoundaryMismatch,
    AllocationFailure,
};

enum class NoteRenderPlacementSnapshotLocalSpliceResult {
    Built,
    InvalidOutput,
    InvalidPreviousSnapshot,
    InvalidSourcePlan,
    InvalidLayoutSnapshot,
    SourceLayoutIdentityMismatch,
    InvalidRange,
    ReplacementRejected,
    AllocationFailure,
};

struct NoteRenderPlacementSnapshotLocalSpliceWork {
    size_t replacement_line_payloads = 0;
    size_t tail_line_payload_rewrites = 0;
};

// Immutable measured placement for exactly one source plan and layout
// snapshot. The adapter may measure with GDI, but it publishes only these
// values; drawing, selection, caret, hit testing, IME, and horizontal extent
// consume the same stored boundaries and run geometry. The complete
// source/syntax/layout/placement/owner route publishes it atomically; legacy
// cache code must never consume it as a partial migration artifact.
class NoteRenderPlacementSnapshot final {
public:
    [[nodiscard]] static NoteRenderPlacementSnapshotBuildResult Build(
        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
        std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
        std::vector<NoteRenderLinePlacement> linePlacements,
        std::vector<NoteRenderAtomicGroupPlacement> atomicGroupPlacements,
        std::shared_ptr<const NoteRenderPlacementSnapshot>* out) noexcept;

    // Kept for component tests that intentionally construct source plans
    // without table/block-math groups.  Any plan containing either group
    // still rejects this overload rather than silently measuring it as lines.
    [[nodiscard]] static NoteRenderPlacementSnapshotBuildResult Build(
        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
        std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
        std::vector<NoteRenderLinePlacement> linePlacements,
        std::shared_ptr<const NoteRenderPlacementSnapshot>* out) noexcept;

    // A new placement may reuse unchanged per-line placement values only when
    // its source plan and layout share one new syntax publication. The caller
    // must provide every affected measured row; no old geometry is read for a
    // changed row.
    [[nodiscard]] static NoteRenderPlacementSnapshotLocalSpliceResult BuildLocalSplice(
        std::shared_ptr<const NoteRenderPlacementSnapshot> previous,
        std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
        std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
        LineIndex first,
        LineIndex last_exclusive,
        std::vector<NoteRenderLinePlacement> replacement,
        std::shared_ptr<const NoteRenderPlacementSnapshot>* out,
        NoteRenderPlacementSnapshotLocalSpliceWork* outWork = nullptr) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::shared_ptr<const NoteRenderSourcePlan>& source_plan() const noexcept {
        return source_plan_;
    }
    [[nodiscard]] const std::shared_ptr<const NoteRenderLayoutSnapshot>& layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] size_t line_count() const noexcept;
    [[nodiscard]] bool ResolveLine(LineIndex index,
                                   NoteRenderLinePlacement* out) const noexcept;
    [[nodiscard]] bool ResolveAtomicGroupContaining(
        LineIndex index,
        NoteRenderAtomicGroupPlacement* out) const noexcept;
    [[nodiscard]] bool CopyAtomicGroupPlacementsForDifferentialTest(
        std::vector<NoteRenderAtomicGroupPlacement>* out) const noexcept;
    [[nodiscard]] bool CopyLinesForDifferentialTest(
        std::vector<NoteRenderLinePlacement>* out) const noexcept;
    [[nodiscard]] bool SharesLinePayloadForDifferentialTest(
        const NoteRenderPlacementSnapshot& other,
        LineIndex index) const noexcept;
    [[nodiscard]] NoteDerivedSnapshotIdentity source_identity() const noexcept;
    [[nodiscard]] bool Matches(const NoteTextCore& textCore,
                               const NoteRenderLayoutKey& layoutKey) const noexcept;

private:
    std::shared_ptr<const NoteRenderSourcePlan> source_plan_;
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout_;
    std::shared_ptr<const NoteRenderPlacementStorage> line_storage_;
    std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage> atomic_group_storage_;
    bool valid_ = false;
};

} // namespace note
