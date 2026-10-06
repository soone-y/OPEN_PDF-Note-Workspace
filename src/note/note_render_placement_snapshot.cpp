#include "note/note_render_placement_snapshot.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

struct PlacementRelativeBoundary {
    LineIndex line{};
    size_t offset_from_line_start = 0;
};

struct PlacementRelativeSpan {
    PlacementRelativeBoundary start{};
    PlacementRelativeBoundary end{};
};

[[nodiscard]] bool MakeRelativeBoundary(const NoteSourceLineMap::Snapshot& sourceLines,
                                        Utf16CodeUnitOffset offset,
                                        PlacementRelativeBoundary* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::FindByOffset(sourceLines, offset);
    if (!location.has_value() || offset.value < location->start.value ||
        offset.value > location->next_start.value) return false;
    *out = {location->line_index, offset.value - location->start.value};
    return true;
}

[[nodiscard]] bool ResolveRelativeBoundary(const NoteSourceLineMap::Snapshot& sourceLines,
                                           PlacementRelativeBoundary boundary,
                                           Utf16CodeUnitOffset* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::LineAt(sourceLines, boundary.line);
    if (!location.has_value() ||
        boundary.offset_from_line_start > location->next_start.value - location->start.value) {
        return false;
    }
    *out = {location->start.value + boundary.offset_from_line_start};
    return true;
}

[[nodiscard]] bool MakeRelativeSpan(const NoteSourceLineMap::Snapshot& sourceLines,
                                    Span span,
                                    PlacementRelativeSpan* out) noexcept {
    if (!out || span.end < span.start) return false;
    PlacementRelativeBoundary start;
    PlacementRelativeBoundary end;
    if (!MakeRelativeBoundary(sourceLines, span.start, &start) ||
        !MakeRelativeBoundary(sourceLines, span.end, &end)) return false;
    *out = {start, end};
    return true;
}

[[nodiscard]] bool ResolveRelativeSpan(const NoteSourceLineMap::Snapshot& sourceLines,
                                       PlacementRelativeSpan span,
                                       Span* out) noexcept {
    if (!out) return false;
    Utf16CodeUnitOffset start;
    Utf16CodeUnitOffset end;
    if (!ResolveRelativeBoundary(sourceLines, span.start, &start) ||
        !ResolveRelativeBoundary(sourceLines, span.end, &end) || end < start) return false;
    *out = {start, end};
    return true;
}

struct StoredPlacementBoundary {
    PlacementRelativeBoundary source_offset{};
    int x_px = 0;
    uint32_t fragment_index = 0;
};

struct StoredRunPlacementFragment {
    PlacementRelativeSpan source_span{};
    int x_px = 0;
    int width_px = 0;
    uint32_t top_offset_px = 0;
    uint32_t height_px = 0;
    int baseline_offset_px = 0;
};

struct StoredRunPlacement {
    PlacementRelativeSpan source_span{};
    int x_px = 0;
    int width_px = 0;
    std::vector<StoredPlacementBoundary> boundaries;
    std::vector<StoredRunPlacementFragment> fragments;
};

struct StoredVisualDecoration {
    NoteRenderVisualDecorationKind kind = NoteRenderVisualDecorationKind::ListMarker;
    PlacementRelativeSpan source_span{};
    int left_px = 0;
    uint32_t top_offset_px = 0;
    int right_px = 0;
    uint32_t bottom_offset_px = 0;
    std::wstring text;
    bool checked = false;
};

struct StoredLinePlacement {
    std::vector<StoredRunPlacement> runs;
    std::vector<StoredVisualDecoration> decorations;
};

struct StoredAtomicGroupPlacement {
    NoteRenderAtomicGroupKind kind = NoteRenderAtomicGroupKind::BlockMath;
    PlacementRelativeSpan source_span{};
    LineIndex first_line{};
    LineIndex last_line{};
    int x_px = 0;
    int width_px = 0;
    uint32_t height_px = 0;
    std::vector<NoteRenderTableColumnPlacement> table_columns;
    std::vector<NoteRenderTableRowPlacement> table_rows;
    int math_baseline_offset_px = 0;
};

[[nodiscard]] bool BoundariesMatchRun(const NoteRenderSourceRun& sourceRun,
                                      const NoteRenderRunPlacement& placement) noexcept {
    if (placement.source_span.start != sourceRun.source_span.start ||
        placement.source_span.end != sourceRun.source_span.end ||
        placement.width_px < 0 || placement.boundaries.size() < 2 ||
        placement.fragments.empty()) return false;
    if (placement.boundaries.front().source_offset != placement.source_span.start ||
        placement.boundaries.back().source_offset != placement.source_span.end) return false;
    int aggregateLeft = std::numeric_limits<int>::max();
    int aggregateRight = std::numeric_limits<int>::min();
    Utf16CodeUnitOffset fragmentCursor = placement.source_span.start;
    for (const NoteRenderRunPlacementFragment& fragment : placement.fragments) {
        if (fragment.source_span.start != fragmentCursor ||
            fragment.source_span.end <= fragment.source_span.start ||
            fragment.width_px < 0 || fragment.height_px == 0 ||
            fragment.baseline_offset_px < 0 ||
            static_cast<uint32_t>(fragment.baseline_offset_px) > fragment.height_px ||
            fragment.x_px > std::numeric_limits<int>::max() - fragment.width_px) {
            return false;
        }
        fragmentCursor = fragment.source_span.end;
        aggregateLeft = std::min(aggregateLeft, fragment.x_px);
        aggregateRight = std::max(aggregateRight, fragment.x_px + fragment.width_px);
    }
    const int64_t aggregateWidth = static_cast<int64_t>(aggregateRight) -
        static_cast<int64_t>(aggregateLeft);
    if (fragmentCursor != placement.source_span.end || aggregateLeft != placement.x_px ||
        aggregateRight < aggregateLeft || aggregateWidth != placement.width_px) {
        return false;
    }
    Utf16CodeUnitOffset previous = placement.source_span.start;
    uint32_t previousFragment = 0;
    bool firstBoundary = true;
    for (const NoteRenderPlacementBoundary& boundary : placement.boundaries) {
        if (boundary.source_offset < previous ||
            boundary.source_offset < placement.source_span.start ||
            boundary.source_offset > placement.source_span.end ||
            boundary.fragment_index >= placement.fragments.size()) return false;
        if (!firstBoundary && boundary.source_offset == previous &&
            boundary.fragment_index < previousFragment) return false;
        const NoteRenderRunPlacementFragment& fragment =
            placement.fragments[boundary.fragment_index];
        if (boundary.source_offset < fragment.source_span.start ||
            boundary.source_offset > fragment.source_span.end ||
            boundary.x_px < fragment.x_px ||
            boundary.x_px > fragment.x_px + fragment.width_px) return false;
        previous = boundary.source_offset;
        previousFragment = boundary.fragment_index;
        firstBoundary = false;
    }
    for (size_t index = 0; index < placement.fragments.size(); ++index) {
        const NoteRenderRunPlacementFragment& fragment = placement.fragments[index];
        bool hasStart = false;
        bool hasEnd = false;
        for (const NoteRenderPlacementBoundary& boundary : placement.boundaries) {
            if (boundary.fragment_index != index) continue;
            hasStart = hasStart || boundary.source_offset == fragment.source_span.start;
            hasEnd = hasEnd || boundary.source_offset == fragment.source_span.end;
        }
        if (!hasStart || !hasEnd) return false;
    }
    return true;
}

[[nodiscard]] bool NormalizeImplicitFragments(NoteRenderLinePlacement* line,
                                              uint32_t lineHeightPx) noexcept {
    if (!line || lineHeightPx == 0) return false;
    try {
        for (NoteRenderRunPlacement& run : line->runs) {
            if (run.fragments.empty()) {
                if (run.source_span.end <= run.source_span.start || run.width_px < 0) return false;
                run.fragments.push_back({run.source_span, run.x_px, run.width_px, 0, lineHeightPx});
            }
            for (const NoteRenderRunPlacementFragment& fragment : run.fragments) {
                if (fragment.top_offset_px > lineHeightPx ||
                    fragment.height_px > lineHeightPx - fragment.top_offset_px) {
                    return false;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool DecorationsMatchLine(const NoteRenderSourceLinePlan& sourceLine,
                                        const NoteRenderLinePlacement& placement,
                                        uint32_t lineHeightPx) noexcept {
    for (const NoteRenderVisualDecoration& decoration : placement.decorations) {
        if (decoration.source_span.start < sourceLine.content_span.start ||
            decoration.source_span.end < decoration.source_span.start ||
            decoration.source_span.end > sourceLine.content_span.end ||
            decoration.right_px <= decoration.left_px ||
            decoration.bottom_offset_px <= decoration.top_offset_px ||
            decoration.bottom_offset_px > lineHeightPx) {
            return false;
        }
        if (decoration.kind == NoteRenderVisualDecorationKind::ListMarker &&
            decoration.text.empty()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsMeasuredAtomicGroupKind(NoteRenderAtomicGroupKind kind) noexcept {
    return kind == NoteRenderAtomicGroupKind::Table ||
           kind == NoteRenderAtomicGroupKind::BlockMath;
}

[[nodiscard]] bool ValidateAtomicGroupPlacements(
    const NoteRenderSourcePlan& sourcePlan,
    const NoteRenderLayoutSnapshot& layout,
    const std::vector<NoteRenderAtomicGroupPlacement>& placements) noexcept {
    std::vector<NoteRenderAtomicGroup> expected;
    if (!sourcePlan.CopyAtomicGroups(&expected)) return false;
    try {
        expected.erase(std::remove_if(expected.begin(), expected.end(),
                                      [](const NoteRenderAtomicGroup& group) {
                                          return !IsMeasuredAtomicGroupKind(group.kind);
                                      }),
                       expected.end());
        if (expected.size() != placements.size()) return false;
        for (size_t index = 0; index < expected.size(); ++index) {
            const NoteRenderAtomicGroup& group = expected[index];
            const NoteRenderAtomicGroupPlacement& placement = placements[index];
            if (placement.kind != group.kind || placement.source_span.start != group.source_span.start ||
                placement.source_span.end != group.source_span.end ||
                placement.first_line != group.first_line || placement.last_line != group.last_line ||
                placement.width_px <= 0 || placement.height_px == 0) {
                return false;
            }
            if (group.kind == NoteRenderAtomicGroupKind::BlockMath &&
                (!placement.table_columns.empty() || !placement.table_rows.empty() ||
                 placement.math_baseline_offset_px < 0 ||
                 static_cast<uint32_t>(placement.math_baseline_offset_px) > placement.height_px)) {
                return false;
            }
            if (group.kind == NoteRenderAtomicGroupKind::Table) {
                if (placement.math_baseline_offset_px != 0) return false;
                if (placement.table_columns.empty() ||
                    placement.table_rows.size() !=
                        group.last_line.value - group.first_line.value + 1) {
                    return false;
                }
                int previousBorder = -1;
                for (const NoteRenderTableColumnPlacement& column : placement.table_columns) {
                    if (column.left_border_x_px < 0 ||
                        column.content_x_px <= column.left_border_x_px ||
                        column.content_width_px <= 0 ||
                        column.right_border_x_px <= column.content_x_px ||
                        column.right_border_x_px > placement.width_px ||
                        (previousBorder >= 0 && column.left_border_x_px != previousBorder)) {
                        return false;
                    }
                    previousBorder = column.right_border_x_px;
                }
                if (placement.table_columns.front().left_border_x_px != 0 ||
                    placement.table_columns.back().right_border_x_px >= placement.width_px) {
                    return false;
                }
                uint64_t rowTop = 0;
                for (size_t row = 0; row < placement.table_rows.size(); ++row) {
                    const NoteRenderTableRowPlacement& tableRow = placement.table_rows[row];
                    const size_t sourceLine = group.first_line.value + row;
                    if (tableRow.source_line.value != sourceLine ||
                        tableRow.top_offset_px != rowTop || tableRow.height_px == 0 ||
                        (tableRow.divider && tableRow.header) ||
                        (tableRow.divider && row != 1)) {
                        return false;
                    }
                    const std::optional<NoteRenderLineLayoutLocation> layoutLine =
                        NoteRenderLineLayoutMap::LineAt(layout.line_layouts(), {sourceLine});
                    if (!layoutLine.has_value() ||
                        layoutLine->layout.height_px != tableRow.height_px ||
                        layoutLine->layout.collapsed_into_atomic_group ||
                        rowTop > std::numeric_limits<uint64_t>::max() - tableRow.height_px) {
                        return false;
                    }
                    rowTop += tableRow.height_px;
                }
                if (rowTop != placement.height_px) return false;
            }
            uint64_t measuredHeight = 0;
            for (size_t line = group.first_line.value; line <= group.last_line.value; ++line) {
                const std::optional<NoteRenderLineLayoutLocation> layoutLine =
                    NoteRenderLineLayoutMap::LineAt(layout.line_layouts(), {line});
                if (!layoutLine.has_value()) return false;
                const NoteRenderLineLayout& lineLayout = layoutLine->layout;
                if (group.kind == NoteRenderAtomicGroupKind::BlockMath) {
                    const bool anchor = line == group.first_line.value;
                    if ((anchor && (lineLayout.height_px == 0 ||
                                    lineLayout.collapsed_into_atomic_group)) ||
                        (!anchor && (!lineLayout.collapsed_into_atomic_group ||
                                     lineLayout.height_px != 0 ||
                                     lineLayout.inline_extent_px != 0))) {
                        return false;
                    }
                } else if (lineLayout.height_px == 0 ||
                           lineLayout.collapsed_into_atomic_group) {
                    return false;
                }
                if (measuredHeight > std::numeric_limits<uint64_t>::max() -
                                         lineLayout.height_px) {
                    return false;
                }
                measuredHeight += lineLayout.height_px;
                if (line == std::numeric_limits<size_t>::max()) return false;
            }
            if (measuredHeight != placement.height_px) return false;
            if (group.kind == NoteRenderAtomicGroupKind::BlockMath) {
                NoteRenderSourceLinePlan anchorLine;
                if (!sourcePlan.ResolveLine(group.first_line, &anchorLine) ||
                    !std::any_of(anchorLine.runs.begin(), anchorLine.runs.end(),
                                 [&group](const NoteRenderSourceRun& run) {
                                     return run.kind == NoteRenderSourceRunKind::BlockMath &&
                                            run.source_span.start == group.source_span.start &&
                                            run.source_span.end == group.source_span.end;
                                 })) {
                    return false;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool ValidateLinePlacement(const NoteRenderSourceLinePlan& sourceLine,
                                         const NoteRenderLineLayout& layoutLine,
                                         NoteRenderLinePlacement* placement) noexcept {
    if (!placement) return false;
    if (layoutLine.collapsed_into_atomic_group) {
        return layoutLine.height_px == 0 && layoutLine.inline_extent_px == 0 &&
               sourceLine.runs.empty() && placement->runs.empty() &&
               placement->decorations.empty();
    }
    return layoutLine.height_px != 0 &&
           NormalizeImplicitFragments(placement, layoutLine.height_px) &&
           DecorationsMatchLine(sourceLine, *placement, layoutLine.height_px) &&
           placement->runs.size() == sourceLine.runs.size() &&
           std::equal(sourceLine.runs.begin(), sourceLine.runs.end(), placement->runs.begin(),
                      [](const NoteRenderSourceRun& sourceRun,
                         const NoteRenderRunPlacement& run) {
                          return BoundariesMatchRun(sourceRun, run);
                      });
}

} // namespace

class NoteRenderAtomicGroupPlacementStorage final {
public:
    [[nodiscard]] static bool Build(
        const std::vector<NoteRenderAtomicGroupPlacement>& placements,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage>* out) noexcept;
    [[nodiscard]] static bool Copy(
        const std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::vector<NoteRenderAtomicGroupPlacement>* out) noexcept;
    [[nodiscard]] static bool ResolveContaining(
        const std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLines,
        LineIndex line,
        NoteRenderAtomicGroupPlacement* out) noexcept;

private:
    std::vector<StoredAtomicGroupPlacement> placements_;
};

bool NoteRenderAtomicGroupPlacementStorage::Build(
    const std::vector<NoteRenderAtomicGroupPlacement>& placements,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage>* out) noexcept {
    if (!out || !sourceLines.valid()) return false;
    try {
        std::shared_ptr<NoteRenderAtomicGroupPlacementStorage> candidate(
            new NoteRenderAtomicGroupPlacementStorage());
        candidate->placements_.reserve(placements.size());
        LineIndex previousLast{};
        bool havePrevious = false;
        for (const NoteRenderAtomicGroupPlacement& placement : placements) {
            StoredAtomicGroupPlacement stored;
            if (placement.first_line > placement.last_line ||
                placement.last_line.value >= sourceLines.line_count() ||
                placement.width_px <= 0 || placement.height_px == 0 ||
                !MakeRelativeSpan(sourceLines, placement.source_span, &stored.source_span) ||
                (havePrevious && placement.first_line <= previousLast)) {
                return false;
            }
            stored.kind = placement.kind;
            stored.first_line = placement.first_line;
            stored.last_line = placement.last_line;
            stored.x_px = placement.x_px;
            stored.width_px = placement.width_px;
            stored.height_px = placement.height_px;
            stored.table_columns = placement.table_columns;
            stored.table_rows = placement.table_rows;
            stored.math_baseline_offset_px = placement.math_baseline_offset_px;
            candidate->placements_.push_back(std::move(stored));
            previousLast = placement.last_line;
            havePrevious = true;
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderAtomicGroupPlacementStorage::Copy(
    const std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::vector<NoteRenderAtomicGroupPlacement>* out) noexcept {
    if (!storage || !out || !sourceLines.valid()) return false;
    try {
        std::vector<NoteRenderAtomicGroupPlacement> candidate;
        candidate.reserve(storage->placements_.size());
        for (const StoredAtomicGroupPlacement& stored : storage->placements_) {
            Span sourceSpan;
            if (stored.first_line > stored.last_line ||
                stored.last_line.value >= sourceLines.line_count() ||
                !ResolveRelativeSpan(sourceLines, stored.source_span, &sourceSpan)) {
                return false;
            }
            NoteRenderAtomicGroupPlacement placement{
                stored.kind, sourceSpan, stored.first_line, stored.last_line,
                stored.x_px, stored.width_px, stored.height_px};
            placement.table_columns = stored.table_columns;
            placement.table_rows = stored.table_rows;
            placement.math_baseline_offset_px = stored.math_baseline_offset_px;
            candidate.push_back(std::move(placement));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderAtomicGroupPlacementStorage::ResolveContaining(
    const std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLines,
    LineIndex line,
    NoteRenderAtomicGroupPlacement* out) noexcept {
    if (!storage || !out || !sourceLines.valid() || line.value >= sourceLines.line_count()) return false;
    const auto upper = std::upper_bound(
        storage->placements_.begin(), storage->placements_.end(), line,
        [](LineIndex target, const StoredAtomicGroupPlacement& placement) {
            return target < placement.first_line;
        });
    if (upper == storage->placements_.begin()) return false;
    const StoredAtomicGroupPlacement& stored = *std::prev(upper);
    if (line > stored.last_line) return false;
    Span sourceSpan;
    if (!ResolveRelativeSpan(sourceLines, stored.source_span, &sourceSpan)) return false;
    NoteRenderAtomicGroupPlacement placement{
        stored.kind, sourceSpan, stored.first_line, stored.last_line,
        stored.x_px, stored.width_px, stored.height_px};
    placement.table_columns = stored.table_columns;
    placement.table_rows = stored.table_rows;
    placement.math_baseline_offset_px = stored.math_baseline_offset_px;
    *out = std::move(placement);
    return true;
}

class NoteRenderPlacementStorage final {
public:
    [[nodiscard]] static bool Build(
        const std::vector<NoteRenderLinePlacement>& lines,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::shared_ptr<const NoteRenderPlacementStorage>* out) noexcept;
    [[nodiscard]] static bool Replace(
        std::shared_ptr<const NoteRenderPlacementStorage> before,
        LineIndex first,
        LineIndex lastExclusive,
        const std::vector<NoteRenderLinePlacement>& replacement,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::shared_ptr<const NoteRenderPlacementStorage>* out) noexcept;
    [[nodiscard]] static bool Resolve(
        const std::shared_ptr<const NoteRenderPlacementStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLines,
        LineIndex index,
        NoteRenderLinePlacement* out) noexcept;
    [[nodiscard]] static bool SharesLinePayload(
        const std::shared_ptr<const NoteRenderPlacementStorage>& lhs,
        const std::shared_ptr<const NoteRenderPlacementStorage>& rhs,
        LineIndex index) noexcept;

    [[nodiscard]] size_t line_count() const noexcept { return line_count_; }

private:
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

    [[nodiscard]] static bool MakeStoredLine(const NoteRenderLinePlacement& line,
                                             const NoteSourceLineMap::Snapshot& sourceLines,
                                             StoredLinePlacement* out) noexcept;
    [[nodiscard]] static bool ResolveStoredLine(const StoredLinePlacement& line,
                                                const NoteSourceLineMap::Snapshot& sourceLines,
                                                NoteRenderLinePlacement* out) noexcept;
    [[nodiscard]] static size_t SubtreeLineCount(const NodePtr& node) noexcept;
    [[nodiscard]] static uint64_t PriorityFor(uint64_t id) noexcept;
    [[nodiscard]] static bool TakeNodeId(uint64_t* nextNodeId, uint64_t* outId) noexcept;
    [[nodiscard]] static NodePtr MakeNode(std::shared_ptr<const StoredLinePlacement> line,
                                          uint64_t priority, NodePtr left, NodePtr right);
    [[nodiscard]] static NodePtr CloneNode(const NodePtr& node, NodePtr left, NodePtr right);
    [[nodiscard]] static NodePtr Merge(const NodePtr& left, const NodePtr& right);
    static void Split(const NodePtr& node, size_t leftLineCount, NodePtr* outLeft, NodePtr* outRight);
    [[nodiscard]] static bool BuildTree(
        const std::vector<std::shared_ptr<const StoredLinePlacement>>& lines,
        uint64_t* nextNodeId, NodePtr* outRoot) noexcept;
    [[nodiscard]] static const StoredLinePlacement* LineAt(const NodePtr& root,
                                                            LineIndex index) noexcept;

    NodePtr root_;
    size_t line_count_ = 0;
    uint64_t next_node_id_ = 1;
};

struct NoteRenderPlacementStorage::Node {
    Node(std::shared_ptr<const StoredLinePlacement> value, uint64_t nodePriority,
         NodePtr nodeLeft, NodePtr nodeRight)
        : line(std::move(value)), priority(nodePriority), left(std::move(nodeLeft)),
          right(std::move(nodeRight)),
          subtree_line_count(SubtreeLineCount(left) + 1 + SubtreeLineCount(right)) {}

    std::shared_ptr<const StoredLinePlacement> line;
    uint64_t priority = 0;
    NodePtr left;
    NodePtr right;
    size_t subtree_line_count = 0;
};

bool NoteRenderPlacementStorage::MakeStoredLine(const NoteRenderLinePlacement& line,
                                                const NoteSourceLineMap::Snapshot& sourceLines,
                                                StoredLinePlacement* out) noexcept {
    if (!out) return false;
    try {
        StoredLinePlacement candidate;
        candidate.runs.reserve(line.runs.size());
        for (const NoteRenderRunPlacement& run : line.runs) {
            StoredRunPlacement stored;
            if (run.width_px < 0 || run.boundaries.size() < 2 ||
                !MakeRelativeSpan(sourceLines, run.source_span, &stored.source_span)) return false;
            stored.x_px = run.x_px;
            stored.width_px = run.width_px;
            stored.boundaries.reserve(run.boundaries.size());
            for (const NoteRenderPlacementBoundary& boundary : run.boundaries) {
                StoredPlacementBoundary storedBoundary;
                if (!MakeRelativeBoundary(sourceLines, boundary.source_offset,
                                          &storedBoundary.source_offset)) return false;
                storedBoundary.x_px = boundary.x_px;
                storedBoundary.fragment_index = boundary.fragment_index;
                stored.boundaries.push_back(std::move(storedBoundary));
            }
            stored.fragments.reserve(run.fragments.size());
            for (const NoteRenderRunPlacementFragment& fragment : run.fragments) {
                StoredRunPlacementFragment storedFragment;
                if (!MakeRelativeSpan(sourceLines, fragment.source_span,
                                      &storedFragment.source_span)) return false;
                storedFragment.x_px = fragment.x_px;
                storedFragment.width_px = fragment.width_px;
                storedFragment.top_offset_px = fragment.top_offset_px;
                storedFragment.height_px = fragment.height_px;
                storedFragment.baseline_offset_px = fragment.baseline_offset_px;
                stored.fragments.push_back(std::move(storedFragment));
            }
            candidate.runs.push_back(std::move(stored));
        }
        candidate.decorations.reserve(line.decorations.size());
        for (const NoteRenderVisualDecoration& decoration : line.decorations) {
            StoredVisualDecoration stored;
            if (!MakeRelativeSpan(sourceLines, decoration.source_span, &stored.source_span)) return false;
            stored.kind = decoration.kind;
            stored.left_px = decoration.left_px;
            stored.top_offset_px = decoration.top_offset_px;
            stored.right_px = decoration.right_px;
            stored.bottom_offset_px = decoration.bottom_offset_px;
            stored.text = decoration.text;
            stored.checked = decoration.checked;
            candidate.decorations.push_back(std::move(stored));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderPlacementStorage::ResolveStoredLine(const StoredLinePlacement& line,
                                                   const NoteSourceLineMap::Snapshot& sourceLines,
                                                   NoteRenderLinePlacement* out) noexcept {
    if (!out) return false;
    try {
        NoteRenderLinePlacement candidate;
        candidate.runs.reserve(line.runs.size());
        for (const StoredRunPlacement& stored : line.runs) {
            NoteRenderRunPlacement run;
            if (!ResolveRelativeSpan(sourceLines, stored.source_span, &run.source_span)) return false;
            run.x_px = stored.x_px;
            run.width_px = stored.width_px;
            run.boundaries.reserve(stored.boundaries.size());
            for (const StoredPlacementBoundary& storedBoundary : stored.boundaries) {
                NoteRenderPlacementBoundary boundary;
                if (!ResolveRelativeBoundary(sourceLines, storedBoundary.source_offset,
                                             &boundary.source_offset)) return false;
                boundary.x_px = storedBoundary.x_px;
                boundary.fragment_index = storedBoundary.fragment_index;
                run.boundaries.push_back(std::move(boundary));
            }
            run.fragments.reserve(stored.fragments.size());
            for (const StoredRunPlacementFragment& storedFragment : stored.fragments) {
                NoteRenderRunPlacementFragment fragment;
                if (!ResolveRelativeSpan(sourceLines, storedFragment.source_span,
                                         &fragment.source_span)) return false;
                fragment.x_px = storedFragment.x_px;
                fragment.width_px = storedFragment.width_px;
                fragment.top_offset_px = storedFragment.top_offset_px;
                fragment.height_px = storedFragment.height_px;
                fragment.baseline_offset_px = storedFragment.baseline_offset_px;
                run.fragments.push_back(std::move(fragment));
            }
            candidate.runs.push_back(std::move(run));
        }
        candidate.decorations.reserve(line.decorations.size());
        for (const StoredVisualDecoration& stored : line.decorations) {
            NoteRenderVisualDecoration decoration;
            if (!ResolveRelativeSpan(sourceLines, stored.source_span, &decoration.source_span)) {
                return false;
            }
            decoration.kind = stored.kind;
            decoration.left_px = stored.left_px;
            decoration.top_offset_px = stored.top_offset_px;
            decoration.right_px = stored.right_px;
            decoration.bottom_offset_px = stored.bottom_offset_px;
            decoration.text = stored.text;
            decoration.checked = stored.checked;
            candidate.decorations.push_back(std::move(decoration));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

size_t NoteRenderPlacementStorage::SubtreeLineCount(const NodePtr& node) noexcept {
    return node ? node->subtree_line_count : 0;
}

uint64_t NoteRenderPlacementStorage::PriorityFor(uint64_t id) noexcept {
    uint64_t value = id + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

bool NoteRenderPlacementStorage::TakeNodeId(uint64_t* nextNodeId, uint64_t* outId) noexcept {
    if (!nextNodeId || !outId || *nextNodeId == 0) return false;
    *outId = *nextNodeId;
    *nextNodeId = *nextNodeId == std::numeric_limits<uint64_t>::max()
        ? 0 : *nextNodeId + 1;
    return true;
}

NoteRenderPlacementStorage::NodePtr NoteRenderPlacementStorage::MakeNode(
    std::shared_ptr<const StoredLinePlacement> line, uint64_t priority, NodePtr left, NodePtr right) {
    return std::make_shared<const Node>(std::move(line), priority, std::move(left), std::move(right));
}

NoteRenderPlacementStorage::NodePtr NoteRenderPlacementStorage::CloneNode(
    const NodePtr& node, NodePtr left, NodePtr right) {
    if (!node) return nullptr;
    return MakeNode(node->line, node->priority, std::move(left), std::move(right));
}

NoteRenderPlacementStorage::NodePtr NoteRenderPlacementStorage::Merge(
    const NodePtr& left, const NodePtr& right) {
    if (!left) return right;
    if (!right) return left;
    if (left->priority <= right->priority) {
        return CloneNode(left, left->left, Merge(left->right, right));
    }
    return CloneNode(right, Merge(left, right->left), right->right);
}

void NoteRenderPlacementStorage::Split(const NodePtr& node, size_t leftLineCount,
                                       NodePtr* outLeft, NodePtr* outRight) {
    if (!outLeft || !outRight) return;
    if (!node) {
        outLeft->reset();
        outRight->reset();
        return;
    }
    const size_t existingLeftCount = SubtreeLineCount(node->left);
    if (leftLineCount <= existingLeftCount) {
        NodePtr splitLeft;
        NodePtr splitRight;
        Split(node->left, leftLineCount, &splitLeft, &splitRight);
        *outLeft = std::move(splitLeft);
        *outRight = CloneNode(node, std::move(splitRight), node->right);
        return;
    }
    NodePtr splitLeft;
    NodePtr splitRight;
    Split(node->right, leftLineCount - existingLeftCount - 1, &splitLeft, &splitRight);
    *outLeft = CloneNode(node, node->left, std::move(splitLeft));
    *outRight = std::move(splitRight);
}

bool NoteRenderPlacementStorage::BuildTree(
    const std::vector<std::shared_ptr<const StoredLinePlacement>>& lines,
    uint64_t* nextNodeId, NodePtr* outRoot) noexcept {
    if (!nextNodeId || !outRoot || lines.empty()) return false;
    try {
        NodePtr root;
        for (const std::shared_ptr<const StoredLinePlacement>& line : lines) {
            uint64_t id = 0;
            if (!line || !TakeNodeId(nextNodeId, &id)) return false;
            root = Merge(root, MakeNode(line, PriorityFor(id), nullptr, nullptr));
        }
        if (!root || SubtreeLineCount(root) != lines.size()) return false;
        *outRoot = std::move(root);
        return true;
    } catch (...) {
        return false;
    }
}

const StoredLinePlacement* NoteRenderPlacementStorage::LineAt(const NodePtr& root,
                                                               LineIndex index) noexcept {
    const Node* current = root.get();
    size_t target = index.value;
    while (current) {
        const size_t leftCount = SubtreeLineCount(current->left);
        if (target < leftCount) current = current->left.get();
        else if (target == leftCount) return current->line.get();
        else {
            target -= leftCount + 1;
            current = current->right.get();
        }
    }
    return nullptr;
}

bool NoteRenderPlacementStorage::Build(
    const std::vector<NoteRenderLinePlacement>& lines,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::shared_ptr<const NoteRenderPlacementStorage>* out) noexcept {
    if (!out || !sourceLines.valid() || lines.empty() || lines.size() != sourceLines.line_count()) {
        return false;
    }
    try {
        std::vector<std::shared_ptr<const StoredLinePlacement>> storedLines;
        storedLines.reserve(lines.size());
        for (const NoteRenderLinePlacement& line : lines) {
            StoredLinePlacement stored;
            if (!MakeStoredLine(line, sourceLines, &stored)) return false;
            storedLines.push_back(std::make_shared<const StoredLinePlacement>(std::move(stored)));
        }
        std::shared_ptr<NoteRenderPlacementStorage> candidate(new NoteRenderPlacementStorage());
        if (!BuildTree(storedLines, &candidate->next_node_id_, &candidate->root_)) return false;
        candidate->line_count_ = SubtreeLineCount(candidate->root_);
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderPlacementStorage::Replace(
    std::shared_ptr<const NoteRenderPlacementStorage> before, LineIndex first, LineIndex lastExclusive,
    const std::vector<NoteRenderLinePlacement>& replacement,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::shared_ptr<const NoteRenderPlacementStorage>* out) noexcept {
    if (!out || !before || first.value > lastExclusive.value ||
        lastExclusive.value > before->line_count_ || replacement.empty()) return false;
    const size_t replacementLineCount = before->line_count_ -
        (lastExclusive.value - first.value) + replacement.size();
    if (!sourceLines.valid() || replacementLineCount != sourceLines.line_count()) return false;
    try {
        std::vector<std::shared_ptr<const StoredLinePlacement>> storedLines;
        storedLines.reserve(replacement.size());
        for (const NoteRenderLinePlacement& line : replacement) {
            StoredLinePlacement stored;
            if (!MakeStoredLine(line, sourceLines, &stored)) return false;
            storedLines.push_back(std::make_shared<const StoredLinePlacement>(std::move(stored)));
        }
        uint64_t nextNodeId = before->next_node_id_;
        NodePtr replacementRoot;
        if (!BuildTree(storedLines, &nextNodeId, &replacementRoot)) return false;
        NodePtr left;
        NodePtr middleAndRight;
        NodePtr removed;
        NodePtr right;
        Split(before->root_, first.value, &left, &middleAndRight);
        Split(middleAndRight, lastExclusive.value - first.value, &removed, &right);
        NodePtr root = Merge(Merge(left, replacementRoot), right);
        if (!root || SubtreeLineCount(root) != replacementLineCount) return false;
        std::shared_ptr<NoteRenderPlacementStorage> candidate(new NoteRenderPlacementStorage());
        candidate->root_ = std::move(root);
        candidate->line_count_ = replacementLineCount;
        candidate->next_node_id_ = nextNodeId;
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderPlacementStorage::Resolve(
    const std::shared_ptr<const NoteRenderPlacementStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLines, LineIndex index,
    NoteRenderLinePlacement* out) noexcept {
    if (!storage || !out || !sourceLines.valid() || index.value >= storage->line_count_ ||
        storage->line_count_ != sourceLines.line_count()) return false;
    const StoredLinePlacement* stored = LineAt(storage->root_, index);
    return stored && ResolveStoredLine(*stored, sourceLines, out);
}

bool NoteRenderPlacementStorage::SharesLinePayload(
    const std::shared_ptr<const NoteRenderPlacementStorage>& lhs,
    const std::shared_ptr<const NoteRenderPlacementStorage>& rhs, LineIndex index) noexcept {
    if (!lhs || !rhs || index.value >= lhs->line_count_ || index.value >= rhs->line_count_) return false;
    const StoredLinePlacement* lhsLine = LineAt(lhs->root_, index);
    const StoredLinePlacement* rhsLine = LineAt(rhs->root_, index);
    return lhsLine && lhsLine == rhsLine;
}

NoteRenderPlacementSnapshotBuildResult NoteRenderPlacementSnapshot::Build(
    std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
    std::vector<NoteRenderLinePlacement> linePlacements,
    std::vector<NoteRenderAtomicGroupPlacement> atomicGroupPlacements,
    std::shared_ptr<const NoteRenderPlacementSnapshot>* out) noexcept {
    if (!out) return NoteRenderPlacementSnapshotBuildResult::InvalidOutput;
    if (!sourcePlan || !sourcePlan->valid()) return NoteRenderPlacementSnapshotBuildResult::InvalidSourcePlan;
    if (!layout || !layout->valid()) return NoteRenderPlacementSnapshotBuildResult::InvalidLayoutSnapshot;
    if (sourcePlan->syntax() != layout->syntax()) {
        return NoteRenderPlacementSnapshotBuildResult::SourceLayoutIdentityMismatch;
    }
    if (linePlacements.size() != sourcePlan->line_count() ||
        linePlacements.size() != layout->line_layouts().line_count()) {
        return NoteRenderPlacementSnapshotBuildResult::LineCountMismatch;
    }
    try {
        if (!ValidateAtomicGroupPlacements(*sourcePlan, *layout, atomicGroupPlacements)) {
            return NoteRenderPlacementSnapshotBuildResult::BoundaryMismatch;
        }
        for (size_t line = 0; line < linePlacements.size(); ++line) {
            const std::optional<NoteRenderLineLayoutLocation> layoutLine =
                NoteRenderLineLayoutMap::LineAt(layout->line_layouts(), {line});
            NoteRenderSourceLinePlan sourceLine;
            if (!layoutLine.has_value() || !sourcePlan->ResolveLine({line}, &sourceLine) ||
                !ValidateLinePlacement(sourceLine, layoutLine->layout, &linePlacements[line])) {
                return NoteRenderPlacementSnapshotBuildResult::BoundaryMismatch;
            }
        }
        std::shared_ptr<const NoteRenderPlacementStorage> storage;
        if (!NoteRenderPlacementStorage::Build(
                linePlacements, sourcePlan->syntax()->source_line_map(), &storage)) {
            return NoteRenderPlacementSnapshotBuildResult::BoundaryMismatch;
        }
        std::shared_ptr<const NoteRenderAtomicGroupPlacementStorage> atomicGroupStorage;
        if (!NoteRenderAtomicGroupPlacementStorage::Build(
                atomicGroupPlacements, sourcePlan->syntax()->source_line_map(),
                &atomicGroupStorage)) {
            return NoteRenderPlacementSnapshotBuildResult::BoundaryMismatch;
        }
        std::shared_ptr<NoteRenderPlacementSnapshot> candidate(new NoteRenderPlacementSnapshot());
        candidate->source_plan_ = std::move(sourcePlan);
        candidate->layout_ = std::move(layout);
        candidate->line_storage_ = std::move(storage);
        candidate->atomic_group_storage_ = std::move(atomicGroupStorage);
        candidate->valid_ = true;
        *out = std::move(candidate);
        return NoteRenderPlacementSnapshotBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderPlacementSnapshotBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderPlacementSnapshotBuildResult::AllocationFailure;
    }
}

NoteRenderPlacementSnapshotBuildResult NoteRenderPlacementSnapshot::Build(
    std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
    std::vector<NoteRenderLinePlacement> linePlacements,
    std::shared_ptr<const NoteRenderPlacementSnapshot>* out) noexcept {
    return Build(std::move(sourcePlan), std::move(layout), std::move(linePlacements), {}, out);
}

NoteRenderPlacementSnapshotLocalSpliceResult NoteRenderPlacementSnapshot::BuildLocalSplice(
    std::shared_ptr<const NoteRenderPlacementSnapshot> previous,
    std::shared_ptr<const NoteRenderSourcePlan> sourcePlan,
    std::shared_ptr<const NoteRenderLayoutSnapshot> layout,
    LineIndex first, LineIndex lastExclusive,
    std::vector<NoteRenderLinePlacement> replacement,
    std::shared_ptr<const NoteRenderPlacementSnapshot>* out,
    NoteRenderPlacementSnapshotLocalSpliceWork* outWork) noexcept {
    if (!out) return NoteRenderPlacementSnapshotLocalSpliceResult::InvalidOutput;
    if (!previous || !previous->valid_ || !previous->source_plan_ || !previous->layout_ ||
        !previous->line_storage_ || !previous->atomic_group_storage_) {
        return NoteRenderPlacementSnapshotLocalSpliceResult::InvalidPreviousSnapshot;
    }
    if (!sourcePlan || !sourcePlan->valid()) return NoteRenderPlacementSnapshotLocalSpliceResult::InvalidSourcePlan;
    if (!layout || !layout->valid()) return NoteRenderPlacementSnapshotLocalSpliceResult::InvalidLayoutSnapshot;
    if (sourcePlan->syntax() != layout->syntax()) {
        return NoteRenderPlacementSnapshotLocalSpliceResult::SourceLayoutIdentityMismatch;
    }
    if (first.value >= lastExclusive.value || lastExclusive.value > previous->line_count() ||
        previous->line_count() != sourcePlan->line_count() ||
        replacement.size() != lastExclusive.value - first.value) {
        return NoteRenderPlacementSnapshotLocalSpliceResult::InvalidRange;
    }
    if (sourcePlan != previous->source_plan_ &&
        !sourcePlan->ProvesUnchangedRowsFrom(*previous->source_plan_, first, lastExclusive)) {
        return NoteRenderPlacementSnapshotLocalSpliceResult::ReplacementRejected;
    }
    try {
        std::vector<NoteRenderAtomicGroupPlacement> atomicGroupPlacements;
        if (!NoteRenderAtomicGroupPlacementStorage::Copy(
                previous->atomic_group_storage_, sourcePlan->syntax()->source_line_map(),
                &atomicGroupPlacements) ||
            !ValidateAtomicGroupPlacements(*sourcePlan, *layout, atomicGroupPlacements)) {
            return NoteRenderPlacementSnapshotLocalSpliceResult::ReplacementRejected;
        }
        for (size_t offset = 0; offset < replacement.size(); ++offset) {
            const LineIndex lineIndex{first.value + offset};
            const std::optional<NoteRenderLineLayoutLocation> layoutLine =
                NoteRenderLineLayoutMap::LineAt(layout->line_layouts(), lineIndex);
            NoteRenderSourceLinePlan sourceLine;
            if (!layoutLine.has_value() || !sourcePlan->ResolveLine(lineIndex, &sourceLine) ||
                !ValidateLinePlacement(sourceLine, layoutLine->layout, &replacement[offset])) {
                return NoteRenderPlacementSnapshotLocalSpliceResult::ReplacementRejected;
            }
        }
        std::shared_ptr<const NoteRenderPlacementStorage> storage;
        if (!NoteRenderPlacementStorage::Replace(
                previous->line_storage_, first, lastExclusive, replacement,
                sourcePlan->syntax()->source_line_map(), &storage)) {
            return NoteRenderPlacementSnapshotLocalSpliceResult::ReplacementRejected;
        }
        std::shared_ptr<NoteRenderPlacementSnapshot> candidate(new NoteRenderPlacementSnapshot());
        candidate->source_plan_ = std::move(sourcePlan);
        candidate->layout_ = std::move(layout);
        candidate->line_storage_ = std::move(storage);
        candidate->atomic_group_storage_ = previous->atomic_group_storage_;
        candidate->valid_ = true;
        if (outWork) *outWork = {replacement.size(), 0};
        *out = std::move(candidate);
        return NoteRenderPlacementSnapshotLocalSpliceResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderPlacementSnapshotLocalSpliceResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderPlacementSnapshotLocalSpliceResult::AllocationFailure;
    }
}

size_t NoteRenderPlacementSnapshot::line_count() const noexcept {
    return line_storage_ ? line_storage_->line_count() : 0;
}

bool NoteRenderPlacementSnapshot::ResolveLine(LineIndex index,
                                              NoteRenderLinePlacement* out) const noexcept {
    return valid_ && source_plan_ && line_storage_ &&
           NoteRenderPlacementStorage::Resolve(
               line_storage_, source_plan_->syntax()->source_line_map(), index, out);
}

bool NoteRenderPlacementSnapshot::ResolveAtomicGroupContaining(
    LineIndex index,
    NoteRenderAtomicGroupPlacement* out) const noexcept {
    return valid_ && source_plan_ && atomic_group_storage_ &&
           NoteRenderAtomicGroupPlacementStorage::ResolveContaining(
               atomic_group_storage_, source_plan_->syntax()->source_line_map(), index, out);
}

bool NoteRenderPlacementSnapshot::CopyAtomicGroupPlacementsForDifferentialTest(
    std::vector<NoteRenderAtomicGroupPlacement>* out) const noexcept {
    return valid_ && source_plan_ && atomic_group_storage_ &&
           NoteRenderAtomicGroupPlacementStorage::Copy(
               atomic_group_storage_, source_plan_->syntax()->source_line_map(), out);
}

bool NoteRenderPlacementSnapshot::CopyLinesForDifferentialTest(
    std::vector<NoteRenderLinePlacement>* out) const noexcept {
    if (!out || !valid_) return false;
    try {
        std::vector<NoteRenderLinePlacement> candidate;
        candidate.reserve(line_count());
        for (size_t index = 0; index < line_count(); ++index) {
            NoteRenderLinePlacement line;
            if (!ResolveLine({index}, &line)) return false;
            candidate.push_back(std::move(line));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderPlacementSnapshot::SharesLinePayloadForDifferentialTest(
    const NoteRenderPlacementSnapshot& other, LineIndex index) const noexcept {
    return valid_ && other.valid_ && line_storage_ && other.line_storage_ &&
           NoteRenderPlacementStorage::SharesLinePayload(line_storage_, other.line_storage_, index);
}

NoteDerivedSnapshotIdentity NoteRenderPlacementSnapshot::source_identity() const noexcept {
    return source_plan_ ? source_plan_->source_identity() : NoteDerivedSnapshotIdentity{};
}

bool NoteRenderPlacementSnapshot::Matches(const NoteTextCore& textCore,
                                          const NoteRenderLayoutKey& layoutKey) const noexcept {
    return valid_ && source_plan_ && layout_ && line_storage_ && source_plan_->Matches(textCore) &&
           layout_->Matches(textCore, layoutKey) && source_plan_->syntax() == layout_->syntax() &&
           line_count() == source_plan_->line_count();
}

} // namespace note
