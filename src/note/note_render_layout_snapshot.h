#pragma once

#include "note/note_syntax_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace note {

// Every value that can change text placement belongs to the key. A layout is
// never read under a merely similar width, DPI, font metric set, wrapping
// policy, or tab policy.
struct NoteRenderLayoutKey {
    uint32_t client_width_px = 0;
    uint32_t dpi_x = 0;
    uint32_t dpi_y = 0;
    uint64_t font_metrics_revision = 0;
    uint32_t tab_columns = 4;
    uint32_t horizontal_padding_px = 0;
    bool word_wrap = false;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool operator==(const NoteRenderLayoutKey& lhs,
                               const NoteRenderLayoutKey& rhs) noexcept;
[[nodiscard]] bool operator!=(const NoteRenderLayoutKey& lhs,
                               const NoteRenderLayoutKey& rhs) noexcept;

// One logical source row's measured geometry. The data is local by design:
// y coordinates and document-wide horizontal extent are tree aggregates, not
// mutable offsets stored in every following row.
struct NoteRenderLineLayout {
    uint32_t height_px = 0;
    uint32_t inline_extent_px = 0;
    // A multi-source-row atomic graphic (currently block math) owns this
    // source row's visual height from its first row.  The collapsed row
    // remains in the persistent source-line order for source coordinates,
    // but contributes neither Y nor horizontal extent.  A placement snapshot
    // must prove the matching atomic-group contract before it can publish
    // this form; ordinary empty rows still have a nonzero line height.
    bool collapsed_into_atomic_group = false;
};

struct NoteRenderLineLayoutLocation {
    LineIndex line_index{};
    NoteRenderLineLayout layout{};
    uint64_t top_px = 0;
    uint64_t bottom_px = 0;
};

// Persistent line geometry for one layout key. Replace path-copies only the
// affected row boundaries. Unchanged tail rows retain their immutable layout
// payload and obtain their shifted Y coordinates from aggregate sums.
class NoteRenderLineLayoutMap {
private:
    struct Node;
    struct VersionIdentity;

public:
    class Snapshot {
    public:
        [[nodiscard]] bool valid() const noexcept { return initialized_; }
        [[nodiscard]] size_t line_count() const noexcept { return line_count_; }
        [[nodiscard]] uint64_t total_height_px() const noexcept { return total_height_px_; }
        [[nodiscard]] uint32_t max_inline_extent_px() const noexcept {
            return max_inline_extent_px_;
        }

    private:
        friend class NoteRenderLineLayoutMap;

        std::shared_ptr<const Node> root_;
        std::shared_ptr<const VersionIdentity> version_;
        size_t line_count_ = 0;
        uint64_t total_height_px_ = 0;
        uint32_t max_inline_extent_px_ = 0;
        uint64_t next_node_id_ = 1;
        bool initialized_ = false;
    };

    [[nodiscard]] static bool Build(const std::vector<NoteRenderLineLayout>& lines,
                                    Snapshot* out) noexcept;
    [[nodiscard]] static bool Replace(const Snapshot& before,
                                      LineIndex first,
                                      LineIndex last_exclusive,
                                      const std::vector<NoteRenderLineLayout>& replacement,
                                      Snapshot* out) noexcept;
    [[nodiscard]] static std::optional<NoteRenderLineLayoutLocation> LineAt(
        const Snapshot& snapshot,
        LineIndex index) noexcept;
    // Finds the one half-open vertical band [top_px, bottom_px) containing
    // `content_y_px`. This descends the persistent aggregate tree and never
    // scans preceding rows, so scrolling and hit testing stay logarithmic
    // after a local height splice shifts the unchanged suffix.
    [[nodiscard]] static std::optional<NoteRenderLineLayoutLocation> LineContainingY(
        const Snapshot& snapshot,
        uint64_t content_y_px) noexcept;

private:
    using NodePtr = std::shared_ptr<const Node>;

    [[nodiscard]] static size_t SubtreeLineCount(const NodePtr& node) noexcept;
    [[nodiscard]] static uint64_t SubtreeHeight(const NodePtr& node) noexcept;
    [[nodiscard]] static uint32_t SubtreeMaxInlineExtent(const NodePtr& node) noexcept;
    [[nodiscard]] static uint64_t PriorityFor(uint64_t id) noexcept;
    [[nodiscard]] static bool TakeNodeId(uint64_t* next_node_id,
                                         uint64_t* out_id) noexcept;
    [[nodiscard]] static bool IsValidLine(const NoteRenderLineLayout& line) noexcept;
    [[nodiscard]] static bool BuildTree(const std::vector<NoteRenderLineLayout>& lines,
                                        uint64_t* next_node_id,
                                        NodePtr* out_root) noexcept;
    [[nodiscard]] static NodePtr MakeNode(NoteRenderLineLayout line,
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
};

enum class NoteRenderLayoutSnapshotBuildResult {
    Built,
    InvalidOutput,
    InvalidSyntaxSnapshot,
    InvalidLayoutKey,
    LineCountMismatch,
    AllocationFailure,
};

enum class NoteRenderLayoutSnapshotLocalSpliceResult {
    Built,
    InvalidOutput,
    InvalidPreviousSnapshot,
    InvalidSyntaxSnapshot,
    LayoutKeyMismatch,
    InvalidRange,
    ReplacementRejected,
    AllocationFailure,
};

// This is the layout layer of the final render snapshot. It binds the exact
// syntax source root to one exact layout key and its geometry in a single
// immutable publication. Render placements and presentation ownership are
// added by the next layer; legacy view caches never contribute data here.
class NoteRenderLayoutSnapshot final {
public:
    [[nodiscard]] static NoteRenderLayoutSnapshotBuildResult Build(
        std::shared_ptr<const NoteSyntaxSnapshot> syntax,
        NoteRenderLayoutKey layoutKey,
        NoteRenderLineLayoutMap::Snapshot lineLayouts,
        std::shared_ptr<const NoteRenderLayoutSnapshot>* out) noexcept;

    // Replaces only measured rows whose source plan was locally proven. The
    // map stores aggregate heights, so the unchanged tail has no rewritten
    // absolute Y coordinates; its new Y is derived when queried.
    [[nodiscard]] static NoteRenderLayoutSnapshotLocalSpliceResult BuildLocalSplice(
        std::shared_ptr<const NoteRenderLayoutSnapshot> previous,
        std::shared_ptr<const NoteSyntaxSnapshot> syntax,
        NoteRenderLayoutKey layoutKey,
        LineIndex first,
        LineIndex last_exclusive,
        const std::vector<NoteRenderLineLayout>& replacement,
        std::shared_ptr<const NoteRenderLayoutSnapshot>* out) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::shared_ptr<const NoteSyntaxSnapshot>& syntax() const noexcept {
        return syntax_;
    }
    [[nodiscard]] const NoteRenderLayoutKey& layout_key() const noexcept {
        return layout_key_;
    }
    [[nodiscard]] const NoteRenderLineLayoutMap::Snapshot& line_layouts() const noexcept {
        return line_layouts_;
    }
    [[nodiscard]] NoteDerivedSnapshotIdentity source_identity() const noexcept;
    [[nodiscard]] bool Matches(const NoteTextCore& textCore,
                               const NoteRenderLayoutKey& layoutKey) const noexcept;

private:
    std::shared_ptr<const NoteSyntaxSnapshot> syntax_;
    NoteRenderLayoutKey layout_key_{};
    NoteRenderLineLayoutMap::Snapshot line_layouts_{};
    bool valid_ = false;
};

} // namespace note
