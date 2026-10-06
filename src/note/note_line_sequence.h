#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace note {

struct NoteLineId {
    uint64_t value = 0;

    [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

[[nodiscard]] bool operator==(NoteLineId lhs, NoteLineId rhs) noexcept;
[[nodiscard]] bool operator!=(NoteLineId lhs, NoteLineId rhs) noexcept;

// Lengths are canonical UTF-16 code units. A line break belongs to the line
// preceding it, so a prefix aggregate maps offsets without rewriting later
// line starts after a local edit.
struct NoteLineSequenceLineSpec {
    size_t content_length = 0;
    size_t line_break_length = 0;
};

struct NoteLineSequenceLine {
    NoteLineId id{};
    size_t content_length = 0;
    size_t line_break_length = 0;
};

struct NoteLineSequenceLocation {
    LineIndex line_index{};
    NoteLineSequenceLine line{};
    Utf16CodeUnitOffset start{};
    Utf16CodeUnitOffset content_end{};
    Utf16CodeUnitOffset next_start{};
};

enum class NoteLineSequenceMutationResult {
    Applied,
    InvalidRange,
    LengthOverflow,
    IdExhausted,
};

struct NoteLineSequenceReplaceResult {
    NoteLineSequenceMutationResult result = NoteLineSequenceMutationResult::InvalidRange;
    std::vector<NoteLineId> removed_line_ids;
    std::vector<NoteLineId> inserted_line_ids;

    [[nodiscard]] bool applied() const noexcept {
        return result == NoteLineSequenceMutationResult::Applied;
    }
};

// UI-thread owned implicit treap. It owns only ordering, stable line IDs, and
// aggregate lengths; render payloads remain in the caller's cache keyed by
// NoteLineId. This keeps source coordinates and cache ownership separable
// without retaining a duplicate canonical text buffer.
class NoteLineSequence {
public:
    NoteLineSequence();
    ~NoteLineSequence();
    NoteLineSequence(NoteLineSequence&&) noexcept;
    NoteLineSequence& operator=(NoteLineSequence&&) noexcept;
    NoteLineSequence(const NoteLineSequence&) = delete;
    NoteLineSequence& operator=(const NoteLineSequence&) = delete;

    [[nodiscard]] NoteLineSequenceMutationResult Reset(
        const std::vector<NoteLineSequenceLineSpec>& lines);
    [[nodiscard]] NoteLineSequenceReplaceResult Replace(
        LineIndex first,
        LineIndex last_exclusive,
        const std::vector<NoteLineSequenceLineSpec>& replacement);

    [[nodiscard]] size_t line_count() const noexcept;
    [[nodiscard]] size_t text_length() const noexcept;
    [[nodiscard]] std::optional<NoteLineSequenceLocation> LineAt(LineIndex index) const noexcept;
    [[nodiscard]] std::optional<NoteLineSequenceLocation> FindByOffset(
        Utf16CodeUnitOffset offset) const noexcept;
    // Full render/layout passes need sequential traversal.  Materializing
    // locations once keeps those passes O(n), while LineAt/FindByOffset remain
    // O(log n) for local interaction and edits.
    [[nodiscard]] std::vector<NoteLineSequenceLocation> SnapshotLocations() const;

private:
    struct Node;

    static size_t SubtreeLineCount(const Node* node) noexcept;
    static size_t SubtreeTextLength(const Node* node) noexcept;
    static void Recompute(Node* node) noexcept;
    static uint64_t PriorityFor(NoteLineId id) noexcept;
    static void Split(std::unique_ptr<Node> node,
                      size_t left_line_count,
                      std::unique_ptr<Node>* out_left,
                      std::unique_ptr<Node>* out_right) noexcept;
    static std::unique_ptr<Node> Merge(std::unique_ptr<Node> left,
                                       std::unique_ptr<Node> right) noexcept;
    static void CollectIds(const Node* node, std::vector<NoteLineId>* out);
    static void AppendLocationsInOrder(const Node* node,
                                       size_t prefix_text_length,
                                       size_t prefix_line_count,
                                       std::vector<NoteLineSequenceLocation>* out);
    static size_t PrefixTextLength(const Node* node, size_t line_count) noexcept;
    static NoteLineSequenceMutationResult MaterializeLines(
        const std::vector<NoteLineSequenceLineSpec>& specs,
        uint64_t first_id,
        std::vector<NoteLineSequenceLine>* out_lines,
        uint64_t* out_next_id,
        size_t* out_text_length);
    static std::unique_ptr<Node> BuildTree(const std::vector<NoteLineSequenceLine>& lines);

    std::unique_ptr<Node> root_;
    uint64_t next_line_id_ = 1;
};

} // namespace note
