#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace note {

// A logical source row stores only local lengths. Absolute UTF-16 positions
// are derived from immutable subtree aggregates, so an edit never rewrites
// offsets in the unchanged tail.
struct NoteSourceLineSpec {
    size_t content_length = 0;
    size_t line_break_length = 0;
};

struct NoteSourceLineLocation {
    LineIndex line_index{};
    NoteSourceLineSpec line{};
    Utf16CodeUnitOffset start{};
    Utf16CodeUnitOffset content_end{};
    Utf16CodeUnitOffset next_start{};
};

// Persistent, canonical source-line index.  It is deliberately separate from
// the view's legacy line/cache data: this type owns no rendered payload or
// stable presentation identifier.  A snapshot is a complete immutable source
// coordinate system for one canonical text root.
class NoteSourceLineMap {
private:
    struct Node;
    struct VersionIdentity;

public:
    class Snapshot {
    public:
        [[nodiscard]] bool valid() const noexcept { return initialized_; }
        [[nodiscard]] size_t line_count() const noexcept { return line_count_; }
        [[nodiscard]] size_t text_length() const noexcept { return text_length_; }
        // RichEdit presents each logical line break as one UTF-16 position,
        // including a CRLF pair.  Keep that transport length next to the
        // canonical source length so view interaction never needs to
        // materialize the complete text merely to convert a caret position.
        [[nodiscard]] size_t rich_edit_text_length() const noexcept {
            return rich_edit_text_length_;
        }

    private:
        friend class NoteSourceLineMap;

        std::shared_ptr<const Node> root_;
        std::shared_ptr<const VersionIdentity> version_;
        size_t line_count_ = 0;
        size_t text_length_ = 0;
        size_t rich_edit_text_length_ = 0;
        uint64_t next_node_id_ = 1;
        bool initialized_ = false;
    };

    // Builds the complete index. This is used at an explicit full source
    // publication only; ordinary edits use Replace on the current snapshot.
    [[nodiscard]] static bool Build(std::wstring_view text, Snapshot* out) noexcept;

    // Replaces complete logical rows [first, last_exclusive).  The caller
    // supplies the post-edit row specifications.  On failure, `out` and the
    // published input snapshot are left unchanged.
    [[nodiscard]] static bool Replace(const Snapshot& before,
                                      LineIndex first,
                                      LineIndex last_exclusive,
                                      const std::vector<NoteSourceLineSpec>& replacement,
                                      Snapshot* out) noexcept;

    [[nodiscard]] static std::optional<NoteSourceLineLocation> LineAt(
        const Snapshot& snapshot,
        LineIndex index) noexcept;
    [[nodiscard]] static std::optional<NoteSourceLineLocation> FindByOffset(
        const Snapshot& snapshot,
        Utf16CodeUnitOffset offset) noexcept;

    // Convert between the canonical UTF-16 source coordinate and RichEdit's
    // CRLF-as-one transport coordinate in O(logical-line-count) time.  The
    // source coordinate immediately between CR and LF is preserved as a
    // canonical position and maps to the transport boundary after that line
    // break, matching RichEdit's existing conversion behavior.
    [[nodiscard]] static std::optional<size_t> CanonicalOffsetToRichEditIndex(
        const Snapshot& snapshot,
        Utf16CodeUnitOffset offset) noexcept;
    [[nodiscard]] static std::optional<Utf16CodeUnitOffset> RichEditIndexToCanonicalOffset(
        const Snapshot& snapshot,
        size_t richEditIndex) noexcept;

    // Coordinate snapshots participate in the same publication identity as
    // the canonical text root. Equal line lengths from an independently
    // rebuilt index are not an interchangeable final-snapshot dependency.
    [[nodiscard]] static bool SameSnapshotIdentity(
        const Snapshot& lhs,
        const Snapshot& rhs) noexcept;

    // Parses one contiguous source range into whole logical rows. When the
    // range ends at the start of a retained following row, its terminal line
    // break is a boundary, not a new EOF row; `include_terminal_empty_row`
    // makes that distinction explicit.
    [[nodiscard]] static bool BuildLineSpecs(
        std::wstring_view text,
        bool include_terminal_empty_row,
        std::vector<NoteSourceLineSpec>* out) noexcept;

private:
    using NodePtr = std::shared_ptr<const Node>;

    [[nodiscard]] static size_t SubtreeLineCount(const NodePtr& node) noexcept;
    [[nodiscard]] static size_t SubtreeTextLength(const NodePtr& node) noexcept;
    [[nodiscard]] static size_t SubtreeRichEditTextLength(const NodePtr& node) noexcept;
    [[nodiscard]] static uint64_t PriorityFor(uint64_t id) noexcept;
    [[nodiscard]] static bool TakeNodeId(uint64_t* next_node_id,
                                         uint64_t* out_id) noexcept;
    [[nodiscard]] static NodePtr MakeNode(NoteSourceLineSpec line,
                                          uint64_t priority,
                                          NodePtr left,
                                          NodePtr right);
    [[nodiscard]] static NodePtr CloneNode(const NodePtr& node,
                                           NodePtr left,
                                           NodePtr right);
    [[nodiscard]] static NodePtr Merge(const NodePtr& left, const NodePtr& right);
    static void Split(const NodePtr& node,
                      size_t left_line_count,
                      NodePtr* out_left,
                      NodePtr* out_right);
    [[nodiscard]] static bool BuildTree(const std::vector<NoteSourceLineSpec>& lines,
                                        uint64_t* next_node_id,
                                        NodePtr* out_root) noexcept;
};

} // namespace note
