#pragma once

#include "note/note_identity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace note {



struct Utf8ByteOffset {
    size_t value;
    bool operator==(const Utf8ByteOffset& o) const { return value == o.value; }
    bool operator!=(const Utf8ByteOffset& o) const { return value != o.value; }
    bool operator<(const Utf8ByteOffset& o) const { return value < o.value; }
    bool operator<=(const Utf8ByteOffset& o) const { return value <= o.value; }
    bool operator>(const Utf8ByteOffset& o) const { return value > o.value; }
    bool operator>=(const Utf8ByteOffset& o) const { return value >= o.value; }
    Utf8ByteOffset operator+(size_t offset) const { return {value + offset}; }
    Utf8ByteOffset operator-(size_t offset) const { return {value - offset}; }
    Utf8ByteOffset& operator+=(size_t offset) { value += offset; return *this; }
    Utf8ByteOffset& operator-=(size_t offset) { value -= offset; return *this; }
    size_t operator-(const Utf8ByteOffset& o) const { return value - o.value; }
};

struct Utf16CodeUnitOffset {
    size_t value;
    bool operator==(const Utf16CodeUnitOffset& o) const { return value == o.value; }
    bool operator!=(const Utf16CodeUnitOffset& o) const { return value != o.value; }
    bool operator<(const Utf16CodeUnitOffset& o) const { return value < o.value; }
    bool operator<=(const Utf16CodeUnitOffset& o) const { return value <= o.value; }
    bool operator>(const Utf16CodeUnitOffset& o) const { return value > o.value; }
    bool operator>=(const Utf16CodeUnitOffset& o) const { return value >= o.value; }
    Utf16CodeUnitOffset operator+(size_t offset) const { return {value + offset}; }
    Utf16CodeUnitOffset operator-(size_t offset) const { return {value - offset}; }
    Utf16CodeUnitOffset& operator+=(size_t offset) { value += offset; return *this; }
    Utf16CodeUnitOffset& operator-=(size_t offset) { value -= offset; return *this; }
    size_t operator-(const Utf16CodeUnitOffset& o) const { return value - o.value; }
};

struct LineIndex {
    size_t value;
    bool operator==(const LineIndex& o) const { return value == o.value; }
    bool operator!=(const LineIndex& o) const { return value != o.value; }
    bool operator<(const LineIndex& o) const { return value < o.value; }
    bool operator<=(const LineIndex& o) const { return value <= o.value; }
    bool operator>(const LineIndex& o) const { return value > o.value; }
    bool operator>=(const LineIndex& o) const { return value >= o.value; }
};

struct Span {
    Utf16CodeUnitOffset start = {0};
    Utf16CodeUnitOffset end = {0};
};

struct LineColumn {
    int line = 1;
    int column = 1;
};

struct TextEdit {
    Utf16CodeUnitOffset start = {0};
    size_t deleted_len = 0;
    std::wstring inserted_text;
};

struct NoteMetadata {
    std::wstring file_name;
    std::wstring title;
};

struct NoteTextModel {
    NoteMetadata meta;
    std::wstring raw;
    uint64_t revision = 0;
    std::vector<size_t> line_starts;
};

enum class BlockKind {
    Paragraph,
    Heading,
    List,
    ListItem,
    Quote,
    CodeBlock,
    HorizontalRule,
    Table,
    TableHead,
    TableBody,
    TableRow,
    TableHeaderCell,
    TableCell,
    Blank,
};

enum class BlockOrigin {
    Markdown,
    LegacyHeadingTag,
};

enum class InlineKind {
    Text,
    Code,
    Emphasis,
    Strong,
    Strike,
    Link,
    Image,
};

enum class TableCellAlign {
    Default,
    Left,
    Center,
    Right,
};

enum class StyleKind {
    Underline,
    TextColor,
    BackgroundColor,
    FontFamily,
    FontSize,
    LineHeight,
    Indent,
    Anchor,
    LinkId,
    LinkAccent,
    LinkUnderline,
    Bold,
    Italic,
    Strike,
};

enum class MathKind {
    Inline,
    Block,
};

enum class MathDelimiter {
    Dollar,
    DoubleDollar,
    BackslashParen,
    BackslashBracket,
    LegacyMathTag,
};

enum class DiagnosticSeverity {
    Warning,
    Error,
};

struct BlockNode {
    BlockKind kind = BlockKind::Paragraph;
    BlockOrigin origin = BlockOrigin::Markdown;
    Span span{};
    LineColumn loc{};
    int level = 0;
    size_t first_inline = 0;
    size_t inline_count = 0;
    size_t parent = static_cast<size_t>(-1);
    bool ordered = false;
    int start_number = 1;
    bool task_item = false;
    bool task_checked = false;
    int table_column_count = 0;
    TableCellAlign table_cell_align = TableCellAlign::Default;
    std::wstring info_string;
};

struct InlineNode {
    InlineKind kind = InlineKind::Text;
    Span span{};
    size_t parent_block = static_cast<size_t>(-1);
    std::wstring target;
};

struct StyleSpan {
    StyleKind kind = StyleKind::TextColor;
    Span span{};
    std::wstring value;
};

struct MathSpan {
    MathKind kind = MathKind::Inline;
    MathDelimiter delimiter = MathDelimiter::Dollar;
    Span span{};
    Span content_span{};
    std::wstring normalized_tex;
    std::vector<size_t> diagnostic_ids;
};

struct Diagnostic {
    std::wstring code;
    std::wstring message;
    Span span{};
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
};

struct NoteDocument {
    NoteMetadata meta;
    NoteDerivedSnapshotIdentity source_identity{};
    std::vector<BlockNode> blocks;
    std::vector<InlineNode> inlines;
    std::vector<StyleSpan> style_spans;
    std::vector<MathSpan> math_spans;
    std::vector<Diagnostic> diagnostics;
};

struct LayoutRun {
    Span source_span{};
    size_t block_id = static_cast<size_t>(-1);
    size_t inline_id = static_cast<size_t>(-1);
    uint32_t style_mask = 0;
    bool is_math = false;
};

struct VisualLine {
    std::vector<LayoutRun> runs;
    int height_px = 0;
};

struct NoteLayout {
    NoteDerivedSnapshotIdentity source_identity{};
    std::vector<VisualLine> lines;
};

std::wstring DeriveTitleFromFileName(std::wstring_view fileName);
[[nodiscard]] std::vector<size_t> BuildLineStarts(std::wstring_view raw);
[[nodiscard]] NoteTextModel MakeNoteTextModel(NoteMetadata meta, std::wstring raw, uint64_t revision = 0);
[[nodiscard]] std::optional<TextEdit> ComputeNoteTextEdit(std::wstring_view before,
                                            std::wstring_view after);
void ApplyTextEdit(NoteTextModel* model, const TextEdit& edit);
[[nodiscard]] LineColumn ResolveLineColumn(const NoteTextModel& model, size_t offset);
void SetNoteDocumentSourceRevision(NoteDocument* doc, uint64_t sourceRevision);
void SetNoteDocumentSourceIdentity(NoteDocument* doc,
                                   NoteDerivedSnapshotIdentity sourceIdentity);
bool NoteDocumentMatchesSourceRevision(const NoteDocument& doc, uint64_t sourceRevision);
bool NoteDocumentMatchesSourceIdentity(
    const NoteDocument& doc,
    NoteDerivedSnapshotIdentity sourceIdentity);
bool NoteDocumentMatchesTextModel(const NoteDocument& doc, const NoteTextModel& model);
void SetNoteLayoutSourceRevision(NoteLayout* layout, uint64_t sourceRevision);
void SetNoteLayoutSourceIdentity(NoteLayout* layout,
                                 NoteDerivedSnapshotIdentity sourceIdentity);
bool NoteLayoutMatchesSourceRevision(const NoteLayout& layout, uint64_t sourceRevision);
bool NoteLayoutMatchesSourceIdentity(
    const NoteLayout& layout,
    NoteDerivedSnapshotIdentity sourceIdentity);
bool NoteLayoutMatchesTextModel(const NoteLayout& layout, const NoteTextModel& model);

} // namespace note
