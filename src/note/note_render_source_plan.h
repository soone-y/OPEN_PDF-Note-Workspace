#pragma once

#include "note/note_syntax_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace note {

class NoteRenderSourcePlanStorage;
class NoteRenderSourceAtomicGroupStorage;

// A source run is immutable semantic input for placement. It deliberately
// carries no GDI object, measured X coordinate, legacy row ID, or view cache.
// The later layout layer turns these source/display ranges into geometry.
enum class NoteRenderSourceRunKind : uint8_t {
    Text,
    InlineCode,
    LinkText,
    ImageAltText,
    InlineMath,
    BlockMath,
    // A source-only Markdown token. It has exactly two zero-width display
    // boundaries, so a caret and selection can address syntax which is not
    // painted in structured mode without a second source/display mapper.
    HiddenSyntax,
};

// Line-level structural presentation belongs to the immutable source plan,
// rather than to a GDI or legacy line-cache heuristic. The final measurement
// provider uses these facts to reserve/draw list and quote decoration, code
// surfaces, rules, and table divider geometry. `runs` still own every source
// code unit (visible or zero-width) for caret and selection mapping.
enum NoteRenderSourceLineDecorationFlag : uint32_t {
    NoteRenderSourceLineDecorationNone = 0,
    NoteRenderSourceLineDecorationHeading = 1u << 0,
    NoteRenderSourceLineDecorationListItem = 1u << 1,
    NoteRenderSourceLineDecorationOrderedListItem = 1u << 2,
    NoteRenderSourceLineDecorationTaskItem = 1u << 3,
    NoteRenderSourceLineDecorationQuote = 1u << 4,
    NoteRenderSourceLineDecorationCodeBlock = 1u << 5,
    NoteRenderSourceLineDecorationCodeFence = 1u << 6,
    NoteRenderSourceLineDecorationHorizontalRule = 1u << 7,
    NoteRenderSourceLineDecorationTable = 1u << 8,
    NoteRenderSourceLineDecorationTableHeader = 1u << 9,
    NoteRenderSourceLineDecorationTableDivider = 1u << 10,
    NoteRenderSourceLineDecorationContainer = 1u << 11,
    NoteRenderSourceLineDecorationContainerOpening = 1u << 12,
    NoteRenderSourceLineDecorationContainerClosing = 1u << 13,
};

struct NoteRenderSourceLineDecoration {
    uint32_t flags = NoteRenderSourceLineDecorationNone;
    int heading_level = 0;
    uint32_t list_depth = 0;
    int ordered_list_number = 0;
    bool task_checked = false;
    uint32_t container_depth = 0;

    [[nodiscard]] bool has(NoteRenderSourceLineDecorationFlag flag) const noexcept {
        return (flags & static_cast<uint32_t>(flag)) != 0;
    }
};

struct NoteRenderSourceStyleAttribute {
    StyleKind kind = StyleKind::TextColor;
    std::wstring value;
};

struct NoteRenderSourceRun {
    static constexpr size_t kNoTableBlock = static_cast<size_t>(-1);
    static constexpr size_t kNoTableColumn = static_cast<size_t>(-1);

    // `source_span` owns every source code unit consumed by this visible run.
    // Math includes its delimiters here, while `display_source_span` points at
    // its TeX body. This makes hidden delimiters explicit rather than asking
    // caret/hit-test code to infer them from painted text.
    Span source_span{};
    Span display_source_span{};
    LineIndex line_index{};
    size_t parent_block = static_cast<size_t>(-1);
    int heading_level = 0;
    NoteRenderSourceRunKind kind = NoteRenderSourceRunKind::Text;
    std::wstring link_target;
    std::vector<NoteRenderSourceStyleAttribute> styles;
    bool decodes_markdown_escapes = false;
    // Table placement is not reconstructed from a view cache.  Text runs
    // carry their parser-derived cell identity so measurement, painting,
    // selection, caret and hit testing can share one table grid. Values stay
    // absent for non-table runs.
    size_t table_block = kNoTableBlock;
    size_t table_row_block = kNoTableBlock;
    size_t table_column = kNoTableColumn;
    size_t table_column_count = 0;
    TableCellAlign table_cell_align = TableCellAlign::Default;
    bool table_header = false;
};

enum class NoteRenderAtomicGroupKind : uint8_t {
    Table,
    BlockMath,
    CodeBlock,
    Container,
};

struct NoteRenderAtomicGroup {
    NoteRenderAtomicGroupKind kind = NoteRenderAtomicGroupKind::Container;
    Span source_span{};
    LineIndex first_line{};
    LineIndex last_line{};
};

struct NoteRenderSourceLinePlan {
    Span content_span{};
    NoteRenderSourceLineDecoration decoration{};
    std::vector<NoteRenderSourceRun> runs;
};

enum class NoteRenderSourcePlanBuildResult {
    Built,
    InvalidOutput,
    InvalidSyntaxSnapshot,
    RawOnlyContent,
    InconsistentSyntax,
    AllocationFailure,
};

enum class NoteRenderSourcePlanLocalPatchResult {
    Built,
    InvalidOutput,
    InvalidPreviousPlan,
    InvalidSyntaxSnapshot,
    InvalidEdit,
    RequiresFullPlan,
    InconsistentCandidate,
    AllocationFailure,
};

// Test-only D-3e accounting.  A successful marker-free same-row splice must
// replace only its changed row payload; all unchanged prefix/suffix payloads
// remain in the persistent source-plan tree.
struct NoteRenderSourcePlanLocalPatchWork {
    size_t replacement_line_payloads = 0;
    size_t tail_line_payload_rewrites = 0;
};

// Test-only observation for the immutable atomic-group interval index. It
// counts index nodes, not copied groups or source rows; a visible-range query
// must scale with the intersecting groups and tree depth, never all groups.
struct NoteRenderAtomicGroupQueryWork {
    size_t visited_index_nodes = 0;
};

// Complete parser-derived source/display plan for one syntax snapshot. It is
// still independent from measurement and presentation ownership, so it can be
// differential-tested without Win32 or a view. The final render snapshot owns
// this plan together with one NoteRenderLayoutSnapshot and one owner plan.
class NoteRenderSourcePlan final {
public:
    [[nodiscard]] static NoteRenderSourcePlanBuildResult Build(
        std::shared_ptr<const NoteSyntaxSnapshot> syntax,
        std::shared_ptr<const NoteRenderSourcePlan>* out) noexcept;

    // Local derived splice for a syntax snapshot that has already passed the
    // checkpoint fixed-point proof. It never invents runs: a syntax-sensitive
    // or line-structural edit must rebuild the complete source plan.
    [[nodiscard]] static NoteRenderSourcePlanLocalPatchResult BuildLocalPlainTextPatch(
        std::shared_ptr<const NoteRenderSourcePlan> previous,
        std::shared_ptr<const NoteSyntaxSnapshot> syntax,
        const NoteTextCore& currentTextCore,
        const TextEdit& edit,
        std::shared_ptr<const NoteRenderSourcePlan>* out,
        NoteRenderSourcePlanLocalPatchWork* outWork = nullptr) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::shared_ptr<const NoteSyntaxSnapshot>& syntax() const noexcept {
        return syntax_;
    }
    [[nodiscard]] size_t line_count() const noexcept;
    // Resolves one immutable line payload against this snapshot's canonical
    // row map. Stored source coordinates are row-relative, so an unchanged
    // tail never needs absolute-offset rewrites after a local edit.
    [[nodiscard]] bool ResolveLine(LineIndex index,
                                   NoteRenderSourceLinePlan* out) const noexcept;
    // Differential tests may materialize all rows for an oracle comparison.
    // Runtime paint/input paths must resolve only their requested rows.
    [[nodiscard]] bool CopyLinesForDifferentialTest(
        std::vector<NoteRenderSourceLinePlan>* out) const noexcept;
    [[nodiscard]] bool SharesLinePayloadForDifferentialTest(
        const NoteRenderSourcePlan& other,
        LineIndex index) const noexcept;
    // Placement may reuse its unchanged row payloads only when this exact
    // source plan was locally spliced from `previous` over the same range.
    [[nodiscard]] bool ProvesUnchangedRowsFrom(
        const NoteRenderSourcePlan& previous,
        LineIndex first,
        LineIndex last_exclusive) const noexcept;
    [[nodiscard]] bool CopyAtomicGroups(
        std::vector<NoteRenderAtomicGroup>* out) const noexcept;
    [[nodiscard]] bool CopyAtomicGroupsIntersecting(
        LineIndex first,
        LineIndex last_exclusive,
        std::vector<NoteRenderAtomicGroup>* out,
        NoteRenderAtomicGroupQueryWork* outWork = nullptr) const noexcept;
    [[nodiscard]] NoteDerivedSnapshotIdentity source_identity() const noexcept;
    [[nodiscard]] bool Matches(const NoteTextCore& textCore) const noexcept;

private:
    std::shared_ptr<const NoteSyntaxSnapshot> syntax_;
    std::shared_ptr<const NoteRenderSourcePlanStorage> line_storage_;
    std::shared_ptr<const NoteRenderSourceAtomicGroupStorage> atomic_group_storage_;
    std::weak_ptr<const NoteRenderSourcePlan> local_reuse_previous_;
    LineIndex local_reuse_first_{};
    LineIndex local_reuse_last_exclusive_{};
    bool valid_ = false;
};

} // namespace note
