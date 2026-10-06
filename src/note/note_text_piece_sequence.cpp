#include "note/note_text_piece_sequence.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace note {

struct NoteTextPieceSequence::Piece {
    std::shared_ptr<const std::wstring> storage;
    size_t offset = 0;
    size_t length = 0;
};

struct NoteTextPieceSequence::Node {
    Node(Piece value, uint64_t node_priority, NodePtr node_left, NodePtr node_right)
        : piece(std::move(value)),
          priority(node_priority),
          left(std::move(node_left)),
          right(std::move(node_right)) {
        subtree_length = SubtreeLength(left) + piece.length + SubtreeLength(right);
    }

    Piece piece;
    uint64_t priority = 0;
    NodePtr left;
    NodePtr right;
    size_t subtree_length = 0;
};

struct NoteTextPieceSequence::VersionIdentity {};

struct NoteTextPieceSequence::Fragment {
    std::shared_ptr<const std::wstring> storage;
    size_t offset = 0;
    size_t length = 0;
};

size_t NoteTextPieceSequence::SubtreeLength(const NodePtr& node) noexcept {
    return node ? node->subtree_length : 0;
}

uint64_t NoteTextPieceSequence::PriorityFor(uint64_t id) noexcept {
    uint64_t value = id + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

NoteTextPieceSequence::NodePtr NoteTextPieceSequence::MakeNode(
    Piece piece,
    uint64_t priority,
    NodePtr left,
    NodePtr right) {
    return std::make_shared<const Node>(std::move(piece), priority,
                                       std::move(left), std::move(right));
}

NoteTextPieceSequence::NodePtr NoteTextPieceSequence::CloneNode(
    const NodePtr& node,
    NodePtr left,
    NodePtr right) {
    if (!node) return nullptr;
    return MakeNode(node->piece, node->priority, std::move(left), std::move(right));
}

NoteTextPieceSequence::NodePtr NoteTextPieceSequence::Merge(const NodePtr& left,
                                                              const NodePtr& right) {
    if (!left) return right;
    if (!right) return left;
    if (left->priority <= right->priority) {
        return CloneNode(left, left->left, Merge(left->right, right));
    }
    return CloneNode(right, Merge(left, right->left), right->right);
}

uint64_t NoteTextPieceSequence::NextPriorityId(uint64_t* next_node_id) {
    if (!next_node_id || *next_node_id == 0 ||
        *next_node_id == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("piece sequence node identifier exhausted");
    }
    const uint64_t id = *next_node_id;
    ++*next_node_id;
    return id;
}

std::pair<NoteTextPieceSequence::NodePtr, NoteTextPieceSequence::NodePtr>
NoteTextPieceSequence::Split(const NodePtr& node,
                             size_t left_length,
                             uint64_t* next_node_id) {
    if (!node) return {};

    const size_t left_subtree_length = SubtreeLength(node->left);
    if (left_length < left_subtree_length) {
        auto split = Split(node->left, left_length, next_node_id);
        return {std::move(split.first),
                CloneNode(node, std::move(split.second), node->right)};
    }

    const size_t piece_start = left_subtree_length;
    const size_t piece_end = piece_start + node->piece.length;
    if (left_length > piece_end) {
        auto split = Split(node->right, left_length - piece_end, next_node_id);
        return {CloneNode(node, node->left, std::move(split.first)),
                std::move(split.second)};
    }
    if (left_length == piece_start) {
        return {node->left, CloneNode(node, nullptr, node->right)};
    }
    if (left_length == piece_end) {
        return {CloneNode(node, node->left, nullptr), node->right};
    }

    const size_t left_piece_length = left_length - piece_start;
    const size_t right_piece_length = node->piece.length - left_piece_length;
    Piece left_piece{node->piece.storage, node->piece.offset, left_piece_length};
    Piece right_piece{node->piece.storage, node->piece.offset + left_piece_length,
                      right_piece_length};
    NodePtr left_leaf = MakeNode(std::move(left_piece),
                                 PriorityFor(NextPriorityId(next_node_id)), nullptr, nullptr);
    NodePtr right_leaf = MakeNode(std::move(right_piece),
                                  PriorityFor(NextPriorityId(next_node_id)), nullptr, nullptr);
    return {Merge(node->left, left_leaf), Merge(right_leaf, node->right)};
}

bool NoteTextPieceSequence::IsValidRange(size_t text_length,
                                         Utf16CodeUnitOffset start,
                                         size_t length) noexcept {
    return start.value <= text_length && length <= text_length - start.value;
}

NoteTextPieceSequenceMutationResult NoteTextPieceSequence::Reset(std::wstring text) {
    try {
        NodePtr next_root;
        const auto next_version = std::make_shared<const VersionIdentity>();
        uint64_t next_node_id = 1;
        if (!text.empty()) {
            const auto storage = std::make_shared<const std::wstring>(std::move(text));
            Piece piece{storage, 0, storage->size()};
            next_root = MakeNode(std::move(piece),
                                 PriorityFor(NextPriorityId(&next_node_id)), nullptr, nullptr);
        }
        root_ = std::move(next_root);
        version_ = std::move(next_version);
        text_length_ = SubtreeLength(root_);
        next_node_id_ = next_node_id;
        initialized_ = true;
        return NoteTextPieceSequenceMutationResult::Applied;
    } catch (const std::overflow_error&) {
        return NoteTextPieceSequenceMutationResult::LengthOverflow;
    } catch (...) {
        return NoteTextPieceSequenceMutationResult::AllocationFailure;
    }
}

NoteTextPieceSequenceMutationResult NoteTextPieceSequence::Apply(const TextEdit& edit) {
    if (!initialized_ || !IsValidRange(text_length_, edit.start, edit.deleted_len)) {
        return NoteTextPieceSequenceMutationResult::InvalidRange;
    }
    if (edit.deleted_len == 0 && edit.inserted_text.empty()) {
        return NoteTextPieceSequenceMutationResult::Applied;
    }
    const size_t retained = text_length_ - edit.deleted_len;
    if (edit.inserted_text.size() > std::numeric_limits<size_t>::max() - retained) {
        return NoteTextPieceSequenceMutationResult::LengthOverflow;
    }

    try {
        uint64_t next_node_id = next_node_id_;
        const auto next_version = std::make_shared<const VersionIdentity>();
        auto prefix_and_remainder = Split(root_, edit.start.value, &next_node_id);
        auto removed_and_suffix = Split(prefix_and_remainder.second, edit.deleted_len,
                                        &next_node_id);
        NodePtr inserted;
        if (!edit.inserted_text.empty()) {
            const auto storage = std::make_shared<const std::wstring>(edit.inserted_text);
            inserted = MakeNode(Piece{storage, 0, storage->size()},
                                PriorityFor(NextPriorityId(&next_node_id)), nullptr, nullptr);
        }
        NodePtr next_root = Merge(Merge(prefix_and_remainder.first, inserted),
                                  removed_and_suffix.second);
        if (SubtreeLength(next_root) != retained + edit.inserted_text.size()) {
            return NoteTextPieceSequenceMutationResult::LengthOverflow;
        }
        root_ = std::move(next_root);
        version_ = std::move(next_version);
        text_length_ = retained + edit.inserted_text.size();
        next_node_id_ = next_node_id;
        return NoteTextPieceSequenceMutationResult::Applied;
    } catch (const std::overflow_error&) {
        return NoteTextPieceSequenceMutationResult::LengthOverflow;
    } catch (...) {
        return NoteTextPieceSequenceMutationResult::AllocationFailure;
    }
}

NoteTextPieceSequence::Snapshot NoteTextPieceSequence::TakeSnapshot() const noexcept {
    Snapshot snapshot;
    snapshot.root_ = root_;
    snapshot.version_ = version_;
    snapshot.text_length_ = text_length_;
    snapshot.initialized_ = initialized_;
    return snapshot;
}

bool NoteTextPieceSequence::SameSnapshotIdentity(const Snapshot& lhs,
                                                 const Snapshot& rhs) noexcept {
    return lhs.initialized_ && rhs.initialized_ && lhs.text_length_ == rhs.text_length_ &&
           lhs.root_ == rhs.root_ && lhs.version_ == rhs.version_;
}

std::wstring NoteTextPieceSequence::CopyRange(const Snapshot& snapshot,
                                              Utf16CodeUnitOffset start,
                                              size_t length) {
    if (!snapshot.initialized_ || !IsValidRange(snapshot.text_length_, start, length)) {
        return {};
    }
    std::wstring out;
    out.reserve(length);
    AppendRange(snapshot.root_, 0, start.value, start.value + length, &out);
    return out;
}

bool NoteTextPieceSequence::MatchesSnapshot(const Snapshot& snapshot) const noexcept {
    return initialized_ && snapshot.initialized_ &&
           text_length_ == snapshot.text_length_ && root_ == snapshot.root_ &&
           version_ == snapshot.version_;
}

void NoteTextPieceSequence::AppendRange(const NodePtr& node,
                                        size_t prefix_length,
                                        size_t range_start,
                                        size_t range_end,
                                        std::wstring* out) {
    if (!node || !out || range_start >= range_end) return;
    const size_t node_end = prefix_length + node->subtree_length;
    if (range_end <= prefix_length || range_start >= node_end) return;

    const size_t left_length = SubtreeLength(node->left);
    AppendRange(node->left, prefix_length, range_start, range_end, out);
    const size_t piece_start = prefix_length + left_length;
    const size_t piece_end = piece_start + node->piece.length;
    const size_t overlap_start = std::max(range_start, piece_start);
    const size_t overlap_end = std::min(range_end, piece_end);
    if (overlap_start < overlap_end) {
        out->append(*node->piece.storage,
                    node->piece.offset + overlap_start - piece_start,
                    overlap_end - overlap_start);
    }
    AppendRange(node->right, piece_end, range_start, range_end, out);
}

std::wstring NoteTextPieceSequence::CopyRange(Utf16CodeUnitOffset start, size_t length) const {
    if (!initialized_ || !IsValidRange(text_length_, start, length)) return {};
    std::wstring out;
    out.reserve(length);
    AppendRange(root_, 0, start.value, start.value + length, &out);
    return out;
}

std::wstring NoteTextPieceSequence::Materialize() const {
    return CopyRange(Utf16CodeUnitOffset{0}, text_length_);
}

bool NoteTextPieceSequence::RangeEquals(const NodePtr& node,
                                        size_t prefix_length,
                                        size_t range_start,
                                        size_t range_end,
                                        std::wstring_view expected,
                                        size_t* expected_offset) noexcept {
    if (!node || range_start >= range_end) return true;
    const size_t node_end = prefix_length + node->subtree_length;
    if (range_end <= prefix_length || range_start >= node_end) return true;

    const size_t left_length = SubtreeLength(node->left);
    if (!RangeEquals(node->left, prefix_length, range_start, range_end,
                     expected, expected_offset)) {
        return false;
    }
    const size_t piece_start = prefix_length + left_length;
    const size_t piece_end = piece_start + node->piece.length;
    const size_t overlap_start = std::max(range_start, piece_start);
    const size_t overlap_end = std::min(range_end, piece_end);
    if (overlap_start < overlap_end) {
        const size_t length = overlap_end - overlap_start;
        if (!expected_offset || *expected_offset > expected.size() ||
            length > expected.size() - *expected_offset ||
            node->piece.storage->compare(node->piece.offset + overlap_start - piece_start,
                                         length, expected.data() + *expected_offset,
                                         length) != 0) {
            return false;
        }
        *expected_offset += length;
    }
    return RangeEquals(node->right, piece_end, range_start, range_end,
                       expected, expected_offset);
}

bool NoteTextPieceSequence::EqualsRange(Utf16CodeUnitOffset start,
                                        std::wstring_view text) const noexcept {
    if (!initialized_ || !IsValidRange(text_length_, start, text.size())) return false;
    size_t expected_offset = 0;
    return RangeEquals(root_, 0, start.value, start.value + text.size(), text,
                       &expected_offset) && expected_offset == text.size();
}

bool NoteTextPieceSequence::Equals(std::wstring_view text) const noexcept {
    return text.size() == text_length_ && EqualsRange(Utf16CodeUnitOffset{0}, text);
}

void NoteTextPieceSequence::AppendFragments(const NodePtr& node,
                                            size_t prefix_length,
                                            size_t range_start,
                                            size_t range_end,
                                            std::vector<Fragment>* out) {
    if (!node || !out || range_start >= range_end) return;
    const size_t node_end = prefix_length + node->subtree_length;
    if (range_end <= prefix_length || range_start >= node_end) return;

    const size_t left_length = SubtreeLength(node->left);
    AppendFragments(node->left, prefix_length, range_start, range_end, out);
    const size_t piece_start = prefix_length + left_length;
    const size_t piece_end = piece_start + node->piece.length;
    const size_t overlap_start = std::max(range_start, piece_start);
    const size_t overlap_end = std::min(range_end, piece_end);
    if (overlap_start < overlap_end) {
        out->push_back(Fragment{node->piece.storage,
                                node->piece.offset + overlap_start - piece_start,
                                overlap_end - overlap_start});
    }
    AppendFragments(node->right, piece_end, range_start, range_end, out);
}

bool NoteTextPieceSequence::FragmentsShareExactly(const std::vector<Fragment>& before,
                                                   const std::vector<Fragment>& current,
                                                   size_t length) noexcept {
    size_t before_index = 0;
    size_t current_index = 0;
    size_t before_offset = 0;
    size_t current_offset = 0;
    size_t compared = 0;
    while (compared < length) {
        if (before_index >= before.size() || current_index >= current.size()) return false;
        const Fragment& before_fragment = before[before_index];
        const Fragment& current_fragment = current[current_index];
        if (before_fragment.storage != current_fragment.storage ||
            before_fragment.offset + before_offset != current_fragment.offset + current_offset) {
            return false;
        }
        const size_t before_remaining = before_fragment.length - before_offset;
        const size_t current_remaining = current_fragment.length - current_offset;
        const size_t step = std::min(before_remaining, current_remaining);
        if (step == 0 || step > length - compared) return false;
        compared += step;
        before_offset += step;
        current_offset += step;
        if (before_offset == before_fragment.length) {
            ++before_index;
            before_offset = 0;
        }
        if (current_offset == current_fragment.length) {
            ++current_index;
            current_offset = 0;
        }
    }
    return true;
}

bool NoteTextPieceSequence::SnapshotsShareExactRange(
    const Snapshot& before,
    Utf16CodeUnitOffset beforeStart,
    const Snapshot& current,
    Utf16CodeUnitOffset currentStart,
    size_t length) noexcept {
    if (!before.valid() || !current.valid() ||
        !IsValidRange(before.text_length_, beforeStart, length) ||
        !IsValidRange(current.text_length_, currentStart, length)) {
        return false;
    }
    if (length == 0) return true;
    try {
        std::vector<Fragment> before_fragments;
        std::vector<Fragment> current_fragments;
        AppendFragments(before.root_, 0, beforeStart.value, beforeStart.value + length,
                        &before_fragments);
        AppendFragments(current.root_, 0, currentStart.value, currentStart.value + length,
                        &current_fragments);
        return FragmentsShareExactly(before_fragments, current_fragments, length);
    } catch (...) {
        return false;
    }
}

bool NoteTextPieceSequence::SharesExactRange(const Snapshot& before,
                                             Utf16CodeUnitOffset beforeStart,
                                             Utf16CodeUnitOffset currentStart,
                                             size_t length) const {
    return SnapshotsShareExactRange(
        before, beforeStart, TakeSnapshot(), currentStart, length);
}

} // namespace note
