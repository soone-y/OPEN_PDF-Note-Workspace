#include "note/note_parser_checkpoint.h"
#include "note/note_syntax_lexical.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

// Build once per complete parse. A balanced interval index skips unrelated
// syntax at each boundary; queries prune disjoint subtrees instead of scanning
// rows * all syntax elements. Worst-case reporting work depends on matching
// intervals and their search paths. IDs keep the parser's original ordering.
class BoundarySpanIndex final {
public:
    explicit BoundarySpanIndex(NoteParserCheckpointBuildWork* work) : work_(work) {}
    void Add(Span span, size_t id) {
        if (span.start < span.end) entries_.push_back({span, id});
    }
    void Build() {
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            if (a.span.start != b.span.start) return a.span.start < b.span.start;
            return a.id < b.id;
        });
        max_ends_.resize(entries_.size());
        (void)BuildMax(0, entries_.size());
        if (work_) work_->indexed_intervals += entries_.size();
    }
    [[nodiscard]] std::vector<size_t> At(size_t position) const {
        return Query(position, position, true);
    }
    [[nodiscard]] std::vector<size_t> Intersect(size_t first, size_t last) const {
        return Query(first, last, false);
    }
private:
    struct Entry { Span span; size_t id; };
    [[nodiscard]] size_t BuildMax(size_t first, size_t last) {
        if (first == last) return 0;
        const size_t mid = first + (last - first) / 2;
        max_ends_[mid] = std::max({entries_[mid].span.end.value,
            BuildMax(first, mid), BuildMax(mid + 1, last)});
        return max_ends_[mid];
    }
    void Find(size_t first, size_t last, size_t low, size_t high,
              bool point, std::vector<size_t>* out) const {
        if (first == last) return;
        if (work_) ++work_->visited_index_nodes;
        const size_t mid = first + (last - first) / 2;
        if (max_ends_[mid] <= low || (point ? entries_[first].span.start.value > low
                                                   : entries_[first].span.start.value >= high)) return;
        Find(first, mid, low, high, point, out);
        const Entry& entry = entries_[mid];
        if ((point ? entry.span.start.value <= low : entry.span.start.value < high) &&
            entry.span.end.value > low) {
            out->push_back(entry.id);
            if (work_) ++work_->matched_intervals;
        }
        Find(mid + 1, last, low, high, point, out);
    }
    [[nodiscard]] std::vector<size_t> Query(size_t first, size_t last, bool point) const {
        if (work_) ++work_->boundary_queries;
        std::vector<size_t> result;
        if (point || first < last) Find(0, entries_.size(), first, last, point, &result);
        std::sort(result.begin(), result.end());
        return result;
    }
    std::vector<Entry> entries_;
    std::vector<size_t> max_ends_;
    NoteParserCheckpointBuildWork* work_;
};

struct BoundaryIndexes {
    explicit BoundaryIndexes(const NoteDocument& doc, NoteParserCheckpointBuildWork* work)
        : math(work), tables(work), containers(work), fences(work), indents(work), inline_crossings(work) {
        for (size_t i = 0; i < doc.math_spans.size(); ++i) {
            if (doc.math_spans[i].kind == MathKind::Block) math.Add(doc.math_spans[i].span, i);
        }
        for (size_t i = 0; i < doc.blocks.size(); ++i) {
            const BlockNode& block = doc.blocks[i];
            if (block.kind == BlockKind::Table || block.kind == BlockKind::TableHead ||
                block.kind == BlockKind::TableBody) tables.Add(block.span, i);
            if (block.kind == BlockKind::Quote || block.kind == BlockKind::List ||
                block.kind == BlockKind::ListItem) containers.Add(block.span, i);
            if (block.kind == BlockKind::FencedContainer && block.span.start < block.span.end) {
                // Fences own boundaries strictly after their opener. An open
                // fence also owns the terminal empty row at its inclusive EOF.
                const size_t end = block.span.end.value;
                fences.Add({{block.span.start.value + 1},
                    {end + (!block.fence_closed && end != std::numeric_limits<size_t>::max() ? 1 : 0)}}, i);
            }
        }
        for (size_t i = 0; i < doc.style_spans.size(); ++i) {
            const StyleSpan& style = doc.style_spans[i];
            if (style.span.start == style.span.end) continue;
            const Span crossing{{style.span.start.value + 1}, style.span.end};
            if (style.kind == StyleKind::Indent) indents.Add(crossing, i);
            else inline_crossings.Add(crossing, i);
        }
        for (size_t i = 0; i < doc.inlines.size(); ++i) {
            const InlineNode& node = doc.inlines[i];
            if (node.kind != InlineKind::Text && node.span.start < node.span.end)
                inline_crossings.Add({{node.span.start.value + 1}, node.span.end}, i);
        }
        math.Build(); tables.Build(); containers.Build(); fences.Build();
        indents.Build(); inline_crossings.Build();
    }
    BoundarySpanIndex math, tables, containers, fences, indents, inline_crossings;
};

struct StoredCheckpointState {
    NoteParserCheckpointState entry_state;
    NoteParserCheckpointState exit_state;
};

[[nodiscard]] bool IsLineStartVectorExact(const NoteTextModel& source) {
    return source.line_starts == BuildLineStarts(source.raw);
}

[[nodiscard]] bool SourceLineMapMatchesTextModel(
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    const NoteTextModel& source) noexcept {
    if (!sourceLineMap.valid() || sourceLineMap.text_length() != source.raw.size() ||
        sourceLineMap.line_count() != source.line_starts.size() ||
        source.line_starts.empty()) {
        return false;
    }
    for (size_t line = 0; line < source.line_starts.size(); ++line) {
        const auto location = NoteSourceLineMap::LineAt(sourceLineMap, {line});
        if (!location.has_value() || location->start.value != source.line_starts[line] ||
            location->next_start.value > source.raw.size() ||
            (line + 1 < source.line_starts.size() &&
             location->next_start.value != source.line_starts[line + 1])) {
            return false;
        }
    }
    const auto last = NoteSourceLineMap::LineAt(
        sourceLineMap, {source.line_starts.size() - 1});
    return last.has_value() && last->next_start.value == source.raw.size();
}

[[nodiscard]] bool IsSpanValid(Span span, size_t textLength) noexcept {
    return span.start.value <= span.end.value && span.end.value <= textLength;
}

[[nodiscard]] bool IsDocumentSpanSetValid(const NoteDocument& document,
                                          size_t textLength) noexcept {
    for (const BlockNode& block : document.blocks) {
        if (!IsSpanValid(block.span, textLength) ||
            (block.parent != kInvalidIndex && block.parent >= document.blocks.size())) {
            return false;
        }
    }
    for (const InlineNode& inlineNode : document.inlines) {
        if (!IsSpanValid(inlineNode.span, textLength) ||
            (inlineNode.parent_block != kInvalidIndex &&
             inlineNode.parent_block >= document.blocks.size())) {
            return false;
        }
    }
    for (const StyleSpan& style : document.style_spans) {
        if (!IsSpanValid(style.span, textLength)) return false;
    }
    for (const MathSpan& math : document.math_spans) {
        if (!IsSpanValid(math.span, textLength) ||
            !IsSpanValid(math.content_span, textLength) ||
            math.content_span.start < math.span.start || math.content_span.end > math.span.end) {
            return false;
        }
    }
    for (const Diagnostic& diagnostic : document.diagnostics) {
        if (!IsSpanValid(diagnostic.span, textLength)) return false;
    }
    return true;
}

[[nodiscard]] size_t LineContentEnd(const NoteTextModel& source, size_t line) {
    if (line >= source.line_starts.size()) return source.raw.size();
    size_t end = line + 1 < source.line_starts.size()
        ? source.line_starts[line + 1]
        : source.raw.size();
    while (end > source.line_starts[line] &&
           (source.raw[end - 1] == L'\r' || source.raw[end - 1] == L'\n')) {
        --end;
    }
    return end;
}

[[nodiscard]] std::wstring_view LineText(const NoteTextModel& source, size_t line) {
    if (line >= source.line_starts.size()) return {};
    const size_t start = source.line_starts[line];
    const size_t end = LineContentEnd(source, line);
    return std::wstring_view(source.raw).substr(start, end - start);
}

struct FenceState {
    bool open = false;
    wchar_t marker = 0;
    size_t marker_count = 0;
};

void AdvanceFenceState(std::wstring_view raw,
                       size_t lineStart,
                       size_t lineEnd,
                       FenceState* state) {
    if (!state) return;
    NoteCodeFenceRun run;
    if (!state->open) {
        if (TryParseNoteCodeFenceRun(raw, {lineStart}, {lineEnd}, false, &run)) {
            state->open = true;
            state->marker = run.marker;
            state->marker_count = run.marker_count;
        }
        return;
    }
    if (TryParseNoteCodeFenceRun(raw, {lineStart}, {lineEnd}, true, &run) &&
        run.marker == state->marker && run.marker_count >= state->marker_count) {
        *state = {};
    }
}

[[nodiscard]] bool SpanCrossesBoundary(Span span, size_t position) noexcept {
    return span.start.value < position && position < span.end.value;
}

[[nodiscard]] const MathSpan* BlockMathAt(const NoteDocument& document,
                                           const BoundaryIndexes& indexes, size_t position) {
    const auto matches = indexes.math.At(position);
    return matches.empty() ? nullptr : &document.math_spans[matches.front()];
}

[[nodiscard]] bool SpanIntersectsRange(Span span, size_t start, size_t end) noexcept {
    return start < end && span.start.value < end && span.end.value > start;
}

[[nodiscard]] NoteParserTableMode TableModeAt(const NoteDocument& document,
                                               const BoundaryIndexes& indexes,
                                               size_t lineStart,
                                               size_t lineEnd,
                                               size_t* outColumnCount) {
    if (outColumnCount) *outColumnCount = 0;
    const auto matches = indexes.tables.Intersect(lineStart, lineEnd);
    for (const size_t id : matches) {
        const BlockNode& block = document.blocks[id];
        if (SpanIntersectsRange(block.span, lineStart, lineEnd) &&
            block.kind == BlockKind::Table && outColumnCount &&
            block.table_column_count > 0) {
            *outColumnCount = static_cast<size_t>(block.table_column_count);
            break;
        }
    }
    for (const size_t id : matches) {
        const BlockNode& block = document.blocks[id];
        if (!SpanIntersectsRange(block.span, lineStart, lineEnd)) continue;
        if (block.kind == BlockKind::TableBody) return NoteParserTableMode::Body;
        if (block.kind == BlockKind::TableHead) return NoteParserTableMode::Header;
    }
    for (const size_t id : matches) {
        const BlockNode& block = document.blocks[id];
        if (!SpanIntersectsRange(block.span, lineStart, lineEnd) ||
            block.kind != BlockKind::Table) {
            continue;
        }
        if (outColumnCount && block.table_column_count > 0) {
            *outColumnCount = static_cast<size_t>(block.table_column_count);
        }
        return NoteParserTableMode::Header;
    }
    return NoteParserTableMode::None;
}

[[nodiscard]] size_t ParentDepth(const NoteDocument& document, size_t blockIndex) noexcept {
    size_t depth = 0;
    size_t current = blockIndex;
    while (current < document.blocks.size() && document.blocks[current].parent != kInvalidIndex) {
        ++depth;
        current = document.blocks[current].parent;
        if (depth > document.blocks.size()) return document.blocks.size();
    }
    return depth;
}

[[nodiscard]] std::vector<NoteParserContainerFrame> ContainerStackAt(
    const NoteDocument& document,
    const BoundaryIndexes& indexes,
    size_t lineStart,
    size_t lineEnd) {
    struct Candidate {
        size_t block_index = 0;
        size_t parent_depth = 0;
    };
    std::vector<Candidate> candidates;
    auto matches = indexes.containers.Intersect(lineStart, lineEnd);
    const auto fenced = indexes.fences.At(lineStart);
    matches.insert(matches.end(), fenced.begin(), fenced.end());
    candidates.reserve(matches.size());
    for (const size_t index : matches) {
        const BlockNode& block = document.blocks[index];
        const bool insideFence = block.kind == BlockKind::FencedContainer &&
            block.span.start.value < lineStart &&
            (lineStart < block.span.end.value ||
             (!block.fence_closed && lineStart == block.span.end.value));
        if ((!insideFence && (block.kind == BlockKind::FencedContainer ||
                              !SpanIntersectsRange(block.span, lineStart, lineEnd))) ||
            (block.kind != BlockKind::Quote && block.kind != BlockKind::List &&
             block.kind != BlockKind::ListItem && block.kind != BlockKind::FencedContainer)) {
            continue;
        }
        candidates.push_back({index, ParentDepth(document, index)});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.parent_depth != rhs.parent_depth) return lhs.parent_depth < rhs.parent_depth;
        return lhs.block_index < rhs.block_index;
    });

    std::vector<NoteParserContainerFrame> result;
    result.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        const BlockNode& block = document.blocks[candidate.block_index];
        NoteParserContainerFrame frame;
        frame.parent_depth = candidate.parent_depth;
        frame.ordered = block.ordered;
        frame.start_number = block.start_number;
        frame.task_item = block.task_item;
        frame.task_checked = block.task_checked;
        frame.fence_marker_count = block.fence_marker_count;
        frame.info_string = block.info_string;
        switch (block.kind) {
        case BlockKind::Quote: frame.kind = NoteParserContainerKind::Quote; break;
        case BlockKind::List: frame.kind = NoteParserContainerKind::List; break;
        case BlockKind::ListItem: frame.kind = NoteParserContainerKind::ListItem; break;
        case BlockKind::FencedContainer: frame.kind = NoteParserContainerKind::FencedContainer; break;
        default: continue;
        }
        result.push_back(frame);
    }
    return result;
}

[[nodiscard]] bool HasOpaqueInlineContinuation(const BoundaryIndexes& indexes, size_t position) {
    return !indexes.inline_crossings.At(position).empty();
}

[[nodiscard]] bool ContainsLegacyMarkupToken(std::wstring_view text) noexcept {
    return text.find(L'<') != std::wstring_view::npos ||
           text.find(L'>') != std::wstring_view::npos;
}

[[nodiscard]] bool ContainsInlineContinuationCandidate(std::wstring_view text) noexcept {
    return text.find_first_of(L"*_~[]()") != std::wstring_view::npos;
}

[[nodiscard]] bool PositionIsInsideBlockMath(const NoteDocument& document,
                                              const BoundaryIndexes& indexes, size_t position) {
    return BlockMathAt(document, indexes, position) != nullptr;
}

[[nodiscard]] bool HasUnresolvedBlockMathCandidate(const NoteDocument& document,
                                                    const BoundaryIndexes& indexes,
                                                    std::wstring_view raw,
                                                    size_t lineStart,
                                                    size_t lineEnd) {
    for (size_t cursor = lineStart; cursor < lineEnd; ++cursor) {
        const bool doubleDollar = raw[cursor] == L'$' && cursor + 1 < lineEnd &&
            raw[cursor + 1] == L'$';
        const bool bracketBoundary = raw[cursor] == L'\\' && cursor + 1 < lineEnd &&
            (raw[cursor + 1] == L'[' || raw[cursor + 1] == L']');
        if ((doubleDollar || bracketBoundary) &&
            !PositionIsInsideBlockMath(document, indexes, cursor)) {
            return true;
        }
    }
    return false;
}

void PopulateBoundaryState(NoteParserCheckpointState* state,
                           const NoteDocument& document,
                           const BoundaryIndexes& indexes,
                           const FenceState& fence,
                           size_t position,
                           size_t lineEnd) {
    if (!state) return;
    state->code_fence_open = fence.open;
    state->code_fence_marker = fence.marker;
    state->code_fence_marker_count = fence.marker_count;
    if (!fence.open) {
        if (const MathSpan* math = BlockMathAt(document, indexes, position)) {
            state->block_math_open = true;
            state->block_math_delimiter = math->delimiter;
        }
    }
    state->table_mode = TableModeAt(document, indexes, position, lineEnd, &state->table_column_count);
    state->container_stack = ContainerStackAt(document, indexes, position, lineEnd);
    for (const size_t id : indexes.indents.At(position)) {
        const StyleSpan& style = document.style_spans[id];
        if (style.kind == StyleKind::Indent && SpanCrossesBoundary(style.span, position)) {
            state->inherited_indent_values.push_back(style.value);
        }
    }
    state->opaque_inline_continuation = state->opaque_inline_continuation ||
        HasOpaqueInlineContinuation(indexes, position);
    const bool unmodeledContainer = std::any_of(
        state->container_stack.begin(), state->container_stack.end(),
        [](const NoteParserContainerFrame& frame) {
            return frame.kind != NoteParserContainerKind::FencedContainer;
        });
    if (state->table_mode != NoteParserTableMode::None || unmodeledContainer ||
        state->opaque_inline_continuation || state->opaque_legacy_markup ||
        state->opaque_block_math_candidate) {
        state->has_unmodeled_context = true;
    }
}

} // namespace

class NoteParserCheckpointStorage final {
public:
    [[nodiscard]] static bool Build(
        const std::vector<NoteParserLineCheckpoint>& lines,
        const NoteSourceLineMap::Snapshot& sourceLineMap,
        std::shared_ptr<const NoteParserCheckpointStorage>* out) noexcept;
    [[nodiscard]] static bool Resolve(
        const std::shared_ptr<const NoteParserCheckpointStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLineMap,
        LineIndex index,
        NoteParserLineCheckpoint* out) noexcept;
    [[nodiscard]] static bool CopyAll(
        const std::shared_ptr<const NoteParserCheckpointStorage>& storage,
        const NoteSourceLineMap::Snapshot& sourceLineMap,
        std::vector<NoteParserLineCheckpoint>* out) noexcept;
    [[nodiscard]] static bool SharesPayload(
        const std::shared_ptr<const NoteParserCheckpointStorage>& lhs,
        const std::shared_ptr<const NoteParserCheckpointStorage>& rhs,
        LineIndex index) noexcept;
    [[nodiscard]] static bool ReplaceForDifferentialTest(
        const std::shared_ptr<const NoteParserCheckpointStorage>& before,
        LineIndex index,
        const NoteParserLineCheckpoint& replacement,
        const NoteSourceLineMap::Snapshot& sourceLineMap,
        std::shared_ptr<const NoteParserCheckpointStorage>* out) noexcept;

private:
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

    [[nodiscard]] static bool MakeStored(
        const NoteParserLineCheckpoint& line,
        const NoteSourceLineMap::Snapshot& sourceLineMap,
        LineIndex index,
        StoredCheckpointState* out) noexcept;
    [[nodiscard]] static bool ResolveStored(
        const StoredCheckpointState& state,
        const NoteSourceLineMap::Snapshot& sourceLineMap,
        LineIndex index,
        NoteParserLineCheckpoint* out) noexcept;
    [[nodiscard]] static size_t SubtreeLineCount(const NodePtr& node) noexcept;
    [[nodiscard]] static uint64_t PriorityFor(uint64_t id) noexcept;
    [[nodiscard]] static bool TakeNodeId(uint64_t* nextNodeId, uint64_t* outId) noexcept;
    [[nodiscard]] static NodePtr MakeNode(
        std::shared_ptr<const StoredCheckpointState> state,
        uint64_t priority,
        NodePtr left,
        NodePtr right);
    [[nodiscard]] static NodePtr CloneNode(const NodePtr& node, NodePtr left, NodePtr right);
    [[nodiscard]] static NodePtr Merge(const NodePtr& left, const NodePtr& right);
    static void Split(
        const NodePtr& node,
        size_t leftLineCount,
        NodePtr* outLeft,
        NodePtr* outRight);
    [[nodiscard]] static bool BuildTree(
        const std::vector<std::shared_ptr<const StoredCheckpointState>>& states,
        uint64_t* nextNodeId,
        NodePtr* outRoot) noexcept;
    [[nodiscard]] static const StoredCheckpointState* StateAt(
        const NodePtr& root,
        LineIndex index) noexcept;

    NodePtr root_;
    size_t line_count_ = 0;
    uint64_t next_node_id_ = 1;
};

struct NoteParserCheckpointStorage::Node {
    Node(std::shared_ptr<const StoredCheckpointState> value,
         uint64_t nodePriority,
         NodePtr nodeLeft,
         NodePtr nodeRight)
        : state(std::move(value)),
          priority(nodePriority),
          left(std::move(nodeLeft)),
          right(std::move(nodeRight)),
          subtree_line_count(SubtreeLineCount(left) + 1 + SubtreeLineCount(right)) {}

    std::shared_ptr<const StoredCheckpointState> state;
    uint64_t priority = 0;
    NodePtr left;
    NodePtr right;
    size_t subtree_line_count = 0;
};

bool NoteParserCheckpointStorage::MakeStored(
    const NoteParserLineCheckpoint& line,
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    LineIndex index,
    StoredCheckpointState* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::LineAt(sourceLineMap, index);
    if (!location.has_value() || line.source_line.start != location->start ||
        line.source_line.end != location->next_start) {
        return false;
    }
    try {
        *out = {line.entry_state, line.exit_state};
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteParserCheckpointStorage::ResolveStored(
    const StoredCheckpointState& state,
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    LineIndex index,
    NoteParserLineCheckpoint* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::LineAt(sourceLineMap, index);
    if (!location.has_value()) return false;
    try {
        *out = {{location->start, location->next_start},
                state.entry_state, state.exit_state};
        return true;
    } catch (...) {
        return false;
    }
}

size_t NoteParserCheckpointStorage::SubtreeLineCount(const NodePtr& node) noexcept {
    return node ? node->subtree_line_count : 0;
}

uint64_t NoteParserCheckpointStorage::PriorityFor(uint64_t id) noexcept {
    uint64_t value = id + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

bool NoteParserCheckpointStorage::TakeNodeId(uint64_t* nextNodeId,
                                              uint64_t* outId) noexcept {
    if (!nextNodeId || !outId || *nextNodeId == 0) return false;
    *outId = *nextNodeId;
    *nextNodeId = *nextNodeId == std::numeric_limits<uint64_t>::max()
        ? 0
        : *nextNodeId + 1;
    return true;
}

NoteParserCheckpointStorage::NodePtr NoteParserCheckpointStorage::MakeNode(
    std::shared_ptr<const StoredCheckpointState> state,
    uint64_t priority,
    NodePtr left,
    NodePtr right) {
    return std::make_shared<const Node>(
        std::move(state), priority, std::move(left), std::move(right));
}

NoteParserCheckpointStorage::NodePtr NoteParserCheckpointStorage::CloneNode(
    const NodePtr& node,
    NodePtr left,
    NodePtr right) {
    if (!node) return nullptr;
    return MakeNode(node->state, node->priority, std::move(left), std::move(right));
}

NoteParserCheckpointStorage::NodePtr NoteParserCheckpointStorage::Merge(
    const NodePtr& left,
    const NodePtr& right) {
    if (!left) return right;
    if (!right) return left;
    if (left->priority <= right->priority) {
        return CloneNode(left, left->left, Merge(left->right, right));
    }
    return CloneNode(right, Merge(left, right->left), right->right);
}

void NoteParserCheckpointStorage::Split(
    const NodePtr& node,
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

bool NoteParserCheckpointStorage::BuildTree(
    const std::vector<std::shared_ptr<const StoredCheckpointState>>& states,
    uint64_t* nextNodeId,
    NodePtr* outRoot) noexcept {
    if (!nextNodeId || !outRoot || states.empty()) return false;
    try {
        NodePtr root;
        for (const std::shared_ptr<const StoredCheckpointState>& state : states) {
            uint64_t id = 0;
            if (!state || !TakeNodeId(nextNodeId, &id)) return false;
            root = Merge(root, MakeNode(state, PriorityFor(id), nullptr, nullptr));
        }
        if (!root || SubtreeLineCount(root) != states.size()) return false;
        *outRoot = std::move(root);
        return true;
    } catch (...) {
        return false;
    }
}

const StoredCheckpointState* NoteParserCheckpointStorage::StateAt(
    const NodePtr& root,
    LineIndex index) noexcept {
    const Node* current = root.get();
    size_t target = index.value;
    while (current) {
        const size_t leftCount = SubtreeLineCount(current->left);
        if (target < leftCount) {
            current = current->left.get();
        } else if (target == leftCount) {
            return current->state.get();
        } else {
            target -= leftCount + 1;
            current = current->right.get();
        }
    }
    return nullptr;
}

bool NoteParserCheckpointStorage::Build(
    const std::vector<NoteParserLineCheckpoint>& lines,
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    std::shared_ptr<const NoteParserCheckpointStorage>* out) noexcept {
    if (!out || !sourceLineMap.valid() || lines.empty() ||
        lines.size() != sourceLineMap.line_count()) {
        return false;
    }
    try {
        std::vector<std::shared_ptr<const StoredCheckpointState>> states;
        states.reserve(lines.size());
        for (size_t index = 0; index < lines.size(); ++index) {
            StoredCheckpointState state;
            if (!MakeStored(lines[index], sourceLineMap, {index}, &state)) return false;
            states.push_back(std::make_shared<const StoredCheckpointState>(std::move(state)));
        }
        std::shared_ptr<NoteParserCheckpointStorage> candidate(
            new NoteParserCheckpointStorage());
        if (!BuildTree(states, &candidate->next_node_id_, &candidate->root_)) return false;
        candidate->line_count_ = SubtreeLineCount(candidate->root_);
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteParserCheckpointStorage::Resolve(
    const std::shared_ptr<const NoteParserCheckpointStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    LineIndex index,
    NoteParserLineCheckpoint* out) noexcept {
    if (!storage || !out || !sourceLineMap.valid() ||
        index.value >= storage->line_count_ ||
        sourceLineMap.line_count() != storage->line_count_) {
        return false;
    }
    const StoredCheckpointState* state = StateAt(storage->root_, index);
    return state && ResolveStored(*state, sourceLineMap, index, out);
}

bool NoteParserCheckpointStorage::CopyAll(
    const std::shared_ptr<const NoteParserCheckpointStorage>& storage,
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    std::vector<NoteParserLineCheckpoint>* out) noexcept {
    if (!storage || !out || !sourceLineMap.valid() ||
        sourceLineMap.line_count() != storage->line_count_) {
        return false;
    }
    try {
        std::vector<NoteParserLineCheckpoint> candidate;
        candidate.reserve(storage->line_count_);
        for (size_t index = 0; index < storage->line_count_; ++index) {
            NoteParserLineCheckpoint line;
            if (!Resolve(storage, sourceLineMap, {index}, &line)) return false;
            candidate.push_back(std::move(line));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteParserCheckpointStorage::SharesPayload(
    const std::shared_ptr<const NoteParserCheckpointStorage>& lhs,
    const std::shared_ptr<const NoteParserCheckpointStorage>& rhs,
    LineIndex index) noexcept {
    if (!lhs || !rhs || index.value >= lhs->line_count_ || index.value >= rhs->line_count_) {
        return false;
    }
    const StoredCheckpointState* lhsState = StateAt(lhs->root_, index);
    const StoredCheckpointState* rhsState = StateAt(rhs->root_, index);
    return lhsState && lhsState == rhsState;
}

bool NoteParserCheckpointStorage::ReplaceForDifferentialTest(
    const std::shared_ptr<const NoteParserCheckpointStorage>& before,
    LineIndex index,
    const NoteParserLineCheckpoint& replacement,
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    std::shared_ptr<const NoteParserCheckpointStorage>* out) noexcept {
    if (!before || !out || index.value >= before->line_count_ ||
        !sourceLineMap.valid() || sourceLineMap.line_count() != before->line_count_) {
        return false;
    }
    try {
        StoredCheckpointState replacementState;
        if (!MakeStored(replacement, sourceLineMap, index, &replacementState)) return false;
        const auto replacementPayload =
            std::make_shared<const StoredCheckpointState>(std::move(replacementState));
        uint64_t nextNodeId = before->next_node_id_;
        uint64_t replacementId = 0;
        if (!TakeNodeId(&nextNodeId, &replacementId)) return false;
        const NodePtr replacementRoot = MakeNode(
            replacementPayload, PriorityFor(replacementId), nullptr, nullptr);
        NodePtr left;
        NodePtr middleAndRight;
        NodePtr removed;
        NodePtr right;
        Split(before->root_, index.value, &left, &middleAndRight);
        Split(middleAndRight, 1, &removed, &right);
        const NodePtr root = Merge(Merge(left, replacementRoot), right);
        if (!root || SubtreeLineCount(root) != before->line_count_) return false;
        std::shared_ptr<NoteParserCheckpointStorage> candidate(
            new NoteParserCheckpointStorage());
        candidate->root_ = root;
        candidate->line_count_ = before->line_count_;
        candidate->next_node_id_ = nextNodeId;
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool operator==(NoteParserLayoutKey lhs, NoteParserLayoutKey rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator!=(NoteParserLayoutKey lhs, NoteParserLayoutKey rhs) noexcept {
    return !(lhs == rhs);
}

bool operator==(const NoteParserContainerFrame& lhs,
                const NoteParserContainerFrame& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.parent_depth == rhs.parent_depth &&
           lhs.ordered == rhs.ordered && lhs.start_number == rhs.start_number &&
           lhs.task_item == rhs.task_item && lhs.task_checked == rhs.task_checked &&
           lhs.fence_marker_count == rhs.fence_marker_count && lhs.info_string == rhs.info_string;
}

bool operator!=(const NoteParserContainerFrame& lhs,
                const NoteParserContainerFrame& rhs) noexcept {
    return !(lhs == rhs);
}

bool NoteParserCheckpointState::supports_suffix_reuse() const noexcept {
    return !has_unmodeled_context && !opaque_inline_continuation &&
           !opaque_legacy_markup && !opaque_block_math_candidate;
}

bool operator==(const NoteParserCheckpointState& lhs,
                const NoteParserCheckpointState& rhs) noexcept {
    return lhs.has_bounded_lookbehind_line == rhs.has_bounded_lookbehind_line &&
           lhs.code_fence_open == rhs.code_fence_open &&
           lhs.code_fence_marker == rhs.code_fence_marker &&
           lhs.code_fence_marker_count == rhs.code_fence_marker_count &&
           lhs.block_math_open == rhs.block_math_open &&
           lhs.block_math_delimiter == rhs.block_math_delimiter &&
           lhs.table_mode == rhs.table_mode &&
           lhs.table_column_count == rhs.table_column_count &&
           lhs.container_stack == rhs.container_stack &&
           lhs.inherited_indent_values == rhs.inherited_indent_values &&
           lhs.opaque_inline_continuation == rhs.opaque_inline_continuation &&
           lhs.opaque_legacy_markup == rhs.opaque_legacy_markup &&
           lhs.opaque_block_math_candidate == rhs.opaque_block_math_candidate &&
           lhs.has_unmodeled_context == rhs.has_unmodeled_context;
}

bool operator!=(const NoteParserCheckpointState& lhs,
                const NoteParserCheckpointState& rhs) noexcept {
    return !(lhs == rhs);
}

size_t NoteParserCheckpointIndex::line_count() const noexcept {
    return valid && line_storage_ ? source_line_map.line_count() : 0;
}

bool NoteParserCheckpointIndex::ResolveLine(
    LineIndex index,
    NoteParserLineCheckpoint* out) const noexcept {
    return valid && line_storage_ && NoteParserCheckpointStorage::Resolve(
        line_storage_, source_line_map, index, out);
}

bool NoteParserCheckpointIndex::CopyLinesForDifferentialTest(
    std::vector<NoteParserLineCheckpoint>* out) const noexcept {
    return valid && line_storage_ && NoteParserCheckpointStorage::CopyAll(
        line_storage_, source_line_map, out);
}

bool NoteParserCheckpointIndex::SharesLinePayloadForDifferentialTest(
    const NoteParserCheckpointIndex& other,
    LineIndex index) const noexcept {
    return valid && other.valid && line_storage_ && other.line_storage_ &&
           NoteParserCheckpointStorage::SharesPayload(
               line_storage_, other.line_storage_, index);
}

bool NoteParserCheckpointIndex::ReplaceLineForDifferentialTest(
    LineIndex index,
    const NoteParserLineCheckpoint& replacement,
    NoteParserCheckpointIndex* out) const noexcept {
    if (!out || !valid || !line_storage_) return false;
    NoteParserCheckpointIndex candidate = *this;
    if (!NoteParserCheckpointStorage::ReplaceForDifferentialTest(
            line_storage_, index, replacement, source_line_map, &candidate.line_storage_)) {
        return false;
    }
    *out = std::move(candidate);
    return true;
}

NoteParserCheckpointIndex BuildNoteParserCheckpointIndex(
    const NoteTextModel& source,
    const NoteDocument& document,
    NoteDerivedSnapshotIdentity sourceIdentity,
    NoteTextPieceSequence::Snapshot canonicalSourceRoot) {
    NoteSourceLineMap::Snapshot sourceLineMap;
    if (!NoteSourceLineMap::Build(source.raw, &sourceLineMap)) {
        return {};
    }
    return BuildNoteParserCheckpointIndex(
        source, document, sourceIdentity, std::move(canonicalSourceRoot), sourceLineMap);
}

NoteParserCheckpointIndex BuildNoteParserCheckpointIndex(
    const NoteTextModel& source,
    const NoteDocument& document,
    NoteDerivedSnapshotIdentity sourceIdentity,
    NoteTextPieceSequence::Snapshot canonicalSourceRoot,
    const NoteSourceLineMap::Snapshot& sourceLineMap,
    NoteParserCheckpointBuildWork* outWork) {
    if (outWork) *outWork = {};
    NoteParserCheckpointIndex result;
    if (source.revision == 0 || !sourceIdentity.valid() ||
        sourceIdentity.source_revision != source.revision ||
        !canonicalSourceRoot.valid() ||
        canonicalSourceRoot.text_length() != source.raw.size() ||
        !NoteDocumentMatchesTextModel(document, source) ||
        !IsLineStartVectorExact(source) ||
        !SourceLineMapMatchesTextModel(sourceLineMap, source) ||
        !IsDocumentSpanSetValid(document, source.raw.size())) {
        return result;
    }
    try {
        const BoundaryIndexes boundaryIndexes(document, outWork);
        std::vector<NoteParserLineCheckpoint> lines;
        lines.reserve(source.line_starts.size());
        std::vector<size_t> containerFenceStarts;
        for (const BlockNode& block : document.blocks) {
            if (block.kind != BlockKind::FencedContainer || block.span.end <= block.span.start) continue;
            containerFenceStarts.push_back(block.span.start.value);
            if (block.fence_closed) {
                const auto last = NoteSourceLineMap::FindByOffset(sourceLineMap, {block.span.end.value - 1});
                if (!last) return {};
                containerFenceStarts.push_back(last->start.value);
            }
        }
        std::sort(containerFenceStarts.begin(), containerFenceStarts.end());
        size_t nextContainerFence = 0;
        std::vector<Span> completedMath;
        completedMath.reserve(document.math_spans.size());
        for (const MathSpan& math : document.math_spans) completedMath.push_back(math.span);
        std::sort(completedMath.begin(), completedMath.end(), [](Span lhs, Span rhs) {
            return lhs.start < rhs.start;
        });
        size_t nextMath = 0;
        FenceState fence;
        bool opaqueLegacyMarkup = false;
        bool opaqueBlockMathCandidate = false;
        bool opaqueInlineCandidate = false;
        for (size_t line = 0; line < source.line_starts.size(); ++line) {
            const auto location = NoteSourceLineMap::LineAt(sourceLineMap, {line});
            if (!location.has_value()) return {};
            const size_t start = location->start.value;
            const size_t contentEnd = location->content_end.value;
            const size_t nextStart = location->next_start.value;
            const std::wstring_view text = LineText(source, line);
            while (nextContainerFence < containerFenceStarts.size() &&
                   containerFenceStarts[nextContainerFence] < start) ++nextContainerFence;
            const bool containerFence = nextContainerFence < containerFenceStarts.size() &&
                containerFenceStarts[nextContainerFence] == start;
            NoteParserLineCheckpoint checkpoint;
            checkpoint.source_line = {location->start, location->next_start};
            const bool requiresBoundedLookBehind = line > 0 &&
                IsNoteBoundedLookBehindDelimiterLine(text);
            checkpoint.entry_state.has_bounded_lookbehind_line = requiresBoundedLookBehind;
            checkpoint.entry_state.opaque_legacy_markup = opaqueLegacyMarkup;
            checkpoint.entry_state.opaque_block_math_candidate = opaqueBlockMathCandidate;
            checkpoint.entry_state.opaque_inline_continuation = opaqueInlineCandidate;
            PopulateBoundaryState(&checkpoint.entry_state, document, boundaryIndexes, fence, start, contentEnd);

            if (!fence.open && !containerFence) {
                // Completed math owns its delimiters and literal TeX body.
                // Do not poison every following row with a closed <math> tag.
                // A monotone cursor visits only this row's uncovered segments.
                const auto scanUncovered = [&](size_t first, size_t last) {
                    const auto part = std::wstring_view(source.raw).substr(first, last - first);
                    opaqueLegacyMarkup = opaqueLegacyMarkup || ContainsLegacyMarkupToken(part);
                    opaqueInlineCandidate = opaqueInlineCandidate || ContainsInlineContinuationCandidate(part);
                    opaqueBlockMathCandidate = opaqueBlockMathCandidate ||
                        HasUnresolvedBlockMathCandidate(document, boundaryIndexes, source.raw, first, last);
                };
                size_t cursor = start;
                while (nextMath < completedMath.size() && completedMath[nextMath].end.value <= cursor) ++nextMath;
                size_t mathIndex = nextMath;
                while (mathIndex < completedMath.size() && completedMath[mathIndex].start.value < contentEnd) {
                    const Span math = completedMath[mathIndex];
                    if (cursor < math.start.value) scanUncovered(cursor, math.start.value);
                    cursor = std::max(cursor, std::min(math.end.value, contentEnd));
                    ++mathIndex;
                }
                if (cursor < contentEnd) scanUncovered(cursor, contentEnd);
            }
            AdvanceFenceState(source.raw, start, contentEnd, &fence);

            checkpoint.exit_state.has_bounded_lookbehind_line = requiresBoundedLookBehind;
            checkpoint.exit_state.opaque_legacy_markup = opaqueLegacyMarkup;
            checkpoint.exit_state.opaque_block_math_candidate = opaqueBlockMathCandidate;
            checkpoint.exit_state.opaque_inline_continuation = opaqueInlineCandidate;
            const size_t nextContentEnd = line + 1 < source.line_starts.size()
                ? LineContentEnd(source, line + 1)
                : nextStart;
            PopulateBoundaryState(
                &checkpoint.exit_state, document, boundaryIndexes, fence, nextStart, nextContentEnd);
            lines.push_back(std::move(checkpoint));
        }
        if (!NoteParserCheckpointStorage::Build(lines, sourceLineMap, &result.line_storage_)) {
            return {};
        }
        result.source_identity = sourceIdentity;
        result.canonical_source_root = std::move(canonicalSourceRoot);
        result.source_line_map = sourceLineMap;
        result.valid = true;
        return result;
    } catch (const std::bad_alloc&) {
        return {};
    } catch (const std::exception&) {
        return {};
    }
}

NoteParserCheckpointLocalPatchResult BuildNoteParserCheckpointLocalPlainTextPatch(
    const NoteParserCheckpointIndex& previous,
    NoteDerivedSnapshotIdentity currentIdentity,
    NoteTextPieceSequence::Snapshot currentCanonicalRoot,
    const NoteSourceLineMap::Snapshot& currentSourceLineMap,
    LineIndex changedLine,
    NoteParserCheckpointIndex* out,
    NoteParserCheckpointLocalPatchWork* outWork) noexcept {
    if (!out) return NoteParserCheckpointLocalPatchResult::InvalidOutput;
    if (!previous.valid || !previous.line_storage_ || !previous.source_identity.valid() ||
        !previous.canonical_source_root.valid() || !previous.source_line_map.valid()) {
        return NoteParserCheckpointLocalPatchResult::InvalidPreviousIndex;
    }
    if (!currentIdentity.valid() || !currentCanonicalRoot.valid() ||
        !currentSourceLineMap.valid() ||
        currentIdentity.note_id != previous.source_identity.note_id ||
        previous.source_identity.source_revision == std::numeric_limits<uint64_t>::max() ||
        currentIdentity.source_revision != previous.source_identity.source_revision + 1 ||
        currentCanonicalRoot.text_length() != currentSourceLineMap.text_length() ||
        currentSourceLineMap.line_count() != previous.line_count()) {
        return NoteParserCheckpointLocalPatchResult::InvalidCurrentSource;
    }
    if (changedLine.value >= previous.line_count()) {
        return NoteParserCheckpointLocalPatchResult::InvalidChangedLine;
    }
    NoteParserLineCheckpoint previousLine;
    if (!previous.ResolveLine(changedLine, &previousLine)) {
        return NoteParserCheckpointLocalPatchResult::InvalidCurrentSource;
    }
    try {
        NoteParserCheckpointIndex candidate = previous;
        candidate.source_identity = currentIdentity;
        candidate.canonical_source_root = std::move(currentCanonicalRoot);
        candidate.source_line_map = currentSourceLineMap;
        candidate.valid = true;
        NoteParserLineCheckpoint currentLine;
        if (!candidate.ResolveLine(changedLine, &currentLine) ||
            candidate.line_count() != previous.line_count()) {
            return NoteParserCheckpointLocalPatchResult::InvalidCurrentSource;
        }
        if (outWork) *outWork = {};
        *out = std::move(candidate);
        return NoteParserCheckpointLocalPatchResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteParserCheckpointLocalPatchResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteParserCheckpointLocalPatchResult::AllocationFailure;
    }
}

bool NoteParserCheckpointIndexMatchesTextModel(
    const NoteParserCheckpointIndex& index,
    const NoteTextModel& source,
    NoteDerivedSnapshotIdentity sourceIdentity) noexcept {
    return index.valid && sourceIdentity.valid() && source.revision != 0 &&
           index.source_identity == sourceIdentity &&
           sourceIdentity.source_revision == source.revision &&
           index.canonical_source_root.valid() &&
           index.canonical_source_root.text_length() == source.raw.size() &&
           index.line_count() != 0 && SourceLineMapMatchesTextModel(index.source_line_map, source) &&
           index.line_count() == source.line_starts.size();
}

namespace {

[[nodiscard]] bool SameExactCanonicalRange(
    const NoteTextPieceSequence::Snapshot& currentRoot,
    const NoteTextPieceSequence::Snapshot& previousRoot,
    Span previousRange,
    Span currentRange) noexcept {
    if (previousRange.end < previousRange.start || currentRange.end < currentRange.start) {
        return false;
    }
    const size_t previousLength = previousRange.end - previousRange.start;
    const size_t currentLength = currentRange.end - currentRange.start;
    return previousLength == currentLength &&
        NoteTextPieceSequence::SnapshotsShareExactRange(
            previousRoot, previousRange.start, currentRoot,
            currentRange.start, previousLength);
}

} // namespace

bool NoteParserCheckpointCanReuseSuffix(
    const NoteParserCheckpointIndex& previous,
    LineIndex previousLine,
    const NoteParserCheckpointIndex& rebuilt,
    LineIndex rebuiltLine,
    const NoteTextPieceSequence::Snapshot& currentCanonicalRoot,
    NoteParserLayoutKey previousLayoutKey,
    NoteParserLayoutKey rebuiltLayoutKey) noexcept {
    if (!previous.valid || !rebuilt.valid || !previous.source_identity.valid() ||
        !rebuilt.source_identity.valid() ||
        previous.source_identity.note_id != rebuilt.source_identity.note_id ||
        !previous.canonical_source_root.valid() ||
        !rebuilt.canonical_source_root.valid() ||
        !NoteTextPieceSequence::SameSnapshotIdentity(
            currentCanonicalRoot, rebuilt.canonical_source_root) ||
        !previousLayoutKey.valid() || previousLayoutKey != rebuiltLayoutKey ||
        previousLine.value >= previous.line_count() ||
        rebuiltLine.value >= rebuilt.line_count()) {
        return false;
    }
    NoteParserLineCheckpoint oldCheckpoint;
    NoteParserLineCheckpoint newCheckpoint;
    if (!previous.ResolveLine(previousLine, &oldCheckpoint) ||
        !rebuilt.ResolveLine(rebuiltLine, &newCheckpoint) ||
        !oldCheckpoint.entry_state.supports_suffix_reuse() ||
        !oldCheckpoint.exit_state.supports_suffix_reuse() ||
        !newCheckpoint.entry_state.supports_suffix_reuse() ||
        !newCheckpoint.exit_state.supports_suffix_reuse() ||
        oldCheckpoint.entry_state != newCheckpoint.entry_state ||
        oldCheckpoint.exit_state != newCheckpoint.exit_state ||
        !SameExactCanonicalRange(currentCanonicalRoot, previous.canonical_source_root,
                                  oldCheckpoint.source_line, newCheckpoint.source_line)) {
        return false;
    }
    if (!oldCheckpoint.entry_state.has_bounded_lookbehind_line) return true;
    if (previousLine.value == 0 || rebuiltLine.value == 0) return false;
    NoteParserLineCheckpoint oldPrevious;
    NoteParserLineCheckpoint newPrevious;
    return previous.ResolveLine({previousLine.value - 1}, &oldPrevious) &&
           rebuilt.ResolveLine({rebuiltLine.value - 1}, &newPrevious) &&
           SameExactCanonicalRange(currentCanonicalRoot, previous.canonical_source_root,
                                    oldPrevious.source_line, newPrevious.source_line);
}

} // namespace note
