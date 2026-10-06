#include "note/note_render_source_plan.h"
#include "note/note_source_coordinate_transform.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {

namespace {

struct SourcePlanRelativeBoundary {
    LineIndex line{};
    size_t offset_from_line_start = 0;
};

struct SourcePlanRelativeSpan {
    SourcePlanRelativeBoundary start{};
    SourcePlanRelativeBoundary end{};
};

[[nodiscard]] bool MakeRelativeBoundary(
    const NoteSourceLineMap::Snapshot& sourceLines,
    Utf16CodeUnitOffset offset,
    SourcePlanRelativeBoundary* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::FindByOffset(sourceLines, offset);
    if (!location.has_value() || offset.value < location->start.value ||
        offset.value > location->next_start.value) {
        return false;
    }
    *out = {location->line_index, offset.value - location->start.value};
    return true;
}

[[nodiscard]] bool ResolveRelativeBoundary(
    const NoteSourceLineMap::Snapshot& sourceLines,
    SourcePlanRelativeBoundary boundary,
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
                                    SourcePlanRelativeSpan* out) noexcept {
    if (!out || span.end < span.start) return false;
    SourcePlanRelativeBoundary start;
    SourcePlanRelativeBoundary end;
    if (!MakeRelativeBoundary(sourceLines, span.start, &start) ||
        !MakeRelativeBoundary(sourceLines, span.end, &end)) {
        return false;
    }
    *out = {start, end};
    return true;
}

[[nodiscard]] bool ResolveRelativeSpan(const NoteSourceLineMap::Snapshot& sourceLines,
                                       SourcePlanRelativeSpan span,
                                       Span* out) noexcept {
    if (!out) return false;
    Utf16CodeUnitOffset start;
    Utf16CodeUnitOffset end;
    if (!ResolveRelativeBoundary(sourceLines, span.start, &start) ||
        !ResolveRelativeBoundary(sourceLines, span.end, &end) || end < start) {
        return false;
    }
    *out = {start, end};
    return true;
}

struct StoredSourceRun {
    SourcePlanRelativeSpan source_span{};
    SourcePlanRelativeSpan display_source_span{};
    LineIndex line_index{};
    size_t parent_block = static_cast<size_t>(-1);
    int heading_level = 0;
    NoteRenderSourceRunKind kind = NoteRenderSourceRunKind::Text;
    std::wstring link_target;
    std::vector<NoteRenderSourceStyleAttribute> styles;
    bool decodes_markdown_escapes = false;
    size_t table_block = NoteRenderSourceRun::kNoTableBlock;
    size_t table_row_block = NoteRenderSourceRun::kNoTableBlock;
    size_t table_column = NoteRenderSourceRun::kNoTableColumn;
    size_t table_column_count = 0;
    TableCellAlign table_cell_align = TableCellAlign::Default;
    bool table_header = false;
};

struct StoredSourceLinePlan {
    NoteRenderSourceLineDecoration decoration{};
    std::vector<StoredSourceRun> runs;
};

struct StoredAtomicGroup {
    NoteRenderAtomicGroupKind kind = NoteRenderAtomicGroupKind::Container;
    SourcePlanRelativeSpan source_span{};
    LineIndex first_line{};
    LineIndex last_line{};
};

} // namespace

class NoteRenderSourcePlanStorage final {
public:
    [[nodiscard]] static bool Build(
        const std::vector<NoteRenderSourceLinePlan>& lines,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::shared_ptr<const NoteRenderSourcePlanStorage>* out) noexcept;
    [[nodiscard]] static bool Replace(
        std::shared_ptr<const NoteRenderSourcePlanStorage> before,
        LineIndex first,
        LineIndex lastExclusive,
        const std::vector<NoteRenderSourceLinePlan>& replacement,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::shared_ptr<const NoteRenderSourcePlanStorage>* out) noexcept;
    [[nodiscard]] static bool Resolve(
        const std::shared_ptr<const NoteRenderSourcePlanStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLines,
        LineIndex index,
        NoteRenderSourceLinePlan* out) noexcept;
    [[nodiscard]] static bool SharesLinePayload(
        const std::shared_ptr<const NoteRenderSourcePlanStorage>& lhs,
        const std::shared_ptr<const NoteRenderSourcePlanStorage>& rhs,
        LineIndex index) noexcept;

    [[nodiscard]] size_t line_count() const noexcept { return line_count_; }

private:
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

    [[nodiscard]] static bool MakeStoredLine(
        const NoteRenderSourceLinePlan& line,
        const NoteSourceLineMap::Snapshot& sourceLines,
        LineIndex index,
        StoredSourceLinePlan* out) noexcept;
    [[nodiscard]] static bool ResolveStoredLine(
        const StoredSourceLinePlan& line,
        const NoteSourceLineMap::Snapshot& sourceLines,
        LineIndex index,
        NoteRenderSourceLinePlan* out) noexcept;
    [[nodiscard]] static size_t SubtreeLineCount(const NodePtr& node) noexcept;
    [[nodiscard]] static uint64_t PriorityFor(uint64_t id) noexcept;
    [[nodiscard]] static bool TakeNodeId(uint64_t* nextNodeId, uint64_t* outId) noexcept;
    [[nodiscard]] static NodePtr MakeNode(std::shared_ptr<const StoredSourceLinePlan> line,
                                          uint64_t priority,
                                          NodePtr left,
                                          NodePtr right);
    [[nodiscard]] static NodePtr CloneNode(const NodePtr& node, NodePtr left, NodePtr right);
    [[nodiscard]] static NodePtr Merge(const NodePtr& left, const NodePtr& right);
    static void Split(const NodePtr& node,
                      size_t leftLineCount,
                      NodePtr* outLeft,
                      NodePtr* outRight);
    [[nodiscard]] static bool BuildTree(
        const std::vector<std::shared_ptr<const StoredSourceLinePlan>>& lines,
        uint64_t* nextNodeId,
        NodePtr* outRoot) noexcept;
    [[nodiscard]] static const StoredSourceLinePlan* LineAt(
        const NodePtr& root,
        LineIndex index) noexcept;

    NodePtr root_;
    size_t line_count_ = 0;
    uint64_t next_node_id_ = 1;
};

class NoteRenderSourceAtomicGroupStorage final {
public:
    [[nodiscard]] static bool Build(
        const std::vector<NoteRenderAtomicGroup>& groups,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>* out) noexcept;
    [[nodiscard]] static bool CopyAll(
        const std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLines,
        std::vector<NoteRenderAtomicGroup>* out) noexcept;
    [[nodiscard]] static bool CopyIntersecting(
        const std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLines,
        LineIndex first,
        LineIndex lastExclusive,
        std::vector<NoteRenderAtomicGroup>* out,
        NoteRenderAtomicGroupQueryWork* outWork) noexcept;
    [[nodiscard]] static bool ContainsLine(
        const std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>& storage,
        LineIndex line) noexcept;

private:
    struct IntervalNode {
        size_t group_index = std::numeric_limits<size_t>::max();
        size_t left = std::numeric_limits<size_t>::max();
        size_t right = std::numeric_limits<size_t>::max();
        size_t subtree_max_last_line = 0;
    };

    static constexpr size_t kNoIntervalNode = std::numeric_limits<size_t>::max();

    [[nodiscard]] static bool BuildIntervalIndex(
        NoteRenderSourceAtomicGroupStorage* storage,
        size_t first,
        size_t lastExclusive,
        size_t* outNode) noexcept;
    [[nodiscard]] static bool AppendIntersecting(
        const NoteRenderSourceAtomicGroupStorage& storage,
        size_t nodeIndex,
        const NoteSourceLineMap::Snapshot& sourceLines,
        LineIndex first,
        LineIndex lastExclusive,
        std::vector<NoteRenderAtomicGroup>* out,
        NoteRenderAtomicGroupQueryWork* outWork) noexcept;
    [[nodiscard]] static bool ContainsLineAt(
        const NoteRenderSourceAtomicGroupStorage& storage,
        size_t nodeIndex,
        LineIndex line) noexcept;

    std::vector<StoredAtomicGroup> groups_;
    std::vector<IntervalNode> interval_nodes_;
    size_t interval_root_ = kNoIntervalNode;
};

struct NoteRenderSourcePlanStorage::Node {
    Node(std::shared_ptr<const StoredSourceLinePlan> value,
         uint64_t nodePriority,
         NodePtr nodeLeft,
         NodePtr nodeRight)
        : line(std::move(value)),
          priority(nodePriority),
          left(std::move(nodeLeft)),
          right(std::move(nodeRight)),
          subtree_line_count(SubtreeLineCount(left) + 1 + SubtreeLineCount(right)) {}

    std::shared_ptr<const StoredSourceLinePlan> line;
    uint64_t priority = 0;
    NodePtr left;
    NodePtr right;
    size_t subtree_line_count = 0;
};

bool NoteRenderSourcePlanStorage::MakeStoredLine(
    const NoteRenderSourceLinePlan& line,
    const NoteSourceLineMap::Snapshot& sourceLines,
    LineIndex index,
    StoredSourceLinePlan* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::LineAt(sourceLines, index);
    if (!location.has_value() || line.content_span.start != location->start ||
        line.content_span.end != location->content_end) {
        return false;
    }
    try {
        StoredSourceLinePlan candidate;
        candidate.decoration = line.decoration;
        candidate.runs.reserve(line.runs.size());
        for (const NoteRenderSourceRun& run : line.runs) {
            StoredSourceRun stored;
            if (run.line_index != index ||
                !MakeRelativeSpan(sourceLines, run.source_span, &stored.source_span) ||
                !MakeRelativeSpan(sourceLines, run.display_source_span,
                                  &stored.display_source_span)) {
                return false;
            }
            stored.line_index = run.line_index;
            stored.parent_block = run.parent_block;
            stored.heading_level = run.heading_level;
            stored.kind = run.kind;
            stored.link_target = run.link_target;
            stored.styles = run.styles;
            stored.decodes_markdown_escapes = run.decodes_markdown_escapes;
            stored.table_block = run.table_block;
            stored.table_row_block = run.table_row_block;
            stored.table_column = run.table_column;
            stored.table_column_count = run.table_column_count;
            stored.table_cell_align = run.table_cell_align;
            stored.table_header = run.table_header;
            candidate.runs.push_back(std::move(stored));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderSourcePlanStorage::ResolveStoredLine(
    const StoredSourceLinePlan& line,
    const NoteSourceLineMap::Snapshot& sourceLines,
    LineIndex index,
    NoteRenderSourceLinePlan* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::LineAt(sourceLines, index);
    if (!location.has_value()) return false;
    try {
        NoteRenderSourceLinePlan candidate;
        candidate.content_span = {location->start, location->content_end};
        candidate.decoration = line.decoration;
        candidate.runs.reserve(line.runs.size());
        for (const StoredSourceRun& stored : line.runs) {
            NoteRenderSourceRun run;
            if (stored.line_index != index ||
                !ResolveRelativeSpan(sourceLines, stored.source_span, &run.source_span) ||
                !ResolveRelativeSpan(sourceLines, stored.display_source_span,
                                     &run.display_source_span)) {
                return false;
            }
            run.line_index = stored.line_index;
            run.parent_block = stored.parent_block;
            run.heading_level = stored.heading_level;
            run.kind = stored.kind;
            run.link_target = stored.link_target;
            run.styles = stored.styles;
            run.decodes_markdown_escapes = stored.decodes_markdown_escapes;
            run.table_block = stored.table_block;
            run.table_row_block = stored.table_row_block;
            run.table_column = stored.table_column;
            run.table_column_count = stored.table_column_count;
            run.table_cell_align = stored.table_cell_align;
            run.table_header = stored.table_header;
            candidate.runs.push_back(std::move(run));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

size_t NoteRenderSourcePlanStorage::SubtreeLineCount(const NodePtr& node) noexcept {
    return node ? node->subtree_line_count : 0;
}

uint64_t NoteRenderSourcePlanStorage::PriorityFor(uint64_t id) noexcept {
    uint64_t value = id + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

bool NoteRenderSourcePlanStorage::TakeNodeId(uint64_t* nextNodeId,
                                              uint64_t* outId) noexcept {
    if (!nextNodeId || !outId || *nextNodeId == 0) return false;
    *outId = *nextNodeId;
    *nextNodeId = *nextNodeId == std::numeric_limits<uint64_t>::max()
        ? 0
        : *nextNodeId + 1;
    return true;
}

NoteRenderSourcePlanStorage::NodePtr NoteRenderSourcePlanStorage::MakeNode(
    std::shared_ptr<const StoredSourceLinePlan> line,
    uint64_t priority,
    NodePtr left,
    NodePtr right) {
    return std::make_shared<const Node>(
        std::move(line), priority, std::move(left), std::move(right));
}

NoteRenderSourcePlanStorage::NodePtr NoteRenderSourcePlanStorage::CloneNode(
    const NodePtr& node,
    NodePtr left,
    NodePtr right) {
    if (!node) return nullptr;
    return MakeNode(node->line, node->priority, std::move(left), std::move(right));
}

NoteRenderSourcePlanStorage::NodePtr NoteRenderSourcePlanStorage::Merge(
    const NodePtr& left,
    const NodePtr& right) {
    if (!left) return right;
    if (!right) return left;
    if (left->priority <= right->priority) {
        return CloneNode(left, left->left, Merge(left->right, right));
    }
    return CloneNode(right, Merge(left, right->left), right->right);
}

void NoteRenderSourcePlanStorage::Split(const NodePtr& node,
                                        size_t leftLineCount,
                                        NodePtr* outLeft,
                                        NodePtr* outRight) {
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

bool NoteRenderSourcePlanStorage::BuildTree(
    const std::vector<std::shared_ptr<const StoredSourceLinePlan>>& lines,
    uint64_t* nextNodeId,
    NodePtr* outRoot) noexcept {
    if (!nextNodeId || !outRoot || lines.empty()) return false;
    try {
        NodePtr root;
        for (const std::shared_ptr<const StoredSourceLinePlan>& line : lines) {
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

const StoredSourceLinePlan* NoteRenderSourcePlanStorage::LineAt(
    const NodePtr& root,
    LineIndex index) noexcept {
    const Node* current = root.get();
    size_t target = index.value;
    while (current) {
        const size_t leftCount = SubtreeLineCount(current->left);
        if (target < leftCount) {
            current = current->left.get();
        } else if (target == leftCount) {
            return current->line.get();
        } else {
            target -= leftCount + 1;
            current = current->right.get();
        }
    }
    return nullptr;
}

bool NoteRenderSourcePlanStorage::Build(
    const std::vector<NoteRenderSourceLinePlan>& lines,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::shared_ptr<const NoteRenderSourcePlanStorage>* out) noexcept {
    if (!out || !sourceLines.valid() || lines.empty() ||
        lines.size() != sourceLines.line_count()) {
        return false;
    }
    try {
        std::vector<std::shared_ptr<const StoredSourceLinePlan>> storedLines;
        storedLines.reserve(lines.size());
        for (size_t index = 0; index < lines.size(); ++index) {
            StoredSourceLinePlan stored;
            if (!MakeStoredLine(lines[index], sourceLines, {index}, &stored)) return false;
            storedLines.push_back(std::make_shared<const StoredSourceLinePlan>(std::move(stored)));
        }
        std::shared_ptr<NoteRenderSourcePlanStorage> candidate(
            new NoteRenderSourcePlanStorage());
        if (!BuildTree(storedLines, &candidate->next_node_id_, &candidate->root_)) return false;
        candidate->line_count_ = SubtreeLineCount(candidate->root_);
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderSourcePlanStorage::Replace(
    std::shared_ptr<const NoteRenderSourcePlanStorage> before,
    LineIndex first,
    LineIndex lastExclusive,
    const std::vector<NoteRenderSourceLinePlan>& replacement,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::shared_ptr<const NoteRenderSourcePlanStorage>* out) noexcept {
    if (!out || !before || first.value > lastExclusive.value ||
        lastExclusive.value > before->line_count_ || replacement.empty()) {
        return false;
    }
    const size_t replacementLineCount = before->line_count_ -
        (lastExclusive.value - first.value) + replacement.size();
    if (!sourceLines.valid() || replacementLineCount != sourceLines.line_count()) return false;
    try {
        std::vector<std::shared_ptr<const StoredSourceLinePlan>> storedLines;
        storedLines.reserve(replacement.size());
        for (size_t index = 0; index < replacement.size(); ++index) {
            StoredSourceLinePlan stored;
            if (!MakeStoredLine(replacement[index], sourceLines,
                                {first.value + index}, &stored)) {
                return false;
            }
            storedLines.push_back(std::make_shared<const StoredSourceLinePlan>(std::move(stored)));
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
        std::shared_ptr<NoteRenderSourcePlanStorage> candidate(
            new NoteRenderSourcePlanStorage());
        candidate->root_ = std::move(root);
        candidate->line_count_ = replacementLineCount;
        candidate->next_node_id_ = nextNodeId;
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderSourcePlanStorage::Resolve(
    const std::shared_ptr<const NoteRenderSourcePlanStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLines,
    LineIndex index,
    NoteRenderSourceLinePlan* out) noexcept {
    if (!storage || !out || !sourceLines.valid() ||
        index.value >= storage->line_count_ ||
        storage->line_count_ != sourceLines.line_count()) {
        return false;
    }
    const StoredSourceLinePlan* stored = LineAt(storage->root_, index);
    return stored && ResolveStoredLine(*stored, sourceLines, index, out);
}

bool NoteRenderSourcePlanStorage::SharesLinePayload(
    const std::shared_ptr<const NoteRenderSourcePlanStorage>& lhs,
    const std::shared_ptr<const NoteRenderSourcePlanStorage>& rhs,
    LineIndex index) noexcept {
    if (!lhs || !rhs || index.value >= lhs->line_count_ || index.value >= rhs->line_count_) {
        return false;
    }
    const StoredSourceLinePlan* lhsLine = LineAt(lhs->root_, index);
    const StoredSourceLinePlan* rhsLine = LineAt(rhs->root_, index);
    return lhsLine && lhsLine == rhsLine;
}

bool NoteRenderSourceAtomicGroupStorage::Build(
    const std::vector<NoteRenderAtomicGroup>& groups,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>* out) noexcept {
    if (!out || !sourceLines.valid()) return false;
    try {
        std::shared_ptr<NoteRenderSourceAtomicGroupStorage> candidate(
            new NoteRenderSourceAtomicGroupStorage());
        candidate->groups_.reserve(groups.size());
        for (const NoteRenderAtomicGroup& group : groups) {
            StoredAtomicGroup stored;
            if (group.first_line > group.last_line ||
                group.last_line.value >= sourceLines.line_count() ||
                !MakeRelativeSpan(sourceLines, group.source_span, &stored.source_span)) {
                return false;
            }
            stored.kind = group.kind;
            stored.first_line = group.first_line;
            stored.last_line = group.last_line;
            candidate->groups_.push_back(std::move(stored));
        }
        // Keep the payload in canonical source order. The separate balanced
        // interval index accelerates visible-range lookup without changing
        // differential-test serialization.
        std::stable_sort(candidate->groups_.begin(), candidate->groups_.end(),
                         [](const StoredAtomicGroup& lhs, const StoredAtomicGroup& rhs) {
                             if (lhs.first_line != rhs.first_line) {
                                 return lhs.first_line < rhs.first_line;
                             }
                             return lhs.last_line < rhs.last_line;
                         });
        candidate->interval_nodes_.reserve(candidate->groups_.size());
        if (!BuildIntervalIndex(
                candidate.get(), 0, candidate->groups_.size(), &candidate->interval_root_)) {
            return false;
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderSourceAtomicGroupStorage::CopyAll(
    const std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::vector<NoteRenderAtomicGroup>* out) noexcept {
    if (!storage || !out || !sourceLines.valid()) return false;
    try {
        std::vector<NoteRenderAtomicGroup> candidate;
        candidate.reserve(storage->groups_.size());
        for (const StoredAtomicGroup& stored : storage->groups_) {
            Span sourceSpan;
            if (stored.first_line > stored.last_line ||
                stored.last_line.value >= sourceLines.line_count() ||
                !ResolveRelativeSpan(sourceLines, stored.source_span, &sourceSpan)) {
                return false;
            }
            candidate.push_back(NoteRenderAtomicGroup{
                stored.kind, sourceSpan, stored.first_line, stored.last_line});
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderSourceAtomicGroupStorage::CopyIntersecting(
    const std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLines,
    LineIndex first,
    LineIndex lastExclusive,
    std::vector<NoteRenderAtomicGroup>* out,
    NoteRenderAtomicGroupQueryWork* outWork) noexcept {
    if (!storage || !out || !sourceLines.valid() || first > lastExclusive ||
        lastExclusive.value > sourceLines.line_count()) {
        return false;
    }
    try {
        std::vector<NoteRenderAtomicGroup> candidate;
        NoteRenderAtomicGroupQueryWork work;
        if (!AppendIntersecting(*storage, storage->interval_root_, sourceLines, first,
                                lastExclusive, &candidate, &work)) {
            return false;
        }
        *out = std::move(candidate);
        if (outWork) *outWork = work;
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderSourceAtomicGroupStorage::ContainsLine(
    const std::shared_ptr<const NoteRenderSourceAtomicGroupStorage>& storage,
    LineIndex line) noexcept {
    if (!storage) return false;
    return ContainsLineAt(*storage, storage->interval_root_, line);
}

bool NoteRenderSourceAtomicGroupStorage::BuildIntervalIndex(
    NoteRenderSourceAtomicGroupStorage* storage,
    size_t first,
    size_t lastExclusive,
    size_t* outNode) noexcept {
    if (!storage || !outNode || first > lastExclusive ||
        lastExclusive > storage->groups_.size()) {
        return false;
    }
    if (first == lastExclusive) {
        *outNode = kNoIntervalNode;
        return true;
    }
    const size_t middle = first + (lastExclusive - first) / 2;
    const size_t nodeIndex = storage->interval_nodes_.size();
    storage->interval_nodes_.push_back(IntervalNode{});
    size_t left = kNoIntervalNode;
    size_t right = kNoIntervalNode;
    if (!BuildIntervalIndex(storage, first, middle, &left) ||
        !BuildIntervalIndex(storage, middle + 1, lastExclusive, &right)) {
        return false;
    }
    size_t subtreeLast = storage->groups_[middle].last_line.value;
    if (left != kNoIntervalNode) {
        subtreeLast = std::max(
            subtreeLast, storage->interval_nodes_[left].subtree_max_last_line);
    }
    if (right != kNoIntervalNode) {
        subtreeLast = std::max(
            subtreeLast, storage->interval_nodes_[right].subtree_max_last_line);
    }
    storage->interval_nodes_[nodeIndex] = {middle, left, right, subtreeLast};
    *outNode = nodeIndex;
    return true;
}

bool NoteRenderSourceAtomicGroupStorage::AppendIntersecting(
    const NoteRenderSourceAtomicGroupStorage& storage,
    size_t nodeIndex,
    const NoteSourceLineMap::Snapshot& sourceLines,
    LineIndex first,
    LineIndex lastExclusive,
    std::vector<NoteRenderAtomicGroup>* out,
    NoteRenderAtomicGroupQueryWork* outWork) noexcept {
    if (!out || !outWork) return false;
    if (nodeIndex == kNoIntervalNode) return true;
    if (nodeIndex >= storage.interval_nodes_.size()) return false;
    ++outWork->visited_index_nodes;
    const IntervalNode& node = storage.interval_nodes_[nodeIndex];
    if (node.group_index >= storage.groups_.size() ||
        node.subtree_max_last_line < first.value) {
        return true;
    }
    if (!AppendIntersecting(
            storage, node.left, sourceLines, first, lastExclusive, out, outWork)) {
        return false;
    }
    const StoredAtomicGroup& stored = storage.groups_[node.group_index];
    if (stored.first_line < lastExclusive && stored.last_line >= first) {
        Span sourceSpan;
        if (!ResolveRelativeSpan(sourceLines, stored.source_span, &sourceSpan)) return false;
        out->push_back(NoteRenderAtomicGroup{
            stored.kind, sourceSpan, stored.first_line, stored.last_line});
    }
    if (stored.first_line >= lastExclusive) return true;
    return AppendIntersecting(
        storage, node.right, sourceLines, first, lastExclusive, out, outWork);
}

bool NoteRenderSourceAtomicGroupStorage::ContainsLineAt(
    const NoteRenderSourceAtomicGroupStorage& storage,
    size_t nodeIndex,
    LineIndex line) noexcept {
    if (nodeIndex == kNoIntervalNode) return false;
    if (nodeIndex >= storage.interval_nodes_.size()) return false;
    const IntervalNode& node = storage.interval_nodes_[nodeIndex];
    if (node.group_index >= storage.groups_.size() ||
        node.subtree_max_last_line < line.value) {
        return false;
    }
    if (ContainsLineAt(storage, node.left, line)) return true;
    const StoredAtomicGroup& stored = storage.groups_[node.group_index];
    if (stored.first_line <= line && line <= stored.last_line) return true;
    if (stored.first_line > line) return false;
    return ContainsLineAt(storage, node.right, line);
}

namespace {

constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

[[nodiscard]] bool IsValidSpan(Span span, size_t sourceLength) noexcept {
    return span.start <= span.end && span.end.value <= sourceLength;
}

[[nodiscard]] bool SpansIntersect(Span lhs, Span rhs) noexcept {
    return lhs.start < rhs.end && rhs.start < lhs.end;
}

[[nodiscard]] bool SpanContains(Span outer, Span inner) noexcept {
    return outer.start <= inner.start && inner.end <= outer.end;
}

void AddCut(std::vector<size_t>* cuts, size_t value, Span range) {
    if (!cuts || value < range.start.value || value > range.end.value) return;
    cuts->push_back(value);
}

// Legacy markup is semantic only after the parser has accepted its paired
// style span.  MD4C leaves those tag characters inside a Text inline node, so
// derive their exact source spans from the already-accepted style boundaries
// instead of treating every HTML-looking token as syntax.  Unknown tags and
// literal/code text consequently remain ordinary visible source text.
[[nodiscard]] bool FindMarkupTagEndingAt(std::wstring_view source,
                                          size_t end,
                                          Span* out) noexcept {
    if (!out || end == 0 || end > source.size() || source[end - 1] != L'>') return false;
    size_t cursor = end - 1;
    while (cursor > 0 && source[cursor - 1] != L'<' && source[cursor - 1] != L'\r' &&
           source[cursor - 1] != L'\n') {
        --cursor;
    }
    if (cursor == 0 || source[cursor - 1] != L'<') return false;
    *out = {{cursor - 1}, {end}};
    return true;
}

[[nodiscard]] bool FindMarkupTagStartingAt(std::wstring_view source,
                                            size_t start,
                                            Span* out) noexcept {
    if (!out || start >= source.size() || source[start] != L'<') return false;
    size_t end = start + 1;
    while (end < source.size() && source[end] != L'>' && source[end] != L'\r' &&
           source[end] != L'\n') {
        ++end;
    }
    if (end >= source.size() || source[end] != L'>') return false;
    *out = {{start}, {end + 1}};
    return true;
}

void AddUniqueMarkupSyntaxSpan(std::vector<Span>* spans, Span candidate) {
    if (!spans || candidate.end <= candidate.start) return;
    for (const Span existing : *spans) {
        if (existing.start == candidate.start && existing.end == candidate.end) return;
    }
    spans->push_back(candidate);
}

[[nodiscard]] bool IsLegacyMarkupSyntaxRange(const std::vector<Span>& spans,
                                              Span range) noexcept {
    return std::any_of(spans.begin(), spans.end(), [range](Span syntax) {
        return SpanContains(syntax, range);
    });
}

// Full-plan construction indexes decorations by intersecting source row once.
// A text fragment therefore never scans all document-wide math, inline, or
// style nodes merely to find its local boundaries.
struct LineSemanticReferences {
    std::vector<size_t> math_indices;
    std::vector<size_t> inline_overlay_indices;
    std::vector<size_t> style_indices;
    // Parser-proven legacy markup tokens intersecting this source row. These
    // are indexed once during complete construction, so run splitting never
    // scans every document-wide style tag.
    std::vector<Span> legacy_markup_syntax_spans;
};

struct TableCellBinding {
    bool valid = false;
    size_t table_block = NoteRenderSourceRun::kNoTableBlock;
    size_t table_row_block = NoteRenderSourceRun::kNoTableBlock;
    size_t column = NoteRenderSourceRun::kNoTableColumn;
    size_t column_count = 0;
    TableCellAlign alignment = TableCellAlign::Default;
    bool header = false;
};

[[nodiscard]] bool IsTableCellKind(BlockKind kind) noexcept {
    return kind == BlockKind::TableHeaderCell || kind == BlockKind::TableCell;
}

// Parent block indices are parser structure, not a view cache. Resolve them
// once during complete source-plan construction and retain only the compact
// table facts each text run needs at measurement and paint time.
[[nodiscard]] bool BuildTableCellBindings(
    const NoteSyntaxDocument& document,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::vector<TableCellBinding>* out) noexcept {
    if (!out) return false;
    try {
        const size_t count = document.node_count(NoteSyntaxDocumentNodeKind::Block);
        std::vector<BlockNode> blocks(count);
        for (size_t index = 0; index < count; ++index) {
            if (!document.ResolveBlock(sourceLines, index, &blocks[index])) return false;
        }
        std::vector<TableCellBinding> bindings(count);
        std::vector<size_t> next_column_by_row(count, 0);
        const auto findTableAncestor = [&blocks](size_t start, size_t* outTable) noexcept {
            if (!outTable) return false;
            size_t current = start;
            for (size_t guard = 0; guard < blocks.size(); ++guard) {
                if (current >= blocks.size()) return false;
                if (blocks[current].kind == BlockKind::Table) {
                    *outTable = current;
                    return true;
                }
                const size_t parent = blocks[current].parent;
                if (parent >= blocks.size()) return false;
                current = parent;
            }
            return false;
        };
        for (size_t index = 0; index < count; ++index) {
            const BlockNode& cell = blocks[index];
            if (!IsTableCellKind(cell.kind)) continue;
            const size_t row = cell.parent;
            if (row >= count || blocks[row].kind != BlockKind::TableRow) return false;
            size_t table = NoteRenderSourceRun::kNoTableBlock;
            if (!findTableAncestor(row, &table) || table >= count) return false;
            const int declared_columns = blocks[table].table_column_count;
            if (declared_columns <= 0) return false;
            const size_t column_count = static_cast<size_t>(declared_columns);
            const size_t column = next_column_by_row[row];
            if (column >= column_count || column == std::numeric_limits<size_t>::max()) return false;
            ++next_column_by_row[row];
            bindings[index] = TableCellBinding{
                true, table, row, column, column_count, cell.table_cell_align,
                cell.kind == BlockKind::TableHeaderCell};
        }
        *out = std::move(bindings);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] const TableCellBinding* FindTableCellBinding(
    const std::vector<TableCellBinding>& bindings,
    const NoteSyntaxDocument& document,
    const NoteSourceLineMap::Snapshot& sourceLines,
    size_t parentBlock) noexcept {
    size_t current = parentBlock;
    for (size_t guard = 0; guard < bindings.size(); ++guard) {
        if (current >= bindings.size()) return nullptr;
        if (bindings[current].valid) return &bindings[current];
        BlockNode block;
        if (!document.ResolveBlock(sourceLines, current, &block) ||
            block.parent >= bindings.size()) {
            return nullptr;
        }
        current = block.parent;
    }
    return nullptr;
}

void AppendStyle(std::vector<NoteRenderSourceStyleAttribute>* styles,
                 StyleKind kind,
                 std::wstring value = {}) {
    if (!styles) return;
    for (const NoteRenderSourceStyleAttribute& existing : *styles) {
        if (existing.kind == kind && existing.value == value) return;
    }
    styles->push_back(NoteRenderSourceStyleAttribute{kind, std::move(value)});
}

[[nodiscard]] bool AppendInlineStyles(
    const NoteSyntaxDocument& document,
    const NoteSourceLineMap::Snapshot& sourceLines,
    const LineSemanticReferences& references,
    Span range,
    std::vector<NoteRenderSourceStyleAttribute>* styles) {
    for (size_t inlineIndex : references.inline_overlay_indices) {
        InlineNode inlineNode;
        if (!document.ResolveInline(sourceLines, inlineIndex, &inlineNode)) return false;
        if (!SpanContains(inlineNode.span, range)) continue;
        switch (inlineNode.kind) {
        case InlineKind::Emphasis:
            AppendStyle(styles, StyleKind::Italic);
            break;
        case InlineKind::Strong:
            AppendStyle(styles, StyleKind::Bold);
            break;
        case InlineKind::Strike:
            AppendStyle(styles, StyleKind::Strike);
            break;
        case InlineKind::Link:
            AppendStyle(styles, StyleKind::LinkAccent);
            AppendStyle(styles, StyleKind::LinkUnderline);
            break;
        default:
            break;
        }
    }
    for (size_t styleIndex : references.style_indices) {
        StyleSpan style;
        if (!document.ResolveStyle(sourceLines, styleIndex, &style)) return false;
        if (SpanContains(style.span, range)) {
            AppendStyle(styles, style.kind, style.value);
        }
    }
    return true;
}

[[nodiscard]] bool ResolveRunKindForRange(
    const NoteSyntaxDocument& document,
    const NoteSourceLineMap::Snapshot& sourceLines,
    const LineSemanticReferences& references,
    Span range,
    NoteRenderSourceRunKind* outKind,
    std::wstring* outTarget) {
    if (!outKind) return false;
    if (outTarget) outTarget->clear();
    for (size_t inlineIndex : references.inline_overlay_indices) {
        InlineNode inlineNode;
        if (!document.ResolveInline(sourceLines, inlineIndex, &inlineNode)) return false;
        if (!SpanContains(inlineNode.span, range)) continue;
        if (inlineNode.kind == InlineKind::Code) {
            *outKind = NoteRenderSourceRunKind::InlineCode;
            return true;
        }
        if (inlineNode.kind == InlineKind::Image) {
            if (outTarget) *outTarget = inlineNode.target;
            *outKind = NoteRenderSourceRunKind::ImageAltText;
            return true;
        }
        if (inlineNode.kind == InlineKind::Link) {
            if (outTarget) *outTarget = inlineNode.target;
            *outKind = NoteRenderSourceRunKind::LinkText;
            return true;
        }
    }
    *outKind = NoteRenderSourceRunKind::Text;
    return true;
}

[[nodiscard]] bool IsMathRange(const NoteSyntaxDocument& document,
                               const NoteSourceLineMap::Snapshot& sourceLines,
                               const LineSemanticReferences& references,
                               Span range,
                               bool* outIsMath) noexcept {
    if (!outIsMath) return false;
    for (size_t mathIndex : references.math_indices) {
        MathSpan math;
        if (!document.ResolveMath(sourceLines, mathIndex, &math)) return false;
        if (SpanContains(math.span, range)) {
            *outIsMath = true;
            return true;
        }
    }
    *outIsMath = false;
    return true;
}

[[nodiscard]] bool SourceLineRangeForSpan(
    const NoteSourceLineMap::Snapshot& sourceLines,
    Span span,
    LineIndex* outFirst,
    LineIndex* outLast) noexcept {
    if (!outFirst || !outLast || span.end <= span.start) return false;
    const auto first = NoteSourceLineMap::FindByOffset(sourceLines, span.start);
    const auto last = NoteSourceLineMap::FindByOffset(
        sourceLines, {span.end.value - 1});
    if (!first.has_value() || !last.has_value() || last->line_index < first->line_index) {
        return false;
    }
    *outFirst = first->line_index;
    *outLast = last->line_index;
    return true;
}

void AddLineDecorationFlag(NoteRenderSourceLineDecoration* decoration,
                           NoteRenderSourceLineDecorationFlag flag) noexcept {
    if (!decoration) return;
    decoration->flags |= static_cast<uint32_t>(flag);
}

[[nodiscard]] bool IsMarkdownTableDividerSourceLine(std::wstring_view line) noexcept {
    bool hasDash = false;
    bool hasPipe = false;
    for (const wchar_t ch : line) {
        if (ch == L'-') {
            hasDash = true;
        } else if (ch == L'|') {
            hasPipe = true;
        } else if (ch == L':' || ch == L' ' || ch == L'\t') {
            continue;
        } else {
            return false;
        }
    }
    return hasDash && hasPipe;
}

[[nodiscard]] bool IsMarkdownFenceSourceLine(std::wstring_view line) noexcept {
    size_t cursor = 0;
    while (cursor < line.size() && cursor < 3 && line[cursor] == L' ') ++cursor;
    if (cursor >= line.size() || (line[cursor] != L'`' && line[cursor] != L'~')) return false;
    const wchar_t marker = line[cursor];
    size_t count = 0;
    while (cursor < line.size() && line[cursor] == marker) {
        ++cursor;
        ++count;
    }
    return count >= 3;
}

[[nodiscard]] bool AddHiddenSyntaxRuns(
    std::vector<NoteRenderSourceLinePlan>* lines,
    const NoteSourceLineMap::Snapshot& sourceLines,
    const std::vector<bool>& blockMathLines) noexcept {
    if (!lines || lines->size() != sourceLines.line_count() ||
        blockMathLines.size() != lines->size()) {
        return false;
    }
    try {
        for (size_t lineIndex = 0; lineIndex < lines->size(); ++lineIndex) {
            if (blockMathLines[lineIndex]) continue;
            const auto location = NoteSourceLineMap::LineAt(sourceLines, {lineIndex});
            if (!location.has_value()) return false;
            NoteRenderSourceLinePlan& line = (*lines)[lineIndex];
            std::sort(line.runs.begin(), line.runs.end(),
                      [](const NoteRenderSourceRun& lhs, const NoteRenderSourceRun& rhs) {
                          if (lhs.source_span.start != rhs.source_span.start) {
                              return lhs.source_span.start < rhs.source_span.start;
                          }
                          return lhs.source_span.end < rhs.source_span.end;
                      });
            std::vector<NoteRenderSourceRun> completed;
            completed.reserve(line.runs.size() + 2);
            size_t cursor = location->start.value;
            for (const NoteRenderSourceRun& run : line.runs) {
                if (run.kind == NoteRenderSourceRunKind::BlockMath ||
                    run.source_span.start.value < cursor ||
                    run.source_span.end.value > location->content_end.value ||
                    run.source_span.end <= run.source_span.start) {
                    return false;
                }
                if (cursor < run.source_span.start.value) {
                    NoteRenderSourceRun hidden;
                    hidden.source_span = {{cursor}, run.source_span.start};
                    hidden.display_source_span = hidden.source_span;
                    hidden.line_index = {lineIndex};
                    hidden.kind = NoteRenderSourceRunKind::HiddenSyntax;
                    completed.push_back(std::move(hidden));
                }
                completed.push_back(run);
                cursor = run.source_span.end.value;
            }
            if (cursor < location->content_end.value) {
                NoteRenderSourceRun hidden;
                hidden.source_span = {{cursor}, location->content_end};
                hidden.display_source_span = hidden.source_span;
                hidden.line_index = {lineIndex};
                hidden.kind = NoteRenderSourceRunKind::HiddenSyntax;
                completed.push_back(std::move(hidden));
            }
            line.runs = std::move(completed);
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] bool SourcePlanIsCoherent(const NoteRenderSourcePlan& plan) noexcept {
    if (!plan.valid() || !plan.syntax() || !plan.syntax()->valid()) return false;
    const NoteSyntaxSnapshot& syntax = *plan.syntax();
    const size_t sourceLength = syntax.source_root().text_length();
    if (plan.line_count() != syntax.source_line_map().line_count()) return false;
    for (size_t index = 0; index < plan.line_count(); ++index) {
        const auto sourceLine = NoteSourceLineMap::LineAt(syntax.source_line_map(), {index});
        NoteRenderSourceLinePlan line;
        if (!sourceLine.has_value() || !plan.ResolveLine({index}, &line) ||
            line.content_span.start != sourceLine->start ||
            line.content_span.end != sourceLine->content_end) {
            return false;
        }
        size_t previousEnd = sourceLine->start.value;
        for (const NoteRenderSourceRun& run : line.runs) {
            const bool blockMath = run.kind == NoteRenderSourceRunKind::BlockMath;
            if (!IsValidSpan(run.source_span, sourceLength) ||
                !IsValidSpan(run.display_source_span, sourceLength) ||
                run.line_index.value != index || run.source_span.start.value < previousEnd ||
                (!blockMath && run.source_span.end.value > sourceLine->content_end.value)) {
                return false;
            }
            if (run.kind == NoteRenderSourceRunKind::HiddenSyntax &&
                (run.display_source_span.start != run.source_span.start ||
                 run.display_source_span.end != run.source_span.end ||
                 run.decodes_markdown_escapes || !run.styles.empty() ||
                 !run.link_target.empty())) {
                return false;
            }
            previousEnd = std::min(run.source_span.end.value, sourceLine->content_end.value);
            const bool tableRun = run.table_column != NoteRenderSourceRun::kNoTableColumn;
            if (tableRun) {
                if (run.table_block == NoteRenderSourceRun::kNoTableBlock ||
                    run.table_row_block == NoteRenderSourceRun::kNoTableBlock ||
                    run.table_column_count == 0 || run.table_column >= run.table_column_count) {
                    return false;
                }
            } else if (run.table_block != NoteRenderSourceRun::kNoTableBlock ||
                       run.table_row_block != NoteRenderSourceRun::kNoTableBlock ||
                       run.table_column_count != 0 || run.table_header) {
                return false;
            }
        }
    }
    std::vector<NoteRenderAtomicGroup> groups;
    if (!plan.CopyAtomicGroups(&groups)) return false;
    for (const NoteRenderAtomicGroup& group : groups) {
        if (!IsValidSpan(group.source_span, sourceLength) ||
            group.first_line.value > group.last_line.value ||
            group.last_line.value >= plan.line_count()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ContainsLocalSyntaxSensitiveText(std::wstring_view text) noexcept {
    return text.find_first_of(L"\r\n`*_~[]()<>#!$\\:|>") != std::wstring_view::npos;
}

[[nodiscard]] bool ApplyLocalTextEditToSpan(Span* span, const TextEdit& edit) noexcept {
    if (!span || edit.start.value > std::numeric_limits<size_t>::max() - edit.deleted_len) {
        return false;
    }
    const size_t start = edit.start.value;
    const size_t oldEnd = start + edit.deleted_len;
    const size_t inserted = edit.inserted_text.size();
    if (inserted >= edit.deleted_len) {
        const size_t delta = inserted - edit.deleted_len;
        if (span->end.value <= start) return true;
        if (span->start.value >= oldEnd) {
            if (span->start.value > std::numeric_limits<size_t>::max() - delta ||
                span->end.value > std::numeric_limits<size_t>::max() - delta) return false;
            span->start.value += delta;
            span->end.value += delta;
            return true;
        }
        if (span->end.value > oldEnd && span->end.value > std::numeric_limits<size_t>::max() - delta) {
            return false;
        }
        span->start.value = std::min(span->start.value, start);
        span->end.value = span->end.value > oldEnd ? span->end.value + delta : start + inserted;
        return span->end.value >= span->start.value;
    }
    const size_t delta = edit.deleted_len - inserted;
    if (span->end.value <= start) return true;
    if (span->start.value >= oldEnd) {
        if (span->start.value < delta || span->end.value < delta) return false;
        span->start.value -= delta;
        span->end.value -= delta;
        return true;
    }
    span->start.value = std::min(span->start.value, start);
    span->end.value = span->end.value > oldEnd ? span->end.value - delta : start + inserted;
    return span->end.value >= span->start.value;
}

[[nodiscard]] bool PlanHasReusableTextRun(const NoteRenderSourcePlan& plan,
                                          size_t start,
                                          size_t oldEnd) noexcept {
    const auto location = NoteSourceLineMap::FindByOffset(
        plan.syntax()->source_line_map(), {start});
    if (!location.has_value()) return false;
    NoteRenderSourceLinePlan line;
    if (!plan.ResolveLine(location->line_index, &line)) return false;
    for (const NoteRenderSourceRun& run : line.runs) {
        if (run.kind != NoteRenderSourceRunKind::Text) continue;
        if (run.source_span.start.value <= start && run.source_span.end.value >= oldEnd &&
            run.source_span.end.value - run.source_span.start.value > oldEnd - start) {
            return true;
        }
    }
    return false;
}

} // namespace

NoteRenderSourcePlanBuildResult NoteRenderSourcePlan::Build(
    std::shared_ptr<const NoteSyntaxSnapshot> syntax,
    std::shared_ptr<const NoteRenderSourcePlan>* out) noexcept {
    if (!out) return NoteRenderSourcePlanBuildResult::InvalidOutput;
    if (!syntax || !syntax->valid()) {
        return NoteRenderSourcePlanBuildResult::InvalidSyntaxSnapshot;
    }
    // Plain text deliberately has no structured source plan. Treating its
    // empty parser document as a committed plan would paint an empty editor;
    // it instead remains a whole-surface native/raw owner.
    if (!syntax->has_structured_syntax()) {
        return NoteRenderSourcePlanBuildResult::RawOnlyContent;
    }
    const NoteSourceLineMap::Snapshot& sourceLines = syntax->source_line_map();
    if (!sourceLines.valid() || sourceLines.line_count() == 0 ||
        sourceLines.text_length() != syntax->source_root().text_length()) {
        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
    }

    try {
        const std::shared_ptr<const NoteSyntaxDocument>& syntaxDocument =
            syntax->syntax_document();
        if (!syntaxDocument || !syntaxDocument->valid()) {
            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
        }
        const NoteSyntaxDocument& document = *syntaxDocument;
        std::vector<TableCellBinding> tableCellBindings;
        if (!BuildTableCellBindings(document, sourceLines, &tableCellBindings)) {
            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
        }
        const size_t sourceLength = sourceLines.text_length();
        std::shared_ptr<NoteRenderSourcePlan> candidate(new NoteRenderSourcePlan());
        candidate->syntax_ = std::move(syntax);
        std::vector<NoteRenderSourceLinePlan> lines(sourceLines.line_count());
        std::vector<bool> blockMathLines(sourceLines.line_count(), false);
        std::vector<NoteRenderAtomicGroup> atomicGroups;
        std::vector<NoteSourceLineLocation> lineLocations;
        lineLocations.reserve(sourceLines.line_count());
        for (size_t line = 0; line < sourceLines.line_count(); ++line) {
            const auto location = NoteSourceLineMap::LineAt(sourceLines, {line});
            if (!location.has_value()) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            lineLocations.push_back(*location);
            lines[line].content_span = {location->start, location->content_end};
        }

        const std::wstring rawSource = candidate->syntax_->CopySourceRange({0}, sourceLength);
        if (rawSource.size() != sourceLength) {
            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
        }
        std::vector<BlockNode> blocks;
        blocks.reserve(document.node_count(NoteSyntaxDocumentNodeKind::Block));
        for (size_t index = 0;
             index < document.node_count(NoteSyntaxDocumentNodeKind::Block); ++index) {
            BlockNode block;
            if (!document.ResolveBlock(sourceLines, index, &block)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            blocks.push_back(std::move(block));
        }

        std::vector<int> headingLevels(blocks.size(), 0);
        std::vector<uint32_t> listDepths(blocks.size(), 0);
        std::vector<int> orderedListNumbers(blocks.size(), 0);
        std::vector<int> nextOrderedListNumbers(blocks.size(), 0);
        for (size_t index = 0; index < blocks.size(); ++index) {
            if (blocks[index].kind == BlockKind::List && blocks[index].ordered) {
                nextOrderedListNumbers[index] = std::max(1, blocks[index].start_number);
            }
        }
        for (size_t index = 0; index < blocks.size(); ++index) {
            const BlockNode& block = blocks[index];
            if (block.kind == BlockKind::Heading) {
                headingLevels[index] = std::max(0, block.level);
            }
            if (block.kind == BlockKind::ListItem) {
                size_t listAncestor = block.parent;
                uint32_t depth = 0;
                for (size_t guard = 0; guard < blocks.size(); ++guard) {
                    if (listAncestor >= blocks.size()) break;
                    const BlockNode& ancestor = blocks[listAncestor];
                    if (ancestor.kind == BlockKind::List) {
                        if (depth == std::numeric_limits<uint32_t>::max()) {
                            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                        }
                        ++depth;
                        if (ancestor.ordered && orderedListNumbers[index] == 0) {
                            if (nextOrderedListNumbers[listAncestor] <= 0 ||
                                nextOrderedListNumbers[listAncestor] ==
                                    std::numeric_limits<int>::max()) {
                                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                            }
                            orderedListNumbers[index] = nextOrderedListNumbers[listAncestor]++;
                        }
                    }
                    if (ancestor.parent >= blocks.size() || ancestor.parent == listAncestor) break;
                    listAncestor = ancestor.parent;
                }
                listDepths[index] = depth;
            }

            if (block.span.end > block.span.start) {
                LineIndex firstLine;
                LineIndex lastLine;
                if (!SourceLineRangeForSpan(sourceLines, block.span, &firstLine, &lastLine)) {
                    return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                }
                for (size_t line = firstLine.value; line <= lastLine.value; ++line) {
                    NoteRenderSourceLineDecoration& decoration = lines[line].decoration;
                    switch (block.kind) {
                    case BlockKind::Heading:
                        AddLineDecorationFlag(&decoration,
                                              NoteRenderSourceLineDecorationHeading);
                        decoration.heading_level = std::max(decoration.heading_level,
                                                            std::max(0, block.level));
                        break;
                    case BlockKind::ListItem:
                        AddLineDecorationFlag(&decoration,
                                              NoteRenderSourceLineDecorationListItem);
                        decoration.list_depth = std::max(decoration.list_depth, listDepths[index]);
                        if (orderedListNumbers[index] > 0) {
                            AddLineDecorationFlag(&decoration,
                                                  NoteRenderSourceLineDecorationOrderedListItem);
                            decoration.ordered_list_number = orderedListNumbers[index];
                        }
                        if (block.task_item) {
                            AddLineDecorationFlag(&decoration,
                                                  NoteRenderSourceLineDecorationTaskItem);
                            decoration.task_checked = block.task_checked;
                        }
                        break;
                    case BlockKind::Quote:
                        AddLineDecorationFlag(&decoration,
                                              NoteRenderSourceLineDecorationQuote);
                        break;
                    case BlockKind::CodeBlock:
                        AddLineDecorationFlag(&decoration,
                                              NoteRenderSourceLineDecorationCodeBlock);
                        break;
                    case BlockKind::FencedContainer:
                        AddLineDecorationFlag(&decoration, NoteRenderSourceLineDecorationContainer);
                        if (decoration.container_depth == std::numeric_limits<uint32_t>::max()) {
                            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                        }
                        ++decoration.container_depth;
                        if (line == firstLine.value) {
                            AddLineDecorationFlag(&decoration,
                                                  NoteRenderSourceLineDecorationContainerOpening);
                        }
                        if (line == lastLine.value && block.fence_closed) {
                            AddLineDecorationFlag(&decoration,
                                                  NoteRenderSourceLineDecorationContainerClosing);
                        }
                        break;
                    case BlockKind::HorizontalRule:
                        AddLineDecorationFlag(&decoration,
                                              NoteRenderSourceLineDecorationHorizontalRule);
                        break;
                    case BlockKind::Table:
                        AddLineDecorationFlag(&decoration,
                                              NoteRenderSourceLineDecorationTable);
                        break;
                    case BlockKind::TableHead:
                        AddLineDecorationFlag(&decoration,
                                              NoteRenderSourceLineDecorationTableHeader);
                        break;
                    default:
                        break;
                    }
                    if (line == std::numeric_limits<size_t>::max()) {
                        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                    }
                }
            }
            NoteRenderAtomicGroupKind groupKind{};
            switch (block.kind) {
            case BlockKind::Table:
                groupKind = NoteRenderAtomicGroupKind::Table;
                break;
            case BlockKind::CodeBlock:
                groupKind = NoteRenderAtomicGroupKind::CodeBlock;
                break;
            case BlockKind::List:
            case BlockKind::Quote:
                groupKind = NoteRenderAtomicGroupKind::Container;
                break;
            // A fenced container is a structural parent, not one indivisible
            // visual object. Its row decorations can be replaced independently.
            // Tables and math inside it retain their own atomic ownership.
            default:
                continue;
            }
            if (!IsValidSpan(block.span, sourceLength) || block.span.end <= block.span.start) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            const auto firstLocation = NoteSourceLineMap::FindByOffset(sourceLines, block.span.start);
            const auto lastLocation = NoteSourceLineMap::FindByOffset(
                sourceLines, {block.span.end.value - 1});
            if (!firstLocation.has_value() || !lastLocation.has_value()) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            atomicGroups.push_back(NoteRenderAtomicGroup{
                groupKind, block.span, firstLocation->line_index, lastLocation->line_index});
        }

        // MD4C's code-block content span may begin after an opening fence and
        // end before its closing fence. Attach those physical source rows to
        // the same immutable decorator record while the parser-derived block
        // still proves they belong together; a future GDI provider must not
        // infer fences from a legacy LineCache.
        for (const BlockNode& block : blocks) {
            if (block.kind != BlockKind::CodeBlock || block.span.end <= block.span.start) {
                continue;
            }
            LineIndex firstLine;
            LineIndex lastLine;
            if (!SourceLineRangeForSpan(sourceLines, block.span, &firstLine, &lastLine)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            const size_t candidateFirst = firstLine.value == 0 ? 0 : firstLine.value - 1;
            const size_t candidateLast = lastLine.value >= lines.size() - 1
                ? lines.size() - 1
                : lastLine.value + 1;
            for (size_t line = candidateFirst; line <= candidateLast; ++line) {
                const NoteSourceLineLocation& location = lineLocations[line];
                const std::wstring_view text(
                    rawSource.data() + location.start.value,
                    location.content_end.value - location.start.value);
                NoteRenderSourceLineDecoration& decoration = lines[line].decoration;
                if ((line >= firstLine.value && line <= lastLine.value) ||
                    IsMarkdownFenceSourceLine(text)) {
                    AddLineDecorationFlag(&decoration,
                                          NoteRenderSourceLineDecorationCodeBlock);
                }
                if (IsMarkdownFenceSourceLine(text)) {
                    AddLineDecorationFlag(&decoration,
                                          NoteRenderSourceLineDecorationCodeFence);
                }
                if (line == std::numeric_limits<size_t>::max()) {
                    return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                }
            }
        }
        for (size_t line = 0; line < lines.size(); ++line) {
            NoteRenderSourceLineDecoration& decoration = lines[line].decoration;
            if (!decoration.has(NoteRenderSourceLineDecorationTable)) continue;
            const NoteSourceLineLocation& location = lineLocations[line];
            const std::wstring_view text(
                rawSource.data() + location.start.value,
                location.content_end.value - location.start.value);
            if (IsMarkdownTableDividerSourceLine(text)) {
                AddLineDecorationFlag(&decoration,
                                      NoteRenderSourceLineDecorationTableDivider);
            }
        }

        std::vector<LineSemanticReferences> lineReferences(sourceLines.line_count());
        std::vector<Span> legacyMarkupSyntaxSpans;
        auto indexSpanByLine = [&](Span span, size_t nodeIndex,
                                   std::vector<size_t> LineSemanticReferences::* member) {
            if (!IsValidSpan(span, sourceLength) || span.end <= span.start) return false;
            const auto first = NoteSourceLineMap::FindByOffset(sourceLines, span.start);
            const auto last = NoteSourceLineMap::FindByOffset(
                sourceLines, {span.end.value - 1});
            if (!first.has_value() || !last.has_value() ||
                first->line_index.value > last->line_index.value) {
                return false;
            }
            for (size_t line = first->line_index.value; line <= last->line_index.value; ++line) {
                (lineReferences[line].*member).push_back(nodeIndex);
            }
            return true;
        };
        for (size_t index = 0;
             index < document.node_count(NoteSyntaxDocumentNodeKind::Math); ++index) {
            MathSpan math;
            if (!document.ResolveMath(sourceLines, index, &math) ||
                !indexSpanByLine(math.span, index,
                                 &LineSemanticReferences::math_indices)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
        }
        for (size_t index = 0;
             index < document.node_count(NoteSyntaxDocumentNodeKind::Inline); ++index) {
            InlineNode inlineNode;
            if (!document.ResolveInline(sourceLines, index, &inlineNode)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            if (inlineNode.kind == InlineKind::Text ||
                inlineNode.span.end <= inlineNode.span.start) {
                continue;
            }
            if (!indexSpanByLine(inlineNode.span, index,
                                 &LineSemanticReferences::inline_overlay_indices)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
        }
        for (size_t index = 0;
             index < document.node_count(NoteSyntaxDocumentNodeKind::Style); ++index) {
            StyleSpan style;
            if (!document.ResolveStyle(sourceLines, index, &style)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            if (style.span.end <= style.span.start) {
                continue;
            }
            if (!indexSpanByLine(style.span, index,
                                 &LineSemanticReferences::style_indices)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            // A style span begins immediately after its accepted opening tag
            // and, when explicitly closed, ends immediately before that
            // closing tag. Only these parser-proven tokens become hidden;
            // ordinary angle-bracket text has no StyleSpan and remains text.
            Span markupTag;
            if (FindMarkupTagEndingAt(rawSource, style.span.start.value, &markupTag)) {
                AddUniqueMarkupSyntaxSpan(&legacyMarkupSyntaxSpans, markupTag);
            }
            if (FindMarkupTagStartingAt(rawSource, style.span.end.value, &markupTag)) {
                AddUniqueMarkupSyntaxSpan(&legacyMarkupSyntaxSpans, markupTag);
            }
        }
        for (const Span markupTag : legacyMarkupSyntaxSpans) {
            const auto first = NoteSourceLineMap::FindByOffset(sourceLines, markupTag.start);
            const auto last = NoteSourceLineMap::FindByOffset(
                sourceLines, {markupTag.end.value - 1});
            if (!first.has_value() || !last.has_value() ||
                first->line_index.value > last->line_index.value) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            for (size_t line = first->line_index.value; line <= last->line_index.value; ++line) {
                lineReferences[line].legacy_markup_syntax_spans.push_back(markupTag);
                if (line == std::numeric_limits<size_t>::max()) {
                    return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                }
            }
        }

        // Text nodes already exclude Markdown block and inline delimiters. Split
        // them at every semantic boundary, then let the later placement layer
        // use the preserved source span instead of guessing delimiter widths.
        for (size_t textNodeIndex = 0;
             textNodeIndex < document.node_count(NoteSyntaxDocumentNodeKind::Inline);
             ++textNodeIndex) {
            InlineNode textNode;
            if (!document.ResolveInline(sourceLines, textNodeIndex, &textNode)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            if (textNode.kind != InlineKind::Text ||
                !IsValidSpan(textNode.span, sourceLength) ||
                textNode.span.end == textNode.span.start) {
                continue;
            }
            const auto firstLocation = NoteSourceLineMap::FindByOffset(
                sourceLines, textNode.span.start);
            const Utf16CodeUnitOffset finalOffset{
                textNode.span.end.value == 0 ? 0 : textNode.span.end.value - 1};
            const auto lastLocation = NoteSourceLineMap::FindByOffset(sourceLines, finalOffset);
            if (!firstLocation.has_value() || !lastLocation.has_value() ||
                firstLocation->line_index.value > lastLocation->line_index.value) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            for (size_t line = firstLocation->line_index.value;
                 line <= lastLocation->line_index.value; ++line) {
                const NoteSourceLineLocation& location = lineLocations[line];
                const Span fragment{
                    {std::max(textNode.span.start.value, location.start.value)},
                    {std::min(textNode.span.end.value, location.content_end.value)}};
                if (fragment.end <= fragment.start) continue;
                const LineSemanticReferences& references = lineReferences[line];
                std::vector<size_t> cuts{fragment.start.value, fragment.end.value};
                for (size_t mathIndex : references.math_indices) {
                    MathSpan math;
                    if (!document.ResolveMath(sourceLines, mathIndex, &math)) {
                        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                    }
                    if (SpansIntersect(math.span, fragment)) {
                        AddCut(&cuts, math.span.start.value, fragment);
                        AddCut(&cuts, math.span.end.value, fragment);
                    }
                }
                for (size_t inlineIndex : references.inline_overlay_indices) {
                    InlineNode inlineNode;
                    if (!document.ResolveInline(sourceLines, inlineIndex, &inlineNode)) {
                        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                    }
                    if (SpansIntersect(inlineNode.span, fragment)) {
                        AddCut(&cuts, inlineNode.span.start.value, fragment);
                        AddCut(&cuts, inlineNode.span.end.value, fragment);
                    }
                }
                for (size_t styleIndex : references.style_indices) {
                    StyleSpan style;
                    if (!document.ResolveStyle(sourceLines, styleIndex, &style)) {
                        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                    }
                    if (SpansIntersect(style.span, fragment)) {
                        AddCut(&cuts, style.span.start.value, fragment);
                        AddCut(&cuts, style.span.end.value, fragment);
                    }
                }
                for (const Span markupTag : references.legacy_markup_syntax_spans) {
                    if (SpansIntersect(markupTag, fragment)) {
                        AddCut(&cuts, markupTag.start.value, fragment);
                        AddCut(&cuts, markupTag.end.value, fragment);
                    }
                }
                std::sort(cuts.begin(), cuts.end());
                cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
                for (size_t cut = 1; cut < cuts.size(); ++cut) {
                    const Span runSpan{{cuts[cut - 1]}, {cuts[cut]}};
                    bool isMath = false;
                    if (!IsMathRange(document, sourceLines, references, runSpan, &isMath)) {
                        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                    }
                    if (runSpan.end <= runSpan.start || isMath) {
                        continue;
                    }
                    NoteRenderSourceRun run;
                    run.source_span = runSpan;
                    run.display_source_span = runSpan;
                    run.line_index = {line};
                    run.parent_block = textNode.parent_block;
                    run.heading_level = textNode.parent_block < headingLevels.size()
                        ? headingLevels[textNode.parent_block]
                        : 0;
                    if (IsLegacyMarkupSyntaxRange(
                            references.legacy_markup_syntax_spans, runSpan)) {
                        run.kind = NoteRenderSourceRunKind::HiddenSyntax;
                        lines[line].runs.push_back(std::move(run));
                        continue;
                    }
                    if (!ResolveRunKindForRange(
                            document, sourceLines, references, runSpan, &run.kind,
                            &run.link_target) ||
                        !AppendInlineStyles(
                            document, sourceLines, references, runSpan, &run.styles)) {
                        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                    }
                    run.decodes_markdown_escapes =
                        run.kind != NoteRenderSourceRunKind::InlineCode;
                    if (const TableCellBinding* tableCell = FindTableCellBinding(
                            tableCellBindings, document, sourceLines, textNode.parent_block)) {
                        run.table_block = tableCell->table_block;
                        run.table_row_block = tableCell->table_row_block;
                        run.table_column = tableCell->column;
                        run.table_column_count = tableCell->column_count;
                        run.table_cell_align = tableCell->alignment;
                        run.table_header = tableCell->header;
                        if (run.table_header) {
                            AppendStyle(&run.styles, StyleKind::Bold);
                        }
                    }
                    lines[line].runs.push_back(std::move(run));
                }
            }
        }

        for (size_t mathIndex = 0;
             mathIndex < document.node_count(NoteSyntaxDocumentNodeKind::Math);
             ++mathIndex) {
            MathSpan math;
            if (!document.ResolveMath(sourceLines, mathIndex, &math)) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            if (!IsValidSpan(math.span, sourceLength) ||
                !IsValidSpan(math.content_span, sourceLength) ||
                !SpanContains(math.span, math.content_span) || math.span.end == math.span.start) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            const auto firstLocation = NoteSourceLineMap::FindByOffset(sourceLines, math.span.start);
            const Utf16CodeUnitOffset finalOffset{math.span.end.value - 1};
            const auto lastLocation = NoteSourceLineMap::FindByOffset(sourceLines, finalOffset);
            if (!firstLocation.has_value() || !lastLocation.has_value()) {
                return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
            }
            NoteRenderSourceRun run;
            run.source_span = math.span;
            run.display_source_span = math.content_span;
            run.line_index = firstLocation->line_index;
            run.kind = math.kind == MathKind::Block
                ? NoteRenderSourceRunKind::BlockMath
                : NoteRenderSourceRunKind::InlineMath;
            lines[run.line_index.value].runs.push_back(std::move(run));
            if (math.kind == MathKind::Block) {
                for (size_t line = firstLocation->line_index.value;
                     line <= lastLocation->line_index.value; ++line) {
                    blockMathLines[line] = true;
                    if (line == std::numeric_limits<size_t>::max()) {
                        return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
                    }
                }
                atomicGroups.push_back(NoteRenderAtomicGroup{
                    NoteRenderAtomicGroupKind::BlockMath, math.span,
                    firstLocation->line_index, lastLocation->line_index});
            }
        }

        if (!AddHiddenSyntaxRuns(&lines, sourceLines, blockMathLines)) {
            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
        }

        for (NoteRenderSourceLinePlan& line : lines) {
            std::sort(line.runs.begin(), line.runs.end(),
                      [](const NoteRenderSourceRun& lhs, const NoteRenderSourceRun& rhs) {
                          if (lhs.source_span.start != rhs.source_span.start) {
                              return lhs.source_span.start < rhs.source_span.start;
                          }
                          return lhs.source_span.end < rhs.source_span.end;
                      });
        }
        std::sort(atomicGroups.begin(), atomicGroups.end(),
                  [](const NoteRenderAtomicGroup& lhs, const NoteRenderAtomicGroup& rhs) {
                      if (lhs.source_span.start != rhs.source_span.start) {
                          return lhs.source_span.start < rhs.source_span.start;
                      }
                      return lhs.source_span.end < rhs.source_span.end;
                  });
        if (!NoteRenderSourcePlanStorage::Build(
                lines, sourceLines, &candidate->line_storage_) ||
            !NoteRenderSourceAtomicGroupStorage::Build(
                atomicGroups, sourceLines, &candidate->atomic_group_storage_)) {
            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
        }
        candidate->valid_ = true;
        if (!SourcePlanIsCoherent(*candidate)) {
            return NoteRenderSourcePlanBuildResult::InconsistentSyntax;
        }
        *out = std::move(candidate);
        return NoteRenderSourcePlanBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderSourcePlanBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderSourcePlanBuildResult::AllocationFailure;
    }
}

NoteRenderSourcePlanLocalPatchResult NoteRenderSourcePlan::BuildLocalPlainTextPatch(
    std::shared_ptr<const NoteRenderSourcePlan> previous,
    std::shared_ptr<const NoteSyntaxSnapshot> syntax,
    const NoteTextCore& currentTextCore,
    const TextEdit& edit,
    std::shared_ptr<const NoteRenderSourcePlan>* out,
    NoteRenderSourcePlanLocalPatchWork* outWork) noexcept {
    if (!out) return NoteRenderSourcePlanLocalPatchResult::InvalidOutput;
    if (!previous || !previous->valid_ || !previous->syntax_ || !previous->syntax_->valid() ||
        !previous->line_storage_ || !previous->atomic_group_storage_) {
        return NoteRenderSourcePlanLocalPatchResult::InvalidPreviousPlan;
    }
    if (!syntax || !syntax->valid() || !syntax->has_structured_syntax() ||
        syntax->content_kind() != NoteContentKind::Markdown || !currentTextCore.valid() ||
        !syntax->MatchesTextCore(currentTextCore)) {
        return NoteRenderSourcePlanLocalPatchResult::InvalidSyntaxSnapshot;
    }
    const size_t beforeLength = previous->syntax_->source_root().text_length();
    if ((edit.deleted_len == 0 && edit.inserted_text.empty()) ||
        edit.start.value > beforeLength || edit.deleted_len > beforeLength - edit.start.value ||
        edit.start.value > std::numeric_limits<size_t>::max() - edit.deleted_len) {
        return NoteRenderSourcePlanLocalPatchResult::InvalidEdit;
    }
    NoteSourceEditCoordinateMap coordinateMap;
    if (NoteSourceEditCoordinateMap::Build(beforeLength, edit, &coordinateMap) !=
        NoteSourceEditCoordinateMapBuildResult::Built) {
        return NoteRenderSourcePlanLocalPatchResult::InvalidEdit;
    }
    const size_t oldEnd = coordinateMap.old_replacement_span().end.value;
    const std::wstring removed = previous->syntax_->CopySourceRange(
        edit.start, edit.deleted_len);
    if (removed.size() != edit.deleted_len || ContainsLocalSyntaxSensitiveText(removed) ||
        ContainsLocalSyntaxSensitiveText(edit.inserted_text) ||
        previous->line_count() != syntax->source_line_map().line_count() ||
        !PlanHasReusableTextRun(*previous, edit.start.value, oldEnd)) {
        return NoteRenderSourcePlanLocalPatchResult::RequiresFullPlan;
    }
    try {
        const NoteDerivedSnapshotIdentity previousIdentity = previous->source_identity();
        const NoteDerivedSnapshotIdentity currentIdentity = syntax->source_identity();
        if (previousIdentity.note_id != currentIdentity.note_id ||
            previousIdentity.source_revision == std::numeric_limits<uint64_t>::max() ||
            currentIdentity.source_revision != previousIdentity.source_revision + 1 ||
            syntax->source_root().text_length() != coordinateMap.new_source_length() ||
            !currentTextCore.RangeMatches(
                coordinateMap.new_replacement_span().start, edit.inserted_text) ||
            !currentTextCore.SharesExactRange(
                previous->syntax_->source_root(), {0}, {0},
                coordinateMap.old_replacement_span().start.value)) {
            return NoteRenderSourcePlanLocalPatchResult::InvalidSyntaxSnapshot;
        }
        const Span oldSuffix{
            coordinateMap.old_replacement_span().end,
            {beforeLength}};
        if (oldSuffix.end.value > oldSuffix.start.value) {
            Span newSuffix;
            if (!coordinateMap.MapUnchangedSpan(oldSuffix, &newSuffix) ||
                !currentTextCore.SharesExactRange(
                    previous->syntax_->source_root(), oldSuffix.start, newSuffix.start,
                    oldSuffix.end.value - oldSuffix.start.value)) {
                return NoteRenderSourcePlanLocalPatchResult::InvalidSyntaxSnapshot;
            }
        }
        const auto changedLocation = NoteSourceLineMap::FindByOffset(
            previous->syntax_->source_line_map(), edit.start);
        if (!changedLocation.has_value()) return NoteRenderSourcePlanLocalPatchResult::InvalidEdit;
        if (NoteRenderSourceAtomicGroupStorage::ContainsLine(
                previous->atomic_group_storage_, changedLocation->line_index)) {
            return NoteRenderSourcePlanLocalPatchResult::RequiresFullPlan;
        }
        NoteRenderSourceLinePlan changedLine;
        if (!previous->ResolveLine(changedLocation->line_index, &changedLine) ||
            !ApplyLocalTextEditToSpan(&changedLine.content_span, edit)) {
            return NoteRenderSourcePlanLocalPatchResult::InconsistentCandidate;
        }
        for (NoteRenderSourceRun& run : changedLine.runs) {
            if (!ApplyLocalTextEditToSpan(&run.source_span, edit) ||
                !ApplyLocalTextEditToSpan(&run.display_source_span, edit)) {
                return NoteRenderSourcePlanLocalPatchResult::InconsistentCandidate;
            }
        }
        if (changedLocation->line_index.value == std::numeric_limits<size_t>::max()) {
            return NoteRenderSourcePlanLocalPatchResult::InconsistentCandidate;
        }
        std::shared_ptr<const NoteRenderSourcePlanStorage> patchedStorage;
        if (!NoteRenderSourcePlanStorage::Replace(
                previous->line_storage_, changedLocation->line_index,
                {changedLocation->line_index.value + 1}, {changedLine},
                syntax->source_line_map(), &patchedStorage)) {
            return NoteRenderSourcePlanLocalPatchResult::InconsistentCandidate;
        }
        std::shared_ptr<NoteRenderSourcePlan> candidate(new NoteRenderSourcePlan());
        candidate->syntax_ = std::move(syntax);
        candidate->line_storage_ = std::move(patchedStorage);
        candidate->atomic_group_storage_ = previous->atomic_group_storage_;
        candidate->local_reuse_previous_ = previous;
        candidate->local_reuse_first_ = changedLocation->line_index;
        candidate->local_reuse_last_exclusive_ = {changedLocation->line_index.value + 1};
        candidate->valid_ = true;
        NoteRenderSourceLinePlan verifiedLine;
        if (!candidate->ResolveLine(changedLocation->line_index, &verifiedLine) ||
            verifiedLine.content_span.start != changedLine.content_span.start ||
            verifiedLine.content_span.end != changedLine.content_span.end ||
            verifiedLine.runs.size() != changedLine.runs.size()) {
            return NoteRenderSourcePlanLocalPatchResult::InconsistentCandidate;
        }
        if (outWork) {
            *outWork = NoteRenderSourcePlanLocalPatchWork{1, 0};
        }
        *out = std::move(candidate);
        return NoteRenderSourcePlanLocalPatchResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderSourcePlanLocalPatchResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderSourcePlanLocalPatchResult::AllocationFailure;
    }
}

size_t NoteRenderSourcePlan::line_count() const noexcept {
    return line_storage_ ? line_storage_->line_count() : 0;
}

bool NoteRenderSourcePlan::ResolveLine(LineIndex index,
                                       NoteRenderSourceLinePlan* out) const noexcept {
    return valid_ && syntax_ && line_storage_ &&
           NoteRenderSourcePlanStorage::Resolve(
               line_storage_, syntax_->source_line_map(), index, out);
}

bool NoteRenderSourcePlan::CopyLinesForDifferentialTest(
    std::vector<NoteRenderSourceLinePlan>* out) const noexcept {
    if (!out || !valid_) return false;
    try {
        std::vector<NoteRenderSourceLinePlan> candidate;
        candidate.reserve(line_count());
        for (size_t index = 0; index < line_count(); ++index) {
            NoteRenderSourceLinePlan line;
            if (!ResolveLine({index}, &line)) return false;
            candidate.push_back(std::move(line));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderSourcePlan::SharesLinePayloadForDifferentialTest(
    const NoteRenderSourcePlan& other,
    LineIndex index) const noexcept {
    return valid_ && other.valid_ && line_storage_ && other.line_storage_ &&
           NoteRenderSourcePlanStorage::SharesLinePayload(
               line_storage_, other.line_storage_, index);
}

bool NoteRenderSourcePlan::ProvesUnchangedRowsFrom(
    const NoteRenderSourcePlan& previous,
    LineIndex first,
    LineIndex lastExclusive) const noexcept {
    const std::shared_ptr<const NoteRenderSourcePlan> lockedPrevious = local_reuse_previous_.lock();
    return valid_ && lockedPrevious && lockedPrevious.get() == &previous &&
           local_reuse_first_ == first && local_reuse_last_exclusive_ == lastExclusive &&
           line_count() == previous.line_count();
}

bool NoteRenderSourcePlan::CopyAtomicGroups(
    std::vector<NoteRenderAtomicGroup>* out) const noexcept {
    return valid_ && syntax_ && atomic_group_storage_ &&
           NoteRenderSourceAtomicGroupStorage::CopyAll(
               atomic_group_storage_, syntax_->source_line_map(), out);
}

bool NoteRenderSourcePlan::CopyAtomicGroupsIntersecting(
    LineIndex first,
    LineIndex lastExclusive,
    std::vector<NoteRenderAtomicGroup>* out,
    NoteRenderAtomicGroupQueryWork* outWork) const noexcept {
    return valid_ && syntax_ && atomic_group_storage_ &&
        NoteRenderSourceAtomicGroupStorage::CopyIntersecting(
            atomic_group_storage_, syntax_->source_line_map(), first, lastExclusive, out,
            outWork);
}

NoteDerivedSnapshotIdentity NoteRenderSourcePlan::source_identity() const noexcept {
    return syntax_ ? syntax_->source_identity() : NoteDerivedSnapshotIdentity{};
}

bool NoteRenderSourcePlan::Matches(const NoteTextCore& textCore) const noexcept {
    return valid_ && syntax_ && syntax_->MatchesTextCore(textCore);
}

} // namespace note
