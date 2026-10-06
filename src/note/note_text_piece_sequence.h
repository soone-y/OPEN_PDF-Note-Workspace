#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace note {

// Result values deliberately distinguish a rejected edit from an allocation
// failure.  Neither case changes the published sequence.
enum class NoteTextPieceSequenceMutationResult {
    Applied,
    InvalidRange,
    LengthOverflow,
    AllocationFailure,
};

// Persistent, UI-independent UTF-16 text sequence.  Existing backing strings
// are never edited: an Apply operation path-copies only the treap paths that
// surround the edited range and shares all unchanged pieces with snapshots.
class NoteTextPieceSequence {
private:
    struct Node;
    struct VersionIdentity;

public:
    class Snapshot {
    public:
        [[nodiscard]] bool valid() const noexcept { return initialized_; }
        [[nodiscard]] size_t text_length() const noexcept { return text_length_; }

    private:
        friend class NoteTextPieceSequence;

        std::shared_ptr<const Node> root_;
        // A root pointer distinguishes every non-empty state, but an empty
        // persistent tree has a null root.  Keep a published-version identity
        // as well so an empty snapshot cannot match an unrelated Reset.
        std::shared_ptr<const VersionIdentity> version_;
        size_t text_length_ = 0;
        bool initialized_ = false;
    };

    NoteTextPieceSequence() = default;
    NoteTextPieceSequence(const NoteTextPieceSequence&) = delete;
    NoteTextPieceSequence& operator=(const NoteTextPieceSequence&) = delete;
    NoteTextPieceSequence(NoteTextPieceSequence&&) noexcept = default;
    NoteTextPieceSequence& operator=(NoteTextPieceSequence&&) noexcept = default;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] size_t text_length() const noexcept { return text_length_; }

    [[nodiscard]] NoteTextPieceSequenceMutationResult Reset(std::wstring text);
    [[nodiscard]] NoteTextPieceSequenceMutationResult Apply(const TextEdit& edit);

    [[nodiscard]] Snapshot TakeSnapshot() const noexcept;
    // Exact published-root identity, including the distinct empty-root
    // version token. This is intentionally stronger than equal text and is
    // used only at immutable snapshot boundaries.
    [[nodiscard]] static bool SameSnapshotIdentity(const Snapshot& lhs,
                                                    const Snapshot& rhs) noexcept;
    // Reads only the requested immutable snapshot range. This does not
    // materialize the complete canonical sequence and is the sole source
    // text-reading primitive available to final derived snapshots.
    [[nodiscard]] static std::wstring CopyRange(const Snapshot& snapshot,
                                                Utf16CodeUnitOffset start,
                                                size_t length);
    // This is stronger than a hash or textual comparison: every code unit in
    // the two ranges must be backed by the identical immutable string region.
    // It accepts snapshots rather than a mutable sequence so test-only
    // derived components need not become friends of NoteTextCore.
    [[nodiscard]] static bool SnapshotsShareExactRange(
        const Snapshot& before,
        Utf16CodeUnitOffset before_start,
        const Snapshot& current,
        Utf16CodeUnitOffset current_start,
        size_t length) noexcept;
    [[nodiscard]] bool MatchesSnapshot(const Snapshot& snapshot) const noexcept;
    [[nodiscard]] std::wstring Materialize() const;
    [[nodiscard]] std::wstring CopyRange(Utf16CodeUnitOffset start, size_t length) const;
    [[nodiscard]] bool Equals(std::wstring_view text) const noexcept;
    [[nodiscard]] bool EqualsRange(Utf16CodeUnitOffset start,
                                   std::wstring_view text) const noexcept;

    // This is stronger than a hash or textual comparison: every code unit in
    // the two ranges must be backed by the identical immutable string region.
    // Legacy callers may use it to prove a suffix was not rewritten.
    // New derived code should use SnapshotsShareExactRange so it depends only
    // on immutable values.
    [[nodiscard]] bool SharesExactRange(const Snapshot& before,
                                        Utf16CodeUnitOffset before_start,
                                        Utf16CodeUnitOffset current_start,
                                        size_t length) const;

private:
    struct Piece;
    struct Fragment;
    using NodePtr = std::shared_ptr<const Node>;

    [[nodiscard]] static size_t SubtreeLength(const NodePtr& node) noexcept;
    [[nodiscard]] static uint64_t PriorityFor(uint64_t id) noexcept;
    [[nodiscard]] static NodePtr MakeNode(Piece piece,
                                          uint64_t priority,
                                          NodePtr left,
                                          NodePtr right);
    [[nodiscard]] static NodePtr CloneNode(const NodePtr& node,
                                           NodePtr left,
                                           NodePtr right);
    [[nodiscard]] static NodePtr Merge(const NodePtr& left, const NodePtr& right);
    [[nodiscard]] static std::pair<NodePtr, NodePtr> Split(
        const NodePtr& node, size_t left_length, uint64_t* next_node_id);
    [[nodiscard]] static bool IsValidRange(size_t text_length,
                                           Utf16CodeUnitOffset start,
                                           size_t length) noexcept;
    static void AppendRange(const NodePtr& node,
                            size_t prefix_length,
                            size_t range_start,
                            size_t range_end,
                            std::wstring* out);
    [[nodiscard]] static bool RangeEquals(const NodePtr& node,
                                          size_t prefix_length,
                                          size_t range_start,
                                          size_t range_end,
                                          std::wstring_view expected,
                                          size_t* expected_offset) noexcept;
    static void AppendFragments(const NodePtr& node,
                                size_t prefix_length,
                                size_t range_start,
                                size_t range_end,
                                std::vector<Fragment>* out);
    [[nodiscard]] static bool FragmentsShareExactly(const std::vector<Fragment>& before,
                                                    const std::vector<Fragment>& current,
                                                    size_t length) noexcept;
    [[nodiscard]] static uint64_t NextPriorityId(uint64_t* next_node_id);

    NodePtr root_;
    std::shared_ptr<const VersionIdentity> version_;
    size_t text_length_ = 0;
    uint64_t next_node_id_ = 1;
    bool initialized_ = false;
};

} // namespace note
