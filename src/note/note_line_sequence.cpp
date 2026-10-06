#include "note/note_line_sequence.h"

#include <limits>
#include <utility>

namespace note {

bool operator==(NoteLineId lhs, NoteLineId rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator!=(NoteLineId lhs, NoteLineId rhs) noexcept {
    return !(lhs == rhs);
}

struct NoteLineSequence::Node {
    explicit Node(NoteLineSequenceLine value)
        : line(std::move(value)), priority(PriorityFor(line.id)) {
        Recompute(this);
    }

    NoteLineSequenceLine line;
    uint64_t priority = 0;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
    size_t subtree_line_count = 1;
    size_t subtree_text_length = 0;
};

NoteLineSequence::NoteLineSequence() = default;
NoteLineSequence::~NoteLineSequence() = default;
NoteLineSequence::NoteLineSequence(NoteLineSequence&&) noexcept = default;
NoteLineSequence& NoteLineSequence::operator=(NoteLineSequence&&) noexcept = default;

size_t NoteLineSequence::SubtreeLineCount(const Node* node) noexcept {
    return node ? node->subtree_line_count : 0;
}

size_t NoteLineSequence::SubtreeTextLength(const Node* node) noexcept {
    return node ? node->subtree_text_length : 0;
}

void NoteLineSequence::Recompute(Node* node) noexcept {
    if (!node) return;
    const size_t own_length = node->line.content_length + node->line.line_break_length;
    node->subtree_line_count = SubtreeLineCount(node->left.get()) + 1 +
        SubtreeLineCount(node->right.get());
    node->subtree_text_length = SubtreeTextLength(node->left.get()) + own_length +
        SubtreeTextLength(node->right.get());
}

uint64_t NoteLineSequence::PriorityFor(NoteLineId id) noexcept {
    uint64_t value = id.value + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void NoteLineSequence::Split(std::unique_ptr<Node> node,
                             size_t left_line_count,
                             std::unique_ptr<Node>* out_left,
                             std::unique_ptr<Node>* out_right) noexcept {
    if (!out_left || !out_right) return;
    if (!node) {
        out_left->reset();
        out_right->reset();
        return;
    }
    const size_t existing_left_count = SubtreeLineCount(node->left.get());
    if (left_line_count <= existing_left_count) {
        std::unique_ptr<Node> split_left;
        Split(std::move(node->left), left_line_count, &split_left, &node->left);
        Recompute(node.get());
        *out_left = std::move(split_left);
        *out_right = std::move(node);
        return;
    }

    std::unique_ptr<Node> split_right;
    Split(std::move(node->right), left_line_count - existing_left_count - 1,
          &node->right, &split_right);
    Recompute(node.get());
    *out_left = std::move(node);
    *out_right = std::move(split_right);
}

std::unique_ptr<NoteLineSequence::Node> NoteLineSequence::Merge(
    std::unique_ptr<Node> left,
    std::unique_ptr<Node> right) noexcept {
    if (!left) return right;
    if (!right) return left;
    if (left->priority <= right->priority) {
        left->right = Merge(std::move(left->right), std::move(right));
        Recompute(left.get());
        return left;
    }
    right->left = Merge(std::move(left), std::move(right->left));
    Recompute(right.get());
    return right;
}

void NoteLineSequence::CollectIds(const Node* node, std::vector<NoteLineId>* out) {
    if (!node || !out) return;
    CollectIds(node->left.get(), out);
    out->push_back(node->line.id);
    CollectIds(node->right.get(), out);
}

void NoteLineSequence::AppendLocationsInOrder(
    const Node* node,
    size_t prefix_text_length,
    size_t prefix_line_count,
    std::vector<NoteLineSequenceLocation>* out) {
    if (!node || !out) return;

    const size_t left_text_length = SubtreeTextLength(node->left.get());
    const size_t left_line_count = SubtreeLineCount(node->left.get());
    AppendLocationsInOrder(node->left.get(), prefix_text_length, prefix_line_count, out);

    const size_t line_start = prefix_text_length + left_text_length;
    const size_t content_end = line_start + node->line.content_length;
    out->push_back(NoteLineSequenceLocation{
        {prefix_line_count + left_line_count}, node->line,
        {line_start}, {content_end}, {content_end + node->line.line_break_length}});

    AppendLocationsInOrder(node->right.get(),
                           content_end + node->line.line_break_length,
                           prefix_line_count + left_line_count + 1,
                           out);
}

size_t NoteLineSequence::PrefixTextLength(const Node* node, size_t line_count) noexcept {
    size_t prefix = 0;
    const Node* current = node;
    size_t remaining = line_count;
    while (current && remaining > 0) {
        const size_t left_count = SubtreeLineCount(current->left.get());
        if (remaining <= left_count) {
            current = current->left.get();
            continue;
        }
        prefix += SubtreeTextLength(current->left.get());
        const size_t own_length = current->line.content_length + current->line.line_break_length;
        if (remaining == left_count + 1) {
            prefix += own_length;
            return prefix;
        }
        prefix += own_length;
        remaining -= left_count + 1;
        current = current->right.get();
    }
    return prefix;
}

NoteLineSequenceMutationResult NoteLineSequence::MaterializeLines(
    const std::vector<NoteLineSequenceLineSpec>& specs,
    uint64_t first_id,
    std::vector<NoteLineSequenceLine>* out_lines,
    uint64_t* out_next_id,
    size_t* out_text_length) {
    if (!out_lines || !out_next_id || !out_text_length) {
        return NoteLineSequenceMutationResult::InvalidRange;
    }
    out_lines->clear();
    out_lines->reserve(specs.size());
    size_t total_length = 0;
    uint64_t next_id = first_id;
    for (const NoteLineSequenceLineSpec& spec : specs) {
        if (spec.line_break_length >
            std::numeric_limits<size_t>::max() - spec.content_length) {
            return NoteLineSequenceMutationResult::LengthOverflow;
        }
        const size_t line_length = spec.content_length + spec.line_break_length;
        if (line_length > std::numeric_limits<size_t>::max() - total_length) {
            return NoteLineSequenceMutationResult::LengthOverflow;
        }
        if (next_id == 0) {
            return NoteLineSequenceMutationResult::IdExhausted;
        }
        out_lines->push_back(NoteLineSequenceLine{
            NoteLineId{next_id}, spec.content_length, spec.line_break_length});
        total_length += line_length;
        ++next_id;
    }
    *out_next_id = next_id;
    *out_text_length = total_length;
    return NoteLineSequenceMutationResult::Applied;
}

std::unique_ptr<NoteLineSequence::Node> NoteLineSequence::BuildTree(
    const std::vector<NoteLineSequenceLine>& lines) {
    std::unique_ptr<Node> root;
    for (const NoteLineSequenceLine& line : lines) {
        root = Merge(std::move(root), std::make_unique<Node>(line));
    }
    return root;
}

NoteLineSequenceMutationResult NoteLineSequence::Reset(
    const std::vector<NoteLineSequenceLineSpec>& lines) {
    std::vector<NoteLineSequenceLine> materialized;
    uint64_t next_id = next_line_id_;
    size_t total_length = 0;
    const NoteLineSequenceMutationResult result = MaterializeLines(
        lines, next_line_id_, &materialized, &next_id, &total_length);
    if (result != NoteLineSequenceMutationResult::Applied) return result;
    root_ = BuildTree(materialized);
    next_line_id_ = next_id;
    return NoteLineSequenceMutationResult::Applied;
}

NoteLineSequenceReplaceResult NoteLineSequence::Replace(
    LineIndex first,
    LineIndex last_exclusive,
    const std::vector<NoteLineSequenceLineSpec>& replacement) {
    NoteLineSequenceReplaceResult result;
    const size_t current_line_count = line_count();
    if (first.value > last_exclusive.value || last_exclusive.value > current_line_count) {
        result.result = NoteLineSequenceMutationResult::InvalidRange;
        return result;
    }

    std::vector<NoteLineSequenceLine> materialized;
    uint64_t next_id = next_line_id_;
    size_t replacement_length = 0;
    result.result = MaterializeLines(
        replacement, next_line_id_, &materialized, &next_id, &replacement_length);
    if (!result.applied()) return result;

    const size_t prefix_before = PrefixTextLength(root_.get(), first.value);
    const size_t prefix_after = PrefixTextLength(root_.get(), last_exclusive.value);
    const size_t removed_length = prefix_after - prefix_before;
    const size_t retained_length = text_length() - removed_length;
    if (replacement_length > std::numeric_limits<size_t>::max() - retained_length) {
        result.result = NoteLineSequenceMutationResult::LengthOverflow;
        return result;
    }

    std::unique_ptr<Node> left;
    std::unique_ptr<Node> middle_and_right;
    std::unique_ptr<Node> removed;
    std::unique_ptr<Node> right;
    Split(std::move(root_), first.value, &left, &middle_and_right);
    Split(std::move(middle_and_right), last_exclusive.value - first.value, &removed, &right);
    result.removed_line_ids.reserve(SubtreeLineCount(removed.get()));
    CollectIds(removed.get(), &result.removed_line_ids);
    result.inserted_line_ids.reserve(materialized.size());
    for (const NoteLineSequenceLine& line : materialized) {
        result.inserted_line_ids.push_back(line.id);
    }
    root_ = Merge(Merge(std::move(left), BuildTree(materialized)), std::move(right));
    next_line_id_ = next_id;
    result.result = NoteLineSequenceMutationResult::Applied;
    return result;
}

size_t NoteLineSequence::line_count() const noexcept {
    return SubtreeLineCount(root_.get());
}

size_t NoteLineSequence::text_length() const noexcept {
    return SubtreeTextLength(root_.get());
}

std::optional<NoteLineSequenceLocation> NoteLineSequence::LineAt(LineIndex index) const noexcept {
    if (index.value >= line_count()) return std::nullopt;
    const Node* current = root_.get();
    size_t target = index.value;
    size_t prefix_length = 0;
    while (current) {
        const size_t left_count = SubtreeLineCount(current->left.get());
        const size_t left_length = SubtreeTextLength(current->left.get());
        if (target < left_count) {
            current = current->left.get();
            continue;
        }
        prefix_length += left_length;
        if (target == left_count) {
            const size_t content_end = prefix_length + current->line.content_length;
            const size_t next_start = content_end + current->line.line_break_length;
            return NoteLineSequenceLocation{
                index, current->line, {prefix_length}, {content_end}, {next_start}};
        }
        prefix_length += current->line.content_length + current->line.line_break_length;
        target -= left_count + 1;
        current = current->right.get();
    }
    return std::nullopt;
}

std::optional<NoteLineSequenceLocation> NoteLineSequence::FindByOffset(
    Utf16CodeUnitOffset offset) const noexcept {
    const size_t total_length = text_length();
    const size_t total_lines = line_count();
    if (total_lines == 0 || offset.value > total_length) return std::nullopt;
    if (offset.value == total_length) {
        return LineAt(LineIndex{total_lines - 1});
    }

    const Node* current = root_.get();
    size_t prefix_length = 0;
    size_t prefix_lines = 0;
    while (current) {
        const size_t left_length = SubtreeTextLength(current->left.get());
        const size_t left_count = SubtreeLineCount(current->left.get());
        const size_t line_start = prefix_length + left_length;
        const size_t line_length = current->line.content_length + current->line.line_break_length;
        if (offset.value < line_start) {
            current = current->left.get();
            continue;
        }
        if (offset.value < line_start + line_length) {
            const size_t content_end = line_start + current->line.content_length;
            return NoteLineSequenceLocation{
                {prefix_lines + left_count}, current->line,
                {line_start}, {content_end}, {line_start + line_length}};
        }
        prefix_length = line_start + line_length;
        prefix_lines += left_count + 1;
        current = current->right.get();
    }
    return std::nullopt;
}

std::vector<NoteLineSequenceLocation> NoteLineSequence::SnapshotLocations() const {
    std::vector<NoteLineSequenceLocation> locations;
    locations.reserve(line_count());
    AppendLocationsInOrder(root_.get(), 0, 0, &locations);
    return locations;
}

} // namespace note
