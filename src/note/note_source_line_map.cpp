#include "note/note_source_line_map.h"

#include <limits>
#include <new>
#include <utility>

namespace note {

struct NoteSourceLineMap::Node {
    Node(NoteSourceLineSpec value,
         uint64_t node_priority,
         NodePtr node_left,
         NodePtr node_right)
        : line(std::move(value)),
          priority(node_priority),
          left(std::move(node_left)),
          right(std::move(node_right)) {
        const size_t own_length = line.content_length + line.line_break_length;
        const size_t own_rich_edit_length = line.content_length +
            (line.line_break_length == 0 ? 0u : 1u);
        subtree_line_count = SubtreeLineCount(left) + 1 + SubtreeLineCount(right);
        subtree_text_length = SubtreeTextLength(left) + own_length +
            SubtreeTextLength(right);
        subtree_rich_edit_text_length = SubtreeRichEditTextLength(left) +
            own_rich_edit_length + SubtreeRichEditTextLength(right);
    }

    NoteSourceLineSpec line;
    uint64_t priority = 0;
    NodePtr left;
    NodePtr right;
    size_t subtree_line_count = 0;
    size_t subtree_text_length = 0;
    size_t subtree_rich_edit_text_length = 0;
};

struct NoteSourceLineMap::VersionIdentity {};

size_t NoteSourceLineMap::SubtreeLineCount(const NodePtr& node) noexcept {
    return node ? node->subtree_line_count : 0;
}

size_t NoteSourceLineMap::SubtreeTextLength(const NodePtr& node) noexcept {
    return node ? node->subtree_text_length : 0;
}

size_t NoteSourceLineMap::SubtreeRichEditTextLength(const NodePtr& node) noexcept {
    return node ? node->subtree_rich_edit_text_length : 0;
}

uint64_t NoteSourceLineMap::PriorityFor(uint64_t id) noexcept {
    uint64_t value = id + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

bool NoteSourceLineMap::TakeNodeId(uint64_t* next_node_id,
                                   uint64_t* out_id) noexcept {
    if (!next_node_id || !out_id || *next_node_id == 0) return false;
    *out_id = *next_node_id;
    if (*next_node_id == std::numeric_limits<uint64_t>::max()) {
        *next_node_id = 0;
    } else {
        ++*next_node_id;
    }
    return true;
}

NoteSourceLineMap::NodePtr NoteSourceLineMap::MakeNode(NoteSourceLineSpec line,
                                                        uint64_t priority,
                                                        NodePtr left,
                                                        NodePtr right) {
    return std::make_shared<const Node>(std::move(line), priority,
                                        std::move(left), std::move(right));
}

NoteSourceLineMap::NodePtr NoteSourceLineMap::CloneNode(const NodePtr& node,
                                                         NodePtr left,
                                                         NodePtr right) {
    if (!node) return nullptr;
    return MakeNode(node->line, node->priority, std::move(left), std::move(right));
}

NoteSourceLineMap::NodePtr NoteSourceLineMap::Merge(const NodePtr& left,
                                                     const NodePtr& right) {
    if (!left) return right;
    if (!right) return left;
    if (left->priority <= right->priority) {
        return CloneNode(left, left->left, Merge(left->right, right));
    }
    return CloneNode(right, Merge(left, right->left), right->right);
}

void NoteSourceLineMap::Split(const NodePtr& node,
                              size_t left_line_count,
                              NodePtr* out_left,
                              NodePtr* out_right) {
    if (!out_left || !out_right) return;
    if (!node) {
        out_left->reset();
        out_right->reset();
        return;
    }
    const size_t existing_left_count = SubtreeLineCount(node->left);
    if (left_line_count <= existing_left_count) {
        NodePtr split_left;
        NodePtr split_right;
        Split(node->left, left_line_count, &split_left, &split_right);
        *out_left = std::move(split_left);
        *out_right = CloneNode(node, std::move(split_right), node->right);
        return;
    }

    NodePtr split_left;
    NodePtr split_right;
    Split(node->right, left_line_count - existing_left_count - 1,
          &split_left, &split_right);
    *out_left = CloneNode(node, node->left, std::move(split_left));
    *out_right = std::move(split_right);
}

bool NoteSourceLineMap::BuildLineSpecs(
    std::wstring_view text,
    bool include_terminal_empty_row,
    std::vector<NoteSourceLineSpec>* out) noexcept {
    if (!out) return false;
    try {
        out->clear();
        out->reserve(1);
        if (text.empty()) {
            if (include_terminal_empty_row) out->push_back({});
            return true;
        }

        size_t start = 0;
        while (start < text.size()) {
            size_t content_end = start;
            while (content_end < text.size() && text[content_end] != L'\r' &&
                   text[content_end] != L'\n') {
                ++content_end;
            }
            size_t line_break_length = 0;
            if (content_end < text.size()) {
                line_break_length = 1;
                if (text[content_end] == L'\r' && content_end + 1 < text.size() &&
                    text[content_end + 1] == L'\n') {
                    line_break_length = 2;
                }
            }
            out->push_back({content_end - start, line_break_length});
            if (line_break_length == 0) break;
            start = content_end + line_break_length;
            if (start == text.size() && include_terminal_empty_row) {
                out->push_back({});
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteSourceLineMap::BuildTree(const std::vector<NoteSourceLineSpec>& lines,
                                  uint64_t* next_node_id,
                                  NodePtr* out_root) noexcept {
    if (!next_node_id || !out_root || lines.empty()) return false;
    try {
        NodePtr result;
        for (const NoteSourceLineSpec& line : lines) {
            if (line.line_break_length >
                std::numeric_limits<size_t>::max() - line.content_length) {
                return false;
            }
            uint64_t id = 0;
            if (!TakeNodeId(next_node_id, &id)) return false;
            result = Merge(result, MakeNode(line, PriorityFor(id), nullptr, nullptr));
        }
        *out_root = std::move(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteSourceLineMap::Build(std::wstring_view text, Snapshot* out) noexcept {
    if (!out) return false;
    try {
        std::vector<NoteSourceLineSpec> lines;
        if (!BuildLineSpecs(text, true, &lines) || lines.empty()) return false;

        uint64_t next_node_id = 1;
        NodePtr root;
        if (!BuildTree(lines, &next_node_id, &root) || !root ||
            SubtreeTextLength(root) != text.size()) {
            return false;
        }
        Snapshot next;
        next.root_ = std::move(root);
        next.version_ = std::make_shared<const VersionIdentity>();
        next.line_count_ = SubtreeLineCount(next.root_);
        next.text_length_ = SubtreeTextLength(next.root_);
        next.rich_edit_text_length_ = SubtreeRichEditTextLength(next.root_);
        next.next_node_id_ = next_node_id;
        next.initialized_ = true;
        *out = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteSourceLineMap::Replace(const Snapshot& before,
                                LineIndex first,
                                LineIndex last_exclusive,
                                const std::vector<NoteSourceLineSpec>& replacement,
                                Snapshot* out) noexcept {
    if (!out || !before.valid() || first.value > last_exclusive.value ||
        last_exclusive.value > before.line_count_ || replacement.empty()) {
        return false;
    }
    try {
        uint64_t next_node_id = before.next_node_id_;
        NodePtr replacement_root;
        if (!BuildTree(replacement, &next_node_id, &replacement_root)) return false;

        NodePtr left;
        NodePtr middle_and_right;
        NodePtr removed;
        NodePtr right;
        Split(before.root_, first.value, &left, &middle_and_right);
        Split(middle_and_right, last_exclusive.value - first.value, &removed, &right);
        const size_t retained_text_length = before.text_length_ - SubtreeTextLength(removed);
        const size_t replacement_text_length = SubtreeTextLength(replacement_root);
        if (replacement_text_length >
            std::numeric_limits<size_t>::max() - retained_text_length) {
            return false;
        }
        NodePtr root = Merge(Merge(left, replacement_root), right);
        if (!root || SubtreeTextLength(root) != retained_text_length + replacement_text_length) {
            return false;
        }

        Snapshot next;
        next.root_ = std::move(root);
        next.version_ = std::make_shared<const VersionIdentity>();
        next.line_count_ = SubtreeLineCount(next.root_);
        next.text_length_ = SubtreeTextLength(next.root_);
        next.rich_edit_text_length_ = SubtreeRichEditTextLength(next.root_);
        next.next_node_id_ = next_node_id;
        next.initialized_ = true;
        *out = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<NoteSourceLineLocation> NoteSourceLineMap::LineAt(
    const Snapshot& snapshot,
    LineIndex index) noexcept {
    if (!snapshot.valid() || index.value >= snapshot.line_count_) return std::nullopt;

    const Node* current = snapshot.root_.get();
    size_t target = index.value;
    size_t prefix_length = 0;
    size_t prefix_lines = 0;
    while (current) {
        const size_t left_lines = current->left ? current->left->subtree_line_count : 0;
        const size_t left_length = current->left ? current->left->subtree_text_length : 0;
        if (target < left_lines) {
            current = current->left.get();
            continue;
        }
        prefix_length += left_length;
        if (target == left_lines) {
            const size_t content_end = prefix_length + current->line.content_length;
            return NoteSourceLineLocation{
                {prefix_lines + left_lines}, current->line,
                {prefix_length}, {content_end},
                {content_end + current->line.line_break_length}};
        }
        prefix_length += current->line.content_length + current->line.line_break_length;
        prefix_lines += left_lines + 1;
        target -= left_lines + 1;
        current = current->right.get();
    }
    return std::nullopt;
}

std::optional<NoteSourceLineLocation> NoteSourceLineMap::FindByOffset(
    const Snapshot& snapshot,
    Utf16CodeUnitOffset offset) noexcept {
    if (!snapshot.valid() || snapshot.line_count_ == 0 ||
        offset.value > snapshot.text_length_) {
        return std::nullopt;
    }
    if (offset.value == snapshot.text_length_) {
        return LineAt(snapshot, {snapshot.line_count_ - 1});
    }

    const Node* current = snapshot.root_.get();
    size_t prefix_length = 0;
    size_t prefix_lines = 0;
    while (current) {
        const size_t left_lines = current->left ? current->left->subtree_line_count : 0;
        const size_t left_length = current->left ? current->left->subtree_text_length : 0;
        const size_t line_start = prefix_length + left_length;
        const size_t line_length = current->line.content_length + current->line.line_break_length;
        if (offset.value < line_start) {
            current = current->left.get();
            continue;
        }
        if (offset.value < line_start + line_length) {
            const size_t content_end = line_start + current->line.content_length;
            return NoteSourceLineLocation{
                {prefix_lines + left_lines}, current->line,
                {line_start}, {content_end},
                {content_end + current->line.line_break_length}};
        }
        prefix_length = line_start + line_length;
        prefix_lines += left_lines + 1;
        current = current->right.get();
    }
    return std::nullopt;
}

std::optional<size_t> NoteSourceLineMap::CanonicalOffsetToRichEditIndex(
    const Snapshot& snapshot,
    Utf16CodeUnitOffset offset) noexcept {
    if (!snapshot.valid() || offset.value > snapshot.text_length_) return std::nullopt;
    if (offset.value == snapshot.text_length_) return snapshot.rich_edit_text_length_;

    const Node* current = snapshot.root_.get();
    size_t canonicalPrefix = 0;
    size_t richEditPrefix = 0;
    while (current) {
        const size_t leftCanonicalLength = SubtreeTextLength(current->left);
        const size_t leftRichEditLength = SubtreeRichEditTextLength(current->left);
        const size_t lineStart = canonicalPrefix + leftCanonicalLength;
        const size_t lineLength = current->line.content_length + current->line.line_break_length;
        if (offset.value < lineStart) {
            current = current->left.get();
            continue;
        }

        richEditPrefix += leftRichEditLength;
        if (offset.value < lineStart + lineLength) {
            const size_t localCanonical = offset.value - lineStart;
            const size_t localRichEdit = localCanonical <= current->line.content_length
                ? localCanonical : current->line.content_length + 1;
            return richEditPrefix + localRichEdit;
        }
        canonicalPrefix = lineStart + lineLength;
        richEditPrefix += current->line.content_length +
            (current->line.line_break_length == 0 ? 0u : 1u);
        current = current->right.get();
    }
    return std::nullopt;
}

std::optional<Utf16CodeUnitOffset> NoteSourceLineMap::RichEditIndexToCanonicalOffset(
    const Snapshot& snapshot,
    size_t richEditIndex) noexcept {
    if (!snapshot.valid() || richEditIndex > snapshot.rich_edit_text_length_) return std::nullopt;
    if (richEditIndex == snapshot.rich_edit_text_length_) {
        return Utf16CodeUnitOffset{snapshot.text_length_};
    }

    const Node* current = snapshot.root_.get();
    size_t canonicalPrefix = 0;
    size_t richEditPrefix = 0;
    while (current) {
        const size_t leftCanonicalLength = SubtreeTextLength(current->left);
        const size_t leftRichEditLength = SubtreeRichEditTextLength(current->left);
        const size_t lineRichEditStart = richEditPrefix + leftRichEditLength;
        const size_t lineRichEditLength = current->line.content_length +
            (current->line.line_break_length == 0 ? 0u : 1u);
        if (richEditIndex < lineRichEditStart) {
            current = current->left.get();
            continue;
        }

        canonicalPrefix += leftCanonicalLength;
        richEditPrefix = lineRichEditStart;
        if (richEditIndex < lineRichEditStart + lineRichEditLength) {
            return Utf16CodeUnitOffset{canonicalPrefix +
                (richEditIndex - lineRichEditStart)};
        }
        canonicalPrefix += current->line.content_length + current->line.line_break_length;
        richEditPrefix += lineRichEditLength;
        current = current->right.get();
    }
    return std::nullopt;
}

bool NoteSourceLineMap::SameSnapshotIdentity(
    const Snapshot& lhs,
    const Snapshot& rhs) noexcept {
    return lhs.initialized_ && rhs.initialized_ &&
           lhs.root_ == rhs.root_ && lhs.version_ == rhs.version_ &&
           lhs.line_count_ == rhs.line_count_ && lhs.text_length_ == rhs.text_length_;
}

} // namespace note
