#include "note/note_syntax_document.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>
#include <variant>
#include <vector>

namespace note {
namespace {

struct RelativeBoundary {
    LineIndex line{};
    size_t offset_from_line_start = 0;
};

struct RelativeSpan {
    RelativeBoundary start{};
    RelativeBoundary end{};
};

[[nodiscard]] bool BoundaryLess(RelativeBoundary lhs, RelativeBoundary rhs) noexcept {
    return lhs.line.value < rhs.line.value ||
           (lhs.line.value == rhs.line.value &&
            lhs.offset_from_line_start < rhs.offset_from_line_start);
}

[[nodiscard]] bool BoundaryLessOrEqual(RelativeBoundary lhs,
                                       RelativeBoundary rhs) noexcept {
    return !BoundaryLess(rhs, lhs);
}

[[nodiscard]] bool MakeRelativeBoundary(const NoteSourceLineMap::Snapshot& sourceLines,
                                        Utf16CodeUnitOffset offset,
                                        RelativeBoundary* out) noexcept {
    if (!out) return false;
    const auto location = NoteSourceLineMap::FindByOffset(sourceLines, offset);
    if (!location.has_value() || offset.value < location->start.value ||
        offset.value > location->next_start.value) {
        return false;
    }
    *out = {location->line_index, offset.value - location->start.value};
    return true;
}

[[nodiscard]] bool ResolveRelativeBoundary(const NoteSourceLineMap::Snapshot& sourceLines,
                                           RelativeBoundary boundary,
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
                                    RelativeSpan* out) noexcept {
    if (!out || span.end < span.start) return false;
    RelativeBoundary start;
    RelativeBoundary end;
    if (!MakeRelativeBoundary(sourceLines, span.start, &start) ||
        !MakeRelativeBoundary(sourceLines, span.end, &end)) {
        return false;
    }
    *out = {start, end};
    return true;
}

[[nodiscard]] bool ResolveRelativeSpan(const NoteSourceLineMap::Snapshot& sourceLines,
                                       RelativeSpan span,
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

[[nodiscard]] bool SpanContainsEditRange(Span span, size_t start, size_t oldEnd) noexcept {
    return start <= oldEnd && span.start.value <= start && span.end.value >= oldEnd;
}

[[nodiscard]] bool SpanIntersectsEdit(Span span, size_t start, size_t oldEnd) noexcept {
    if (oldEnd == start) return span.start.value <= start && span.end.value > start;
    return span.start.value < oldEnd && span.end.value > start;
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
        if (span->end.value > oldEnd &&
            span->end.value > std::numeric_limits<size_t>::max() - delta) {
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

struct StoredBlock {
    BlockNode value;
    RelativeSpan span{};
};

struct StoredInline {
    InlineNode value;
    RelativeSpan span{};
};

struct StoredStyle {
    StyleSpan value;
    RelativeSpan span{};
};

struct StoredMath {
    MathSpan value;
    RelativeSpan span{};
    RelativeSpan content_span{};
};

struct StoredDiagnostic {
    Diagnostic value;
    RelativeSpan span{};
};

using StoredNodeValue = std::variant<StoredBlock, StoredInline, StoredStyle,
                                     StoredMath, StoredDiagnostic>;

struct StoredNode {
    NoteSyntaxDocumentNodeKind kind = NoteSyntaxDocumentNodeKind::Block;
    StoredNodeValue value{};
};

[[nodiscard]] RelativeSpan PrimarySpan(const StoredNode& node) noexcept {
    return std::visit([](const auto& value) { return value.span; }, node.value);
}

[[nodiscard]] bool StoredNodeFromDocumentNode(NoteSyntaxDocumentNodeKind kind,
                                              const void* source,
                                              const NoteSourceLineMap::Snapshot& sourceLines,
                                              StoredNode* out) noexcept {
    if (!source || !out) return false;
    StoredNode stored;
    stored.kind = kind;
    switch (kind) {
    case NoteSyntaxDocumentNodeKind::Block: {
        StoredBlock value;
        value.value = *static_cast<const BlockNode*>(source);
        if (!MakeRelativeSpan(sourceLines, value.value.span, &value.span)) return false;
        value.value.span = {};
        value.value.loc = {};
        stored.value = std::move(value);
        break;
    }
    case NoteSyntaxDocumentNodeKind::Inline: {
        StoredInline value;
        value.value = *static_cast<const InlineNode*>(source);
        if (!MakeRelativeSpan(sourceLines, value.value.span, &value.span)) return false;
        value.value.span = {};
        stored.value = std::move(value);
        break;
    }
    case NoteSyntaxDocumentNodeKind::Style: {
        StoredStyle value;
        value.value = *static_cast<const StyleSpan*>(source);
        if (!MakeRelativeSpan(sourceLines, value.value.span, &value.span)) return false;
        value.value.span = {};
        stored.value = std::move(value);
        break;
    }
    case NoteSyntaxDocumentNodeKind::Math: {
        StoredMath value;
        value.value = *static_cast<const MathSpan*>(source);
        if (!MakeRelativeSpan(sourceLines, value.value.span, &value.span) ||
            !MakeRelativeSpan(sourceLines, value.value.content_span, &value.content_span)) {
            return false;
        }
        value.value.span = {};
        value.value.content_span = {};
        stored.value = std::move(value);
        break;
    }
    case NoteSyntaxDocumentNodeKind::Diagnostic: {
        StoredDiagnostic value;
        value.value = *static_cast<const Diagnostic*>(source);
        if (!MakeRelativeSpan(sourceLines, value.value.span, &value.span)) return false;
        value.value.span = {};
        stored.value = std::move(value);
        break;
    }
    }
    *out = std::move(stored);
    return true;
}

[[nodiscard]] bool ResolveStoredNode(const StoredNode& stored,
                                     const NoteSourceLineMap::Snapshot& sourceLines,
                                     BlockNode* block,
                                     InlineNode* inlineNode,
                                     StyleSpan* style,
                                     MathSpan* math,
                                     Diagnostic* diagnostic) noexcept {
    switch (stored.kind) {
    case NoteSyntaxDocumentNodeKind::Block: {
        if (!block) return false;
        const StoredBlock* value = std::get_if<StoredBlock>(&stored.value);
        if (!value) return false;
        *block = value->value;
        if (!ResolveRelativeSpan(sourceLines, value->span, &block->span)) return false;
        const auto location = NoteSourceLineMap::FindByOffset(sourceLines, block->span.start);
        if (!location.has_value() || block->span.start.value < location->start.value) return false;
        block->loc.line = static_cast<int>(location->line_index.value + 1);
        block->loc.column = static_cast<int>(block->span.start.value - location->start.value + 1);
        return true;
    }
    case NoteSyntaxDocumentNodeKind::Inline: {
        if (!inlineNode) return false;
        const StoredInline* value = std::get_if<StoredInline>(&stored.value);
        if (!value) return false;
        *inlineNode = value->value;
        return ResolveRelativeSpan(sourceLines, value->span, &inlineNode->span);
    }
    case NoteSyntaxDocumentNodeKind::Style: {
        if (!style) return false;
        const StoredStyle* value = std::get_if<StoredStyle>(&stored.value);
        if (!value) return false;
        *style = value->value;
        return ResolveRelativeSpan(sourceLines, value->span, &style->span);
    }
    case NoteSyntaxDocumentNodeKind::Math: {
        if (!math) return false;
        const StoredMath* value = std::get_if<StoredMath>(&stored.value);
        if (!value) return false;
        *math = value->value;
        return ResolveRelativeSpan(sourceLines, value->span, &math->span) &&
               ResolveRelativeSpan(sourceLines, value->content_span, &math->content_span);
    }
    case NoteSyntaxDocumentNodeKind::Diagnostic: {
        if (!diagnostic) return false;
        const StoredDiagnostic* value = std::get_if<StoredDiagnostic>(&stored.value);
        if (!value) return false;
        *diagnostic = value->value;
        return ResolveRelativeSpan(sourceLines, value->span, &diagnostic->span);
    }
    }
    return false;
}

[[nodiscard]] bool TransformStoredNodeForLocalEdit(
    StoredNode* node,
    const NoteSourceLineMap::Snapshot& beforeSourceLines,
    const NoteSourceLineMap::Snapshot& currentSourceLines,
    const TextEdit& edit,
    bool* changed) noexcept {
    if (!node || !changed) return false;
    *changed = false;
    const auto transform = [&](RelativeSpan* relative) -> bool {
        if (!relative) return false;
        Span before;
        if (!ResolveRelativeSpan(beforeSourceLines, *relative, &before)) return false;
        Span current = before;
        if (!ApplyLocalTextEditToSpan(&current, edit)) return false;
        RelativeSpan replacement;
        if (!MakeRelativeSpan(currentSourceLines, current, &replacement)) return false;
        if (replacement.start.line.value != relative->start.line.value ||
            replacement.start.offset_from_line_start != relative->start.offset_from_line_start ||
            replacement.end.line.value != relative->end.line.value ||
            replacement.end.offset_from_line_start != relative->end.offset_from_line_start) {
            *changed = true;
            *relative = replacement;
        }
        return true;
    };
    return std::visit([&](auto& value) -> bool {
        if (!transform(&value.span)) return false;
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, StoredMath>) {
            return transform(&value.content_span);
        }
        return true;
    }, node->value);
}

[[nodiscard]] size_t SaturatingAdd(size_t lhs, size_t rhs) noexcept {
    return rhs > std::numeric_limits<size_t>::max() - lhs
        ? std::numeric_limits<size_t>::max()
        : lhs + rhs;
}

} // namespace

class NoteSyntaxDocumentStorage final {
private:
    struct Node {
        std::shared_ptr<const StoredNode> payload;
        std::shared_ptr<const Node> left;
        std::shared_ptr<const Node> right;
        size_t subtree_size = 0;
    };

public:
    [[nodiscard]] static bool Build(std::vector<StoredNode> nodes,
                                    std::shared_ptr<const NoteSyntaxDocumentStorage>* out) noexcept {
        if (!out) return false;
        try {
            std::shared_ptr<NoteSyntaxDocumentStorage> candidate(new NoteSyntaxDocumentStorage());
            candidate->root_ = BuildRange(nodes, 0, nodes.size());
            candidate->node_count_ = nodes.size();
            *out = std::move(candidate);
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    [[nodiscard]] size_t size() const noexcept { return node_count_; }

    [[nodiscard]] bool Resolve(size_t index, StoredNode* out) const noexcept {
        if (!out || index >= node_count_) return false;
        const Node* node = root_.get();
        size_t relative = index;
        while (node) {
            const size_t leftSize = Size(node->left);
            if (relative < leftSize) {
                node = node->left.get();
            } else if (relative == leftSize) {
                if (!node->payload) return false;
                *out = *node->payload;
                return true;
            } else {
                relative -= leftSize + 1;
                node = node->right.get();
            }
        }
        return false;
    }

    [[nodiscard]] static bool Replace(std::shared_ptr<const NoteSyntaxDocumentStorage> before,
                                      size_t index,
                                      StoredNode replacement,
                                      std::shared_ptr<const NoteSyntaxDocumentStorage>* out) noexcept {
        if (!before || !out || index >= before->node_count_) return false;
        try {
            std::shared_ptr<NoteSyntaxDocumentStorage> candidate(new NoteSyntaxDocumentStorage());
            candidate->root_ = ReplaceAt(before->root_, index,
                                         std::make_shared<const StoredNode>(std::move(replacement)));
            candidate->node_count_ = before->node_count_;
            if (!candidate->root_) return false;
            *out = std::move(candidate);
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    [[nodiscard]] bool SharesPayload(const NoteSyntaxDocumentStorage& other,
                                     size_t index) const noexcept {
        const std::shared_ptr<const StoredNode> lhs = PayloadAt(root_, index);
        const std::shared_ptr<const StoredNode> rhs = PayloadAt(other.root_, index);
        return lhs && lhs == rhs;
    }

private:
    [[nodiscard]] static size_t Size(const std::shared_ptr<const Node>& node) noexcept {
        return node ? node->subtree_size : 0;
    }

    [[nodiscard]] static std::shared_ptr<const Node> BuildRange(
        const std::vector<StoredNode>& nodes, size_t first, size_t last) {
        if (first >= last) return {};
        const size_t middle = first + (last - first) / 2;
        std::shared_ptr<Node> node(new Node());
        node->payload = std::make_shared<const StoredNode>(nodes[middle]);
        node->left = BuildRange(nodes, first, middle);
        node->right = BuildRange(nodes, middle + 1, last);
        node->subtree_size = SaturatingAdd(SaturatingAdd(Size(node->left), 1), Size(node->right));
        return node;
    }

    [[nodiscard]] static std::shared_ptr<const Node> ReplaceAt(
        const std::shared_ptr<const Node>& before,
        size_t index,
        std::shared_ptr<const StoredNode> replacement) {
        if (!before || !replacement) return {};
        const size_t leftSize = Size(before->left);
        std::shared_ptr<Node> node(new Node(*before));
        if (index < leftSize) {
            node->left = ReplaceAt(before->left, index, std::move(replacement));
            if (!node->left) return {};
        } else if (index == leftSize) {
            node->payload = std::move(replacement);
        } else {
            node->right = ReplaceAt(before->right, index - leftSize - 1, std::move(replacement));
            if (!node->right) return {};
        }
        return node;
    }

    [[nodiscard]] static std::shared_ptr<const StoredNode> PayloadAt(
        const std::shared_ptr<const Node>& root, size_t index) noexcept {
        const Node* node = root.get();
        size_t relative = index;
        while (node) {
            const size_t leftSize = Size(node->left);
            if (relative < leftSize) {
                node = node->left.get();
            } else if (relative == leftSize) {
                return node->payload;
            } else {
                relative -= leftSize + 1;
                node = node->right.get();
            }
        }
        return {};
    }

    std::shared_ptr<const Node> root_;
    size_t node_count_ = 0;
};

class NoteSyntaxDocumentSpanIndex final {
private:
    struct Entry {
        RelativeSpan span{};
        size_t node_index = 0;
    };

    struct Node {
        Entry entry{};
        RelativeBoundary max_end{};
        std::shared_ptr<const Node> left;
        std::shared_ptr<const Node> right;
        size_t subtree_size = 0;
    };

public:
    [[nodiscard]] static bool Build(const std::vector<StoredNode>& stored,
                                    std::shared_ptr<const NoteSyntaxDocumentSpanIndex>* out) noexcept {
        if (!out) return false;
        try {
            std::shared_ptr<NoteSyntaxDocumentSpanIndex> candidate(new NoteSyntaxDocumentSpanIndex());
            std::vector<Entry> entries;
            entries.reserve(stored.size());
            std::shared_ptr<std::vector<size_t>> positionByNode(new std::vector<size_t>());
            positionByNode->resize(stored.size());
            for (size_t index = 0; index < stored.size(); ++index) {
                entries.push_back({PrimarySpan(stored[index]), index});
            }
            std::sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) {
                if (BoundaryLess(lhs.span.start, rhs.span.start)) return true;
                if (BoundaryLess(rhs.span.start, lhs.span.start)) return false;
                return lhs.node_index < rhs.node_index;
            });
            for (size_t position = 0; position < entries.size(); ++position) {
                (*positionByNode)[entries[position].node_index] = position;
            }
            candidate->position_by_node_ = std::move(positionByNode);
            candidate->root_ = BuildRange(entries, 0, entries.size());
            *out = std::move(candidate);
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    [[nodiscard]] static bool ReplaceSpan(std::shared_ptr<const NoteSyntaxDocumentSpanIndex> before,
                                          size_t nodeIndex,
                                          RelativeSpan replacement,
                                          std::shared_ptr<const NoteSyntaxDocumentSpanIndex>* out) noexcept {
        if (!before || !out || !before->position_by_node_ ||
            nodeIndex >= before->position_by_node_->size()) return false;
        try {
            const size_t position = (*before->position_by_node_)[nodeIndex];
            std::shared_ptr<NoteSyntaxDocumentSpanIndex> candidate(new NoteSyntaxDocumentSpanIndex(*before));
            candidate->root_ = ReplaceAt(before->root_, position, replacement);
            if (!candidate->root_) return false;
            *out = std::move(candidate);
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void FindIntersecting(RelativeBoundary start,
                          RelativeBoundary end,
                          bool insertion,
                          std::vector<size_t>* out,
                          size_t* visitedIndexNodes) const {
        if (!out) return;
        FindIntersecting(root_, start, end, insertion, out, visitedIndexNodes);
    }

private:
    [[nodiscard]] static RelativeBoundary MaxEnd(const std::shared_ptr<const Node>& node) noexcept {
        return node ? node->max_end : RelativeBoundary{};
    }

    [[nodiscard]] static RelativeBoundary Maximum(RelativeBoundary lhs,
                                                   RelativeBoundary rhs) noexcept {
        return BoundaryLess(lhs, rhs) ? rhs : lhs;
    }

    [[nodiscard]] static std::shared_ptr<const Node> BuildRange(
        const std::vector<Entry>& entries, size_t first, size_t last) {
        if (first >= last) return {};
        const size_t middle = first + (last - first) / 2;
        std::shared_ptr<Node> node(new Node());
        node->entry = entries[middle];
        node->left = BuildRange(entries, first, middle);
        node->right = BuildRange(entries, middle + 1, last);
        node->max_end = Maximum(node->entry.span.end,
                                Maximum(MaxEnd(node->left), MaxEnd(node->right)));
        node->subtree_size = SaturatingAdd(SaturatingAdd(Count(node->left), 1),
                                           Count(node->right));
        return node;
    }

    [[nodiscard]] static std::shared_ptr<const Node> ReplaceAt(
        const std::shared_ptr<const Node>& before,
        size_t position,
        RelativeSpan replacement) {
        if (!before) return {};
        const size_t leftSize = Count(before->left);
        std::shared_ptr<Node> node(new Node(*before));
        if (position < leftSize) {
            node->left = ReplaceAt(before->left, position, replacement);
            if (!node->left) return {};
        } else if (position == leftSize) {
            node->entry.span = replacement;
        } else {
            node->right = ReplaceAt(before->right, position - leftSize - 1, replacement);
            if (!node->right) return {};
        }
        node->max_end = Maximum(node->entry.span.end,
                                Maximum(MaxEnd(node->left), MaxEnd(node->right)));
        node->subtree_size = SaturatingAdd(SaturatingAdd(Count(node->left), 1),
                                           Count(node->right));
        return node;
    }

    [[nodiscard]] static size_t Count(const std::shared_ptr<const Node>& node) noexcept {
        return node ? node->subtree_size : 0;
    }

    static void FindIntersecting(const std::shared_ptr<const Node>& node,
                                 RelativeBoundary start,
                                 RelativeBoundary end,
                                 bool insertion,
                                 std::vector<size_t>* out,
                                 size_t* visitedIndexNodes) {
        if (!node || !out) return;
        if (visitedIndexNodes && *visitedIndexNodes != std::numeric_limits<size_t>::max()) {
            ++*visitedIndexNodes;
        }
        if (BoundaryLess(node->max_end, start) ||
            (!insertion && node->max_end.line.value == start.line.value &&
             node->max_end.offset_from_line_start == start.offset_from_line_start)) {
            return;
        }
        FindIntersecting(node->left, start, end, insertion, out, visitedIndexNodes);
        const bool startsBeforeEnd = insertion
            ? BoundaryLessOrEqual(node->entry.span.start, start)
            : BoundaryLess(node->entry.span.start, end);
        const bool endsAfterStart = insertion
            ? BoundaryLessOrEqual(start, node->entry.span.end)
            : BoundaryLess(start, node->entry.span.end);
        if (startsBeforeEnd && endsAfterStart) out->push_back(node->entry.node_index);
        if (insertion ? BoundaryLessOrEqual(node->entry.span.start, start)
                      : BoundaryLess(node->entry.span.start, end)) {
            FindIntersecting(node->right, start, end, insertion, out, visitedIndexNodes);
        }
    }

    std::shared_ptr<const Node> root_;
    std::shared_ptr<const std::vector<size_t>> position_by_node_;
};

namespace {

[[nodiscard]] size_t CategoryCount(const NoteSyntaxDocument& document,
                                   NoteSyntaxDocumentNodeKind kind) noexcept {
    return document.node_count(kind);
}

} // namespace

size_t NoteSyntaxDocument::node_count(NoteSyntaxDocumentNodeKind kind) const noexcept {
    switch (kind) {
    case NoteSyntaxDocumentNodeKind::Block: return block_count_;
    case NoteSyntaxDocumentNodeKind::Inline: return inline_count_;
    case NoteSyntaxDocumentNodeKind::Style: return style_count_;
    case NoteSyntaxDocumentNodeKind::Math: return math_count_;
    case NoteSyntaxDocumentNodeKind::Diagnostic: return diagnostic_count_;
    }
    return 0;
}

namespace {

[[nodiscard]] bool FirstGlobalIndex(const NoteSyntaxDocument& document,
                                    NoteSyntaxDocumentNodeKind kind,
                                    size_t* out) noexcept {
    if (!out) return false;
    size_t first = 0;
    switch (kind) {
    case NoteSyntaxDocumentNodeKind::Block:
        break;
    case NoteSyntaxDocumentNodeKind::Inline:
        first = CategoryCount(document, NoteSyntaxDocumentNodeKind::Block);
        break;
    case NoteSyntaxDocumentNodeKind::Style:
        first = SaturatingAdd(CategoryCount(document, NoteSyntaxDocumentNodeKind::Block),
                              CategoryCount(document, NoteSyntaxDocumentNodeKind::Inline));
        break;
    case NoteSyntaxDocumentNodeKind::Math:
        first = SaturatingAdd(
            SaturatingAdd(CategoryCount(document, NoteSyntaxDocumentNodeKind::Block),
                          CategoryCount(document, NoteSyntaxDocumentNodeKind::Inline)),
            CategoryCount(document, NoteSyntaxDocumentNodeKind::Style));
        break;
    case NoteSyntaxDocumentNodeKind::Diagnostic:
        first = SaturatingAdd(
            SaturatingAdd(
                SaturatingAdd(CategoryCount(document, NoteSyntaxDocumentNodeKind::Block),
                              CategoryCount(document, NoteSyntaxDocumentNodeKind::Inline)),
                CategoryCount(document, NoteSyntaxDocumentNodeKind::Style)),
            CategoryCount(document, NoteSyntaxDocumentNodeKind::Math));
        break;
    }
    *out = first;
    return true;
}

[[nodiscard]] bool GlobalNodeIndex(const NoteSyntaxDocument& document,
                                   NoteSyntaxDocumentNodeKind kind,
                                   size_t index,
                                   size_t* out) noexcept {
    if (!out || index >= CategoryCount(document, kind)) return false;
    size_t first = 0;
    return FirstGlobalIndex(document, kind, &first) &&
           first <= std::numeric_limits<size_t>::max() - index &&
           (*out = first + index, true);
}

} // namespace

NoteSyntaxDocumentBuildResult NoteSyntaxDocument::Build(
    const NoteDocument& document,
    const NoteSourceLineMap::Snapshot& sourceLines,
    std::shared_ptr<const NoteSyntaxDocument>* out) noexcept {
    if (!out) return NoteSyntaxDocumentBuildResult::InvalidOutput;
    if (!sourceLines.valid() || sourceLines.line_count() == 0) {
        return NoteSyntaxDocumentBuildResult::InvalidSourceLines;
    }
    try {
        std::vector<StoredNode> stored;
        const size_t expected = SaturatingAdd(
            SaturatingAdd(document.blocks.size(), document.inlines.size()),
            SaturatingAdd(document.style_spans.size(),
                          SaturatingAdd(document.math_spans.size(), document.diagnostics.size())));
        if (expected == std::numeric_limits<size_t>::max()) {
            return NoteSyntaxDocumentBuildResult::InconsistentDocument;
        }
        stored.reserve(expected);
        const auto append = [&](NoteSyntaxDocumentNodeKind kind, const auto& values) -> bool {
            for (const auto& value : values) {
                StoredNode node;
                if (!StoredNodeFromDocumentNode(kind, &value, sourceLines, &node)) return false;
                stored.push_back(std::move(node));
            }
            return true;
        };
        if (!append(NoteSyntaxDocumentNodeKind::Block, document.blocks) ||
            !append(NoteSyntaxDocumentNodeKind::Inline, document.inlines) ||
            !append(NoteSyntaxDocumentNodeKind::Style, document.style_spans) ||
            !append(NoteSyntaxDocumentNodeKind::Math, document.math_spans) ||
            !append(NoteSyntaxDocumentNodeKind::Diagnostic, document.diagnostics) ||
            stored.size() != expected) {
            return NoteSyntaxDocumentBuildResult::InconsistentDocument;
        }

        std::shared_ptr<const NoteSyntaxDocumentSpanIndex> spanIndex;
        std::shared_ptr<const NoteSyntaxDocumentStorage> storage;
        if (!NoteSyntaxDocumentSpanIndex::Build(stored, &spanIndex) ||
            !NoteSyntaxDocumentStorage::Build(stored, &storage)) {
            return NoteSyntaxDocumentBuildResult::AllocationFailure;
        }

        std::shared_ptr<std::vector<std::vector<size_t>>> endpointIndex(
            new std::vector<std::vector<size_t>>(sourceLines.line_count()));
        const auto appendEndpoint = [&](RelativeBoundary boundary, size_t nodeIndex) -> bool {
            if (boundary.line.value >= endpointIndex->size()) return false;
            (*endpointIndex)[boundary.line.value].push_back(nodeIndex);
            return true;
        };
        for (size_t nodeIndex = 0; nodeIndex < stored.size(); ++nodeIndex) {
            const RelativeSpan primary = PrimarySpan(stored[nodeIndex]);
            if (!appendEndpoint(primary.start, nodeIndex) ||
                !appendEndpoint(primary.end, nodeIndex)) {
                return NoteSyntaxDocumentBuildResult::InconsistentDocument;
            }
            if (const StoredMath* math = std::get_if<StoredMath>(&stored[nodeIndex].value)) {
                if (!appendEndpoint(math->content_span.start, nodeIndex) ||
                    !appendEndpoint(math->content_span.end, nodeIndex)) {
                    return NoteSyntaxDocumentBuildResult::InconsistentDocument;
                }
            }
        }
        for (std::vector<size_t>& nodes : *endpointIndex) {
            std::sort(nodes.begin(), nodes.end());
            nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
        }

        std::shared_ptr<NoteSyntaxDocument> candidate(new NoteSyntaxDocument());
        candidate->storage_ = std::move(storage);
        candidate->span_index_ = std::move(spanIndex);
        candidate->endpoint_nodes_by_line_ = std::move(endpointIndex);
        candidate->source_line_map_ = sourceLines;
        candidate->metadata_ = document.meta;
        candidate->block_count_ = document.blocks.size();
        candidate->inline_count_ = document.inlines.size();
        candidate->style_count_ = document.style_spans.size();
        candidate->math_count_ = document.math_spans.size();
        candidate->diagnostic_count_ = document.diagnostics.size();
        candidate->valid_ = candidate->storage_ && candidate->span_index_ &&
                            candidate->endpoint_nodes_by_line_ &&
                            candidate->storage_->size() == expected;
        if (!candidate->valid_) return NoteSyntaxDocumentBuildResult::InconsistentDocument;
        *out = std::move(candidate);
        return NoteSyntaxDocumentBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteSyntaxDocumentBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteSyntaxDocumentBuildResult::AllocationFailure;
    }
}

bool NoteSyntaxDocument::PermitsPlainTextLeafEdit(
    const NoteSourceLineMap::Snapshot& sourceLines,
    Utf16CodeUnitOffset start,
    size_t deletedLength,
    NoteSyntaxDocumentQueryWork* outWork) const noexcept {
    if (!valid_ || !storage_ || !span_index_ || !sourceLines.valid() ||
        !NoteSourceLineMap::SameSnapshotIdentity(sourceLines, source_line_map_) ||
        start.value > sourceLines.text_length() ||
        deletedLength > sourceLines.text_length() - start.value) {
        return false;
    }
    const size_t oldEnd = start.value + deletedLength;
    RelativeBoundary relativeStart;
    RelativeBoundary relativeEnd;
    if (!MakeRelativeBoundary(sourceLines, start, &relativeStart) ||
        !MakeRelativeBoundary(sourceLines, {oldEnd}, &relativeEnd)) {
        return false;
    }
    std::vector<size_t> matches;
    NoteSyntaxDocumentQueryWork work;
    try {
        span_index_->FindIntersecting(relativeStart, relativeEnd, deletedLength == 0, &matches,
                                      &work.visited_index_nodes);
    } catch (const std::bad_alloc&) {
        return false;
    }
    bool ownedByText = false;
    for (size_t globalIndex : matches) {
        StoredNode stored;
        if (!storage_->Resolve(globalIndex, &stored)) return false;
        Span span;
        if (!ResolveRelativeSpan(sourceLines, PrimarySpan(stored), &span)) return false;
        switch (stored.kind) {
        case NoteSyntaxDocumentNodeKind::Inline: {
            const StoredInline* inlineNode = std::get_if<StoredInline>(&stored.value);
            if (!inlineNode) return false;
            if (inlineNode->value.kind == InlineKind::Text &&
                SpanContainsEditRange(span, start.value, oldEnd) &&
                span.end.value - span.start.value > deletedLength) {
                ownedByText = true;
            } else if (inlineNode->value.kind != InlineKind::Text &&
                       SpanIntersectsEdit(span, start.value, oldEnd)) {
                return false;
            }
            break;
        }
        case NoteSyntaxDocumentNodeKind::Math:
        case NoteSyntaxDocumentNodeKind::Diagnostic:
            if (SpanIntersectsEdit(span, start.value, oldEnd)) return false;
            break;
        case NoteSyntaxDocumentNodeKind::Block:
        case NoteSyntaxDocumentNodeKind::Style:
            break;
        }
    }
    if (outWork) *outWork = work;
    return ownedByText;
}

NoteSyntaxDocumentLocalPatchResult NoteSyntaxDocument::BuildLocalPlainTextPatch(
    std::shared_ptr<const NoteSyntaxDocument> previous,
    const NoteSourceLineMap::Snapshot& beforeSourceLines,
    const NoteSourceLineMap::Snapshot& currentSourceLines,
    const TextEdit& edit,
    LineIndex changedLine,
    std::shared_ptr<const NoteSyntaxDocument>* out,
    NoteSyntaxDocumentLocalPatchWork* outWork) noexcept {
    if (!out) return NoteSyntaxDocumentLocalPatchResult::InvalidOutput;
    if (!previous || !previous->valid_ || !previous->storage_ || !previous->span_index_ ||
        !previous->endpoint_nodes_by_line_) {
        return NoteSyntaxDocumentLocalPatchResult::InvalidPreviousDocument;
    }
    if (!beforeSourceLines.valid() || !currentSourceLines.valid() ||
        !NoteSourceLineMap::SameSnapshotIdentity(beforeSourceLines, previous->source_line_map_) ||
        beforeSourceLines.line_count() != currentSourceLines.line_count() ||
        changedLine.value >= beforeSourceLines.line_count() ||
        edit.start.value > beforeSourceLines.text_length() ||
        edit.deleted_len > beforeSourceLines.text_length() - edit.start.value ||
        edit.inserted_text.find_first_of(L"\r\n") != std::wstring::npos) {
        return NoteSyntaxDocumentLocalPatchResult::InvalidSourceLines;
    }
    if (edit.deleted_len == 0 && edit.inserted_text.empty()) {
        return NoteSyntaxDocumentLocalPatchResult::InvalidEdit;
    }
    const size_t oldEnd = edit.start.value + edit.deleted_len;
    const size_t retained = beforeSourceLines.text_length() - edit.deleted_len;
    if (edit.inserted_text.size() > std::numeric_limits<size_t>::max() - retained ||
        currentSourceLines.text_length() != retained + edit.inserted_text.size()) {
        return NoteSyntaxDocumentLocalPatchResult::InvalidSourceLines;
    }
    const auto beforeLocation = NoteSourceLineMap::FindByOffset(beforeSourceLines, edit.start);
    const auto currentLocation = NoteSourceLineMap::FindByOffset(currentSourceLines, edit.start);
    if (!beforeLocation.has_value() || !currentLocation.has_value() ||
        beforeLocation->line_index != changedLine || currentLocation->line_index != changedLine ||
        oldEnd > beforeLocation->content_end.value ||
        !previous->PermitsPlainTextLeafEdit(beforeSourceLines, edit.start, edit.deleted_len)) {
        return NoteSyntaxDocumentLocalPatchResult::RequiresFullDocument;
    }
    try {
        std::shared_ptr<NoteSyntaxDocument> candidate(new NoteSyntaxDocument(*previous));
        candidate->source_line_map_ = currentSourceLines;
        NoteSyntaxDocumentLocalPatchWork work;
        const std::vector<size_t>& endpoints =
            (*previous->endpoint_nodes_by_line_)[changedLine.value];
        for (size_t globalIndex : endpoints) {
            StoredNode stored;
            if (!candidate->storage_->Resolve(globalIndex, &stored)) {
                return NoteSyntaxDocumentLocalPatchResult::InconsistentCandidate;
            }
            const RelativeSpan beforePrimary = PrimarySpan(stored);
            bool changed = false;
            if (!TransformStoredNodeForLocalEdit(&stored, beforeSourceLines,
                                                 currentSourceLines, edit, &changed)) {
                return NoteSyntaxDocumentLocalPatchResult::InconsistentCandidate;
            }
            if (!changed) continue;
            std::shared_ptr<const NoteSyntaxDocumentStorage> patchedStorage;
            if (!NoteSyntaxDocumentStorage::Replace(candidate->storage_, globalIndex,
                                                     std::move(stored), &patchedStorage)) {
                return NoteSyntaxDocumentLocalPatchResult::InconsistentCandidate;
            }
            candidate->storage_ = std::move(patchedStorage);
            ++work.replacement_node_payloads;

            StoredNode changedStored;
            if (!candidate->storage_->Resolve(globalIndex, &changedStored)) {
                return NoteSyntaxDocumentLocalPatchResult::InconsistentCandidate;
            }
            const RelativeSpan currentPrimary = PrimarySpan(changedStored);
            const bool primaryChanged =
                beforePrimary.start.line.value != currentPrimary.start.line.value ||
                beforePrimary.start.offset_from_line_start != currentPrimary.start.offset_from_line_start ||
                beforePrimary.end.line.value != currentPrimary.end.line.value ||
                beforePrimary.end.offset_from_line_start != currentPrimary.end.offset_from_line_start;
            if (primaryChanged) {
                std::shared_ptr<const NoteSyntaxDocumentSpanIndex> patchedIndex;
                if (!NoteSyntaxDocumentSpanIndex::ReplaceSpan(
                        candidate->span_index_, globalIndex, currentPrimary, &patchedIndex)) {
                    return NoteSyntaxDocumentLocalPatchResult::InconsistentCandidate;
                }
                candidate->span_index_ = std::move(patchedIndex);
                ++work.interval_index_payload_rewrites;
            }
        }
        *out = std::move(candidate);
        if (outWork) *outWork = work;
        return NoteSyntaxDocumentLocalPatchResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteSyntaxDocumentLocalPatchResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteSyntaxDocumentLocalPatchResult::AllocationFailure;
    }
}

bool NoteSyntaxDocument::ResolveBlock(const NoteSourceLineMap::Snapshot& sourceLines,
                                      size_t index,
                                      BlockNode* out) const noexcept {
    StoredNode stored;
    size_t globalIndex = 0;
    return valid_ && storage_ &&
           NoteSourceLineMap::SameSnapshotIdentity(sourceLines, source_line_map_) &&
           GlobalNodeIndex(*this, NoteSyntaxDocumentNodeKind::Block, index, &globalIndex) &&
           storage_->Resolve(globalIndex, &stored) &&
           ResolveStoredNode(stored, sourceLines, out, nullptr, nullptr, nullptr, nullptr);
}

bool NoteSyntaxDocument::ResolveInline(const NoteSourceLineMap::Snapshot& sourceLines,
                                       size_t index,
                                       InlineNode* out) const noexcept {
    StoredNode stored;
    size_t globalIndex = 0;
    return valid_ && storage_ &&
           NoteSourceLineMap::SameSnapshotIdentity(sourceLines, source_line_map_) &&
           GlobalNodeIndex(*this, NoteSyntaxDocumentNodeKind::Inline, index, &globalIndex) &&
           storage_->Resolve(globalIndex, &stored) &&
           ResolveStoredNode(stored, sourceLines, nullptr, out, nullptr, nullptr, nullptr);
}

bool NoteSyntaxDocument::ResolveStyle(const NoteSourceLineMap::Snapshot& sourceLines,
                                      size_t index,
                                      StyleSpan* out) const noexcept {
    StoredNode stored;
    size_t globalIndex = 0;
    return valid_ && storage_ &&
           NoteSourceLineMap::SameSnapshotIdentity(sourceLines, source_line_map_) &&
           GlobalNodeIndex(*this, NoteSyntaxDocumentNodeKind::Style, index, &globalIndex) &&
           storage_->Resolve(globalIndex, &stored) &&
           ResolveStoredNode(stored, sourceLines, nullptr, nullptr, out, nullptr, nullptr);
}

bool NoteSyntaxDocument::ResolveMath(const NoteSourceLineMap::Snapshot& sourceLines,
                                     size_t index,
                                     MathSpan* out) const noexcept {
    StoredNode stored;
    size_t globalIndex = 0;
    return valid_ && storage_ &&
           NoteSourceLineMap::SameSnapshotIdentity(sourceLines, source_line_map_) &&
           GlobalNodeIndex(*this, NoteSyntaxDocumentNodeKind::Math, index, &globalIndex) &&
           storage_->Resolve(globalIndex, &stored) &&
           ResolveStoredNode(stored, sourceLines, nullptr, nullptr, nullptr, out, nullptr);
}

bool NoteSyntaxDocument::ResolveDiagnostic(const NoteSourceLineMap::Snapshot& sourceLines,
                                           size_t index,
                                           Diagnostic* out) const noexcept {
    StoredNode stored;
    size_t globalIndex = 0;
    return valid_ && storage_ &&
           NoteSourceLineMap::SameSnapshotIdentity(sourceLines, source_line_map_) &&
           GlobalNodeIndex(*this, NoteSyntaxDocumentNodeKind::Diagnostic, index, &globalIndex) &&
           storage_->Resolve(globalIndex, &stored) &&
           ResolveStoredNode(stored, sourceLines, nullptr, nullptr, nullptr, nullptr, out);
}

bool NoteSyntaxDocument::CopyDocumentForCompleteBuild(
    const NoteSourceLineMap::Snapshot& sourceLines,
    NoteDerivedSnapshotIdentity sourceIdentity,
    NoteDocument* out) const noexcept {
    if (!out || !valid_ || !sourceLines.valid() || !sourceIdentity.valid() ||
        !NoteSourceLineMap::SameSnapshotIdentity(sourceLines, source_line_map_)) return false;
    try {
        NoteDocument candidate;
        candidate.meta = metadata_;
        candidate.source_identity = sourceIdentity;
        candidate.blocks.reserve(block_count_);
        candidate.inlines.reserve(inline_count_);
        candidate.style_spans.reserve(style_count_);
        candidate.math_spans.reserve(math_count_);
        candidate.diagnostics.reserve(diagnostic_count_);
        for (size_t index = 0; index < block_count_; ++index) {
            BlockNode node;
            if (!ResolveBlock(sourceLines, index, &node)) return false;
            candidate.blocks.push_back(std::move(node));
        }
        for (size_t index = 0; index < inline_count_; ++index) {
            InlineNode node;
            if (!ResolveInline(sourceLines, index, &node)) return false;
            candidate.inlines.push_back(std::move(node));
        }
        for (size_t index = 0; index < style_count_; ++index) {
            StyleSpan node;
            if (!ResolveStyle(sourceLines, index, &node)) return false;
            candidate.style_spans.push_back(std::move(node));
        }
        for (size_t index = 0; index < math_count_; ++index) {
            MathSpan node;
            if (!ResolveMath(sourceLines, index, &node)) return false;
            candidate.math_spans.push_back(std::move(node));
        }
        for (size_t index = 0; index < diagnostic_count_; ++index) {
            Diagnostic node;
            if (!ResolveDiagnostic(sourceLines, index, &node)) return false;
            candidate.diagnostics.push_back(std::move(node));
        }
        *out = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

bool NoteSyntaxDocument::CopyDocumentForDifferentialTest(
    const NoteSourceLineMap::Snapshot& sourceLines,
    NoteDerivedSnapshotIdentity sourceIdentity,
    NoteDocument* out) const noexcept {
    return CopyDocumentForCompleteBuild(sourceLines, sourceIdentity, out);
}

bool NoteSyntaxDocument::SharesNodePayloadForDifferentialTest(
    const NoteSyntaxDocument& other,
    NoteSyntaxDocumentNodeKind kind,
    size_t index) const noexcept {
    size_t globalIndex = 0;
    return valid_ && other.valid_ && storage_ && other.storage_ &&
           node_count(kind) == other.node_count(kind) &&
           GlobalNodeIndex(*this, kind, index, &globalIndex) &&
           storage_->SharesPayload(*other.storage_, globalIndex);
}

} // namespace note
