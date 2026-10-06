#include "note/note_render_layout_snapshot.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool CanAddHeight(uint64_t lhs, uint64_t rhs) noexcept {
    return rhs <= std::numeric_limits<uint64_t>::max() - lhs;
}

} // namespace

bool NoteRenderLayoutKey::valid() const noexcept {
    return client_width_px != 0 && dpi_x != 0 && dpi_y != 0 &&
           font_metrics_revision != 0 && tab_columns == 4;
}

bool operator==(const NoteRenderLayoutKey& lhs,
                const NoteRenderLayoutKey& rhs) noexcept {
    return lhs.client_width_px == rhs.client_width_px && lhs.dpi_x == rhs.dpi_x &&
           lhs.dpi_y == rhs.dpi_y &&
           lhs.font_metrics_revision == rhs.font_metrics_revision &&
           lhs.tab_columns == rhs.tab_columns &&
           lhs.horizontal_padding_px == rhs.horizontal_padding_px &&
           lhs.word_wrap == rhs.word_wrap;
}

bool operator!=(const NoteRenderLayoutKey& lhs,
                const NoteRenderLayoutKey& rhs) noexcept {
    return !(lhs == rhs);
}

struct NoteRenderLineLayoutMap::Node {
    Node(NoteRenderLineLayout value,
         uint64_t nodePriority,
         NodePtr nodeLeft,
         NodePtr nodeRight)
        : layout(value),
          priority(nodePriority),
          left(std::move(nodeLeft)),
          right(std::move(nodeRight)) {
        subtree_line_count = SubtreeLineCount(left) + 1 + SubtreeLineCount(right);
        subtree_total_height_px = SubtreeHeight(left) + layout.height_px +
            SubtreeHeight(right);
        subtree_max_inline_extent_px = std::max(
            layout.inline_extent_px,
            std::max(SubtreeMaxInlineExtent(left), SubtreeMaxInlineExtent(right)));
    }

    NoteRenderLineLayout layout{};
    uint64_t priority = 0;
    NodePtr left;
    NodePtr right;
    size_t subtree_line_count = 0;
    uint64_t subtree_total_height_px = 0;
    uint32_t subtree_max_inline_extent_px = 0;
};

struct NoteRenderLineLayoutMap::VersionIdentity {};

size_t NoteRenderLineLayoutMap::SubtreeLineCount(const NodePtr& node) noexcept {
    return node ? node->subtree_line_count : 0;
}

uint64_t NoteRenderLineLayoutMap::SubtreeHeight(const NodePtr& node) noexcept {
    return node ? node->subtree_total_height_px : 0;
}

uint32_t NoteRenderLineLayoutMap::SubtreeMaxInlineExtent(const NodePtr& node) noexcept {
    return node ? node->subtree_max_inline_extent_px : 0;
}

uint64_t NoteRenderLineLayoutMap::PriorityFor(uint64_t id) noexcept {
    uint64_t value = id + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

bool NoteRenderLineLayoutMap::TakeNodeId(uint64_t* nextNodeId,
                                         uint64_t* outId) noexcept {
    if (!nextNodeId || !outId || *nextNodeId == 0) return false;
    *outId = *nextNodeId;
    if (*nextNodeId == std::numeric_limits<uint64_t>::max()) {
        *nextNodeId = 0;
    } else {
        ++*nextNodeId;
    }
    return true;
}

bool NoteRenderLineLayoutMap::IsValidLine(const NoteRenderLineLayout& line) noexcept {
    return line.height_px != 0 ||
           (line.collapsed_into_atomic_group && line.inline_extent_px == 0);
}

NoteRenderLineLayoutMap::NodePtr NoteRenderLineLayoutMap::MakeNode(
    NoteRenderLineLayout line,
    uint64_t priority,
    NodePtr left,
    NodePtr right) {
    return std::make_shared<const Node>(line, priority, std::move(left), std::move(right));
}

NoteRenderLineLayoutMap::NodePtr NoteRenderLineLayoutMap::CloneNode(
    const NodePtr& node,
    NodePtr left,
    NodePtr right) {
    if (!node) return nullptr;
    return MakeNode(node->layout, node->priority, std::move(left), std::move(right));
}

NoteRenderLineLayoutMap::NodePtr NoteRenderLineLayoutMap::Merge(const NodePtr& left,
                                                                  const NodePtr& right) {
    if (!left) return right;
    if (!right) return left;
    if (left->priority <= right->priority) {
        return CloneNode(left, left->left, Merge(left->right, right));
    }
    return CloneNode(right, Merge(left, right->left), right->right);
}

void NoteRenderLineLayoutMap::Split(const NodePtr& node,
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

bool NoteRenderLineLayoutMap::BuildTree(const std::vector<NoteRenderLineLayout>& lines,
                                        uint64_t* nextNodeId,
                                        NodePtr* outRoot) noexcept {
    if (!nextNodeId || !outRoot || lines.empty()) return false;
    try {
        uint64_t totalHeight = 0;
        NodePtr result;
        for (const NoteRenderLineLayout& line : lines) {
            if (!IsValidLine(line) || !CanAddHeight(totalHeight, line.height_px)) return false;
            totalHeight += line.height_px;
            uint64_t id = 0;
            if (!TakeNodeId(nextNodeId, &id)) return false;
            result = Merge(result, MakeNode(line, PriorityFor(id), nullptr, nullptr));
        }
        if (!result || SubtreeHeight(result) != totalHeight) return false;
        *outRoot = std::move(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderLineLayoutMap::Build(const std::vector<NoteRenderLineLayout>& lines,
                                    Snapshot* out) noexcept {
    if (!out || lines.empty()) return false;
    try {
        uint64_t nextNodeId = 1;
        NodePtr root;
        if (!BuildTree(lines, &nextNodeId, &root) || !root) return false;
        Snapshot next;
        next.root_ = std::move(root);
        next.version_ = std::make_shared<const VersionIdentity>();
        next.line_count_ = SubtreeLineCount(next.root_);
        next.total_height_px_ = SubtreeHeight(next.root_);
        next.max_inline_extent_px_ = SubtreeMaxInlineExtent(next.root_);
        next.next_node_id_ = nextNodeId;
        next.initialized_ = true;
        *out = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteRenderLineLayoutMap::Replace(
    const Snapshot& before,
    LineIndex first,
    LineIndex lastExclusive,
    const std::vector<NoteRenderLineLayout>& replacement,
    Snapshot* out) noexcept {
    if (!out || !before.valid() || first.value > lastExclusive.value ||
        lastExclusive.value > before.line_count_ || replacement.empty()) {
        return false;
    }
    try {
        uint64_t nextNodeId = before.next_node_id_;
        NodePtr replacementRoot;
        if (!BuildTree(replacement, &nextNodeId, &replacementRoot)) return false;

        NodePtr left;
        NodePtr middleAndRight;
        NodePtr removed;
        NodePtr right;
        Split(before.root_, first.value, &left, &middleAndRight);
        Split(middleAndRight, lastExclusive.value - first.value, &removed, &right);
        const uint64_t retainedHeight = before.total_height_px_ - SubtreeHeight(removed);
        const uint64_t replacementHeight = SubtreeHeight(replacementRoot);
        if (!CanAddHeight(retainedHeight, replacementHeight)) return false;
        NodePtr root = Merge(Merge(left, replacementRoot), right);
        if (!root || SubtreeHeight(root) != retainedHeight + replacementHeight) return false;

        Snapshot next;
        next.root_ = std::move(root);
        next.version_ = std::make_shared<const VersionIdentity>();
        next.line_count_ = SubtreeLineCount(next.root_);
        next.total_height_px_ = SubtreeHeight(next.root_);
        next.max_inline_extent_px_ = SubtreeMaxInlineExtent(next.root_);
        next.next_node_id_ = nextNodeId;
        next.initialized_ = true;
        *out = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<NoteRenderLineLayoutLocation> NoteRenderLineLayoutMap::LineAt(
    const Snapshot& snapshot,
    LineIndex index) noexcept {
    if (!snapshot.valid() || index.value >= snapshot.line_count_) return std::nullopt;

    const Node* current = snapshot.root_.get();
    size_t target = index.value;
    size_t prefixLines = 0;
    uint64_t prefixHeight = 0;
    while (current) {
        const size_t leftLines = SubtreeLineCount(current->left);
        const uint64_t leftHeight = SubtreeHeight(current->left);
        if (target < leftLines) {
            current = current->left.get();
            continue;
        }
        prefixHeight += leftHeight;
        if (target == leftLines) {
            return NoteRenderLineLayoutLocation{
                {prefixLines + leftLines}, current->layout, prefixHeight,
                prefixHeight + current->layout.height_px};
        }
        prefixHeight += current->layout.height_px;
        prefixLines += leftLines + 1;
        target -= leftLines + 1;
        current = current->right.get();
    }
    return std::nullopt;
}

std::optional<NoteRenderLineLayoutLocation> NoteRenderLineLayoutMap::LineContainingY(
    const Snapshot& snapshot,
    uint64_t contentY) noexcept {
    if (!snapshot.valid() || contentY >= snapshot.total_height_px_) return std::nullopt;

    const Node* current = snapshot.root_.get();
    size_t prefixLines = 0;
    uint64_t prefixHeight = 0;
    while (current) {
        const uint64_t leftHeight = SubtreeHeight(current->left);
        const size_t leftLines = SubtreeLineCount(current->left);
        if (contentY < prefixHeight + leftHeight) {
            current = current->left.get();
            continue;
        }
        const uint64_t top = prefixHeight + leftHeight;
        const uint64_t bottom = top + current->layout.height_px;
        if (contentY < bottom) {
            return NoteRenderLineLayoutLocation{
                {prefixLines + leftLines}, current->layout, top, bottom};
        }
        prefixHeight = bottom;
        prefixLines += leftLines + 1;
        current = current->right.get();
    }
    return std::nullopt;
}

NoteRenderLayoutSnapshotBuildResult NoteRenderLayoutSnapshot::Build(
    std::shared_ptr<const NoteSyntaxSnapshot> syntax,
    NoteRenderLayoutKey layoutKey,
    NoteRenderLineLayoutMap::Snapshot lineLayouts,
    std::shared_ptr<const NoteRenderLayoutSnapshot>* out) noexcept {
    if (!out) return NoteRenderLayoutSnapshotBuildResult::InvalidOutput;
    if (!syntax || !syntax->valid()) {
        return NoteRenderLayoutSnapshotBuildResult::InvalidSyntaxSnapshot;
    }
    if (!layoutKey.valid()) return NoteRenderLayoutSnapshotBuildResult::InvalidLayoutKey;
    if (!lineLayouts.valid() ||
        lineLayouts.line_count() != syntax->source_line_map().line_count()) {
        return NoteRenderLayoutSnapshotBuildResult::LineCountMismatch;
    }
    try {
        std::shared_ptr<NoteRenderLayoutSnapshot> candidate(new NoteRenderLayoutSnapshot());
        candidate->syntax_ = std::move(syntax);
        candidate->layout_key_ = layoutKey;
        candidate->line_layouts_ = std::move(lineLayouts);
        candidate->valid_ = true;
        *out = std::move(candidate);
        return NoteRenderLayoutSnapshotBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderLayoutSnapshotBuildResult::AllocationFailure;
    } catch (...) {
        return NoteRenderLayoutSnapshotBuildResult::AllocationFailure;
    }
}

NoteRenderLayoutSnapshotLocalSpliceResult NoteRenderLayoutSnapshot::BuildLocalSplice(
    std::shared_ptr<const NoteRenderLayoutSnapshot> previous,
    std::shared_ptr<const NoteSyntaxSnapshot> syntax,
    NoteRenderLayoutKey layoutKey,
    LineIndex first,
    LineIndex last_exclusive,
    const std::vector<NoteRenderLineLayout>& replacement,
    std::shared_ptr<const NoteRenderLayoutSnapshot>* out) noexcept {
    if (!out) return NoteRenderLayoutSnapshotLocalSpliceResult::InvalidOutput;
    if (!previous || !previous->valid_ || !previous->syntax_ || !previous->syntax_->valid() ||
        !previous->line_layouts_.valid()) {
        return NoteRenderLayoutSnapshotLocalSpliceResult::InvalidPreviousSnapshot;
    }
    if (!syntax || !syntax->valid() ||
        syntax->source_line_map().line_count() != previous->line_layouts_.line_count()) {
        return NoteRenderLayoutSnapshotLocalSpliceResult::InvalidSyntaxSnapshot;
    }
    if (layoutKey != previous->layout_key_) {
        return NoteRenderLayoutSnapshotLocalSpliceResult::LayoutKeyMismatch;
    }
    if (first.value >= last_exclusive.value ||
        last_exclusive.value > previous->line_layouts_.line_count() ||
        replacement.size() != last_exclusive.value - first.value) {
        return NoteRenderLayoutSnapshotLocalSpliceResult::InvalidRange;
    }
    NoteRenderLineLayoutMap::Snapshot spliced;
    if (!NoteRenderLineLayoutMap::Replace(
            previous->line_layouts_, first, last_exclusive, replacement, &spliced)) {
        return NoteRenderLayoutSnapshotLocalSpliceResult::ReplacementRejected;
    }
    const NoteRenderLayoutSnapshotBuildResult built = Build(
        std::move(syntax), layoutKey, std::move(spliced), out);
    switch (built) {
    case NoteRenderLayoutSnapshotBuildResult::Built:
        return NoteRenderLayoutSnapshotLocalSpliceResult::Built;
    case NoteRenderLayoutSnapshotBuildResult::AllocationFailure:
        return NoteRenderLayoutSnapshotLocalSpliceResult::AllocationFailure;
    default:
        return NoteRenderLayoutSnapshotLocalSpliceResult::ReplacementRejected;
    }
}

NoteDerivedSnapshotIdentity NoteRenderLayoutSnapshot::source_identity() const noexcept {
    return syntax_ ? syntax_->source_identity() : NoteDerivedSnapshotIdentity{};
}

bool NoteRenderLayoutSnapshot::Matches(const NoteTextCore& textCore,
                                       const NoteRenderLayoutKey& layoutKey) const noexcept {
    return valid_ && syntax_ && syntax_->MatchesTextCore(textCore) &&
           line_layouts_.valid() && layout_key_ == layoutKey;
}

} // namespace note
