#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "note/note_export.h"
#include "note/note_dirty_graph.h"
#include "note/note_influence.h"
#include "note/note_identity.h"
#include "note/note_identity_store.h"
#include "note/note_layout.h"
#include "note/note_kernel.h"
#include "note/note_line_sequence.h"
#include "note/note_parser_checkpoint.h"
#include "note/note_source_line_map.h"
#include "note/note_source_coordinate_transform.h"
#include "note/note_render_layout_snapshot.h"
#include "note/note_render_placement_snapshot.h"
#include "note/note_render_final_publication.h"
#include "note/note_render_final_transaction.h"
#include "note/note_render_final_interaction.h"
#include "note/note_render_final_display_run.h"
#include "note/note_render_final_gdi_measurement_provider.h"
#include "note/note_render_final_gdi_painter.h"
#include "note/note_render_final_ime_preedit_presentation.h"
#include "note/note_render_final_presentation_interaction.h"
#include "note/note_render_final_presentation_snapshot.h"
#include "note/note_render_final_win32_adapter.h"
#include "note/note_render_source_plan.h"
#include "note/note_presentation_owner_plan.h"
#include "note/note_syntax_snapshot.h"
#include "note/note_syntax_document.h"
#include "note/note_syntax_lexical.h"
#include "note/note_text_piece_sequence.h"
#include "note/note_math.h"
#include "note/note_parser.h"
#include "note/note_persistence.h"
#include "note/note_render_index_request.h"
#include "note/note_revision_gate.h"
#include "note/note_semantic_index.h"
#include "note/note_transaction.h"
#include "note/note_text_core.h"
#include "note/note_text_boundaries.h"
#include "note/note_workspace_index.h"
#include "note/note_workspace_service.h"
#include "file_output/note_snapshot.h"
#include "math/math_render.h"
#include "app/main_close_policy.h"
#include "core/setup_json_policy.h"
#include "core/cache_dir_policy.h"
#include "core/sha256.h"

std::wstring UTF8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string TestWideToUTF8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        out.data(), length, nullptr, nullptr);
    return out;
}

namespace note {

} // namespace note

namespace {

int g_failed = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return;
    }
    std::cout << "[FAIL] " << message << "\n";
    ++g_failed;
}

class DeterministicFinalMeasurementProvider final
    : public note::NoteRenderFinalMeasurementProvider {
public:
    [[nodiscard]] bool Measure(
        const note::NoteRenderSourcePlan& sourcePlan,
        const note::NoteRenderLayoutKey& layoutKey,
        note::NoteRenderFinalMeasuredFrame* out) const noexcept override {
        if (!out || !sourcePlan.valid() || !layoutKey.valid()) return false;
        try {
            note::NoteRenderFinalMeasuredFrame candidate;
            candidate.line_layouts.reserve(sourcePlan.line_count());
            candidate.line_placements.resize(sourcePlan.line_count());
            for (size_t line = 0; line < sourcePlan.line_count(); ++line) {
                note::NoteRenderSourceLinePlan sourceLine;
                if (!sourcePlan.ResolveLine({line}, &sourceLine)) return false;
                candidate.line_layouts.push_back(
                    {16, static_cast<uint32_t>(80 + std::min<size_t>(line, 1000))});
                int x = 12;
                for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                    const size_t sourceLength = run.source_span.end.value - run.source_span.start.value;
                    if (sourceLength > static_cast<size_t>(std::numeric_limits<int>::max() - 20)) {
                        return false;
                    }
                    const int width = 20 + static_cast<int>(sourceLength);
                    if (x > std::numeric_limits<int>::max() - width - 2) return false;
                    candidate.line_placements[line].runs.push_back(note::NoteRenderRunPlacement{
                        run.source_span, x, width,
                        {{run.source_span.start, x}, {run.source_span.end, x + width}},
                    });
                    x += width + 2;
                }
            }
            std::vector<note::NoteRenderAtomicGroup> sourceGroups;
            if (!sourcePlan.CopyAtomicGroups(&sourceGroups)) return false;
            for (const note::NoteRenderAtomicGroup& group : sourceGroups) {
                if (group.kind != note::NoteRenderAtomicGroupKind::Table &&
                    group.kind != note::NoteRenderAtomicGroupKind::BlockMath) {
                    continue;
                }
                if (group.first_line > group.last_line ||
                    group.last_line.value >= candidate.line_layouts.size()) {
                    return false;
                }
                const size_t sourceRows = group.last_line.value - group.first_line.value + 1;
                if (sourceRows > std::numeric_limits<uint32_t>::max() / 16u) return false;
                const uint32_t groupHeight = static_cast<uint32_t>(sourceRows) * 16u;
                if (group.kind == note::NoteRenderAtomicGroupKind::BlockMath) {
                    candidate.line_layouts[group.first_line.value] = {groupHeight, 80, false};
                    for (size_t line = group.first_line.value + 1; line <= group.last_line.value;
                         ++line) {
                        candidate.line_layouts[line] = {0, 0, true};
                        candidate.line_placements[line] = {};
                        if (line == std::numeric_limits<size_t>::max()) return false;
                    }
                }
                note::NoteRenderAtomicGroupPlacement groupPlacement{
                    group.kind, group.source_span, group.first_line, group.last_line,
                    12, 80, groupHeight};
                if (group.kind == note::NoteRenderAtomicGroupKind::Table) {
                    size_t columnCount = 0;
                    note::NoteTableLayoutInput tableInput;
                    for (size_t line = group.first_line.value; line <= group.last_line.value;
                         ++line) {
                        note::NoteRenderSourceLinePlan sourceLine;
                        if (!sourcePlan.ResolveLine({line}, &sourceLine)) return false;
                        for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                            if (run.table_column == note::NoteRenderSourceRun::kNoTableColumn) {
                                continue;
                            }
                            columnCount = std::max(columnCount, run.table_column_count);
                            tableInput.cell_measures.push_back({run.table_column, 20});
                        }
                        if (line == std::numeric_limits<size_t>::max()) return false;
                    }
                    tableInput.column_count = columnCount;
                    tableInput.minimum_content_width_px = 8;
                    tableInput.cell_horizontal_padding_px = 2;
                    tableInput.border_width_px = 1;
                    const note::NoteTableLayout tableLayout =
                        note::ResolveNoteTableLayout(tableInput);
                    if (!tableLayout.valid || tableLayout.columns.size() != columnCount ||
                        tableLayout.total_width_px <= 0) {
                        return false;
                    }
                    groupPlacement.width_px = tableLayout.total_width_px;
                    uint32_t top = 0;
                    for (size_t line = group.first_line.value; line <= group.last_line.value;
                         ++line) {
                        note::NoteRenderSourceLinePlan sourceLine;
                        if (!sourcePlan.ResolveLine({line}, &sourceLine) ||
                            top > std::numeric_limits<uint32_t>::max() - 16u) {
                            return false;
                        }
                        groupPlacement.table_rows.push_back(
                            {{line}, top, 16,
                             sourceLine.decoration.has(
                                 note::NoteRenderSourceLineDecorationTableHeader),
                             sourceLine.decoration.has(
                                 note::NoteRenderSourceLineDecorationTableDivider)});
                        top += 16;
                        if (line == std::numeric_limits<size_t>::max()) return false;
                    }
                    for (const note::NoteTableColumnLayout& column : tableLayout.columns) {
                        groupPlacement.table_columns.push_back(
                            {column.left_border_x_px, column.content_x_px,
                             column.content_width_px, column.right_border_x_px});
                    }
                }
                candidate.atomic_group_placements.push_back(std::move(groupPlacement));
            }
            *out = std::move(candidate);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool MeasureReplacement(
        const note::NoteRenderSourcePlan& sourcePlan,
        const note::NoteRenderLayoutKey& layoutKey,
        note::LineIndex firstLine,
        note::LineIndex lastLineExclusive,
        note::NoteRenderFinalMeasuredLineRange* out) const noexcept override {
        if (!out || !sourcePlan.valid() || !layoutKey.valid() ||
            firstLine.value >= lastLineExclusive.value ||
            lastLineExclusive.value > sourcePlan.line_count()) {
            return false;
        }
        try {
            note::NoteRenderFinalMeasuredLineRange candidate;
            const size_t lineCount = lastLineExclusive.value - firstLine.value;
            candidate.line_layouts.reserve(lineCount);
            candidate.line_placements.resize(lineCount);
            for (size_t line = firstLine.value; line < lastLineExclusive.value; ++line) {
                note::NoteRenderSourceLinePlan sourceLine;
                if (!sourcePlan.ResolveLine({line}, &sourceLine)) return false;
                candidate.line_layouts.push_back(
                    {16, static_cast<uint32_t>(80 + std::min<size_t>(line, 1000))});
                int x = 12;
                for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                    const size_t sourceLength = run.source_span.end.value - run.source_span.start.value;
                    if (sourceLength > static_cast<size_t>(std::numeric_limits<int>::max() - 20)) {
                        return false;
                    }
                    const int width = 20 + static_cast<int>(sourceLength);
                    if (x > std::numeric_limits<int>::max() - width - 2) return false;
                    candidate.line_placements[line - firstLine.value].runs.push_back(
                        note::NoteRenderRunPlacement{
                            run.source_span, x, width,
                            {{run.source_span.start, x}, {run.source_span.end, x + width}},
                        });
                    x += width + 2;
                }
            }
            *out = std::move(candidate);
            return true;
        } catch (...) {
            return false;
        }
    }
};

class InvalidPlacementFinalMeasurementProvider final
    : public note::NoteRenderFinalMeasurementProvider {
public:
    [[nodiscard]] bool Measure(
        const note::NoteRenderSourcePlan& sourcePlan,
        const note::NoteRenderLayoutKey& layoutKey,
        note::NoteRenderFinalMeasuredFrame* out) const noexcept override {
        if (!valid_provider_.Measure(sourcePlan, layoutKey, out) || !out) return false;
        out->line_placements.clear();
        return true;
    }

    [[nodiscard]] bool MeasureReplacement(
        const note::NoteRenderSourcePlan& sourcePlan,
        const note::NoteRenderLayoutKey& layoutKey,
        note::LineIndex firstLine,
        note::LineIndex lastLineExclusive,
        note::NoteRenderFinalMeasuredLineRange* out) const noexcept override {
        if (!valid_provider_.MeasureReplacement(
                sourcePlan, layoutKey, firstLine, lastLineExclusive, out) || !out) {
            return false;
        }
        out->line_placements.clear();
        return true;
    }

private:
    DeterministicFinalMeasurementProvider valid_provider_;
};

class WrappedFinalMeasurementProvider final
    : public note::NoteRenderFinalMeasurementProvider {
public:
    [[nodiscard]] bool Measure(
        const note::NoteRenderSourcePlan& sourcePlan,
        const note::NoteRenderLayoutKey& layoutKey,
        note::NoteRenderFinalMeasuredFrame* out) const noexcept override {
        if (!out || !sourcePlan.valid() || !layoutKey.valid() ||
            sourcePlan.line_count() != 1) {
            return false;
        }
        try {
            note::NoteRenderSourceLinePlan sourceLine;
            if (!sourcePlan.ResolveLine({0}, &sourceLine)) return false;
            note::NoteRenderFinalMeasuredFrame candidate;
            candidate.line_layouts.push_back({32, 40});
            note::NoteRenderLinePlacement placementLine;
            for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                const size_t sourceLength = run.source_span.end.value - run.source_span.start.value;
                if (sourceLength < 2 || sourceLength > 8) return false;
                const note::Utf16CodeUnitOffset split{run.source_span.start.value + sourceLength / 2};
                placementLine.runs.push_back(note::NoteRenderRunPlacement{
                    run.source_span,
                    10,
                    40,
                    {
                        {run.source_span.start, 10, 0},
                        {split, 50, 0},
                        {split, 10, 1},
                        {run.source_span.end, 50, 1},
                    },
                    {
                        {{run.source_span.start, split}, 10, 40, 0, 16},
                        {{split, run.source_span.end}, 10, 40, 16, 16},
                    },
                });
            }
            candidate.line_placements.push_back(std::move(placementLine));
            *out = std::move(candidate);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool MeasureReplacement(
        const note::NoteRenderSourcePlan&,
        const note::NoteRenderLayoutKey&,
        note::LineIndex,
        note::LineIndex,
        note::NoteRenderFinalMeasuredLineRange*) const noexcept override {
        return false;
    }
};

std::string Sha256Hex(const core_hash::Sha256Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t value : digest) {
        text.push_back(kHex[value >> 4]);
        text.push_back(kHex[value & 0x0f]);
    }
    return text;
}

const note::Diagnostic* FindDiagnostic(const note::NoteDocument& doc, std::wstring_view code) {
    for (const auto& diag : doc.diagnostics) {
        if (diag.code == code) {
            return &diag;
        }
    }
    return nullptr;
}

const note::StyleSpan* FindStyleSpan(const note::NoteDocument& doc,
                                     note::StyleKind kind,
                                     std::wstring_view value = {}) {
    for (const auto& span : doc.style_spans) {
        if (span.kind != kind) continue;
        if (!value.empty() && span.value != value) continue;
        return &span;
    }
    return nullptr;
}

const note::BlockNode* FindBlock(const note::NoteDocument& doc,
                                 note::BlockKind kind,
                                 int ordinal = 0) {
    for (const auto& block : doc.blocks) {
        if (block.kind != kind) continue;
        if (ordinal == 0) {
            return &block;
        }
        --ordinal;
    }
    return nullptr;
}

const note::InlineNode* FindInline(const note::NoteDocument& doc,
                                   note::InlineKind kind,
                                   int ordinal = 0) {
    for (const auto& inlineNode : doc.inlines) {
        if (inlineNode.kind != kind) continue;
        if (ordinal == 0) {
            return &inlineNode;
        }
        --ordinal;
    }
    return nullptr;
}

bool HasError(const note::MathInputAnalysis& analysis, std::wstring_view code) {
    for (const auto& diag : analysis.diagnostics) {
        if (diag.severity == note::DiagnosticSeverity::Error && diag.code == code) {
            return true;
        }
    }
    return false;
}

bool ContainsMathNodeType(const mathrender::Node* node, mathrender::Node::Type type) {
    if (!node) return false;
    if (node->type == type) return true;
    if (ContainsMathNodeType(node->a.get(), type)) return true;
    if (ContainsMathNodeType(node->b.get(), type)) return true;
    if (ContainsMathNodeType(node->super.get(), type)) return true;
    if (ContainsMathNodeType(node->sub.get(), type)) return true;
    for (const auto& row : node->rows) {
        for (const auto& cell : row) {
            if (ContainsMathNodeType(cell.get(), type)) return true;
        }
    }
    return false;
}

note::NoteDocument ParseMd4c(std::wstring text) {
    note::NoteMetadata meta;
    meta.file_name = L"note.md";
    note::NoteTextModel model = note::MakeNoteTextModel(std::move(meta), std::move(text), 1);
    return note::ParseNoteDocument(model);
}

note::NoteDocument ParseTeXSource(std::wstring text) {
    note::NoteMetadata meta;
    meta.file_name = L"note.tex";
    note::NoteTextModel model = note::MakeNoteTextModel(std::move(meta), std::move(text), 1);
    return note::ParseTeXMathDocument(model);
}

std::wstring CopyFinalSyntaxSnapshotText(
    const std::shared_ptr<const note::NoteSyntaxSnapshot>& snapshot) {
    return snapshot ? snapshot->CopySourceRange(
        {0}, snapshot->source_root().text_length()) : std::wstring{};
}

bool CopyFinalSyntaxSnapshotDocument(
    const std::shared_ptr<const note::NoteSyntaxSnapshot>& snapshot,
    note::NoteDocument* out) {
    return snapshot && snapshot->CopyDocumentForCompleteBuild(out);
}

std::pair<note::NoteTextModel, note::NoteDocument> BuildMd4cModelAndDoc(std::wstring text) {
    note::NoteMetadata meta;
    meta.file_name = L"note.md";
    note::NoteTextModel model = note::MakeNoteTextModel(std::move(meta), std::move(text), 1);
    note::NoteDocument doc = note::ParseNoteDocument(model);
    return {std::move(model), std::move(doc)};
}

bool ApplyTextEditKeepsLineStartsInSync(std::wstring text, note::TextEdit edit) {
    note::NoteMetadata meta;
    meta.file_name = L"note.md";
    note::NoteTextModel model = note::MakeNoteTextModel(std::move(meta), std::move(text), 1);
    note::ApplyTextEdit(&model, edit);
    return model.line_starts == note::BuildLineStarts(model.raw);
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    {
        const std::wstring text = L"before\r\n::: note\r\n# 見出し\r\n**本文** $x$\r\n"
            L"::: inner\r\n`code`\r\n:::\r\n```md\r\n::: literal\r\n```\r\n:::\r\nafter";
        const auto model = note::MakeNoteTextModel({L"container.md", L""}, text, 1);
        const auto doc = note::ParseNoteDocument(model);
        std::vector<size_t> containers;
        bool nested = false, headingChild = false, codeChild = false, outsideAfter = false;
        for (size_t index = 0; index < doc.blocks.size(); ++index) {
            const auto& block = doc.blocks[index];
            if (block.kind == note::BlockKind::FencedContainer) {
                containers.push_back(index);
                nested = nested || (block.parent != static_cast<size_t>(-1) &&
                    doc.blocks[block.parent].kind == note::BlockKind::FencedContainer);
            }
            headingChild = headingChild || (block.kind == note::BlockKind::Heading &&
                block.parent != static_cast<size_t>(-1));
            codeChild = codeChild || (block.kind == note::BlockKind::CodeBlock &&
                block.parent != static_cast<size_t>(-1));
            outsideAfter = outsideAfter || (block.span.start.value >= text.find(L"after") &&
                block.parent == static_cast<size_t>(-1));
        }
        Expect(containers.size() == 2 && nested && headingChild && codeChild && outsideAfter &&
               doc.blocks[containers.front()].fence_closed && doc.math_spans.size() == 1 && model.raw == text,
               "fenced containers own nested Markdown blocks but not literal code fences, preserving CRLF source");
        const auto plain = note::ExportPlainText(model, doc, note::TextExportConfig{});
        Expect(plain.find("before") != std::string::npos && plain.find("after") != std::string::npos &&
               plain.find("::: note") == std::string::npos && plain.find("::: literal") != std::string::npos &&
               plain.find("code") != std::string::npos,
               "container text export removes only wrapper syntax and preserves literal code and outside text");
        const auto html = note::ExportHtml(model, doc, note::MarkupExportConfig{});
        Expect(html.find("note-container") != std::string::npos && html.find("<h1>") != std::string::npos &&
               html.find("<strong>") != std::string::npos && html.find("<pre><code") != std::string::npos,
               "container HTML export retains nested Markdown structure");
        for (const std::wstring source : {L":::\nbody\n:::", L"::: note\nbody", L"::: note\n:::"}) {
            const auto fixture = note::MakeNoteTextModel({L"container.md", L""}, source, 2);
            const auto parsed = note::ParseNoteDocument(fixture);
            Expect(!parsed.blocks.empty() && parsed.blocks.front().kind == note::BlockKind::FencedContainer &&
                   parsed.blocks.front().span.end.value == source.size(),
                   "bare, unclosed and empty container boundaries remain source-addressable");
        }
        const auto literalModel = note::MakeNoteTextModel({L"literal.md", L""},
            L"`one\n::: literal\ntwo`\n\n$$\n::: math\n$$\n\\::: escaped\ntext::: text", 3);
        const auto literal = note::ParseNoteDocument(literalModel);
        Expect(std::none_of(literal.blocks.begin(), literal.blocks.end(), [](const auto& block) {
            return block.kind == note::BlockKind::FencedContainer;
        }), "inline code, math, escaped and non-standalone colons do not open containers");
    }

    {
        core_hash::Sha256 hasher;
        static constexpr std::uint8_t kAbc[] = {'a', 'b', 'c'};
        hasher.Update(kAbc, sizeof(kAbc));
        Expect(Sha256Hex(hasher.Finalize()) ==
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256 integrity guard matches the standard abc test vector");
    }

    {
        const std::wstring text = L"abc\r\nかな、e\x0301\xD83D\xDE00";
        const auto units = note::BuildTextUnits(text);
        Expect(units.size() == 9, "text boundaries split character-type runs into grapheme-safe units");
        Expect(units[3].klass == note::TextUnitClass::LineBreak &&
                   units[3].span.start.value == 3 && units[3].span.end.value == 5,
               "text boundaries keep CRLF as one deletion unit");
        Expect(units[7].span.end.value - units[7].span.start.value == 2,
               "text boundaries keep combining marks with their base character");
        Expect(units[8].span.end.value - units[8].span.start.value == 2,
               "text boundaries keep surrogate pairs intact");
        const note::Span backward = note::PreviousTextUnitRun(text, {7});
        Expect(backward.start.value == 5 && backward.end.value == 7,
               "backward character-type deletion restores the exact preceding run");
        const note::Span forward = note::NextTextUnitRun(text, {7});
        Expect(forward.start.value == 7 && forward.end.value == 8,
               "forward character-type deletion stops at punctuation boundaries");
    }

    {
        note::NoteIdentityRegistry registry;
        const note::NoteIdentity first = registry.ResolvePath(L"c:\\notes\\first.md");
        const note::NoteIdentity firstAgain = registry.ResolvePath(L"c:\\notes\\first.md");
        const note::NoteIdentity second = registry.ResolvePath(L"c:\\notes\\second.md");
        Expect(first.valid() && first.note_id == firstAgain.note_id,
               "note identity registry resolves one runtime id per normalized path");
        Expect(second.valid() && second.note_id != first.note_id,
               "note identity registry separates different notes");

        const note::NoteIdentity transient = registry.CreateTransient();
        Expect(transient.valid() && transient.transient,
               "note identity registry creates pathless transient notes");
        Expect(registry.BindPath(transient.note_id, L"c:\\notes\\created.md"),
               "transient note can acquire a path without changing note id");
        const auto created = registry.FindPath(L"c:\\notes\\created.md");
        Expect(created.has_value() && created->note_id == transient.note_id &&
                   !created->transient,
               "bound path resolves to the original transient note identity");
        Expect(!registry.BindPath(second.note_id, L"c:\\notes\\first.md"),
               "path alias cannot be stolen from a different note identity");
        Expect(registry.BindPath(first.note_id, L"c:\\notes\\renamed.md") &&
                   !registry.FindPath(L"c:\\notes\\first.md").has_value(),
               "renaming a note rebinds its path alias without retaining the old alias");
        const auto renamed = registry.FindPath(L"c:\\notes\\renamed.md");
        Expect(renamed.has_value() && renamed->note_id == first.note_id,
               "renamed path preserves the runtime note identity");

        const note::ViewIdentity view = registry.CreateView(first.note_id);
        const auto rebound = registry.RebindView(view.view_id, second.note_id);
        Expect(view.valid() && rebound.has_value() &&
                   rebound->view_id == view.view_id &&
                   rebound->note_id == second.note_id &&
                   rebound->binding_revision == view.binding_revision + 1,
               "view identity survives note rebinding and advances its binding revision");
        registry.ForgetView(view.view_id);
        Expect(!registry.FindView(view.view_id).has_value(),
               "forgotten view identity cannot retain stale presentation state");

        const note::SnapshotIdentity firstRevision =
            note::BuildSnapshotIdentity(first.note_id, 7, 2, "line1\r\nline2");
        const note::SnapshotIdentity sameRevision =
            note::BuildSnapshotIdentity(first.note_id, 7, 2, "\xEF\xBB\xBFline1\nline2");
        const note::SnapshotIdentity newerRevision =
            note::BuildSnapshotIdentity(first.note_id, 8, 2, "line1\nline2");
        const note::SnapshotIdentity changedContent =
            note::BuildSnapshotIdentity(first.note_id, 8, 2, "changed");
        Expect(note::SameSnapshotIdentity(firstRevision, sameRevision),
               "snapshot fingerprint canonicalizes UTF-8 BOM and line endings");
        Expect(!note::SameSnapshotIdentity(firstRevision, newerRevision) &&
                   note::SameSnapshotOwner(firstRevision, newerRevision) &&
                   note::SameSnapshotContent(firstRevision, newerRevision),
               "snapshot owner remains stable while content revision advances");
        Expect(!note::SameSnapshotContent(firstRevision, changedContent),
               "snapshot content comparison rejects changed text for the same note");

        note::SemanticLinkTargetResolution linkTarget;
        linkTarget.target_path = L"c:\\notes\\renamed.md";
        linkTarget.snapshot_identity = firstRevision;
        linkTarget.anchor_position = 12;
        Expect(note::SemanticLinkTargetMatchesSnapshot(linkTarget, newerRevision),
               "resolved link target accepts the same canonical note content");
        Expect(!note::SemanticLinkTargetMatchesSnapshot(linkTarget, changedContent),
               "resolved link target rejects a changed target snapshot");

        note_snapshot::TextStorageSnapshot diskStorage;
        diskStorage.noteIdentity = first;
        diskStorage.diskOk = true;
        diskStorage.diskBytes = "disk text";
        const note_snapshot::LatestTextSnapshot diskSnapshot =
            note_snapshot::ResolveLatestTextSnapshot(
                L"c:\\notes\\renamed.md", std::move(diskStorage));
        Expect(diskSnapshot.ok &&
                   diskSnapshot.source == note_snapshot::TextSnapshotSource::Disk &&
                   diskSnapshot.identity.note_id == first.note_id &&
                   diskSnapshot.identity.content_fingerprint != 0,
               "snapshot resolver labels disk source with note id and content fingerprint");

        note_snapshot::TextStorageSnapshot stageStorage;
        stageStorage.noteIdentity = first;
        stageStorage.diskOk = true;
        stageStorage.diskBytes = "old disk";
        stageStorage.hasStage = true;
        stageStorage.stageResolved = true;
        stageStorage.stageContentRevision = 17;
        stageStorage.stagePersistenceRevision = 42;
        stageStorage.stageBytes.clear();
        const note_snapshot::LatestTextSnapshot stageSnapshot =
            note_snapshot::ResolveLatestTextSnapshot(
                L"c:\\notes\\renamed.md", std::move(stageStorage));
        Expect(stageSnapshot.ok &&
                   stageSnapshot.source == note_snapshot::TextSnapshotSource::Stage &&
                   stageSnapshot.bytes.empty() &&
                   stageSnapshot.identity.content_revision == 17 &&
                   stageSnapshot.identity.persistence_revision == 42 &&
                   stageSnapshot.identity.content_fingerprint != 0,
               "snapshot resolver preserves empty staged text and persistence revision");

        note_snapshot::CurrentEditTextSnapshot wrongOwner;
        wrongOwner.available = true;
        wrongOwner.targetPath = L"c:\\notes\\renamed.md";
        wrongOwner.identity = note::BuildSnapshotIdentity(second.note_id, 9, 0, "edit");
        wrongOwner.bytes = "edit";
        Expect(!note_snapshot::ResolveCurrentEditTextSnapshot(
                    L"c:\\notes\\renamed.md", *renamed, wrongOwner).has_value(),
               "snapshot resolver rejects current edit owned by another note id");

        wrongOwner.identity = note::BuildSnapshotIdentity(first.note_id, 9, 0, "edit");
        const auto currentSnapshot = note_snapshot::ResolveCurrentEditTextSnapshot(
            L"C:\\NOTES\\RENAMED.MD", *renamed, wrongOwner);
        Expect(currentSnapshot.has_value() &&
                   currentSnapshot->source == note_snapshot::TextSnapshotSource::CurrentEdit &&
                   currentSnapshot->identity.content_revision == 9 &&
                   currentSnapshot->identity.note_id == first.note_id,
               "snapshot resolver accepts normalized-path current edit for the same note id");
    }

    {
        note::PersistentNoteIdentityCatalog catalog;
        catalog.Reset(0x123456789ABCDEF0ULL);
        const note::NoteId firstId = catalog.AllocateNoteId();
        const note::NoteId secondId = catalog.AllocateNoteId();
        Expect(firstId.valid() && secondId.valid() && firstId != secondId,
               "persistent identity catalog allocates distinct nonzero note ids");
        Expect(catalog.BindPath(firstId, "class/session/note-a.md") &&
                   catalog.BindPath(secondId, "class/session/note-b.md"),
               "persistent identity catalog binds workspace-relative aliases");
        Expect(catalog.FindRecord(firstId).has_value() &&
                   catalog.FindRecord(firstId)->persistence_revision == 0 &&
                   catalog.ObserveDisk(firstId, 0xA1ULL) == std::optional<uint64_t>(1) &&
                   catalog.ObserveDisk(firstId, 0xA1ULL) == std::optional<uint64_t>(1) &&
                   catalog.ObserveDisk(firstId, 0xB2ULL) == std::optional<uint64_t>(2),
               "persistent identity catalog advances only when disk content changes");
        Expect(catalog.CommitPersistence(firstId, 2, 3, 0xC3ULL) &&
                   !catalog.CommitPersistence(firstId, 1, 2, 0xD4ULL),
               "persistent identity catalog commits the next generation and rejects stale bases");

        const std::string serialized = catalog.Serialize();
        note::PersistentNoteIdentityCatalog restored;
        std::wstring parseError;
        Expect(restored.Parse(serialized, &parseError) &&
                   restored.workspace_nonce() == catalog.workspace_nonce() &&
                   restored.FindPath("class/session/note-a.md") == firstId &&
                   restored.FindNote(secondId) ==
                       std::optional<std::string>("class/session/note-b.md") &&
                   restored.FindRecord(firstId).has_value() &&
                   restored.FindRecord(firstId)->persistence_revision == 3 &&
                   restored.FindRecord(firstId)->disk_fingerprint == 0xC3ULL,
               "persistent identity catalog round-trips binary records and allocator state");
        Expect(restored.RebindPath(firstId, "class/session/renamed.md") &&
                   !restored.FindPath("class/session/note-a.md").has_value() &&
                   restored.FindPath("class/session/renamed.md") == firstId &&
                   restored.FindRecord(firstId)->persistence_revision == 3,
               "persistent identity catalog preserves note id across path rebind");
        Expect(!restored.BindPath(secondId, "class/session/renamed.md"),
               "persistent identity catalog rejects alias ownership collisions");
        Expect(!restored.RebindPath(firstId, "../outside.md") &&
                   !restored.BindPath(secondId, "class/session/bad:name.md"),
               "persistent identity catalog rejects traversal and invalid Windows paths");

        std::string corrupt = serialized;
        corrupt[corrupt.size() / 2] ^= 0x01;
        note::PersistentNoteIdentityCatalog rejected;
        Expect(!rejected.Parse(corrupt, &parseError),
               "persistent identity catalog rejects checksum-corrupted data");
    }

    {
        using Decision = note::NotePersistenceCommitDecision;
        const note::NoteId owner{0x9901ULL};
        const note::NotePersistenceCommitIntent intent{
            owner,
            7,
            8,
            0xC3ULL,
        };
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, true, 7, 0xB2ULL},
                   intent) == Decision::ReadyToWrite,
               "persistence core allows a write only from the expected base generation");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, false, 0, 0},
                   note::NotePersistenceCommitIntent{owner, 0, 1, 0xA1ULL}) ==
                   Decision::ReadyToWrite,
               "persistence core permits creation from an empty generation");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, true, 8, 0xC3ULL},
                   intent) == Decision::AlreadyWritten,
               "persistence core recognizes an idempotent retry after the destination write");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, true, 9, 0xD4ULL},
                   intent) == Decision::Conflict &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, true, 8, 0xD4ULL},
                       intent) == Decision::Conflict &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, false, 8, 0},
                       intent) == Decision::Conflict,
               "persistence core rejects newer, divergent, and missing written destinations");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{note::NoteId{2}, true, 7, 0xB2ULL},
                   intent) == Decision::InvalidInput &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, true, 7, 0xB2ULL},
                       note::NotePersistenceCommitIntent{owner, 7, 9, 0xC3ULL}) ==
                       Decision::InvalidInput &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, true, 7, 0xB2ULL},
                       note::NotePersistenceCommitIntent{owner, 7, 8, 0}) ==
                       Decision::InvalidInput &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, false, 7, 0xB2ULL},
                       intent) == Decision::InvalidInput,
               "persistence core fails closed for invalid ownership, generations, and observations");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{
                       owner, true, std::numeric_limits<uint64_t>::max(), 0xB2ULL},
                   note::NotePersistenceCommitIntent{
                       owner,
                       std::numeric_limits<uint64_t>::max(),
                       1,
                       0xC3ULL}) == Decision::InvalidInput,
               "persistence core rejects generation overflow without mutation");
    }

    {
        note::NoteTransactionCore transactions;
        const note::NoteId owner{0x1234ULL};
        const auto load = transactions.Begin(
            note::NoteChangeOrigin::ProgramLoad, owner, true);
        const auto active = transactions.Snapshot();
        Expect(load.valid() && active.suppress_change &&
                   active.program_mutation_active && active.depth == 1 &&
                   active.origin == note::NoteChangeOrigin::ProgramLoad &&
                   active.owner_note_id == owner &&
                   !transactions.ObserveUserInput(),
               "note transaction keeps program load active despite input-like notifications");

        const auto presentation = transactions.Begin(
            note::NoteChangeOrigin::PresentationUpdate, owner, false);
        Expect(!transactions.Commit(load) &&
                   transactions.Snapshot().depth == 2 &&
                   transactions.Commit(presentation) &&
                   transactions.Snapshot().depth == 1,
               "note transaction enforces LIFO nesting for presentation updates");
        Expect(transactions.Commit(load) &&
                   transactions.Snapshot().waiting_for_user_input &&
                   transactions.ShouldSuppressChange(),
               "note transaction converts committed load into a user-input latch");
        Expect(transactions.ObserveUserInput() &&
                   !transactions.ShouldSuppressChange() &&
                   !transactions.ObserveUserInput(),
               "note transaction releases suppression only on explicit user input");

        const auto clear = transactions.Begin(
            note::NoteChangeOrigin::ProgramClear, owner, true);
        Expect(transactions.Commit(clear),
               "note transaction can arm a clear-operation latch");
        const auto nested = transactions.Begin(
            note::NoteChangeOrigin::PresentationUpdate, owner, false);
        Expect(transactions.Cancel(nested) &&
                   transactions.Snapshot().waiting_for_user_input,
               "cancelled nested transaction preserves the prior input latch");
    }

    {
        using Action = main_close_policy::CloseRequestAction;
        Expect(main_close_policy::ResolveCloseRequestAction({true, false, false, false}) ==
                   Action::IgnoreAlreadyExiting,
               "main close policy ignores duplicate close while exit is already in progress");
        Expect(main_close_policy::ResolveCloseRequestAction({false, true, false, false}) ==
                   Action::WaitForActiveSave,
               "main close policy waits for an active save before exiting");
        Expect(main_close_policy::ResolveCloseRequestAction({false, false, true, true}) ==
                   Action::BypassFinishedAutomation,
               "main close policy bypasses unattended staged-diff prompts after automation finishes");
        Expect(main_close_policy::ResolveCloseRequestAction({false, false, false, false}) ==
                   Action::RunExitFlow,
               "main close policy routes normal close requests through the exit flow");

        const auto failedExit = main_close_policy::ResolveExitFlowCompletion(false);
        Expect(!failedExit.destroy_window && !failedExit.exit_in_progress,
               "main close policy keeps the window open when exit save/cancel flow fails");
        const auto completedExit = main_close_policy::ResolveExitFlowCompletion(true);
        Expect(completedExit.destroy_window && completedExit.exit_in_progress,
               "main close policy destroys the window only after the exit flow succeeds");
    }

    {
        using Decision = setup_json_policy::AutoUpdateDecision;
        Expect(setup_json_policy::IsKnownTopLevelField("workspaceRoot") &&
                   setup_json_policy::IsKnownTopLevelField("annotToolModeOrder") &&
                   setup_json_policy::IsKnownTopLevelField("annotToolOrder"),
               "setup json policy recognizes current and legacy setup fields");
        Expect(!setup_json_policy::IsKnownTopLevelField("futureWorkspaceRootSchema"),
               "setup json policy treats unknown setup fields as preservation barriers");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, true, true, false) ==
                   Decision::Allow,
               "setup json policy allows auto-update for known valid setup json");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(false, true, true, false) ==
                   Decision::BlockReadFailure,
               "setup json policy blocks auto-update when setup json cannot be read");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, false, true, false) ==
                   Decision::BlockInvalidJson,
               "setup json policy blocks auto-update for malformed setup json");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, true, false, false) ==
                   Decision::BlockMissingWorkspaceRoot,
               "setup json policy blocks default overwrite when workspaceRoot is missing");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, true, true, true) ==
                   Decision::BlockUnknownTopLevelField,
               "setup json policy blocks auto-update when unknown setup fields are present");
    }

    {
        using Decision = cache_dir_policy::CacheDirDecision;
        Expect(cache_dir_policy::ResolveCacheDirDecision(L"__resource__/__tmp__") ==
                   Decision::ManagedDefault &&
                   cache_dir_policy::ResolveCacheDirDecision(L"__resource__\\__tmp__") ==
                       Decision::ManagedDefault &&
                   cache_dir_policy::ResolveCacheDirDecision(L"") == Decision::ManagedDefault,
               "cache dir policy accepts only the reserved resource tmp cache as the default");
        Expect(cache_dir_policy::ResolveCacheDirDecision(L"bin") ==
                   Decision::UnsafeCustom &&
                   cache_dir_policy::ResolveCacheDirDecision(L"__cache__") ==
                       Decision::UnsafeCustom &&
                   cache_dir_policy::ResolveCacheDirDecision(L"somewhere/__cache__") ==
                       Decision::UnsafeCustom &&
                   cache_dir_policy::ResolveCacheDirDecision(L"__resource__/user-cache") ==
                       Decision::UnsafeCustom,
               "cache dir policy rejects user-like or arbitrary cache directories");
        Expect(cache_dir_policy::EffectiveCacheDir(L"bin") ==
                   std::wstring(cache_dir_policy::kManagedDefault) &&
                   cache_dir_policy::EffectiveCacheDir(L"__cache__") ==
                       std::wstring(cache_dir_policy::kManagedDefault),
               "cache dir policy always falls back to the reserved tmp root for runtime use");
    }


    {
        using Decision = note::NoteDerivedReadDecision;
        const note::NoteId owner{0x4401ULL};
        const note::NoteDerivedSnapshotState derived{
            note::NoteDerivedSnapshotIdentity{owner, 12}, true};
        const note::NoteDerivedReadContext current{owner, 12, false, false};
        Expect(note::ResolveNoteDerivedRead(derived, current) == Decision::Current &&
                   note::CanReadCurrentNoteDerivedSnapshot(derived, current),
               "revision gate accepts a present derived snapshot from the current owner and revision");
        Expect(note::ResolveNoteDerivedRead(
                   note::NoteDerivedSnapshotState{
                       note::NoteDerivedSnapshotIdentity{owner, 12}, false},
                   current) ==
                   Decision::Missing,
               "revision gate distinguishes an absent derived snapshot");
        Expect(note::ResolveNoteDerivedRead(
                   note::NoteDerivedSnapshotState{
                       note::NoteDerivedSnapshotIdentity{note::NoteId{0x4402ULL}, 12},
                       true},
                   current) == Decision::OwnerMismatch,
               "revision gate rejects another note even when revisions coincide");
        Expect(note::ResolveNoteDerivedRead(
                   note::NoteDerivedSnapshotState{
                       note::NoteDerivedSnapshotIdentity{owner, 11}, true},
                   current) ==
                   Decision::RevisionMismatch,
               "revision gate rejects an older derived revision");
        Expect(note::ResolveNoteDerivedRead(
                   derived,
                   note::NoteDerivedReadContext{owner, 12, true, false}) ==
                   Decision::BlockedByPendingEdit,
               "revision gate blocks stable reads while a content edit is pending");
        Expect(note::ResolveNoteDerivedRead(
                   derived,
                   note::NoteDerivedReadContext{owner, 12, false, true}) ==
                   Decision::BlockedByDeferredRefresh,
               "revision gate blocks stable reads while full derived refresh is deferred");
    }

    {
        const note::NoteId owner{0x4501ULL};
        const note::NoteDerivedSnapshotIdentity revision12{owner, 12};
        const note::NoteDerivedSnapshotIdentity revision13{owner, 13};
        std::vector<note::NoteLayoutLineMetrics> lines(3);
        lines[1].line_height_permille = 1450;
        lines[1].max_font_px = 28;
        lines[1].visual_ascent_px = 20;

        note::NoteLayoutMetricsSnapshot snapshot;
        snapshot.Commit(revision12, lines);
        Expect(snapshot.Matches(revision12) && !snapshot.Matches(revision13),
               "layout metrics snapshot requires exact owner and source revision for reads");
        Expect(note::CanAdvanceLayoutSnapshot(snapshot, revision13) &&
                   !note::CanAdvanceLayoutSnapshot(
                       snapshot,
                       note::NoteDerivedSnapshotIdentity{note::NoteId{0x4502ULL}, 13}) &&
                   !note::CanAdvanceLayoutSnapshot(
                       snapshot, note::NoteDerivedSnapshotIdentity{owner, 11}),
               "layout metrics can advance only to a newer revision of the same note");

        std::vector<note::NoteLayoutLineMetrics> visualOnly = lines;
        visualOnly[1].visual_descent_px = 9;
        Expect(note::SameParagraphSpacing(lines, visualOnly) &&
                   !note::SameVisualMetrics(lines, visualOnly),
               "layout metrics distinguish paragraph formatting from paint geometry");
        Expect(note::LayoutMetricsUnchangedOutsideRange(lines, visualOnly, 1, 1),
               "layout dirty-range check accepts changes confined to the requested line");
        visualOnly[2].max_font_px = 18;
        Expect(!note::LayoutMetricsUnchangedOutsideRange(lines, visualOnly, 1, 1),
               "layout dirty-range check rejects changes outside the requested line");

        std::vector<note::NoteLayoutLineMetrics> compactSyntaxLine = lines;
        compactSyntaxLine[1].exact_line_height_px = 1;
        Expect(!note::SameParagraphSpacing(lines, compactSyntaxLine) &&
                   !note::LayoutMetricsUnchangedOutsideRange(lines, compactSyntaxLine, 0, 0),
               "a collapsed structured syntax line updates paragraph geometry and invalidation");

        snapshot.Reset();
        Expect(!snapshot.source_identity.valid() && snapshot.lines.empty(),
               "layout metrics reset clears identity and line records together");
    }

    {
        const note::NoteId owner{0x4601ULL};
        note::PendingNoteEditTransaction typed;
        Expect(typed.Append({owner, 10}, note::TextEdit{3, 0, L"a"}) &&
                   typed.Append({owner, 11}, note::TextEdit{4, 0, L"b"}),
               "pending note edit transaction records ordered canonical revisions");
        const auto typedEquivalent = typed.TryBuildEquivalentEdit();
        Expect(typedEquivalent.has_value() && typedEquivalent->start.value == 3 &&
                   typedEquivalent->deleted_len == 0 &&
                   typedEquivalent->inserted_text == L"ab",
               "pending note edit transaction composes contiguous typed insertion exactly");

        note::PendingNoteEditTransaction backwardDelete;
        Expect(backwardDelete.Append({owner, 20}, note::TextEdit{6, 1, L""}) &&
                   backwardDelete.Append({owner, 21}, note::TextEdit{5, 1, L""}),
               "pending note edit transaction records backward deletion revisions");
        const auto deleteEquivalent = backwardDelete.TryBuildEquivalentEdit();
        Expect(deleteEquivalent.has_value() && deleteEquivalent->start.value == 5 &&
                   deleteEquivalent->deleted_len == 2 &&
                   deleteEquivalent->inserted_text.empty(),
               "pending note edit transaction composes contiguous backward deletion exactly");

        note::PendingNoteEditTransaction forwardDelete;
        Expect(forwardDelete.Append({owner, 24}, note::TextEdit{5, 1, L""}) &&
                   forwardDelete.Append({owner, 25}, note::TextEdit{5, 1, L""}),
               "pending note edit transaction records forward deletion revisions");
        const auto forwardDeleteEquivalent = forwardDelete.TryBuildEquivalentEdit();
        Expect(forwardDeleteEquivalent.has_value() && forwardDeleteEquivalent->start.value == 5 &&
                   forwardDeleteEquivalent->deleted_len == 2 &&
                   forwardDeleteEquivalent->inserted_text.empty(),
               "pending note edit transaction composes contiguous forward deletion exactly");

        note::PendingNoteEditTransaction disjoint;
        Expect(disjoint.Append({owner, 30}, note::TextEdit{1, 0, L"a"}) &&
                   disjoint.Append({owner, 31}, note::TextEdit{9, 0, L"b"}) &&
                   !disjoint.TryBuildEquivalentEdit().has_value(),
               "disjoint edits remain ordered transactions instead of being guessed as one edit");
        Expect(!disjoint.Append({owner, 31}, note::TextEdit{10, 0, L"c"}),
               "pending transaction rejects a revision that is not its current identity");

        note::LocalNoteKernel kernel;
        kernel.Reset(owner,
                     note::NoteMetadata{L"kernel.md", L"kernel"},
                     L"# Head\n\nBody",
                     10,
                     3,
                     note::NoteContentKind::Markdown);
        Expect(kernel.valid() && !kernel.CanReadSyntax(),
               "local note kernel owns text before derived state is built");

        const note::NoteKernelRefreshResult full = kernel.RefreshDerived();
        Expect(full.kind == note::NoteKernelRefreshKind::Full &&
                   full.current && kernel.CanReadSyntax() &&
                   kernel.CanReadSemantic() &&
                   kernel.semantic_index().headings.size() == 1,
               "local note kernel atomically builds syntax and semantic snapshots");

        const size_t bodyEnd = kernel.text_core().model().raw.size();
        const note::NoteKernelApplyResult ordinary =
            kernel.Apply(note::TextEdit{bodyEnd, 0, L" text"}, true);
        Expect(ordinary.text_result == note::NoteTextApplyResult::Applied &&
                   ordinary.dirty_graph.has_edit &&
                   kernel.has_pending_edit() && !kernel.CanReadSyntax(),
               "local note kernel advances raw text and blocks stale derived reads");
        const note::NoteKernelRefreshResult incremental = kernel.RefreshDerived();
        Expect(incremental.kind == note::NoteKernelRefreshKind::Incremental &&
                   incremental.current &&
                   incremental.consumed_dirty_graph.has_value() &&
                   kernel.CanReadSemantic(),
               "local note kernel incrementally commits ordinary text edits");

        const size_t lineBreakAt = kernel.text_core().model().raw.size();
        (void)kernel.Apply(note::TextEdit{lineBreakAt, 0, L"\n"}, true);
        const note::NoteKernelRefreshResult deferred = kernel.RefreshDerived();
        Expect(deferred.kind == note::NoteKernelRefreshKind::Deferred &&
                   kernel.has_deferred_full_refresh() &&
                   !kernel.CanReadSyntax() && !kernel.CanReadSemantic(),
               "local note kernel fails closed until a line-break repair parse commits");
        kernel.RequestFullRefresh();
        const note::NoteKernelRefreshResult repaired = kernel.RefreshDerived();
        Expect(repaired.kind == note::NoteKernelRefreshKind::Full &&
                   repaired.current && !kernel.has_deferred_full_refresh(),
               "local note kernel repairs deferred structure with one full snapshot commit");

        note::LocalNoteKernel coalescingKernel;
        coalescingKernel.Reset(owner,
                               note::NoteMetadata{L"coalescing.md", L"coalescing"},
                               L"body",
                               40,
                               4,
                               note::NoteContentKind::Markdown);
        (void)coalescingKernel.RefreshDerived();
        const uint64_t materializationsBeforeCoalescedInput =
            coalescingKernel.text_core().model_materialization_count();
        (void)coalescingKernel.Apply(note::TextEdit{4, 0, L"a"}, true);
        (void)coalescingKernel.Apply(note::TextEdit{5, 0, L"b"}, true);
        Expect(coalescingKernel.has_pending_edit() &&
                   !coalescingKernel.requires_full_refresh() &&
                   coalescingKernel.text_core().model_materialization_count() ==
                       materializationsBeforeCoalescedInput,
               "contiguous ordinary input avoids a canonical full-text materialization while pending");
        const note::NoteKernelRefreshResult coalesced = coalescingKernel.RefreshDerived();
        const std::wstring& coalescedRaw = coalescingKernel.text_core().model().raw;
        Expect(coalesced.kind == note::NoteKernelRefreshKind::Incremental &&
                   coalesced.current && coalescedRaw == L"bodyab",
               "local note kernel incrementally commits contiguous typed input as one transaction");

        note::LocalNoteKernel deleteKernel;
        deleteKernel.Reset(owner,
                           note::NoteMetadata{L"deleting.md", L"deleting"},
                           L"abcd",
                           50,
                           4,
                           note::NoteContentKind::Markdown);
        (void)deleteKernel.RefreshDerived();
        const uint64_t materializationsBeforeDelete =
            deleteKernel.text_core().model_materialization_count();
        (void)deleteKernel.Apply(note::TextEdit{3, 1, L""}, true);
        (void)deleteKernel.Apply(note::TextEdit{2, 1, L""}, true);
        const note::NoteKernelRefreshResult deleted = deleteKernel.RefreshDerived();
        Expect(deleted.kind == note::NoteKernelRefreshKind::Incremental &&
                   deleted.current &&
                   deleteKernel.text_core().model_materialization_count() ==
                       materializationsBeforeDelete &&
                   deleteKernel.text_core().MatchesRaw(L"ab"),
               "local note kernel incrementally commits contiguous backward deletion without pending full text");

        note::LocalNoteKernel codeBodyKernel;
        codeBodyKernel.Reset(owner,
                             note::NoteMetadata{L"code-body.md", L"code-body"},
                             L"```md\nalpha\n```\ntail",
                             55,
                             4,
                             note::NoteContentKind::Markdown);
        (void)codeBodyKernel.RefreshDerived();
        const uint64_t materializationsBeforeCodeBodyInput =
            codeBodyKernel.text_core().model_materialization_count();
        // "```md\nalpha" occupies 11 canonical UTF-16 code units.  Keep the
        // fixture offset literal: reading the test's source through TextCore
        // would itself materialize the root and invalidate this hot-path test.
        const size_t codeBodyEnd = 11;
        const note::NoteKernelApplyResult firstCodeBodyEdit =
            codeBodyKernel.Apply(note::TextEdit{{codeBodyEnd}, 0, L"x"}, true);
        Expect(codeBodyKernel.text_core().model_materialization_count() ==
                   materializationsBeforeCodeBodyInput,
               "the first fenced-code body edit stays on the local proof path");
        const note::NoteKernelApplyResult secondCodeBodyEdit =
            codeBodyKernel.Apply(note::TextEdit{{codeBodyEnd + 1}, 0, L"y"}, true);
        Expect(firstCodeBodyEdit.dirty_graph.downstream_parser_state_proven_unchanged &&
                   secondCodeBodyEdit.dirty_graph.downstream_parser_state_proven_unchanged,
               "fenced-code body edits carry a fixed parser-state proof across the transaction");
        Expect(secondCodeBodyEdit.used_pending_local_dirty_proof,
               "the second fenced-code body edit consumes its carried local proof");
        Expect(codeBodyKernel.text_core().model_materialization_count() ==
                   materializationsBeforeCodeBodyInput,
               "the second fenced-code body edit retains the local proof path");
        const note::NoteKernelRefreshResult codeBodyRefresh =
            codeBodyKernel.RefreshDerived();
        Expect(codeBodyKernel.text_core().model_materialization_count() ==
                   materializationsBeforeCodeBodyInput,
               "fenced-code body typing keeps the pending proof without canonical materialization");
        Expect(codeBodyRefresh.kind == note::NoteKernelRefreshKind::Incremental &&
                   codeBodyRefresh.current,
               "fenced-code body typing commits through the incremental syntax path");
        Expect(codeBodyKernel.text_core().MatchesRaw(L"```md\nalphaxy\n```\ntail"),
               "fenced-code body typing preserves canonical source content");

        note::LocalNoteKernel containerBodyKernel;
        containerBodyKernel.Reset(owner,
                                  note::NoteMetadata{L"container-body.md", L"container-body"},
                                  L"- alpha\n- beta\ntail",
                                  56,
                                  4,
                                  note::NoteContentKind::Markdown);
        (void)containerBodyKernel.RefreshDerived();
        const uint64_t materializationsBeforeContainerBodyInput =
            containerBodyKernel.text_core().model_materialization_count();
        const note::NoteKernelApplyResult firstContainerBodyEdit =
            containerBodyKernel.Apply(note::TextEdit{{7}, 0, L"x"}, true);
        const note::NoteKernelApplyResult secondContainerBodyEdit =
            containerBodyKernel.Apply(note::TextEdit{{8}, 0, L"y"}, true);
        const note::NoteKernelRefreshResult containerBodyRefresh =
            containerBodyKernel.RefreshDerived();
        Expect(firstContainerBodyEdit.dirty_graph.downstream_parser_state_proven_unchanged &&
                   secondContainerBodyEdit.used_pending_local_dirty_proof &&
                   containerBodyKernel.text_core().model_materialization_count() ==
                       materializationsBeforeContainerBodyInput,
               "literal list-item body typing keeps the local parser-state proof");
        Expect(containerBodyRefresh.kind == note::NoteKernelRefreshKind::Incremental &&
                   containerBodyRefresh.current &&
                   containerBodyKernel.text_core().MatchesRaw(L"- alphaxy\n- beta\ntail"),
               "literal list-item body typing commits without reparsing its container");

        note::LocalNoteKernel inlineMathKernel;
        inlineMathKernel.Reset(owner,
                               note::NoteMetadata{L"inline-math.md", L"inline-math"},
                               L"formula $E=mc^2$ tail",
                               58,
                               4,
                               note::NoteContentKind::Markdown);
        (void)inlineMathKernel.RefreshDerived();
        const uint64_t materializationsBeforeInlineMathInput =
            inlineMathKernel.text_core().model_materialization_count();
        // The content starts at 9. Insert after "mc" and then immediately
        // continue the same transaction without materializing canonical text.
        const note::NoteKernelApplyResult firstInlineMathEdit =
            inlineMathKernel.Apply(note::TextEdit{{13}, 0, L"x"}, true);
        const note::NoteKernelApplyResult secondInlineMathEdit =
            inlineMathKernel.Apply(note::TextEdit{{14}, 0, L"y"}, true);
        const note::NoteKernelRefreshResult inlineMathRefresh =
            inlineMathKernel.RefreshDerived();
        const auto expectedInlineMath = BuildMd4cModelAndDoc(L"formula $E=mcxy^2$ tail");
        const note::NoteDocument& expectedInlineMathDocument = expectedInlineMath.second;
        Expect(firstInlineMathEdit.dirty_graph.downstream_parser_state_proven_unchanged &&
                   secondInlineMathEdit.used_pending_local_dirty_proof &&
                   inlineMathKernel.text_core().model_materialization_count() ==
                       materializationsBeforeInlineMathInput,
               "literal inline-math typing keeps the local parser-state proof");
        Expect(inlineMathRefresh.kind == note::NoteKernelRefreshKind::Incremental &&
                   inlineMathRefresh.current &&
                   inlineMathKernel.text_core().MatchesRaw(L"formula $E=mcxy^2$ tail") &&
                   inlineMathKernel.document().math_spans.size() == 1 &&
                   inlineMathKernel.document().math_spans[0].normalized_tex == L"E=mcxy^2",
               "literal inline-math typing incrementally updates normalized TeX");
        Expect(expectedInlineMathDocument.math_spans.size() == 1 &&
                   inlineMathKernel.document().math_spans.size() == 1 &&
                   inlineMathKernel.document().math_spans[0].kind ==
                       expectedInlineMathDocument.math_spans[0].kind &&
                   inlineMathKernel.document().math_spans[0].delimiter ==
                       expectedInlineMathDocument.math_spans[0].delimiter &&
                   inlineMathKernel.document().math_spans[0].span.start ==
                       expectedInlineMathDocument.math_spans[0].span.start &&
                   inlineMathKernel.document().math_spans[0].span.end ==
                       expectedInlineMathDocument.math_spans[0].span.end &&
                   inlineMathKernel.document().math_spans[0].content_span.start ==
                       expectedInlineMathDocument.math_spans[0].content_span.start &&
                   inlineMathKernel.document().math_spans[0].content_span.end ==
                       expectedInlineMathDocument.math_spans[0].content_span.end &&
                   inlineMathKernel.document().math_spans[0].normalized_tex ==
                       expectedInlineMathDocument.math_spans[0].normalized_tex &&
                   inlineMathKernel.semantic_index().math.size() == 1 &&
                   inlineMathKernel.semantic_index().math[0].normalized_tex == L"E=mcxy^2",
               "literal inline-math patch matches the full parser's math and semantic result");

        note::LocalNoteKernel inlineMathBackspaceKernel;
        inlineMathBackspaceKernel.Reset(owner,
                                        note::NoteMetadata{L"inline-math-delete.md",
                                                           L"inline-math-delete"},
                                        L"$E=mc^2$",
                                        59,
                                        4,
                                        note::NoteContentKind::Markdown);
        (void)inlineMathBackspaceKernel.RefreshDerived();
        const uint64_t materializationsBeforeInlineMathDelete =
            inlineMathBackspaceKernel.text_core().model_materialization_count();
        const note::NoteKernelApplyResult inlineMathBackspace =
            inlineMathBackspaceKernel.Apply(note::TextEdit{{3}, 1, L""}, true);
        const note::NoteKernelRefreshResult inlineMathDeleteRefresh =
            inlineMathBackspaceKernel.RefreshDerived();
        Expect(inlineMathBackspace.dirty_graph.downstream_parser_state_proven_unchanged &&
                   inlineMathDeleteRefresh.kind == note::NoteKernelRefreshKind::Incremental &&
                   inlineMathDeleteRefresh.current &&
                   inlineMathBackspaceKernel.text_core().model_materialization_count() ==
                       materializationsBeforeInlineMathDelete &&
                   inlineMathBackspaceKernel.text_core().MatchesRaw(L"$E=c^2$") &&
                   inlineMathBackspaceKernel.document().math_spans.size() == 1 &&
                   inlineMathBackspaceKernel.document().math_spans[0].normalized_tex == L"E=c^2",
               "literal inline-math Backspace keeps a current incremental snapshot");

        note::LocalNoteKernel fenceRevealKernel;
        fenceRevealKernel.Reset(owner,
                                note::NoteMetadata{L"fence-reveal.md", L"fence-reveal"},
                                L"```md\nx```\ninside\n```\ntail",
                                57,
                                4,
                                note::NoteContentKind::Markdown);
        (void)fenceRevealKernel.RefreshDerived();
        (void)fenceRevealKernel.Apply(note::TextEdit{{7}, 0, L"y"}, true);
        const note::NoteKernelApplyResult removePrefix =
            fenceRevealKernel.Apply(note::TextEdit{{6}, 1, L""}, true);
        const note::NoteKernelApplyResult exposeFence =
            fenceRevealKernel.Apply(note::TextEdit{{6}, 1, L""}, true);
        const note::NoteKernelRefreshResult fenceRevealRefresh =
            fenceRevealKernel.RefreshDerived();
        Expect(removePrefix.used_pending_local_dirty_proof &&
                   !exposeFence.used_pending_local_dirty_proof &&
                   fenceRevealRefresh.kind == note::NoteKernelRefreshKind::Full &&
                   fenceRevealRefresh.current,
               "a pending code-body proof is revoked before deletion exposes a closing fence");

        note::LocalNoteKernel rowShapeKernel;
        rowShapeKernel.Reset(owner,
                             note::NoteMetadata{L"row-shape.md", L"row-shape"},
                             L"x",
                             60,
                             4,
                             note::NoteContentKind::Markdown);
        (void)rowShapeKernel.RefreshDerived();
        (void)rowShapeKernel.Apply(note::TextEdit{0, 1, L""}, true);
        const note::NoteKernelRefreshResult emptiedRow = rowShapeKernel.RefreshDerived();
        Expect(emptiedRow.kind == note::NoteKernelRefreshKind::Full &&
                   emptiedRow.current && rowShapeKernel.text_core().model().raw.empty(),
               "kernel reparses rather than patching syntax when deletion empties a row");

        note::LocalNoteKernel listMarkerKernel;
        listMarkerKernel.Reset(owner,
                               note::NoteMetadata{L"list-marker.md", L"list-marker"},
                               L"- item",
                               70,
                               4,
                               note::NoteContentKind::Markdown);
        (void)listMarkerKernel.RefreshDerived();
        (void)listMarkerKernel.Apply(note::TextEdit{0, 0, L"x"}, true);
        const note::NoteKernelRefreshResult editedMarker = listMarkerKernel.RefreshDerived();
        Expect(editedMarker.kind == note::NoteKernelRefreshKind::Full &&
                   editedMarker.current &&
                   FindBlock(listMarkerKernel.document(), note::BlockKind::List) == nullptr,
               "kernel reparses a list-marker edit instead of retaining stale list structure");

        note::LocalNoteKernel exposedMarkerKernel;
        exposedMarkerKernel.Reset(owner,
                                  note::NoteMetadata{L"exposed-marker.md", L"exposed-marker"},
                                  L"x - item",
                                  71,
                                  4,
                                  note::NoteContentKind::Markdown);
        (void)exposedMarkerKernel.RefreshDerived();
        (void)exposedMarkerKernel.Apply(note::TextEdit{0, 2, L""}, true);
        const note::NoteKernelRefreshResult exposedMarker =
            exposedMarkerKernel.RefreshDerived();
        Expect(exposedMarker.kind == note::NoteKernelRefreshKind::Full &&
                   exposedMarker.current &&
                   FindBlock(exposedMarkerKernel.document(), note::BlockKind::List) != nullptr,
               "kernel reparses when prefix deletion exposes a list marker");

        kernel.Reset(owner,
                     note::NoteMetadata{L"kernel.txt", L"kernel"},
                     L"plain",
                     30,
                     4,
                     note::NoteContentKind::PlainText);
        const note::NoteKernelRefreshResult plain = kernel.RefreshDerived();
        Expect(plain.kind == note::NoteKernelRefreshKind::Cleared &&
                   kernel.text_core().MatchesRaw(L"plain") &&
                   !kernel.CanReadSyntax(),
               "local note kernel keeps plain text canonical without fabricating syntax state");

        kernel.Reset(owner,
                     note::NoteMetadata{L"kernel.tex", L"kernel"},
                     L"# TeX source\n$x$\n",
                     31,
                     4,
                     note::NoteContentKind::TeXSource);
        const note::NoteKernelRefreshResult tex = kernel.RefreshDerived();
        Expect(tex.kind == note::NoteKernelRefreshKind::Full && tex.current &&
                   kernel.CanReadSyntax() && kernel.CanReadSemantic() &&
                   kernel.document().math_spans.size() == 1 &&
                   kernel.semantic_index().headings.empty(),
               "local note kernel derives TeX math without interpreting TeX source as Markdown");
    }

    {
        note::LocalNoteKernelRegistry registry;
        const note::NoteId first{0x4701ULL};
        const note::NoteId second{0x4702ULL};
        registry.Reset(first, note::NoteMetadata{}, L"first", 1, 0,
                       note::NoteContentKind::Markdown);
        registry.Reset(second, note::NoteMetadata{}, L"second", 1, 0,
                       note::NoteContentKind::Markdown);
        const note::ViewIdentity firstView{note::ViewId{0x91ULL}, first, 1};
        Expect(registry.FindForView(firstView) == registry.Find(first) &&
                   registry.Find(first) != registry.Find(second) &&
                   registry.size() == 2,
               "local note kernel registry isolates notes while sharing one kernel per view owner");
    }

    {
        const std::wstring before = L"alpha\nbeta";
        const std::optional<note::TextEdit> computed =
            note::ComputeNoteTextEdit(before, L"alpha\nXbeta");
        Expect(computed.has_value() && computed->start.value == 6 &&
                   computed->deleted_len == 0 && computed->inserted_text == L"X",
               "text model computes one canonical edit between raw snapshots");
        const std::vector<size_t> starts{0, 6};
        const note::TextEdit plain{2, 0, L"x"};
        const note::NoteDirtyGraph plainGraph =
            note::BuildNoteDirtyGraph(before, starts, plain, true);
        Expect(plainGraph.has_edit &&
                   plainGraph.edit_kind == note::NoteEditKind::PlainText &&
                   plainGraph.content_dirty && plainGraph.syntax_dirty &&
                   plainGraph.render_dirty && plainGraph.layout_dirty &&
                   !plainGraph.line_count_may_change &&
                   plainGraph.stale_lines.valid &&
                   plainGraph.stale_lines.first == 0 &&
                   plainGraph.stale_lines.last == 0,
               "dirty graph classifies plain insertion and limits stale lines");
        Expect(note::NoteDirtyGraphAllowsRenderEarlyStop(plainGraph, false) &&
                   note::NoteDirtyGraphAllowsLineSpacingFastPath(plainGraph),
               "dirty graph enables bounded render and spacing fast paths for short plain edits");

        const note::NoteDirtyGraph structuralGraph = note::BuildNoteDirtyGraph(
            before, starts, note::TextEdit{2, 0, L":"}, true);
        Expect(structuralGraph.edit_kind == note::NoteEditKind::StructuralText &&
                   structuralGraph.structure_dirty &&
                   note::HasNoteDirtySyntaxFeature(
                       structuralGraph.syntax_features,
                       note::NoteDirtySyntaxFeature::BlockStructure) &&
                   !structuralGraph.line_count_may_change &&
                   structuralGraph.stale_lines.first == 0 &&
                   structuralGraph.stale_lines.last == 0 &&
                   !note::NoteDirtyGraphAllowsRenderEarlyStop(structuralGraph, false),
               "dirty graph keeps same-line structural edits local without early stop");

        const note::NoteDirtyGraph newlineGraph = note::BuildNoteDirtyGraph(
            before, starts, note::TextEdit{2, 0, L"\r\n"}, true);
        Expect(newlineGraph.edit_kind == note::NoteEditKind::LineBreakOnly &&
                   newlineGraph.line_count_may_change &&
                   newlineGraph.stale_lines.first == 0 &&
                   newlineGraph.stale_lines.last == 1,
               "dirty graph expands line-count changes to a full committed mapping");

        const std::wstring crlfText = L"one\r\ntwo\r\nthree";
        const note::NoteDirtyGraph deleteCrlf = note::BuildNoteDirtyGraph(
            crlfText, {0, 5, 10}, note::TextEdit{3, 2, L""}, true);
        Expect(deleteCrlf.edit_kind == note::NoteEditKind::LineBreakOnly &&
                   deleteCrlf.line_count_may_change &&
                   deleteCrlf.stale_lines.last == 2,
               "dirty graph treats CRLF deletion as one logical line break");

        const note::TextEdit invalid{99, 1, L"x"};
        const note::NoteDirtyGraph invalidGraph =
            note::BuildNoteDirtyGraph(before, starts, invalid, true);
        Expect(invalidGraph.edit_kind == note::NoteEditKind::StructuralText &&
                   invalidGraph.line_count_may_change &&
                   invalidGraph.stale_lines.first == 0 &&
                   invalidGraph.stale_lines.last == 1,
               "dirty graph fails closed for an invalid source edit range");
        Expect(note::TextEditsEqual(plain, note::TextEdit{2, 0, L"x"}) &&
                   !note::TextEditsEqual(plain, structuralGraph.edit),
               "dirty graph can carry one exact edit across derived layers");

        {
            auto [scopedModel, scopedDocument] = BuildMd4cModelAndDoc(
                L"before\nordinary text\nafter");
            const note::TextEdit scopedEdit{10, 0, L"x"};
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(scopedModel, scopedDocument);
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                scopedModel, scopedDocument, scopedEdit, &index);
            const note::NoteDirtyGraph scopedGraph = note::BuildNoteDirtyGraph(
                scopedModel.raw, scopedModel.line_starts, scopedEdit, true, &scope);
            Expect(note::NoteInfluenceIndexMatchesTextModel(index, scopedModel) &&
                       index.line_entry_offsets.size() == scopedModel.line_starts.size() + 1 &&
                       scope.kind == note::NoteInfluenceScopeKind::LocalLine &&
                       scope.permits_local_dirty_graph &&
                       scope.source_lines.valid && scope.source_lines.first == 1 &&
                       scope.source_lines.last == 1,
                   "influence scope proves an ordinary middle line local");
            Expect(scopedGraph.propagation == note::NoteDirtyPropagation::LocalLine &&
                       scopedGraph.stale_lines.valid && scopedGraph.stale_lines.first == 1 &&
                       scopedGraph.stale_lines.last == 1 &&
                       note::NoteDirtyGraphAllowsRenderEarlyStop(scopedGraph, false),
                   "local influence scope avoids after-text scanning");
            note::ApplyTextEdit(&scopedModel, scopedEdit);
            const note::NoteInfluenceScope continued =
                note::ResolveNoteInfluenceContinuationScope(
                    scopedModel, scope, note::TextEdit{11, 0, L"y"});
            Expect(continued.kind == note::NoteInfluenceScopeKind::LocalLine &&
                       continued.source_revision == scopedModel.revision &&
                       continued.source_lines.valid && continued.source_lines.first == 1,
                   "ordinary typing continues a proof only onto its next canonical revision");
        }

        {
            const std::wstring tableText =
                L"| Item | State |\n"
                L"| --- | --- |\n"
                L"| Markdown | Ready |\n"
                L"\n"
                L"tail";
            auto [tableModel, tableDocument] = BuildMd4cModelAndDoc(tableText);
            const size_t editStart = tableText.find(L"Markdown") + 1;
            const note::TextEdit tableEdit{{editStart}, 0, L"x"};
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(tableModel, tableDocument);
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                tableModel, tableDocument, tableEdit, &index);
            const note::NoteDirtyGraph tableScopedGraph = note::BuildNoteDirtyGraph(
                tableModel.raw, tableModel.line_starts, tableEdit, true, &scope);
            Expect(scope.kind == note::NoteInfluenceScopeKind::SharedTableGeometry &&
                       scope.permits_local_dirty_graph && scope.source_lines.valid &&
                       scope.source_lines.first == 0 && scope.source_lines.last == 2,
                   "influence scope gives a table one shared-geometry owner");
            Expect(tableScopedGraph.propagation ==
                       note::NoteDirtyPropagation::DelimiterRegion &&
                       tableScopedGraph.stale_lines.valid &&
                       tableScopedGraph.stale_lines.first == 0 &&
                       tableScopedGraph.stale_lines.last == 2 &&
                       note::HasNoteDirtySyntaxFeature(
                           tableScopedGraph.syntax_features,
                           note::NoteDirtySyntaxFeature::BlockStructure),
                   "table influence invalidates its atomic visual region only");
        }

        {
            const std::wstring mathText = L"before\n$$\nx\n$$\nafter";
            auto [mathModel, mathDocument] = BuildMd4cModelAndDoc(mathText);
            const note::TextEdit mathEdit{{mathText.find(L"\nx\n") + 1}, 0, L"y"};
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(mathModel, mathDocument);
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                mathModel, mathDocument, mathEdit, &index);
            const note::NoteDirtyGraph mathScopedGraph = note::BuildNoteDirtyGraph(
                mathModel.raw, mathModel.line_starts, mathEdit, true, &scope);
            Expect(scope.kind == note::NoteInfluenceScopeKind::BlockMath &&
                       scope.permits_local_dirty_graph &&
                       mathScopedGraph.propagation ==
                           note::NoteDirtyPropagation::DelimiterRegion &&
                       mathScopedGraph.stale_lines.valid &&
                       mathScopedGraph.stale_lines.first == 1 &&
                       mathScopedGraph.stale_lines.last == 3 &&
                       note::HasNoteDirtySyntaxFeature(
                           mathScopedGraph.syntax_features,
                           note::NoteDirtySyntaxFeature::Math),
                   "influence scope keeps block math as one atomic render group");
        }

        {
            const std::wstring fencedCode =
                L"```md\n"
                L"alpha\n"
                L"```\n"
                L"tail";
            auto [codeModel, codeDocument] = BuildMd4cModelAndDoc(fencedCode);
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(codeModel, codeDocument);
            const size_t codeEditStart = fencedCode.find(L"alpha") + 2;
            const note::TextEdit codeEdit{{codeEditStart}, 0, L"x"};
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                codeModel, codeDocument, codeEdit, &index);
            const note::NoteDirtyGraph graph = note::BuildNoteDirtyGraph(
                codeModel.raw, codeModel.line_starts, codeEdit, true, &scope);
            Expect(scope.kind == note::NoteInfluenceScopeKind::CodeBlockContent &&
                       scope.permits_local_dirty_graph && scope.source_lines.valid &&
                       scope.source_lines.first == 1 && scope.source_lines.last == 1 &&
                       note::NoteInfluenceScopeAllowsIncrementalSyntaxPatch(scope),
                   "influence scope keeps delimiter-free fenced-code body edits on their source row");
            Expect(graph.propagation == note::NoteDirtyPropagation::LocalLine &&
                       graph.stale_lines.valid && graph.stale_lines.first == 1 &&
                       graph.stale_lines.last == 1 &&
                       graph.downstream_parser_state_proven_unchanged &&
                       note::NoteDirtyGraphAllowsRenderEarlyStop(graph, false),
                   "fenced-code body proof permits a tail fixed point after the unchanged next row");

            const size_t closingMarker = fencedCode.find(L"\n```\n") + 1;
            const note::NoteInfluenceScope markerScope = note::ResolveNoteInfluenceScope(
                codeModel, codeDocument, note::TextEdit{{closingMarker}, 1, L""}, &index);
            Expect(markerScope.kind != note::NoteInfluenceScopeKind::CodeBlockContent,
                   "editing a code fence marker never receives the code-body local proof");

            const std::wstring exposedFence =
                L"```md\n"
                L"x```\n"
                L"inside\n"
                L"```\n"
                L"tail";
            auto [exposedModel, exposedDocument] = BuildMd4cModelAndDoc(exposedFence);
            const note::NoteInfluenceIndex exposedIndex =
                note::BuildNoteInfluenceIndex(exposedModel, exposedDocument);
            const size_t exposedPrefix = exposedFence.find(L"x```");
            const note::NoteInfluenceScope exposedScope = note::ResolveNoteInfluenceScope(
                exposedModel, exposedDocument, note::TextEdit{{exposedPrefix}, 1, L""},
                &exposedIndex);
            Expect(exposedScope.kind == note::NoteInfluenceScopeKind::Unknown,
                   "deleting ordinary code text that exposes a fence marker revokes the local proof");
        }

        {
            auto [headingModel, headingDocument] = BuildMd4cModelAndDoc(L"# heading");
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(headingModel, headingDocument);
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                headingModel, headingDocument, note::TextEdit{2, 0, L"x"}, &index);
            Expect(scope.kind == note::NoteInfluenceScopeKind::Unknown &&
                       !scope.permits_local_dirty_graph,
                   "influence scope refuses ordinary text near an unproven block delimiter");
        }

        {
            auto [listModel, listDocument] = BuildMd4cModelAndDoc(L"- item");
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(listModel, listDocument);
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                listModel, listDocument, note::TextEdit{0, 0, L"x"}, &index);
            Expect(scope.kind == note::NoteInfluenceScopeKind::Container &&
                       scope.source_lines.valid && scope.source_lines.first == 0 &&
                       scope.source_lines.last == 0 &&
                       !scope.permits_local_dirty_graph,
                   "influence scope records a list's exact structural owner without local proof");
            Expect(!note::NoteInfluenceScopeAllowsIncrementalSyntaxPatch(scope),
                   "list-container influence cannot patch a syntax snapshot");
        }

        {
            const std::wstring listText =
                L"- alpha\n"
                L"- beta\n"
                L"tail";
            auto [listModel, listDocument] = BuildMd4cModelAndDoc(listText);
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(listModel, listDocument);
            const size_t bodyEditStart = listText.find(L"alpha") + 2;
            const note::TextEdit bodyEdit{{bodyEditStart}, 0, L"x"};
            const note::NoteInfluenceScope bodyScope = note::ResolveNoteInfluenceScope(
                listModel, listDocument, bodyEdit, &index);
            const note::NoteDirtyGraph bodyGraph = note::BuildNoteDirtyGraph(
                listModel.raw, listModel.line_starts, bodyEdit, true, &bodyScope);
            Expect(bodyScope.kind == note::NoteInfluenceScopeKind::ContainerContent &&
                       bodyScope.permits_local_dirty_graph &&
                       bodyScope.source_lines.valid && bodyScope.source_lines.first == 0 &&
                       bodyScope.source_lines.last == 0 &&
                       bodyScope.protected_prefix_end == 3 &&
                       note::NoteInfluenceScopeAllowsIncrementalSyntaxPatch(bodyScope) &&
                       bodyGraph.downstream_parser_state_proven_unchanged &&
                       note::NoteDirtyGraphAllowsRenderEarlyStop(bodyGraph, false),
                   "literal list-item body has a one-row fixed-point proof");

            const note::NoteInfluenceScope emptyBodyScope = note::ResolveNoteInfluenceScope(
                listModel, listDocument,
                note::TextEdit{{listText.find(L"alpha")}, 5, L""}, &index);
            Expect(emptyBodyScope.kind == note::NoteInfluenceScopeKind::Container &&
                       !emptyBodyScope.permits_local_dirty_graph,
                   "emptying a list-item body stays on the container reparse path");

            auto [styledListModel, styledListDocument] = BuildMd4cModelAndDoc(L"- **bold**");
            const note::NoteInfluenceIndex styledListIndex =
                note::BuildNoteInfluenceIndex(styledListModel, styledListDocument);
            const note::NoteInfluenceScope styledListScope = note::ResolveNoteInfluenceScope(
                styledListModel, styledListDocument, note::TextEdit{{5}, 0, L"x"},
                &styledListIndex);
            Expect(styledListScope.kind == note::NoteInfluenceScopeKind::Container &&
                       !styledListScope.permits_local_dirty_graph,
                   "markup inside a list-item body stays on the container reparse path");
        }

        {
            const std::wstring quoteText =
                L"> quoted one\n"
                L"> quoted two\n"
                L"\n"
                L"tail";
            auto [quoteModel, quoteDocument] = BuildMd4cModelAndDoc(quoteText);
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(quoteModel, quoteDocument);
            const size_t quoteEditStart = quoteText.find(L"two") + 1;
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                quoteModel, quoteDocument, note::TextEdit{{quoteEditStart}, 0, L"x"}, &index);
            Expect(scope.kind == note::NoteInfluenceScopeKind::ContainerContent &&
                       scope.source_lines.valid && scope.source_lines.first == 1 &&
                       scope.source_lines.last == 1 && scope.protected_prefix_end > 0 &&
                       scope.permits_local_dirty_graph,
                   "literal quote body has a one-row fixed-point proof");
        }

        {
            const std::wstring inlineMathText = L"formula $E=mc^2$ tail";
            auto [inlineMathModel, inlineMathDocument] = BuildMd4cModelAndDoc(inlineMathText);
            const note::NoteInfluenceIndex index =
                note::BuildNoteInfluenceIndex(inlineMathModel, inlineMathDocument);
            const note::TextEdit contentEdit{{13}, 0, L"x"};
            const note::NoteInfluenceScope contentScope = note::ResolveNoteInfluenceScope(
                inlineMathModel, inlineMathDocument, contentEdit, &index);
            const note::NoteDirtyGraph contentGraph = note::BuildNoteDirtyGraph(
                inlineMathModel.raw, inlineMathModel.line_starts, contentEdit, true, &contentScope);
            Expect(contentScope.kind == note::NoteInfluenceScopeKind::InlineMathContent &&
                       contentScope.permits_local_dirty_graph &&
                       contentScope.source_lines.valid && contentScope.source_lines.first == 0 &&
                       contentScope.source_lines.last == 0 &&
                       contentScope.protected_prefix_end == 10 &&
                       contentScope.source_node_index == 0 &&
                       note::NoteInfluenceScopeAllowsIncrementalSyntaxPatch(contentScope) &&
                       contentGraph.downstream_parser_state_proven_unchanged &&
                       note::NoteDirtyGraphAllowsRenderEarlyStop(contentGraph, false),
                   "literal inline-math content has a one-row fixed-point proof");

            const note::NoteInfluenceScope delimiterScope = note::ResolveNoteInfluenceScope(
                inlineMathModel, inlineMathDocument, note::TextEdit{{15}, 1, L""}, &index);
            Expect(delimiterScope.kind != note::NoteInfluenceScopeKind::InlineMathContent,
                   "removing an inline-math delimiter stays on the full-parse path");

            auto [commandMathModel, commandMathDocument] = BuildMd4cModelAndDoc(L"$\\alpha$");
            const note::NoteInfluenceIndex commandMathIndex =
                note::BuildNoteInfluenceIndex(commandMathModel, commandMathDocument);
            const note::NoteInfluenceScope commandMathScope = note::ResolveNoteInfluenceScope(
                commandMathModel, commandMathDocument, note::TextEdit{{7}, 0, L"x"},
                &commandMathIndex);
            Expect(commandMathScope.kind != note::NoteInfluenceScopeKind::InlineMathContent &&
                       !note::NoteInfluenceScopeAllowsIncrementalSyntaxPatch(commandMathScope),
                   "TeX command content remains on the conservative math path");
        }

        {
            auto [singleCharModel, singleCharDocument] = BuildMd4cModelAndDoc(L"x");
            const note::NoteInfluenceScope scope = note::ResolveNoteInfluenceScope(
                singleCharModel, singleCharDocument, note::TextEdit{0, 1, L""});
            Expect(scope.kind == note::NoteInfluenceScopeKind::Unknown,
                   "influence scope rejects an edit that empties a source row");
        }

        const std::wstring fenced = L"```md\nalpha\n```\ntail";
        const note::NoteDirtyGraph fencedGraph = note::BuildNoteDirtyGraph(
            fenced, note::BuildLineStarts(fenced),
            note::TextEdit{8, 0, L"x"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   fencedGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::CodeFence) &&
                   fencedGraph.propagation ==
                       note::NoteDirtyPropagation::DelimiterRegion &&
                   fencedGraph.stale_lines.first == 0 &&
                   fencedGraph.stale_lines.last == 2 &&
                   !note::NoteDirtyGraphAllowsRenderEarlyStop(fencedGraph, false),
               "dirty graph expands edits inside a code fence to its closing boundary");

        const std::wstring blockMath = L"$$\nx\n$$\ntail";
        const note::NoteDirtyGraph blockMathGraph = note::BuildNoteDirtyGraph(
            blockMath, note::BuildLineStarts(blockMath),
            note::TextEdit{4, 0, L"y"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   blockMathGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::Math) &&
                   blockMathGraph.propagation ==
                       note::NoteDirtyPropagation::DelimiterRegion &&
                   blockMathGraph.stale_lines.first == 0 &&
                   blockMathGraph.stale_lines.last == 2,
               "dirty graph expands edits inside block math to its closing boundary");

        const std::wstring inlineMath = L"before $x$ after\n";
        const note::NoteDirtyGraph inlineMathGraph = note::BuildNoteDirtyGraph(
            inlineMath, note::BuildLineStarts(inlineMath),
            note::TextEdit{9, 0, L"y"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   inlineMathGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::Math) &&
                   inlineMathGraph.propagation == note::NoteDirtyPropagation::LocalLine &&
                   inlineMathGraph.stale_lines.first == 0 &&
                   inlineMathGraph.stale_lines.last == 0,
               "dirty graph keeps balanced inline math changes on the affected line");

        const std::wstring linkText = L"[label](target)\nnext";
        const note::NoteDirtyGraph linkGraph = note::BuildNoteDirtyGraph(
            linkText, note::BuildLineStarts(linkText),
            note::TextEdit{2, 0, L"x"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   linkGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::Link) &&
                   linkGraph.propagation == note::NoteDirtyPropagation::LocalLine,
               "dirty graph marks link semantics without invalidating unrelated lines");

        const std::wstring styleText = L"<color=red>text</>\n";
        const note::NoteDirtyGraph styleGraph = note::BuildNoteDirtyGraph(
            styleText, note::BuildLineStarts(styleText),
            note::TextEdit{12, 0, L"x"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   styleGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::LegacyStyle) &&
                   styleGraph.propagation == note::NoteDirtyPropagation::LocalLine,
               "dirty graph marks balanced legacy style changes as line-local");

        const std::wstring setextCreate = L"title\n--\ntail";
        const note::NoteDirtyGraph setextCreateGraph = note::BuildNoteDirtyGraph(
            setextCreate, note::BuildLineStarts(setextCreate),
            note::TextEdit{{6}, 0, L"-"}, true);
        Expect(setextCreateGraph.structure_dirty &&
                   setextCreateGraph.propagation == note::NoteDirtyPropagation::DelimiterRegion &&
                   setextCreateGraph.stale_lines.valid &&
                   setextCreateGraph.stale_lines.first == 0 &&
                   setextCreateGraph.stale_lines.last == 1,
               "creating a Setext delimiter invalidates its unchanged preceding row");

        const std::wstring setextPredecessor = L"titl\n---\ntail";
        const note::NoteDirtyGraph setextPredecessorGraph = note::BuildNoteDirtyGraph(
            setextPredecessor, note::BuildLineStarts(setextPredecessor),
            note::TextEdit{{4}, 0, L"e"}, true);
        Expect(setextPredecessorGraph.structure_dirty &&
                   setextPredecessorGraph.propagation == note::NoteDirtyPropagation::DelimiterRegion &&
                   setextPredecessorGraph.stale_lines.valid &&
                   setextPredecessorGraph.stale_lines.first == 0 &&
                   setextPredecessorGraph.stale_lines.last == 1,
               "editing before an existing Setext delimiter invalidates the pair");

        const std::wstring tableDelimiterCreate = L"a|b\n|-\ntail";
        const note::NoteDirtyGraph tableDelimiterGraph = note::BuildNoteDirtyGraph(
            tableDelimiterCreate, note::BuildLineStarts(tableDelimiterCreate),
            note::TextEdit{{6}, 0, L"-"}, true);
        Expect(tableDelimiterGraph.structure_dirty &&
                   tableDelimiterGraph.propagation == note::NoteDirtyPropagation::DelimiterRegion &&
                   tableDelimiterGraph.stale_lines.valid &&
                   tableDelimiterGraph.stale_lines.first == 0 &&
                   tableDelimiterGraph.stale_lines.last == 1,
               "creating a table-shaped delimiter invalidates its header row");
    }

    {
        note::NoteCodeFenceRun openingFence;
        note::NoteCodeFenceRun closingFence;
        const std::wstring fenceSyntax = L"  ```cpp";
        const std::wstring closingFenceSyntax = L"````  \t";
        Expect(note::TryParseNoteCodeFenceRun(
                   fenceSyntax, {0}, {fenceSyntax.size()}, false, &openingFence) &&
                   openingFence.marker == L'`' && openingFence.marker_count == 3 &&
                   note::TryParseNoteCodeFenceRun(
                       closingFenceSyntax, {0}, {closingFenceSyntax.size()}, true, &closingFence) &&
                   closingFence.marker == L'`' && closingFence.marker_count == 4 &&
                   !note::TryParseNoteCodeFenceRun(
                       fenceSyntax, {0}, {fenceSyntax.size()}, true, &closingFence) &&
                   !note::TryParseNoteCodeFenceRun(
                       fenceSyntax, {4}, {3}, false, &closingFence),
               "parser and checkpoint share one exact fenced-code lexical rule");
        Expect(note::IsNoteBoundedLookBehindDelimiterLine(L"---\r\n") &&
                   note::IsNoteBoundedLookBehindDelimiterLine(L"|:--|--:|") &&
                   !note::IsNoteBoundedLookBehindDelimiterLine(L"- item"),
               "dirty graph and checkpoint share bounded look-behind delimiter recognition");

        // The index must retain the old boundary semantics without visiting
        // every paragraph/inline node for every source row. Deterministic
        // work limits complement the developer timing probe.
        for (const size_t paragraphs : {128u, 1024u, 4096u}) {
            std::wstring source = L"<b>closed</b>\n\n";
            for (size_t i = 0; i < paragraphs; ++i) {
                source += i % 32 == 0 ? L"$$x$$\n\n| A | B |\n| --- | --- |\n| a | b |\n\n"
                                      : L"Paragraph **bold** and `code`.\n\n";
            }
            source += L"tail text";
            note::NoteTextCore core;
            core.Reset({1180}, {}, source, 1, 1);
            const auto document = note::ParseNoteDocument(core.model());
            note::NoteParserCheckpointBuildWork work;
            const auto index = note::BuildNoteParserCheckpointIndex(core.model(), document,
                {{1180}, 1}, core.TakeSnapshot(), core.source_line_map(), &work);
            Expect(index.valid && work.indexed_intervals > paragraphs &&
                   work.boundary_queries >= index.line_count() * 8 &&
                   work.visited_index_nodes < index.line_count() * 200,
                   "long mixed-note checkpoint queries are bounded by indexed boundary work, not rows times all nodes");
            bool boundariesExact = index.valid;
            for (size_t line = 0; boundariesExact && line < core.logical_line_count(); ++line) {
                note::NoteParserLineCheckpoint checkpoint;
                boundariesExact = index.ResolveLine({line}, &checkpoint);
                const auto location = note::NoteSourceLineMap::LineAt(core.source_line_map(), {line});
                if (!boundariesExact || !location) { boundariesExact = false; break; }
                const auto next = note::NoteSourceLineMap::LineAt(core.source_line_map(), {line + 1});
                const auto verifyBoundary = [&](const note::NoteParserCheckpointState& state,
                                                 size_t position, size_t end) {
                    bool math = false;
                    for (const auto& span : document.math_spans) {
                        if (span.kind == note::MathKind::Block && span.span.start.value <= position &&
                            position < span.span.end.value) math = true;
                    }
                    size_t columns = 0;
                    note::NoteParserTableMode table = note::NoteParserTableMode::None;
                    for (const auto& block : document.blocks) {
                        if (!(position < end && block.span.start.value < end && block.span.end.value > position)) continue;
                        if (block.kind == note::BlockKind::Table && block.table_column_count > 0 && columns == 0)
                            columns = static_cast<size_t>(block.table_column_count);
                        if (block.kind == note::BlockKind::TableBody) table = note::NoteParserTableMode::Body;
                        else if (block.kind == note::BlockKind::TableHead && table == note::NoteParserTableMode::None)
                            table = note::NoteParserTableMode::Header;
                        else if (block.kind == note::BlockKind::Table && table == note::NoteParserTableMode::None)
                            table = note::NoteParserTableMode::Header;
                    }
                    return state.block_math_open == math && state.table_mode == table && state.table_column_count == columns;
                };
                // The slow oracle is intentionally confined to the small case.
                if (paragraphs == 128) boundariesExact =
                    verifyBoundary(checkpoint.entry_state, location->start.value, location->content_end.value) &&
                    verifyBoundary(checkpoint.exit_state, location->next_start.value,
                        next ? next->content_end.value : location->next_start.value);
            }
            Expect(boundariesExact, "indexed math/table boundaries agree with the independent full-scan oracle");
            const note::TextEdit removeClosingTag{{source.find(L"</b>")}, 4, L""};
            bool deletionExact = core.Apply(removeClosingTag) == note::NoteTextApplyResult::Applied;
            const auto afterDeletion = note::ParseNoteDocument(core.model());
            const auto afterIndex = note::BuildNoteParserCheckpointIndex(core.model(), afterDeletion,
                {{1180}, core.content_revision()}, core.TakeSnapshot(), core.source_line_map(), &work);
            const auto* inheritedBold = FindStyleSpan(afterDeletion, note::StyleKind::Bold);
            deletionExact = deletionExact && afterIndex.valid && inheritedBold &&
                inheritedBold->span.start.value == 3 &&
                inheritedBold->span.end.value == core.text_length() &&
                work.visited_index_nodes < afterIndex.line_count() * 200;
            Expect(deletionExact,
                   "closing-tag deletion extends its style to EOF while indexed boundary work remains bounded on long notes");
        }

        const std::wstring checkpointText =
            L"plain\n"
            L"```\n"
            L"code\n"
            L"```\n"
            L"$$\n"
            L"x\n"
            L"$$\n"
            L"\n"
            L"| Item | State |\n"
            L"| --- | --- |\n"
            L"| Markdown | Ready |\n"
            L"| Local | Reused |\n"
            L"\n"
            L"- outer\n"
            L"  - nested\n"
            L"- next\n"
            L"\n"
            L"tail";
        auto [checkpointModel, checkpointDocument] = BuildMd4cModelAndDoc(checkpointText);
        note::NoteTextPieceSequence checkpointCanonical;
        const bool checkpointRootReady = checkpointCanonical.Reset(checkpointModel.raw) ==
            note::NoteTextPieceSequenceMutationResult::Applied;
        const note::NoteDerivedSnapshotIdentity checkpointIdentity{note::NoteId{880},
                                                                    checkpointModel.revision};
        const note::NoteParserCheckpointIndex checkpoints =
            note::BuildNoteParserCheckpointIndex(
                checkpointModel, checkpointDocument, checkpointIdentity,
                checkpointCanonical.TakeSnapshot());
        std::vector<note::NoteParserLineCheckpoint> checkpointLines;
        const bool copiedCheckpointLines =
            checkpoints.CopyLinesForDifferentialTest(&checkpointLines);
        Expect(checkpointRootReady && checkpoints.valid &&
                   note::NoteParserCheckpointIndexMatchesTextModel(
                       checkpoints, checkpointModel, checkpointIdentity) &&
                   copiedCheckpointLines &&
                   checkpointLines.size() == checkpointModel.line_starts.size(),
               "parser checkpoint snapshot is revision-bound to its exact source rows");
        Expect(checkpointLines.size() > 6 &&
                   checkpointLines[2].entry_state.code_fence_open &&
                   checkpointLines[2].entry_state.code_fence_marker == L'`' &&
                   checkpointLines[2].entry_state.code_fence_marker_count == 3 &&
                   checkpointLines[5].entry_state.block_math_open &&
                   checkpointLines[5].entry_state.block_math_delimiter ==
                       note::MathDelimiter::DoubleDollar,
               "parser checkpoints retain fenced-code and block-math entry state");

        bool observedTable = false;
        bool observedContainer = false;
        for (const note::NoteParserLineCheckpoint& checkpoint : checkpointLines) {
            observedTable = observedTable ||
                checkpoint.entry_state.table_mode != note::NoteParserTableMode::None ||
                checkpoint.exit_state.table_mode != note::NoteParserTableMode::None;
            observedContainer = observedContainer ||
                !checkpoint.entry_state.container_stack.empty() ||
                !checkpoint.exit_state.container_stack.empty();
        }
        Expect(observedTable && observedContainer,
               "parser checkpoints retain table mode and nested container state for later local parsing");

        const size_t tailLine = checkpointLines.empty() ? 0 : checkpointLines.size() - 1;
        Expect(note::NoteParserCheckpointCanReuseSuffix(
                   checkpoints, note::LineIndex{tailLine}, checkpoints, note::LineIndex{tailLine},
                   checkpointCanonical.TakeSnapshot(),
                   note::NoteParserLayoutKey{17}, note::NoteParserLayoutKey{17}),
               "a shared plain suffix with equal parser state and layout key reaches a fixed point");

        Expect(!note::NoteParserCheckpointCanReuseSuffix(
                   checkpoints, note::LineIndex{tailLine}, checkpoints, note::LineIndex{tailLine},
                   checkpointCanonical.TakeSnapshot(),
                   note::NoteParserLayoutKey{17}, note::NoteParserLayoutKey{18}),
               "a layout-key change forbids parser suffix reuse even with equal text");

        note::NoteTextPieceSequence unrelatedCanonical;
        const bool unrelatedRootReady = unrelatedCanonical.Reset(checkpointModel.raw) ==
            note::NoteTextPieceSequenceMutationResult::Applied;
        Expect(!note::NoteParserCheckpointCanReuseSuffix(
                   checkpoints, note::LineIndex{tailLine}, checkpoints, note::LineIndex{tailLine},
                   unrelatedCanonical.TakeSnapshot(),
                   note::NoteParserLayoutKey{17}, note::NoteParserLayoutKey{17}),
               "a separately published but text-equal canonical root forbids suffix reuse");
        Expect(unrelatedRootReady,
               "checkpoint test can construct an unrelated canonical root");

        note::NoteParserCheckpointIndex stateChanged;
        note::NoteParserLineCheckpoint alteredTailLine;
        const bool alteredTailResolved = checkpoints.ResolveLine({tailLine}, &alteredTailLine);
        if (alteredTailResolved) {
            alteredTailLine.entry_state.has_bounded_lookbehind_line =
                !alteredTailLine.entry_state.has_bounded_lookbehind_line;
        }
        Expect(alteredTailResolved && checkpoints.ReplaceLineForDifferentialTest(
                   {tailLine}, alteredTailLine, &stateChanged) &&
                   !note::NoteParserCheckpointCanReuseSuffix(
                   checkpoints, note::LineIndex{tailLine}, stateChanged, note::LineIndex{tailLine},
                   checkpointCanonical.TakeSnapshot(),
                   note::NoteParserLayoutKey{17}, note::NoteParserLayoutKey{17}),
               "a changed inherited parser state forbids parser suffix reuse");

        note::NoteParserCheckpointIndex indentChanged;
        note::NoteParserLineCheckpoint alteredIndentLine;
        const bool alteredIndentResolved = checkpoints.ResolveLine({tailLine}, &alteredIndentLine);
        if (alteredIndentResolved) {
            alteredIndentLine.entry_state.inherited_indent_values.push_back(L"2");
        }
        Expect(alteredIndentResolved && checkpoints.ReplaceLineForDifferentialTest(
                   {tailLine}, alteredIndentLine, &indentChanged) &&
                   !note::NoteParserCheckpointCanReuseSuffix(
                       checkpoints, {tailLine}, indentChanged, {tailLine},
                       checkpointCanonical.TakeSnapshot(), {17}, {17}),
               "changed inherited indentation forbids suffix reuse despite identical text");

        size_t tableLine = 0;
        while (tableLine < checkpointLines.size() &&
               checkpointLines[tableLine].entry_state.table_mode == note::NoteParserTableMode::None &&
               checkpointLines[tableLine].exit_state.table_mode == note::NoteParserTableMode::None) {
            ++tableLine;
        }
        Expect(tableLine < checkpointLines.size() &&
                   !note::NoteParserCheckpointCanReuseSuffix(
                       checkpoints, note::LineIndex{tableLine}, checkpoints,
                       note::LineIndex{tableLine}, checkpointCanonical.TakeSnapshot(), note::NoteParserLayoutKey{17},
                       note::NoteParserLayoutKey{17}),
               "an unimplemented shared-table state remains on the conservative fallback path");

        note::NoteDocument staleDocument = checkpointDocument;
        staleDocument.source_identity.source_revision = checkpointModel.revision + 1;
        const note::NoteParserCheckpointIndex staleCheckpoints =
            note::BuildNoteParserCheckpointIndex(
                checkpointModel, staleDocument, checkpointIdentity,
                checkpointCanonical.TakeSnapshot());
        Expect(!staleCheckpoints.valid,
               "parser checkpoint construction rejects a stale immutable syntax snapshot");

        note::NoteMetadata checkpointMeta;
        checkpointMeta.file_name = L"checkpoint.md";
        note::NoteTextModel oldSuffixModel = note::MakeNoteTextModel(
            checkpointMeta, L"prefix\nshared suffix", 70);
        note::NoteTextPieceSequence sharedCanonical;
        const bool sharedRootReady = sharedCanonical.Reset(oldSuffixModel.raw) ==
            note::NoteTextPieceSequenceMutationResult::Applied;
        const note::NoteDerivedSnapshotIdentity oldSuffixIdentity{note::NoteId{881}, 70};
        const note::NoteParserCheckpointIndex oldSuffixCheckpoints =
            note::BuildNoteParserCheckpointIndex(
                oldSuffixModel, note::ParseNoteDocument(oldSuffixModel), oldSuffixIdentity,
                sharedCanonical.TakeSnapshot());
        const note::TextEdit prefixEdit{{2}, 0, L"X"};
        note::ApplyTextEdit(&oldSuffixModel, prefixEdit);
        const bool sharedApply = sharedCanonical.Apply(prefixEdit) ==
            note::NoteTextPieceSequenceMutationResult::Applied;
        const note::NoteDerivedSnapshotIdentity newSuffixIdentity{note::NoteId{881}, 71};
        const note::NoteParserCheckpointIndex newSuffixCheckpoints =
            note::BuildNoteParserCheckpointIndex(
                oldSuffixModel, note::ParseNoteDocument(oldSuffixModel), newSuffixIdentity,
                sharedCanonical.TakeSnapshot());
        note::NoteParserLineCheckpoint oldSuffixLine;
        note::NoteParserLineCheckpoint newSuffixLine;
        const bool resolvedSuffixLines =
            oldSuffixCheckpoints.ResolveLine({1}, &oldSuffixLine) &&
            newSuffixCheckpoints.ResolveLine({1}, &newSuffixLine);
        const bool suffixRootMatches = newSuffixCheckpoints.valid &&
            sharedCanonical.MatchesSnapshot(newSuffixCheckpoints.canonical_source_root);
        const bool suffixLineRangeShares = resolvedSuffixLines &&
            sharedCanonical.SharesExactRange(
                oldSuffixCheckpoints.canonical_source_root,
                oldSuffixLine.source_line.start,
                newSuffixLine.source_line.start,
                oldSuffixLine.source_line.end - oldSuffixLine.source_line.start);
        const bool suffixStatesMatch = resolvedSuffixLines &&
            oldSuffixLine.entry_state == newSuffixLine.entry_state &&
            oldSuffixLine.exit_state == newSuffixLine.exit_state;
        Expect(suffixRootMatches && suffixLineRangeShares && suffixStatesMatch,
               "shifted plain suffix retains its canonical root, exact range, and parser state");
        Expect(sharedRootReady && sharedApply && oldSuffixCheckpoints.valid &&
                   newSuffixCheckpoints.valid &&
                   note::NoteParserCheckpointCanReuseSuffix(
                       oldSuffixCheckpoints, note::LineIndex{1}, newSuffixCheckpoints,
                       note::LineIndex{1}, sharedCanonical.TakeSnapshot(), note::NoteParserLayoutKey{17},
                       note::NoteParserLayoutKey{17}),
               "a shifted unchanged suffix is reused only through exact canonical backing identity");

        note::NoteTextModel oldLookBehindModel = note::MakeNoteTextModel(
            checkpointMeta, L"title\n---\ntail", 80);
        note::NoteTextPieceSequence lookBehindCanonical;
        const bool lookBehindRootReady = lookBehindCanonical.Reset(oldLookBehindModel.raw) ==
            note::NoteTextPieceSequenceMutationResult::Applied;
        const note::NoteDerivedSnapshotIdentity oldLookBehindIdentity{note::NoteId{882}, 80};
        const note::NoteParserCheckpointIndex oldLookBehindCheckpoints =
            note::BuildNoteParserCheckpointIndex(
                oldLookBehindModel, note::ParseNoteDocument(oldLookBehindModel),
                oldLookBehindIdentity, lookBehindCanonical.TakeSnapshot());
        const note::TextEdit titleEdit{{2}, 0, L"X"};
        note::ApplyTextEdit(&oldLookBehindModel, titleEdit);
        const bool lookBehindApply = lookBehindCanonical.Apply(titleEdit) ==
            note::NoteTextPieceSequenceMutationResult::Applied;
        const note::NoteDerivedSnapshotIdentity newLookBehindIdentity{note::NoteId{882}, 81};
        const note::NoteParserCheckpointIndex newLookBehindCheckpoints =
            note::BuildNoteParserCheckpointIndex(
                oldLookBehindModel, note::ParseNoteDocument(oldLookBehindModel),
                newLookBehindIdentity, lookBehindCanonical.TakeSnapshot());
        Expect(lookBehindRootReady && lookBehindApply && oldLookBehindCheckpoints.valid &&
                   newLookBehindCheckpoints.valid &&
                   !note::NoteParserCheckpointCanReuseSuffix(
                       oldLookBehindCheckpoints, note::LineIndex{1}, newLookBehindCheckpoints,
                       note::LineIndex{1}, lookBehindCanonical.TakeSnapshot(), note::NoteParserLayoutKey{17},
                       note::NoteParserLayoutKey{17}),
               "a Setext delimiter cannot reuse its suffix when its look-behind row changed");
    }

    {
        const note::NoteRenderIndexApplyRequest validRequest{
            note::NoteDerivedSnapshotIdentity{note::NoteId{901}, 70},
            note::NoteDerivedSnapshotIdentity{note::NoteId{901}, 71},
            note::TextEdit{{4}, 2, L"xyz"},
            12,
            13,
        };
        Expect(note::ValidateNoteRenderIndexApplyRequest(validRequest) ==
                   note::NoteRenderIndexApplyRequestValidation::Valid,
               "render index request accepts one exact adjacent canonical edit");

        note::NoteRenderIndexApplyRequest wrongOwner = validRequest;
        wrongOwner.after_source.note_id = note::NoteId{902};
        Expect(note::ValidateNoteRenderIndexApplyRequest(wrongOwner) ==
                   note::NoteRenderIndexApplyRequestValidation::OwnerMismatch,
               "render index request rejects a cache from another note");

        note::NoteRenderIndexApplyRequest skippedRevision = validRequest;
        skippedRevision.after_source.source_revision = 72;
        Expect(note::ValidateNoteRenderIndexApplyRequest(skippedRevision) ==
                   note::NoteRenderIndexApplyRequestValidation::NonAdjacentRevision,
               "render index request rejects a transaction it cannot map exactly");

        note::NoteRenderIndexApplyRequest invalidRange = validRequest;
        invalidRange.edit.start = {11};
        invalidRange.edit.deleted_len = 2;
        Expect(note::ValidateNoteRenderIndexApplyRequest(invalidRange) ==
                   note::NoteRenderIndexApplyRequestValidation::InvalidEditRange,
               "render index request rejects an out-of-range edit before mutation");

        note::NoteRenderIndexApplyRequest wrongLength = validRequest;
        wrongLength.after_text_length = 12;
        Expect(note::ValidateNoteRenderIndexApplyRequest(wrongLength) ==
                   note::NoteRenderIndexApplyRequestValidation::LengthMismatch,
               "render index request rejects text lengths inconsistent with its edit");
    }

    {
        note::NoteSourceEditCoordinateMap replacementMap;
        const note::NoteSourceEditCoordinateMapBuildResult replacementBuild =
            note::NoteSourceEditCoordinateMap::Build(
                10, note::TextEdit{{3}, 2, L"wxyz"}, &replacementMap);
        note::Span mappedPrefix{};
        note::Span mappedSuffix{};
        note::Span rejectedSpan{{88}, {99}};
        note::Utf16CodeUnitOffset startBefore{};
        note::Utf16CodeUnitOffset startAfter{};
        note::Utf16CodeUnitOffset endBefore{};
        note::Utf16CodeUnitOffset endAfter{};
        const bool mapsReplacementEdges =
            replacementMap.MapUnchangedSpan({{1}, {3}}, &mappedPrefix) &&
            replacementMap.MapUnchangedSpan({{5}, {10}}, &mappedSuffix) &&
            replacementMap.MapUnchangedBoundary(
                {3}, note::NoteSourceEditBoundarySide::BeforeReplacement, &startBefore) &&
            replacementMap.MapUnchangedBoundary(
                {3}, note::NoteSourceEditBoundarySide::AfterReplacement, &startAfter) &&
            replacementMap.MapUnchangedBoundary(
                {5}, note::NoteSourceEditBoundarySide::BeforeReplacement, &endBefore) &&
            replacementMap.MapUnchangedBoundary(
                {5}, note::NoteSourceEditBoundarySide::AfterReplacement, &endAfter);
        Expect(replacementBuild == note::NoteSourceEditCoordinateMapBuildResult::Built &&
                   replacementMap.valid() && replacementMap.new_source_length() == 12 &&
                   mapsReplacementEdges &&
                   mappedPrefix.start.value == 1 && mappedPrefix.end.value == 3 &&
                   mappedSuffix.start.value == 7 && mappedSuffix.end.value == 12 &&
                   startBefore.value == 3 && startAfter.value == 7 &&
                   endBefore.value == 3 && endAfter.value == 7 &&
                   !replacementMap.MapUnchangedSpan({{2}, {6}}, &rejectedSpan) &&
                   rejectedSpan.start.value == 88 && rejectedSpan.end.value == 99,
               "an exact source-edit coordinate map preserves only proven prefix/suffix spans and requires an explicit replacement edge");

        note::Utf16CodeUnitOffset inversePrefix{};
        note::Utf16CodeUnitOffset inverseSuffix{};
        note::Utf16CodeUnitOffset inverseReplacement{};
        note::Span inverseSuffixSpan{};
        const bool mapsNewReplacementEdges =
            replacementMap.MapUnchangedNewBoundary(
                {2}, note::NoteSourceEditBoundarySide::BeforeReplacement, &inversePrefix) &&
            inversePrefix.value == 2 &&
            replacementMap.MapUnchangedNewBoundary(
                {8}, note::NoteSourceEditBoundarySide::AfterReplacement, &inverseSuffix) &&
            inverseSuffix.value == 6 &&
            !replacementMap.MapUnchangedNewBoundary(
                {5}, note::NoteSourceEditBoundarySide::AfterReplacement, &inverseReplacement) &&
            replacementMap.MapUnchangedNewSpan({{7}, {10}}, &inverseSuffixSpan) &&
            inverseSuffixSpan.start.value == 5 && inverseSuffixSpan.end.value == 8 &&
            !replacementMap.MapUnchangedNewSpan({{4}, {7}}, &inverseSuffixSpan);
        Expect(mapsNewReplacementEdges,
               "an inverse source-edit map refuses transient replacement text while recovering unchanged editor prefix and suffix coordinates");

        note::NoteTextCore imePreeditCore;
        imePreeditCore.Reset(
            note::NoteId{601}, note::NoteMetadata{L"ime-preedit.md", L"ime-preedit"},
            L"before\nabcdef\nafter", 73, 4);
        note::NoteRenderFinalImePreeditInput imePreeditInput;
        imePreeditInput.canonical_replacement_span = {{9}, {12}};
        imePreeditInput.composition_text = L"文Z";
        imePreeditInput.editor_selection = {{10}, {11}};
        note::NoteRenderFinalImePreeditPresentation imePreedit;
        const note::NoteRenderFinalImePreeditPresentationBuildResult imePreeditResult =
            note::NoteRenderFinalImePreeditPresentation::Build(
                imePreeditCore, imePreeditInput, &imePreedit);
        note::Utf16CodeUnitOffset mappedCommittedLineEnd{};
        note::Utf16CodeUnitOffset mappedEditorLineEnd{};
        note::Utf16CodeUnitOffset rejectedCompositionOffset{};
        const bool imePreeditExact =
            imePreeditResult == note::NoteRenderFinalImePreeditPresentationBuildResult::Built &&
            imePreedit.valid() && imePreedit.Matches(imePreeditCore) &&
            imePreedit.line_index() == note::LineIndex{1} &&
            imePreedit.editor_replacement_span().start == note::Utf16CodeUnitOffset{9} &&
            imePreedit.editor_replacement_span().end == note::Utf16CodeUnitOffset{11} &&
            imePreedit.editor_line_span().start == note::Utf16CodeUnitOffset{7} &&
            imePreedit.editor_line_span().end == note::Utf16CodeUnitOffset{12} &&
            imePreedit.temporary_raw_line() == L"ab文Zf" &&
            imePreedit.MapCanonicalUnchangedBoundary(
                {13}, note::NoteSourceEditBoundarySide::AfterReplacement,
                &mappedEditorLineEnd) &&
            mappedEditorLineEnd == note::Utf16CodeUnitOffset{12} &&
            imePreedit.MapEditorUnchangedBoundary(
                {12}, note::NoteSourceEditBoundarySide::AfterReplacement,
                &mappedCommittedLineEnd) &&
            mappedCommittedLineEnd == note::Utf16CodeUnitOffset{13} &&
            !imePreedit.MapEditorUnchangedBoundary(
                {10}, note::NoteSourceEditBoundarySide::AfterReplacement,
                &rejectedCompositionOffset) &&
            imePreeditCore.RangeMatches({7}, L"abcdef");
        Expect(imePreeditExact,
               "IME preedit keeps canonical text immutable while supplying one exact temporary raw line and coordinate boundary");
        using ImeAttribute = note::NoteImeCharacterAttribute;
        auto attributedImeInput = imePreeditInput;
        attributedImeInput.composition_attributes = {ImeAttribute::Input, ImeAttribute::TargetConverted};
        attributedImeInput.composition_clauses = {{0}, {1}, {2}};
        note::NoteRenderFinalImePreeditPresentation attributedIme;
        Expect(note::NoteRenderFinalImePreeditPresentation::Build(
                   imePreeditCore, attributedImeInput, &attributedIme) ==
                   note::NoteRenderFinalImePreeditPresentationBuildResult::Built &&
                   attributedIme.segments().size() == 2 &&
                   attributedIme.segments()[0].editor_span.start == note::Utf16CodeUnitOffset{9} &&
                   attributedIme.segments()[1].editor_span.end == note::Utf16CodeUnitOffset{11} &&
                   attributedIme.segments()[1].attribute == ImeAttribute::TargetConverted &&
                   imePreeditCore.RangeMatches({7}, L"abcdef"),
               "IME attributes and UTF-16 clauses become exact temporary editor segments without canonical styles");
        auto malformedImeInput = attributedImeInput;
        const auto rejectsImeMetadata = [&]() {
            return note::NoteRenderFinalImePreeditPresentation::Build(
                imePreeditCore, malformedImeInput, &attributedIme) ==
                note::NoteRenderFinalImePreeditPresentationBuildResult::InvalidCompositionMetadata &&
                attributedIme.valid() && attributedIme.segments().size() == 2;
        };
        malformedImeInput.composition_attributes.pop_back();
        bool rejectedImeMetadata = rejectsImeMetadata();
        malformedImeInput = attributedImeInput;
        malformedImeInput.composition_attributes[1] = static_cast<ImeAttribute>(255);
        rejectedImeMetadata = rejectedImeMetadata && rejectsImeMetadata();
        malformedImeInput = attributedImeInput;
        malformedImeInput.composition_clauses = {{0}, {1}, {1}, {2}};
        rejectedImeMetadata = rejectedImeMetadata && rejectsImeMetadata();
        malformedImeInput.composition_clauses = {{1}, {2}};
        rejectedImeMetadata = rejectedImeMetadata && rejectsImeMetadata();
        malformedImeInput = attributedImeInput;
        malformedImeInput.composition_text = L"\xD83D\xDE00";
        rejectedImeMetadata = rejectedImeMetadata && rejectsImeMetadata();
        malformedImeInput.composition_text = L"e\x0301";
        rejectedImeMetadata = rejectedImeMetadata && rejectsImeMetadata();
        Expect(rejectedImeMetadata,
               "malformed IME attributes/clauses or split graphemes are rejected atomically without replacing the prior presentation");
        note::NoteRenderFinalImePreeditSession imeSession;
        const note::NoteDerivedSnapshotIdentity imeSessionSource{imePreeditCore.note_id(),
                                                                imePreeditCore.content_revision()};
        using ImeUpdateResult = note::NoteRenderFinalImePreeditUpdateResult;
        const bool beganImeSession = imeSession.Begin(
            imeSessionSource, imePreeditInput.canonical_replacement_span);
        Expect(beganImeSession && imeSession.awaiting_first_payload() && !imeSession.has_payload(),
               "IME start without COMPSTR is an explicit initial phase, not an empty replacement or failed read");
        const bool cursorOnlyUpdate = beganImeSession &&
            imeSession.Update(L"文Z", {2}) == ImeUpdateResult::Updated &&
            imeSession.Update(L"文Z", {0}) == ImeUpdateResult::Updated &&
            imeSession.composition_text() == L"文Z" &&
            imeSession.composition_caret() == note::Utf16CodeUnitOffset{0};
        Expect(cursorOnlyUpdate && !imeSession.awaiting_first_payload(),
               "IME candidate cursor-only updates preserve their text and canonical replacement anchor");
        Expect(imeSession.Update(L"文Z", {0}, {ImeAttribute::Input, ImeAttribute::Converted}, {{0}, {2}}) ==
                   ImeUpdateResult::Updated &&
                   imeSession.Update(L"文Z", {0}, {ImeAttribute::Input, ImeAttribute::TargetConverted}, {{0}, {1}, {2}}) ==
                   ImeUpdateResult::Updated &&
                   imeSession.composition_attributes()[1] == ImeAttribute::TargetConverted &&
                   imeSession.composition_clauses().size() == 3,
               "attribute/clause-only IME updates atomically replace candidate metadata on unchanged preedit text");
        const auto sessionAnchorPreserved = [&]() {
            const note::Span replacement = imeSession.canonical_replacement();
            return imeSession.active() && imeSession.source_identity() == imeSessionSource &&
                replacement.start == imePreeditInput.canonical_replacement_span.start &&
                replacement.end == imePreeditInput.canonical_replacement_span.end;
        };
        imeSession.Suspend(); // unavailable or inconsistent IMM snapshot
        const bool recoveredImeRead = sessionAnchorPreserved() &&
            !imeSession.awaiting_first_payload() && !imeSession.has_payload() &&
            imeSession.composition_text().empty() &&
            imeSession.Update(L"文章", {1}) == ImeUpdateResult::Updated;
        Expect(recoveredImeRead,
               "a transient IMM snapshot failure suspends only preedit payload and the next valid read resumes the same session");
        Expect(imeSession.Update(L"", {0}) == ImeUpdateResult::UpdatedEmptyComposition &&
                   sessionAnchorPreserved() && !imeSession.awaiting_first_payload() &&
                   imeSession.has_payload() && imeSession.composition_text().empty() &&
                   imeSession.Update(L"文Z", {1}) == ImeUpdateResult::Updated &&
                   imeSession.Update(L"文Z", {3}) == ImeUpdateResult::InvalidComposition &&
                   sessionAnchorPreserved() && imeSession.composition_text().empty() &&
                   imeSession.Update(L"a\nb", {1}) == ImeUpdateResult::InvalidComposition &&
                   sessionAnchorPreserved() &&
                   imeSession.Update(L"文Z", {2}) == ImeUpdateResult::Updated &&
                   imePreeditCore.RangeMatches({7}, L"abcdef"),
               "known empty preedit and invalid-payload suspensions preserve the anchor and never modify canonical text");
        note::NoteRenderFinalImePreeditInput emptyImeInput = imePreeditInput;
        emptyImeInput.composition_text.clear();
        emptyImeInput.editor_selection = {{9}, {9}};
        note::NoteRenderFinalImePreeditPresentation emptyImePresentation;
        Expect(note::NoteRenderFinalImePreeditPresentation::Build(
                   imePreeditCore, emptyImeInput, &emptyImePresentation) ==
                   note::NoteRenderFinalImePreeditPresentationBuildResult::Built &&
                   emptyImePresentation.temporary_raw_line() == L"abf" &&
                   imePreeditCore.RangeMatches({7}, L"abcdef"),
               "an empty IME replacement has one exact temporary raw row without deleting canonical selection text");
        imeSession.Reset();
        Expect(!imeSession.active() && !imeSession.awaiting_first_payload() &&
                   imeSession.Update(L"residue", {1}) == ImeUpdateResult::InactiveSession &&
                   !imeSession.Begin({}, {{9}, {12}}) &&
                   !imeSession.Begin(imeSessionSource, {{12}, {9}}),
               "IME result/end/focus reset prevents retained composition text from reopening a closed session");
        note::NoteRenderFinalImePreeditSession continuationSession;
        Expect(continuationSession.Begin(imeSessionSource, {{9}, {12}}),
               "continuing IME results begin from an exact canonical replacement");
        note::NoteImeCommittedContinuation continuation;
        using ContinuationResult = note::NoteImeCommittedContinuationResult;
        Expect(note::PrepareNoteImeCommittedContinuation(imePreeditCore, continuationSession,
                   L"確", L"未", L"before\nab確未f\nafter", &continuation) == ContinuationResult::Prepared &&
                   continuation.committed_edit.start == note::Utf16CodeUnitOffset{9} &&
                   continuation.committed_edit.deleted_len == 3 &&
                   continuation.committed_edit.inserted_text == L"確" &&
                   continuation.committed_caret == note::Utf16CodeUnitOffset{10} &&
                   imePreeditCore.RangeMatches({7}, L"abcdef"),
               "a mixed result/preedit observation proposes only the committed result without mutating canonical text");
        bool mismatchesRejected = true;
        for (const auto* observed : {L"Before\nab確未f\nafter", L"before\nab確未X\nafter",
                                    L"before\nab別未f\nafter", L"before\nab確別f\nafter",
                                    L"before\nab確未ff\nafter"}) {
            mismatchesRejected = mismatchesRejected &&
                note::PrepareNoteImeCommittedContinuation(imePreeditCore, continuationSession,
                    L"確", L"未", observed, &continuation) == ContinuationResult::ObservedTextMismatch &&
                continuation.committed_edit.inserted_text == L"確";
        }
        Expect(mismatchesRejected,
               "a changed prefix, suffix, result, preedit or length cannot publish partial IME input and preserves the prior proposal");
        note::NoteTextCore continuationCore;
        continuationCore.Reset(note::NoteId{601}, note::NoteMetadata{L"ime-preedit.md", L"ime-preedit"},
                               L"before\nabcdef\nafter", 73, 4);
        Expect(continuationCore.Apply(continuation.committed_edit) == note::NoteTextApplyResult::Applied &&
                   continuationCore.MatchesRaw(L"before\nab確f\nafter") &&
                   note::PrepareNoteImeCommittedContinuation(continuationCore, continuationSession,
                       L"定", L"", L"before\nab確定f\nafter", &continuation) == ContinuationResult::StaleSource,
               "the committed result excludes the remaining preedit and invalidates the old source anchor");
        Expect(continuationSession.Begin({continuationCore.note_id(), continuationCore.content_revision()}, {{10}, {10}}) &&
                   note::PrepareNoteImeCommittedContinuation(continuationCore, continuationSession,
                       L"定", L"", L"before\nab確定f\nafter", &continuation) == ContinuationResult::Prepared &&
                   continuationCore.Apply(continuation.committed_edit) == note::NoteTextApplyResult::Applied &&
                   continuationCore.MatchesRaw(L"before\nab確定f\nafter"),
               "successive confirmed portions reanchor exactly after the preceding result, including empty remaining preedit");
        Expect(note::PrepareNoteImeCommittedContinuation(imePreeditCore, continuationSession,
                   L"確", L"未", L"before\nab確未f\nafter", nullptr) == ContinuationResult::InvalidOutput,
               "continuing IME preparation reports a missing output explicitly");
        Expect(continuationSession.Begin(imeSessionSource, {{9}, {12}}) &&
                   note::PrepareNoteImeCommittedContinuation(imePreeditCore, continuationSession,
                       L"確\n定", L"未", L"before\nab確\n定未f\nafter", &continuation) == ContinuationResult::Prepared &&
                   continuation.committed_edit.inserted_text == L"確\n定" &&
                   note::PrepareNoteImeCommittedContinuation(imePreeditCore, continuationSession,
                       L"確\r\n定", L"未", L"before\nab確\n定未f\nafter", &continuation) == ContinuationResult::InvalidRange,
               "confirmed multiline input uses canonical LF coordinates and rejects unnormalized native CRLF");
        const note::NoteRenderFinalImePreeditPresentation preservedImePreedit = imePreedit;
        note::NoteRenderFinalImePreeditInput multilineImePreedit = imePreeditInput;
        multilineImePreedit.composition_text = L"x\ny";
        note::NoteRenderFinalImePreeditInput crossLineImePreedit = imePreeditInput;
        crossLineImePreedit.canonical_replacement_span = {{9}, {14}};
        Expect(note::NoteRenderFinalImePreeditPresentation::Build(
                   imePreeditCore, multilineImePreedit, &imePreedit) ==
                   note::NoteRenderFinalImePreeditPresentationBuildResult::ContainsLineBreak &&
                   note::NoteRenderFinalImePreeditPresentation::Build(
                       imePreeditCore, crossLineImePreedit, &imePreedit) ==
                       note::NoteRenderFinalImePreeditPresentationBuildResult::MultipleLogicalLines &&
                   imePreedit.valid() &&
                   imePreedit.temporary_raw_line() == preservedImePreedit.temporary_raw_line(),
               "IME preedit rejects line-breaking or multi-row composition without replacing the published temporary surface");

        note::NoteSourceEditCoordinateMap insertionMap;
        note::Utf16CodeUnitOffset insertionBefore{};
        note::Utf16CodeUnitOffset insertionAfter{};
        note::Span insertionSuffix{};
        const note::NoteSourceEditCoordinateMapBuildResult insertionBuild =
            note::NoteSourceEditCoordinateMap::Build(
                8, note::TextEdit{{4}, 0, L"xy"}, &insertionMap);
        Expect(insertionBuild == note::NoteSourceEditCoordinateMapBuildResult::Built &&
                   insertionMap.MapUnchangedBoundary(
                       {4}, note::NoteSourceEditBoundarySide::BeforeReplacement,
                       &insertionBefore) &&
                   insertionMap.MapUnchangedBoundary(
                       {4}, note::NoteSourceEditBoundarySide::AfterReplacement,
                       &insertionAfter) &&
                   insertionMap.MapUnchangedSpan({{4}, {8}}, &insertionSuffix) &&
                   insertionBefore.value == 4 && insertionAfter.value == 6 &&
                   insertionSuffix.start.value == 6 && insertionSuffix.end.value == 10,
               "an insertion maps the shared suffix after its new replacement while retaining the before-insertion boundary");

        const note::NoteSourceEditCoordinateMap preservedCoordinateMap = replacementMap;
        Expect(note::NoteSourceEditCoordinateMap::Build(
                   10, note::TextEdit{{11}, 0, L"x"}, &replacementMap) ==
                   note::NoteSourceEditCoordinateMapBuildResult::InvalidEditRange &&
                   replacementMap.valid() &&
                   replacementMap.old_source_length() ==
                       preservedCoordinateMap.old_source_length() &&
                   replacementMap.new_source_length() ==
                       preservedCoordinateMap.new_source_length(),
               "a rejected coordinate-map build keeps the prior immutable mapping available to its caller");
    }

    {
        note::NoteSourceLineMap::Snapshot initial;
        Expect(note::NoteSourceLineMap::Build(L"alpha\r\nbeta\ngamma", &initial) &&
                   initial.valid() && initial.line_count() == 3 && initial.text_length() == 17,
               "persistent source-line map builds canonical CRLF/LF row boundaries");
        const auto initialFirst = note::NoteSourceLineMap::LineAt(initial, {0});
        const auto initialLast = note::NoteSourceLineMap::LineAt(initial, {2});
        Expect(initialFirst.has_value() && initialLast.has_value() &&
                   initialFirst->content_end.value == 5 && initialFirst->next_start.value == 7 &&
                   initialLast->start.value == 12 && initialLast->next_start.value == 17 &&
                   note::NoteSourceLineMap::FindByOffset(initial, {6}).has_value() &&
                   note::NoteSourceLineMap::FindByOffset(initial, {6})->line_index.value == 0 &&
                   note::NoteSourceLineMap::FindByOffset(initial, {7}).has_value() &&
                   note::NoteSourceLineMap::FindByOffset(initial, {7})->line_index.value == 1,
               "persistent source-line map resolves CRLF transport and EOF positions from aggregates");
        const auto richAtCanonicalBeforeCrlf =
            note::NoteSourceLineMap::CanonicalOffsetToRichEditIndex(initial, {5});
        const auto richAtCanonicalWithinCrlf =
            note::NoteSourceLineMap::CanonicalOffsetToRichEditIndex(initial, {6});
        const auto richAtCanonicalAfterCrlf =
            note::NoteSourceLineMap::CanonicalOffsetToRichEditIndex(initial, {7});
        const auto richAtCanonicalAfterLf =
            note::NoteSourceLineMap::CanonicalOffsetToRichEditIndex(initial, {12});
        const auto canonicalAtRichAfterCrlf =
            note::NoteSourceLineMap::RichEditIndexToCanonicalOffset(initial, 6);
        const auto canonicalAtRichAfterLf =
            note::NoteSourceLineMap::RichEditIndexToCanonicalOffset(initial, 11);
        const auto canonicalAtRichEof =
            note::NoteSourceLineMap::RichEditIndexToCanonicalOffset(initial, 16);
        Expect(initial.rich_edit_text_length() == 16 &&
                   richAtCanonicalBeforeCrlf == std::optional<size_t>{5} &&
                   richAtCanonicalWithinCrlf == std::optional<size_t>{6} &&
                   richAtCanonicalAfterCrlf == std::optional<size_t>{6} &&
                   richAtCanonicalAfterLf == std::optional<size_t>{11} &&
                   canonicalAtRichAfterCrlf == std::optional<note::Utf16CodeUnitOffset>{{7}} &&
                   canonicalAtRichAfterLf == std::optional<note::Utf16CodeUnitOffset>{{12}} &&
                   canonicalAtRichEof == std::optional<note::Utf16CodeUnitOffset>{{17}},
               "persistent source-line map converts canonical and RichEdit CRLF coordinates without text access");

        note::NoteTextCore sourceCore;
        sourceCore.Reset(note::NoteId{944}, note::NoteMetadata{L"rows.md", L"rows"},
                         L"alpha\r\nbeta\ngamma", 20, 3);
        const note::NoteSourceLineMap::Snapshot beforeEdit = sourceCore.source_line_map();
        const note::NoteTextApplyResult inserted = sourceCore.Apply({{11}, 0, L"++"});
        const auto shiftedTail = note::NoteSourceLineMap::LineAt(
            sourceCore.source_line_map(), {2});
        const auto preservedOldTail = note::NoteSourceLineMap::LineAt(beforeEdit, {2});
        Expect(inserted == note::NoteTextApplyResult::Applied &&
                   sourceCore.MatchesRaw(L"alpha\r\nbeta++\ngamma") &&
                   sourceCore.model_materialization_count() == 0 &&
                   shiftedTail.has_value() && shiftedTail->start.value == 14 &&
                   preservedOldTail.has_value() && preservedOldTail->start.value == 12,
               "ordinary source edits publish a new immutable row map without materializing text or rewriting the old tail");

        const note::NoteTextApplyResult insertedBreak = sourceCore.Apply({{13}, 0, L"\n"});
        const auto currentEmptyRow = note::NoteSourceLineMap::LineAt(
            sourceCore.source_line_map(), {2});
        const auto currentTail = note::NoteSourceLineMap::LineAt(
            sourceCore.source_line_map(), {3});
        const auto currentTailRichEditStart =
            note::NoteSourceLineMap::CanonicalOffsetToRichEditIndex(
                sourceCore.source_line_map(), {15});
        Expect(insertedBreak == note::NoteTextApplyResult::Applied &&
                   sourceCore.MatchesRaw(L"alpha\r\nbeta++\n\ngamma") &&
                   sourceCore.logical_line_count() == 4 &&
                   sourceCore.model_materialization_count() == 0 &&
                   sourceCore.source_line_map().rich_edit_text_length() == 19 &&
                   currentEmptyRow.has_value() && currentEmptyRow->start.value == 14 &&
                   currentEmptyRow->content_end.value == 14 && currentEmptyRow->next_start.value == 15 &&
                   currentTail.has_value() && currentTail->start.value == 15 &&
                   currentTailRichEditStart == std::optional<size_t>{14},
                   "a local line-break insertion splices only affected row descriptors and retains canonical/RichEdit tail coordinates");
    }

    {
        note::NoteTextCore syntaxCore;
        syntaxCore.Reset(note::NoteId{945}, note::NoteMetadata{L"snapshot.md", L"snapshot"},
                         L"## title\r\nvalue $E=mc^2$", 90, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> markdownSnapshot;
        const note::NoteSyntaxSnapshotBuildResult markdownBuild =
            note::NoteSyntaxSnapshot::BuildFromTextCore(
                syntaxCore, note::NoteContentKind::Markdown, &markdownSnapshot);
        note::NoteDocument markdownSnapshotDocument;
        const bool copiedMarkdownSnapshotDocument = CopyFinalSyntaxSnapshotDocument(
            markdownSnapshot, &markdownSnapshotDocument);
        Expect(markdownBuild == note::NoteSyntaxSnapshotBuildResult::Built &&
                   markdownSnapshot && markdownSnapshot->valid() &&
                   markdownSnapshot->has_structured_syntax() &&
                   markdownSnapshot->MatchesTextCore(syntaxCore) &&
                   markdownSnapshot->source_identity() ==
                       note::NoteDerivedSnapshotIdentity{note::NoteId{945}, 90} &&
                   CopyFinalSyntaxSnapshotText(markdownSnapshot) == L"## title\r\nvalue $E=mc^2$" &&
                   markdownSnapshot->source_line_map().line_count() == 2 &&
                   copiedMarkdownSnapshotDocument &&
                   !markdownSnapshotDocument.blocks.empty() &&
                   !markdownSnapshotDocument.math_spans.empty(),
               "a complete final syntax snapshot binds canonical root, row coordinates, and persistent syntax data");

        const std::shared_ptr<const note::NoteSyntaxSnapshot> oldMarkdownSnapshot =
            markdownSnapshot;
        Expect(syntaxCore.Apply({{10}, 0, L"revised "}) == note::NoteTextApplyResult::Applied &&
                   !oldMarkdownSnapshot->MatchesTextCore(syntaxCore) &&
                   CopyFinalSyntaxSnapshotText(oldMarkdownSnapshot) == L"## title\r\nvalue $E=mc^2$",
               "a prior syntax snapshot remains immutable and cannot validate a newer canonical root");

        std::shared_ptr<const note::NoteSyntaxSnapshot> revisedSnapshot;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   syntaxCore, note::NoteContentKind::Markdown, &revisedSnapshot) ==
                   note::NoteSyntaxSnapshotBuildResult::Built &&
                   revisedSnapshot && revisedSnapshot->MatchesTextCore(syntaxCore) &&
                   CopyFinalSyntaxSnapshotText(revisedSnapshot) == L"## title\r\nrevised value $E=mc^2$",
               "a replacement syntax snapshot is published only after every derived member has the current source identity");

        std::shared_ptr<const note::NoteSyntaxSnapshot> plainSnapshot;
        note::NoteDocument plainSnapshotDocument;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   syntaxCore, note::NoteContentKind::PlainText, &plainSnapshot) ==
                   note::NoteSyntaxSnapshotBuildResult::Built &&
                   plainSnapshot && plainSnapshot->valid() &&
                   !plainSnapshot->has_structured_syntax() &&
                   CopyFinalSyntaxSnapshotDocument(plainSnapshot, &plainSnapshotDocument) &&
                   plainSnapshotDocument.blocks.empty() &&
                   plainSnapshot->MatchesTextCore(syntaxCore),
               "plain-text snapshots retain canonical identity and source coordinates without inventing Markdown syntax");

        note::NoteTextCore texCore;
        texCore.Reset(note::NoteId{946}, note::NoteMetadata{L"snapshot.tex", L"snapshot"},
                      L"TeX source: \\(E=mc^2\\)\n$$\nx+y\n$$", 91, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> texSnapshot;
        note::NoteDocument texSnapshotDocument;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   texCore, note::NoteContentKind::TeXSource, &texSnapshot) ==
                   note::NoteSyntaxSnapshotBuildResult::Built &&
                   texSnapshot && texSnapshot->valid() &&
                   texSnapshot->has_structured_syntax() &&
                   texSnapshot->MatchesTextCore(texCore) &&
                   CopyFinalSyntaxSnapshotDocument(texSnapshot, &texSnapshotDocument) &&
                   texSnapshotDocument.style_spans.empty() &&
                   texSnapshotDocument.math_spans.size() == 2,
               "TeX-source snapshots preserve source text while deriving only TeX math syntax");

        note::NoteTextCore invalidCore;
        const std::shared_ptr<const note::NoteSyntaxSnapshot> preservedSnapshot = revisedSnapshot;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   invalidCore, note::NoteContentKind::Markdown, &revisedSnapshot) ==
                   note::NoteSyntaxSnapshotBuildResult::InvalidSource &&
                   revisedSnapshot == preservedSnapshot,
               "failed syntax-snapshot construction leaves the caller's complete published snapshot unchanged");
    }

    {
        note::NoteTextCore localPatchCore;
        localPatchCore.Reset(
            note::NoteId{947}, note::NoteMetadata{L"local-patch.md", L"local patch"},
            L"first\nsecond\nthird", 100, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> beforeLocalPatch;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   localPatchCore, note::NoteContentKind::Markdown, &beforeLocalPatch) ==
                   note::NoteSyntaxSnapshotBuildResult::Built,
               "local syntax-patch test creates one complete prior final snapshot");
        const note::NoteTextModel beforeLocalSource = localPatchCore.model();
        note::NoteDocument beforeLocalDocument;
        const bool copiedBeforeLocalDocument = CopyFinalSyntaxSnapshotDocument(
            beforeLocalPatch, &beforeLocalDocument);
        const note::NoteParserCheckpointIndex beforeLocalCheckpoint =
            copiedBeforeLocalDocument ? note::BuildNoteParserCheckpointIndex(
                beforeLocalSource, beforeLocalDocument,
                beforeLocalPatch->source_identity(), beforeLocalPatch->source_root(),
                beforeLocalPatch->source_line_map()) :
            note::NoteParserCheckpointIndex{};
        const note::TextEdit plainInsertion{{8}, 0, L"X"};
        Expect(localPatchCore.Apply(plainInsertion) == note::NoteTextApplyResult::Applied,
               "local syntax-patch test applies one same-row canonical edit");
        const uint64_t materializationsBeforeLocalSyntaxPatch =
            localPatchCore.model_materialization_count();

        note::NoteSourceLineMap::Snapshot independentlyBuiltLocalLineMap;
        const bool builtIndependentLocalLineMap =
            note::NoteSourceLineMap::Build(
                CopyFinalSyntaxSnapshotText(beforeLocalPatch), &independentlyBuiltLocalLineMap);
        const note::NoteParserCheckpointIndex independentlyMappedLocalCheckpoint =
            builtIndependentLocalLineMap && copiedBeforeLocalDocument
                ? note::BuildNoteParserCheckpointIndex(
                beforeLocalSource, beforeLocalDocument,
                beforeLocalPatch->source_identity(), beforeLocalPatch->source_root(),
                independentlyBuiltLocalLineMap) : note::NoteParserCheckpointIndex{};
        std::shared_ptr<const note::NoteSyntaxSnapshot> rejectedMapIdentitySnapshot;
        note::NoteParserCheckpointIndex rejectedMapIdentityCheckpoint;
        Expect(builtIndependentLocalLineMap && independentlyMappedLocalCheckpoint.valid &&
                   note::NoteSyntaxSnapshot::BuildLocalPlainTextPatch(
                       beforeLocalPatch, independentlyMappedLocalCheckpoint,
                       note::NoteParserLayoutKey{23}, localPatchCore, plainInsertion,
                       &rejectedMapIdentitySnapshot, &rejectedMapIdentityCheckpoint) ==
                       note::NoteSyntaxSnapshotLocalPatchResult::InvalidPreviousSnapshot &&
                   !rejectedMapIdentitySnapshot && !rejectedMapIdentityCheckpoint.valid,
               "a separately rebuilt but equal source-line map cannot seed a local final snapshot");

        std::shared_ptr<const note::NoteSyntaxSnapshot> locallyPatchedSnapshot;
        note::NoteParserCheckpointIndex locallyPatchedCheckpoint;
        note::NoteSyntaxSnapshotLocalPatchWork localPatchWork;
        const note::NoteSyntaxSnapshotLocalPatchResult localPatchResult =
            note::NoteSyntaxSnapshot::BuildLocalPlainTextPatch(
                beforeLocalPatch, beforeLocalCheckpoint, note::NoteParserLayoutKey{23},
                localPatchCore, plainInsertion, &locallyPatchedSnapshot,
                &locallyPatchedCheckpoint, &localPatchWork);
        const bool localPatchAvoidedTextCoreMaterialization =
            localPatchCore.model_materialization_count() ==
            materializationsBeforeLocalSyntaxPatch;
        std::shared_ptr<const note::NoteSyntaxSnapshot> fullPatchedSnapshot;
        const note::NoteSyntaxSnapshotBuildResult fullPatchResult =
            note::NoteSyntaxSnapshot::BuildFromTextCore(
                localPatchCore, note::NoteContentKind::Markdown, &fullPatchedSnapshot);
        note::NoteDocument fullPatchedDocument;
        const bool copiedFullPatchedDocument = CopyFinalSyntaxSnapshotDocument(
            fullPatchedSnapshot, &fullPatchedDocument);

        const auto sameSpan = [](note::Span lhs, note::Span rhs) {
            return lhs.start == rhs.start && lhs.end == rhs.end;
        };
        const auto sameDocument = [&sameSpan](const note::NoteDocument& lhs,
                                              const note::NoteDocument& rhs) {
            if (lhs.source_identity != rhs.source_identity || lhs.blocks.size() != rhs.blocks.size() ||
                lhs.inlines.size() != rhs.inlines.size() || lhs.style_spans.size() != rhs.style_spans.size() ||
                lhs.math_spans.size() != rhs.math_spans.size() ||
                lhs.diagnostics.size() != rhs.diagnostics.size()) return false;
            for (size_t index = 0; index < lhs.blocks.size(); ++index) {
                const note::BlockNode& a = lhs.blocks[index];
                const note::BlockNode& b = rhs.blocks[index];
                if (a.kind != b.kind || a.origin != b.origin || !sameSpan(a.span, b.span) ||
                    a.loc.line != b.loc.line || a.loc.column != b.loc.column || a.level != b.level ||
                    a.first_inline != b.first_inline || a.inline_count != b.inline_count ||
                    a.parent != b.parent || a.ordered != b.ordered ||
                    a.start_number != b.start_number || a.task_item != b.task_item ||
                    a.task_checked != b.task_checked || a.table_column_count != b.table_column_count ||
                    a.table_cell_align != b.table_cell_align || a.info_string != b.info_string ||
                    a.fence_marker_count != b.fence_marker_count || a.fence_closed != b.fence_closed) return false;
            }
            for (size_t index = 0; index < lhs.inlines.size(); ++index) {
                const note::InlineNode& a = lhs.inlines[index];
                const note::InlineNode& b = rhs.inlines[index];
                if (a.kind != b.kind || !sameSpan(a.span, b.span) ||
                    a.parent_block != b.parent_block || a.target != b.target) return false;
            }
            for (size_t index = 0; index < lhs.style_spans.size(); ++index) {
                if (lhs.style_spans[index].kind != rhs.style_spans[index].kind ||
                    !sameSpan(lhs.style_spans[index].span, rhs.style_spans[index].span) ||
                    lhs.style_spans[index].value != rhs.style_spans[index].value) return false;
            }
            for (size_t index = 0; index < lhs.math_spans.size(); ++index) {
                const note::MathSpan& a = lhs.math_spans[index];
                const note::MathSpan& b = rhs.math_spans[index];
                if (a.kind != b.kind || a.delimiter != b.delimiter || !sameSpan(a.span, b.span) ||
                    !sameSpan(a.content_span, b.content_span) || a.normalized_tex != b.normalized_tex ||
                    a.diagnostic_ids != b.diagnostic_ids) return false;
            }
            for (size_t index = 0; index < lhs.diagnostics.size(); ++index) {
                const note::Diagnostic& a = lhs.diagnostics[index];
                const note::Diagnostic& b = rhs.diagnostics[index];
                if (a.code != b.code || a.message != b.message || !sameSpan(a.span, b.span) ||
                    a.severity != b.severity) return false;
            }
            return true;
        };

        std::shared_ptr<const note::NoteSyntaxDocument> beforePersistentDocument;
        const note::NoteSyntaxDocumentBuildResult beforePersistentDocumentResult =
            copiedBeforeLocalDocument ? note::NoteSyntaxDocument::Build(
                beforeLocalDocument, beforeLocalPatch->source_line_map(),
                &beforePersistentDocument) :
            note::NoteSyntaxDocumentBuildResult::InvalidSourceLines;
        std::shared_ptr<const note::NoteSyntaxDocument> locallyPatchedPersistentDocument;
        note::NoteSyntaxDocumentLocalPatchWork persistentDocumentPatchWork;
        const note::NoteSyntaxDocumentLocalPatchResult persistentDocumentPatchResult =
            beforePersistentDocument ? note::NoteSyntaxDocument::BuildLocalPlainTextPatch(
                beforePersistentDocument, beforeLocalPatch->source_line_map(),
                localPatchCore.source_line_map(), plainInsertion, {1},
                &locallyPatchedPersistentDocument, &persistentDocumentPatchWork) :
            note::NoteSyntaxDocumentLocalPatchResult::InvalidPreviousDocument;
        note::NoteDocument locallyPatchedPersistentDocumentOracle;
        const bool copiedPersistentDocumentOracle = locallyPatchedPersistentDocument &&
            locallyPatchedPersistentDocument->CopyDocumentForDifferentialTest(
                localPatchCore.source_line_map(),
                note::NoteDerivedSnapshotIdentity{localPatchCore.note_id(),
                                                   localPatchCore.content_revision()},
                &locallyPatchedPersistentDocumentOracle);
        Expect(beforePersistentDocumentResult == note::NoteSyntaxDocumentBuildResult::Built &&
                   persistentDocumentPatchResult ==
                       note::NoteSyntaxDocumentLocalPatchResult::Built,
               "persistent syntax document accepts a checkpoint-proven plain-text row patch");
        Expect(copiedPersistentDocumentOracle && fullPatchedSnapshot &&
                   sameDocument(locallyPatchedPersistentDocumentOracle,
                                fullPatchedDocument),
               "persistent syntax document patch materializes exactly as the full parser result");
        Expect(locallyPatchedPersistentDocument && beforePersistentDocument &&
                   persistentDocumentPatchWork.tail_node_payload_rewrites == 0 &&
                   beforePersistentDocument->SharesNodePayloadForDifferentialTest(
                       *locallyPatchedPersistentDocument,
                       note::NoteSyntaxDocumentNodeKind::Inline, 0),
               "persistent syntax document patch shares its untouched inline prefix without tail rewrites");
        note::NoteTextCore separatedPersistentDocumentCore;
        separatedPersistentDocumentCore.Reset(
            note::NoteId{948}, note::NoteMetadata{L"persistent-document.md", L"persistent"},
            L"first\n\nsecond\n\nthird", 200, 4);
        note::NoteDocument separatedBeforeDocument = note::ParseNoteDocument(
            separatedPersistentDocumentCore.model());
        note::SetNoteDocumentSourceIdentity(
            &separatedBeforeDocument,
            {separatedPersistentDocumentCore.note_id(),
             separatedPersistentDocumentCore.content_revision()});
        std::shared_ptr<const note::NoteSyntaxDocument> separatedBeforePersistentDocument;
        const note::NoteSyntaxDocumentBuildResult separatedBeforeBuild =
            note::NoteSyntaxDocument::Build(
                separatedBeforeDocument, separatedPersistentDocumentCore.source_line_map(),
                &separatedBeforePersistentDocument);
        const note::NoteSourceLineMap::Snapshot separatedBeforeSourceLines =
            separatedPersistentDocumentCore.source_line_map();
        note::NoteSourceLineMap::Snapshot independentlyBuiltPersistentDocumentLines;
        const bool builtIndependentPersistentDocumentLines = note::NoteSourceLineMap::Build(
            L"first\n\nsecond\n\nthird", &independentlyBuiltPersistentDocumentLines);
        std::shared_ptr<const note::NoteSyntaxDocument> rejectedPersistentDocument;
        const note::NoteSyntaxDocumentLocalPatchResult independentMapPatchResult =
            builtIndependentPersistentDocumentLines && separatedBeforePersistentDocument
            ? note::NoteSyntaxDocument::BuildLocalPlainTextPatch(
                separatedBeforePersistentDocument, independentlyBuiltPersistentDocumentLines,
                independentlyBuiltPersistentDocumentLines, {{9}, 0, L"X"}, {2},
                &rejectedPersistentDocument)
            : note::NoteSyntaxDocumentLocalPatchResult::InvalidPreviousDocument;
        Expect(independentMapPatchResult == note::NoteSyntaxDocumentLocalPatchResult::InvalidSourceLines &&
                   !rejectedPersistentDocument,
               "a separately rebuilt but equal source-line map cannot seed a persistent syntax-document splice");
        const note::TextEdit separatedEdit{{9}, 0, L"X"};
        const bool appliedSeparatedEdit =
            separatedPersistentDocumentCore.Apply(separatedEdit) == note::NoteTextApplyResult::Applied;
        std::shared_ptr<const note::NoteSyntaxDocument> separatedPatchedPersistentDocument;
        note::NoteSyntaxDocumentLocalPatchWork separatedPersistentDocumentWork;
        const note::NoteSyntaxDocumentLocalPatchResult separatedPatchResult =
            appliedSeparatedEdit && separatedBeforePersistentDocument
            ? note::NoteSyntaxDocument::BuildLocalPlainTextPatch(
                separatedBeforePersistentDocument,
                separatedBeforeSourceLines,
                separatedPersistentDocumentCore.source_line_map(), separatedEdit, {2},
                &separatedPatchedPersistentDocument, &separatedPersistentDocumentWork)
            : note::NoteSyntaxDocumentLocalPatchResult::InvalidPreviousDocument;
        note::NoteDocument separatedFullDocument = note::ParseNoteDocument(
            separatedPersistentDocumentCore.model());
        const note::NoteDerivedSnapshotIdentity separatedIdentity{
            separatedPersistentDocumentCore.note_id(),
            separatedPersistentDocumentCore.content_revision()};
        note::SetNoteDocumentSourceIdentity(&separatedFullDocument, separatedIdentity);
        note::NoteDocument separatedPatchedDocumentOracle;
        const bool copiedSeparatedPersistentDocument = separatedPatchedPersistentDocument &&
            separatedPatchedPersistentDocument->CopyDocumentForDifferentialTest(
                separatedPersistentDocumentCore.source_line_map(), separatedIdentity,
                &separatedPatchedDocumentOracle);
        Expect(separatedBeforeBuild == note::NoteSyntaxDocumentBuildResult::Built &&
                   separatedPatchResult == note::NoteSyntaxDocumentLocalPatchResult::Built &&
                   copiedSeparatedPersistentDocument &&
                   sameDocument(separatedPatchedDocumentOracle, separatedFullDocument) &&
                   separatedPersistentDocumentWork.tail_node_payload_rewrites == 0 &&
                   separatedBeforePersistentDocument->node_count(
                       note::NoteSyntaxDocumentNodeKind::Inline) > 2 &&
                   separatedBeforePersistentDocument->SharesNodePayloadForDifferentialTest(
                       *separatedPatchedPersistentDocument,
                       note::NoteSyntaxDocumentNodeKind::Inline, 0) &&
                   separatedBeforePersistentDocument->SharesNodePayloadForDifferentialTest(
                       *separatedPatchedPersistentDocument,
                       note::NoteSyntaxDocumentNodeKind::Inline, 2),
               "persistent syntax document patch shares separated untouched prefixes and suffixes without tail rewrites");
        Expect(localPatchResult == note::NoteSyntaxSnapshotLocalPatchResult::Built &&
                   locallyPatchedSnapshot && locallyPatchedCheckpoint.valid &&
                   locallyPatchedSnapshot->MatchesTextCore(localPatchCore) &&
                   localPatchAvoidedTextCoreMaterialization &&
                   localPatchWork.text_core_materializations == 0 &&
                   localPatchWork.syntax_document_replacement_payloads == 0 &&
                   localPatchWork.syntax_document_tail_payload_rewrites == 0 &&
                   beforeLocalPatch->syntax_document()->SharesNodePayloadForDifferentialTest(
                       *locallyPatchedSnapshot->syntax_document(),
                       note::NoteSyntaxDocumentNodeKind::Inline, 0) &&
                   localPatchWork.checkpoint_lines_built == 0 &&
                   beforeLocalCheckpoint.SharesLinePayloadForDifferentialTest(
                       locallyPatchedCheckpoint, {0}) &&
                   beforeLocalCheckpoint.SharesLinePayloadForDifferentialTest(
                       locallyPatchedCheckpoint, {2}) &&
                   fullPatchResult == note::NoteSyntaxSnapshotBuildResult::Built &&
                   copiedFullPatchedDocument &&
                   locallyPatchedSnapshot->CopyDocumentForCompleteBuild(
                       &locallyPatchedPersistentDocumentOracle) &&
                   sameDocument(locallyPatchedPersistentDocumentOracle, fullPatchedDocument) &&
                   CopyFinalSyntaxSnapshotText(locallyPatchedSnapshot) ==
                       CopyFinalSyntaxSnapshotText(fullPatchedSnapshot),
               "a checkpoint-proven same-row final snapshot shares persistent syntax state, avoids TextCore materialization, and matches a full Markdown parse");

        std::shared_ptr<const note::NoteRenderSourcePlan> beforeLocalSourcePlan;
        std::shared_ptr<const note::NoteRenderSourcePlan> locallyPatchedSourcePlan;
        std::shared_ptr<const note::NoteRenderSourcePlan> fullPatchedSourcePlan;
        note::NoteRenderSourcePlanLocalPatchWork localSourcePatchWork;
        const note::NoteRenderSourcePlanBuildResult beforeLocalSourceBuild = beforeLocalPatch
            ? note::NoteRenderSourcePlan::Build(beforeLocalPatch, &beforeLocalSourcePlan)
            : note::NoteRenderSourcePlanBuildResult::InvalidSyntaxSnapshot;
        const note::NoteRenderSourcePlanLocalPatchResult localSourcePatchResult =
            note::NoteRenderSourcePlan::BuildLocalPlainTextPatch(
                beforeLocalSourcePlan, locallyPatchedSnapshot, localPatchCore, plainInsertion,
                &locallyPatchedSourcePlan, &localSourcePatchWork);
        const note::NoteRenderSourcePlanBuildResult fullLocalSourceBuild = fullPatchedSnapshot
            ? note::NoteRenderSourcePlan::Build(fullPatchedSnapshot, &fullPatchedSourcePlan)
            : note::NoteRenderSourcePlanBuildResult::InvalidSyntaxSnapshot;
        const auto sameSourcePlan = [&sameSpan](const note::NoteRenderSourcePlan& lhs,
                                                const note::NoteRenderSourcePlan& rhs) {
            std::vector<note::NoteRenderSourceLinePlan> lhsLines;
            std::vector<note::NoteRenderSourceLinePlan> rhsLines;
            std::vector<note::NoteRenderAtomicGroup> lhsGroups;
            std::vector<note::NoteRenderAtomicGroup> rhsGroups;
            if (!lhs.CopyLinesForDifferentialTest(&lhsLines) ||
                !rhs.CopyLinesForDifferentialTest(&rhsLines) ||
                !lhs.CopyAtomicGroups(&lhsGroups) || !rhs.CopyAtomicGroups(&rhsGroups) ||
                lhsLines.size() != rhsLines.size() || lhsGroups.size() != rhsGroups.size()) {
                return false;
            }
            for (size_t line = 0; line < lhsLines.size(); ++line) {
                const note::NoteRenderSourceLinePlan& a = lhsLines[line];
                const note::NoteRenderSourceLinePlan& b = rhsLines[line];
                if (!sameSpan(a.content_span, b.content_span) || a.runs.size() != b.runs.size()) {
                    return false;
                }
                for (size_t run = 0; run < a.runs.size(); ++run) {
                    const note::NoteRenderSourceRun& x = a.runs[run];
                    const note::NoteRenderSourceRun& y = b.runs[run];
                    if (!sameSpan(x.source_span, y.source_span) ||
                        !sameSpan(x.display_source_span, y.display_source_span) ||
                        x.line_index != y.line_index || x.parent_block != y.parent_block ||
                        x.heading_level != y.heading_level || x.kind != y.kind ||
                        x.link_target != y.link_target ||
                        x.decodes_markdown_escapes != y.decodes_markdown_escapes ||
                        x.table_block != y.table_block ||
                        x.table_row_block != y.table_row_block ||
                        x.table_column != y.table_column ||
                        x.table_column_count != y.table_column_count ||
                        x.table_cell_align != y.table_cell_align ||
                        x.table_header != y.table_header ||
                        x.styles.size() != y.styles.size()) return false;
                    for (size_t style = 0; style < x.styles.size(); ++style) {
                        if (x.styles[style].kind != y.styles[style].kind ||
                            x.styles[style].value != y.styles[style].value) return false;
                    }
                }
            }
            for (size_t group = 0; group < lhsGroups.size(); ++group) {
                const note::NoteRenderAtomicGroup& a = lhsGroups[group];
                const note::NoteRenderAtomicGroup& b = rhsGroups[group];
                if (a.kind != b.kind || !sameSpan(a.source_span, b.source_span) ||
                    a.first_line != b.first_line || a.last_line != b.last_line) return false;
            }
            return true;
        };
        Expect(beforeLocalSourceBuild == note::NoteRenderSourcePlanBuildResult::Built &&
                   localSourcePatchResult == note::NoteRenderSourcePlanLocalPatchResult::Built &&
                   locallyPatchedSourcePlan &&
                   localSourcePatchWork.replacement_line_payloads == 1 &&
                   localSourcePatchWork.tail_line_payload_rewrites == 0 &&
                   beforeLocalSourcePlan->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedSourcePlan, {0}) &&
                   !beforeLocalSourcePlan->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedSourcePlan, {1}) &&
                   beforeLocalSourcePlan->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedSourcePlan, {2}) &&
                   fullLocalSourceBuild == note::NoteRenderSourcePlanBuildResult::Built &&
                   fullPatchedSourcePlan &&
                   sameSourcePlan(*locallyPatchedSourcePlan, *fullPatchedSourcePlan),
               "a checkpoint-proven source-plan splice replaces only its changed row payload and matches a complete rebuild");

        const note::NoteRenderLayoutKey localPatchLayoutKey{720, 96, 96, 4, 4, 12, true};
        note::NoteRenderLineLayoutMap::Snapshot beforeLocalLineLayouts;
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> beforeLocalLayout;
        const std::vector<note::NoteRenderLineLayout> beforeLocalMeasures(
            beforeLocalPatch ? beforeLocalPatch->source_line_map().line_count() : 0, {16, 80});
        Expect(note::NoteRenderLineLayoutMap::Build(beforeLocalMeasures, &beforeLocalLineLayouts) &&
                   note::NoteRenderLayoutSnapshot::Build(
                       beforeLocalPatch, localPatchLayoutKey, beforeLocalLineLayouts,
                       &beforeLocalLayout) == note::NoteRenderLayoutSnapshotBuildResult::Built,
               "local layout-splice test creates one prior immutable measured layout");
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> locallySplicedLayout;
        Expect(note::NoteRenderLayoutSnapshot::BuildLocalSplice(
                   beforeLocalLayout, locallyPatchedSnapshot, localPatchLayoutKey, {1}, {2},
                   {{22, 133}}, &locallySplicedLayout) ==
                   note::NoteRenderLayoutSnapshotLocalSpliceResult::Built &&
                   locallySplicedLayout && locallySplicedLayout->Matches(
                       localPatchCore, localPatchLayoutKey) &&
                   note::NoteRenderLineLayoutMap::LineAt(
                       locallySplicedLayout->line_layouts(), {0})->bottom_px == 16 &&
                   note::NoteRenderLineLayoutMap::LineAt(
                       locallySplicedLayout->line_layouts(), {1})->top_px == 16 &&
                   note::NoteRenderLineLayoutMap::LineAt(
                       locallySplicedLayout->line_layouts(), {1})->bottom_px == 38 &&
                   note::NoteRenderLineLayoutMap::LineAt(
                       locallySplicedLayout->line_layouts(), {2})->top_px == 38 &&
                   note::NoteRenderLineLayoutMap::LineAt(
                       locallySplicedLayout->line_layouts(), {2})->bottom_px == 54,
               "a local measured-row splice changes aggregate tail Y without rewriting unchanged tail rows");
        const std::shared_ptr<const note::NoteRenderLayoutSnapshot> preservedSplicedLayout =
            locallySplicedLayout;
        note::NoteRenderLayoutKey wrongLocalPatchKey = localPatchLayoutKey;
        wrongLocalPatchKey.dpi_x = 120;
        Expect(note::NoteRenderLayoutSnapshot::BuildLocalSplice(
                   beforeLocalLayout, locallyPatchedSnapshot, wrongLocalPatchKey, {1}, {2},
                   {{22, 133}}, &locallySplicedLayout) ==
                   note::NoteRenderLayoutSnapshotLocalSpliceResult::LayoutKeyMismatch &&
                   locallySplicedLayout == preservedSplicedLayout,
               "a layout-key mismatch cannot replace a locally spliced immutable layout");

        note::NoteTextCore structuralPatchCore;
        structuralPatchCore.Reset(
            note::NoteId{947}, note::NoteMetadata{L"local-patch.md", L"local patch"},
            L"first\nsecond\nthird", 100, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> structuralBefore;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   structuralPatchCore, note::NoteContentKind::Markdown, &structuralBefore) ==
                   note::NoteSyntaxSnapshotBuildResult::Built,
               "structural local-patch rejection test creates one complete prior snapshot");
        const note::NoteTextModel structuralBeforeSource = structuralPatchCore.model();
        note::NoteDocument structuralBeforeDocument;
        const bool copiedStructuralBeforeDocument = CopyFinalSyntaxSnapshotDocument(
            structuralBefore, &structuralBeforeDocument);
        const note::NoteParserCheckpointIndex structuralCheckpoint =
            copiedStructuralBeforeDocument
            ? note::BuildNoteParserCheckpointIndex(
                structuralBeforeSource, structuralBeforeDocument,
                structuralBefore->source_identity(), structuralBefore->source_root(),
                structuralBefore->source_line_map())
            : note::NoteParserCheckpointIndex{};
        const note::TextEdit structuralInsertion{{8}, 0, L"*"};
        Expect(structuralPatchCore.Apply(structuralInsertion) == note::NoteTextApplyResult::Applied,
               "structural local-patch rejection test applies its canonical edit");
        const std::shared_ptr<const note::NoteSyntaxSnapshot> preservedLocalOutput =
            locallyPatchedSnapshot;
        const note::NoteParserCheckpointIndex preservedLocalCheckpoint = locallyPatchedCheckpoint;
        Expect(note::NoteSyntaxSnapshot::BuildLocalPlainTextPatch(
                   structuralBefore, structuralCheckpoint, note::NoteParserLayoutKey{23},
                   structuralPatchCore, structuralInsertion, &locallyPatchedSnapshot,
                   &locallyPatchedCheckpoint) ==
                   note::NoteSyntaxSnapshotLocalPatchResult::RequiresFullSnapshot &&
                   locallyPatchedSnapshot == preservedLocalOutput &&
                   note::NoteTextPieceSequence::SameSnapshotIdentity(
                       locallyPatchedCheckpoint.canonical_source_root,
                       preservedLocalCheckpoint.canonical_source_root),
               "a syntax-sensitive edit cannot publish a locally patched final snapshot and preserves the prior output");
    }

    {
        constexpr size_t kLongSourceRows = 4096;
        constexpr size_t kEditedLongSourceRow = kLongSourceRows / 2;
        std::wstring longSource;
        longSource.reserve(kLongSourceRows * 6);
        for (size_t row = 0; row < kLongSourceRows; ++row) {
            longSource.append(L"plain");
            if (row + 1 < kLongSourceRows) longSource.push_back(L'\n');
        }
        note::NoteTextCore longSourceCore;
        longSourceCore.Reset(
            note::NoteId{951}, note::NoteMetadata{L"long-source-plan.md", L"long source plan"},
            std::move(longSource), 201, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> beforeLongSourceSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> beforeLongSourcePlan;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   longSourceCore, note::NoteContentKind::Markdown, &beforeLongSourceSyntax) ==
                   note::NoteSyntaxSnapshotBuildResult::Built &&
                   note::NoteRenderSourcePlan::Build(
                       beforeLongSourceSyntax, &beforeLongSourcePlan) ==
                   note::NoteRenderSourcePlanBuildResult::Built,
               "long source-plan splice test creates one immutable source-plan publication");
        const note::NoteTextModel beforeLongSourceModel = longSourceCore.model();
        note::NoteDocument beforeLongSourceDocument;
        const bool copiedBeforeLongSourceDocument = CopyFinalSyntaxSnapshotDocument(
            beforeLongSourceSyntax, &beforeLongSourceDocument);
        const note::NoteParserCheckpointIndex beforeLongSourceCheckpoint =
            copiedBeforeLongSourceDocument
            ? note::BuildNoteParserCheckpointIndex(
                beforeLongSourceModel, beforeLongSourceDocument,
                beforeLongSourceSyntax->source_identity(), beforeLongSourceSyntax->source_root(),
                beforeLongSourceSyntax->source_line_map())
            : note::NoteParserCheckpointIndex{};
        const note::TextEdit longSourceEdit{{kEditedLongSourceRow * 6 + 2}, 0, L"X"};
        const std::shared_ptr<const note::NoteSyntaxDocument> longPersistentDocument =
            beforeLongSourceSyntax ? beforeLongSourceSyntax->syntax_document() : nullptr;
        note::NoteSyntaxDocumentQueryWork longPersistentDocumentQueryWork;
        const bool builtLongPersistentDocument = longPersistentDocument &&
            longPersistentDocument->valid();
        const bool longPersistentDocumentPermitsEdit = builtLongPersistentDocument &&
            longPersistentDocument->PermitsPlainTextLeafEdit(
                beforeLongSourceSyntax->source_line_map(), longSourceEdit.start,
                longSourceEdit.deleted_len, &longPersistentDocumentQueryWork);
        Expect(longPersistentDocumentPermitsEdit &&
                   longPersistentDocumentQueryWork.visited_index_nodes < 64,
               "persistent syntax-document interval lookup avoids scanning a long document");
        Expect(longSourceCore.Apply(longSourceEdit) == note::NoteTextApplyResult::Applied,
               "long source-plan splice test applies one marker-free middle-row insertion");
        std::shared_ptr<const note::NoteSyntaxSnapshot> locallyPatchedLongSourceSyntax;
        note::NoteParserCheckpointIndex locallyPatchedLongSourceCheckpoint;
        note::NoteSyntaxSnapshotLocalPatchWork longSyntaxPatchWork;
        std::shared_ptr<const note::NoteRenderSourcePlan> locallyPatchedLongSourcePlan;
        note::NoteRenderSourcePlanLocalPatchWork longSourcePatchWork;
        const note::NoteSyntaxSnapshotLocalPatchResult longSyntaxPatchResult =
            note::NoteSyntaxSnapshot::BuildLocalPlainTextPatch(
                beforeLongSourceSyntax, beforeLongSourceCheckpoint, note::NoteParserLayoutKey{31},
                longSourceCore, longSourceEdit, &locallyPatchedLongSourceSyntax,
                &locallyPatchedLongSourceCheckpoint, &longSyntaxPatchWork);
        const note::NoteRenderSourcePlanLocalPatchResult longSourcePatchResult =
            note::NoteRenderSourcePlan::BuildLocalPlainTextPatch(
                beforeLongSourcePlan, locallyPatchedLongSourceSyntax, longSourceCore, longSourceEdit,
                &locallyPatchedLongSourcePlan, &longSourcePatchWork);
        note::NoteRenderSourceLinePlan beforeLongTail;
        note::NoteRenderSourceLinePlan afterLongTail;
        note::NoteParserLineCheckpoint beforeLongCheckpointTail;
        note::NoteParserLineCheckpoint afterLongCheckpointTail;
        Expect(longSyntaxPatchResult == note::NoteSyntaxSnapshotLocalPatchResult::Built &&
                   longSourcePatchResult == note::NoteRenderSourcePlanLocalPatchResult::Built &&
                   locallyPatchedLongSourcePlan &&
                   longSyntaxPatchWork.checkpoint_lines_built == 0 &&
                   beforeLongSourceCheckpoint.SharesLinePayloadForDifferentialTest(
                       locallyPatchedLongSourceCheckpoint, {0}) &&
                   beforeLongSourceCheckpoint.SharesLinePayloadForDifferentialTest(
                       locallyPatchedLongSourceCheckpoint, {kEditedLongSourceRow}) &&
                   beforeLongSourceCheckpoint.SharesLinePayloadForDifferentialTest(
                       locallyPatchedLongSourceCheckpoint, {kLongSourceRows - 1}) &&
                   beforeLongSourceCheckpoint.ResolveLine(
                       {kLongSourceRows - 1}, &beforeLongCheckpointTail) &&
                   locallyPatchedLongSourceCheckpoint.ResolveLine(
                       {kLongSourceRows - 1}, &afterLongCheckpointTail) &&
                   beforeLongCheckpointTail.source_line.end.value + 1 ==
                       afterLongCheckpointTail.source_line.end.value &&
                   longSourcePatchWork.replacement_line_payloads == 1 &&
                   longSourcePatchWork.tail_line_payload_rewrites == 0 &&
                   beforeLongSourcePlan->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedLongSourcePlan, {0}) &&
                   !beforeLongSourcePlan->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedLongSourcePlan, {kEditedLongSourceRow}) &&
                   beforeLongSourcePlan->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedLongSourcePlan, {kLongSourceRows - 1}) &&
                   beforeLongSourcePlan->ResolveLine({kLongSourceRows - 1}, &beforeLongTail) &&
                   locallyPatchedLongSourcePlan->ResolveLine(
                       {kLongSourceRows - 1}, &afterLongTail) &&
                   beforeLongTail.content_span.end.value + 1 == afterLongTail.content_span.end.value &&
                   !beforeLongTail.runs.empty() && !afterLongTail.runs.empty() &&
                   beforeLongTail.runs.front().source_span.start.value + 1 ==
                       afterLongTail.runs.front().source_span.start.value,
               "a long middle-row source-plan splice shares both unchanged tails and resolves their shifted coordinates lazily");

        const note::NoteRenderLayoutKey longPlacementKey{720, 96, 96, 4, 4, 12, true};
        const std::vector<note::NoteRenderLineLayout> longPlacementMeasures(
            kLongSourceRows, {16, 80});
        note::NoteRenderLineLayoutMap::Snapshot beforeLongPlacementLineLayouts;
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> beforeLongPlacementLayout;
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> locallyPatchedLongPlacementLayout;
        const bool builtLongPlacementLayouts =
            note::NoteRenderLineLayoutMap::Build(
                longPlacementMeasures, &beforeLongPlacementLineLayouts) &&
            note::NoteRenderLayoutSnapshot::Build(
                beforeLongSourceSyntax, longPlacementKey, beforeLongPlacementLineLayouts,
                &beforeLongPlacementLayout) == note::NoteRenderLayoutSnapshotBuildResult::Built &&
            note::NoteRenderLayoutSnapshot::BuildLocalSplice(
                beforeLongPlacementLayout, locallyPatchedLongSourceSyntax, longPlacementKey,
                {kEditedLongSourceRow}, {kEditedLongSourceRow + 1}, {{16, 80}},
                &locallyPatchedLongPlacementLayout) ==
                note::NoteRenderLayoutSnapshotLocalSpliceResult::Built;
        const auto buildPlacements = [](const std::shared_ptr<const note::NoteRenderSourcePlan>& plan,
                                        std::vector<note::NoteRenderLinePlacement>* out) {
            if (!plan || !out) return false;
            std::vector<note::NoteRenderLinePlacement> candidate(plan->line_count());
            for (size_t line = 0; line < plan->line_count(); ++line) {
                note::NoteRenderSourceLinePlan sourceLine;
                if (!plan->ResolveLine({line}, &sourceLine)) return false;
                int x = 10;
                for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                    const int width = 20 + static_cast<int>(
                        run.source_span.end.value - run.source_span.start.value);
                    candidate[line].runs.push_back(note::NoteRenderRunPlacement{
                        run.source_span, x, width,
                        {{run.source_span.start, x}, {run.source_span.end, x + width}},
                    });
                    x += width + 2;
                }
            }
            *out = std::move(candidate);
            return true;
        };
        std::vector<note::NoteRenderLinePlacement> beforeLongPlacements;
        std::shared_ptr<const note::NoteRenderPlacementSnapshot> beforeLongPlacement;
        const bool builtBeforeLongPlacement =
            buildPlacements(beforeLongSourcePlan, &beforeLongPlacements) &&
            note::NoteRenderPlacementSnapshot::Build(
                beforeLongSourcePlan, beforeLongPlacementLayout, beforeLongPlacements,
                &beforeLongPlacement) == note::NoteRenderPlacementSnapshotBuildResult::Built;
        note::NoteRenderSourceLinePlan changedLongSourceLine;
        note::NoteRenderLinePlacement changedLongPlacement;
        const bool builtChangedLongPlacement =
            locallyPatchedLongSourcePlan &&
            locallyPatchedLongSourcePlan->ResolveLine(
                {kEditedLongSourceRow}, &changedLongSourceLine);
        if (builtChangedLongPlacement) {
            int x = 10;
            for (const note::NoteRenderSourceRun& run : changedLongSourceLine.runs) {
                const int width = 20 + static_cast<int>(
                    run.source_span.end.value - run.source_span.start.value);
                changedLongPlacement.runs.push_back(note::NoteRenderRunPlacement{
                    run.source_span, x, width,
                    {{run.source_span.start, x}, {run.source_span.end, x + width}},
                });
                x += width + 2;
            }
        }
        std::shared_ptr<const note::NoteRenderPlacementSnapshot> locallyPatchedLongPlacement;
        note::NoteRenderPlacementSnapshotLocalSpliceWork longPlacementWork;
        const note::NoteRenderPlacementSnapshotLocalSpliceResult longPlacementResult =
            note::NoteRenderPlacementSnapshot::BuildLocalSplice(
                beforeLongPlacement, locallyPatchedLongSourcePlan, locallyPatchedLongPlacementLayout,
                {kEditedLongSourceRow}, {kEditedLongSourceRow + 1}, {changedLongPlacement},
                &locallyPatchedLongPlacement, &longPlacementWork);
        note::NoteRenderLinePlacement beforeLongPlacementTail;
        note::NoteRenderLinePlacement afterLongPlacementTail;
        Expect(builtLongPlacementLayouts && builtBeforeLongPlacement && builtChangedLongPlacement &&
                   longPlacementResult == note::NoteRenderPlacementSnapshotLocalSpliceResult::Built &&
                   locallyPatchedLongPlacement &&
                   longPlacementWork.replacement_line_payloads == 1 &&
                   longPlacementWork.tail_line_payload_rewrites == 0 &&
                   beforeLongPlacement->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedLongPlacement, {0}) &&
                   !beforeLongPlacement->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedLongPlacement, {kEditedLongSourceRow}) &&
                   beforeLongPlacement->SharesLinePayloadForDifferentialTest(
                       *locallyPatchedLongPlacement, {kLongSourceRows - 1}) &&
                   beforeLongPlacement->ResolveLine(
                       {kLongSourceRows - 1}, &beforeLongPlacementTail) &&
                   locallyPatchedLongPlacement->ResolveLine(
                       {kLongSourceRows - 1}, &afterLongPlacementTail) &&
                   !beforeLongPlacementTail.runs.empty() &&
                   !afterLongPlacementTail.runs.empty() &&
                   beforeLongPlacementTail.runs.front().source_span.start.value + 1 ==
                       afterLongPlacementTail.runs.front().source_span.start.value &&
                   beforeLongPlacementTail.runs.front().boundaries.front().source_offset.value + 1 ==
                       afterLongPlacementTail.runs.front().boundaries.front().source_offset.value,
               "a long middle-row placement splice shares unchanged payloads and lazily shifts tail source boundaries");
    }

    {
        const std::vector<note::NoteRenderLineLayout> initialLayouts{
            {12, 40},
            {40, 120},
            {15, 60},
        };
        note::NoteRenderLineLayoutMap::Snapshot initial;
        Expect(note::NoteRenderLineLayoutMap::Build(initialLayouts, &initial) &&
                   initial.valid() && initial.line_count() == 3 &&
                   initial.total_height_px() == 67 && initial.max_inline_extent_px() == 120,
               "persistent render-layout rows aggregate height and horizontal extent without absolute tail coordinates");
        const auto firstLayout = note::NoteRenderLineLayoutMap::LineAt(initial, {0});
        const auto finalLayout = note::NoteRenderLineLayoutMap::LineAt(initial, {2});
        Expect(firstLayout.has_value() && finalLayout.has_value() &&
                   firstLayout->top_px == 0 && firstLayout->bottom_px == 12 &&
                   finalLayout->top_px == 52 && finalLayout->bottom_px == 67,
               "render-layout row positions are derived from immutable prefix aggregates");

        note::NoteRenderLineLayoutMap::Snapshot replaced;
        const std::vector<note::NoteRenderLineLayout> replacementLayouts{
            {18, 80},
            {50, 160},
        };
        const auto originalTail = note::NoteRenderLineLayoutMap::LineAt(initial, {2});
        Expect(note::NoteRenderLineLayoutMap::Replace(
                   initial, {1}, {2}, replacementLayouts, &replaced) &&
                   replaced.line_count() == 4 && replaced.total_height_px() == 95 &&
                   replaced.max_inline_extent_px() == 160 &&
                   note::NoteRenderLineLayoutMap::LineAt(replaced, {3}).has_value() &&
                   note::NoteRenderLineLayoutMap::LineAt(replaced, {3})->top_px == 80 &&
                   originalTail.has_value() && originalTail->top_px == 52,
               "a layout splice shifts only aggregate coordinates while the old snapshot remains unchanged");
        const note::NoteRenderLineLayoutMap::Snapshot preservedLayouts = replaced;
        Expect(!note::NoteRenderLineLayoutMap::Replace(
                   replaced, {1}, {2}, std::vector<note::NoteRenderLineLayout>{{0, 1}},
                   &replaced) &&
                   replaced.line_count() == preservedLayouts.line_count() &&
                   replaced.total_height_px() == preservedLayouts.total_height_px(),
               "invalid measured geometry cannot partially publish a layout snapshot");

        note::NoteTextCore layoutCore;
        layoutCore.Reset(note::NoteId{947}, note::NoteMetadata{L"layout.md", L"layout"},
                         L"first\nsecond", 92, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> layoutSyntax;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   layoutCore, note::NoteContentKind::Markdown, &layoutSyntax) ==
                   note::NoteSyntaxSnapshotBuildResult::Built,
               "layout-snapshot test constructs one exact immutable syntax input");
        note::NoteRenderLineLayoutMap::Snapshot matchingLineLayouts;
        Expect(note::NoteRenderLineLayoutMap::Build(
                   std::vector<note::NoteRenderLineLayout>{{16, 48}, {20, 72}},
                   &matchingLineLayouts),
               "layout-snapshot test constructs geometry for every source row");
        const note::NoteRenderLayoutKey layoutKey{640, 96, 96, 1, 4, 12, true};
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> renderLayout;
        Expect(note::NoteRenderLayoutSnapshot::Build(
                   layoutSyntax, layoutKey, matchingLineLayouts, &renderLayout) ==
                   note::NoteRenderLayoutSnapshotBuildResult::Built &&
                   renderLayout && renderLayout->valid() &&
                   renderLayout->source_identity() == layoutSyntax->source_identity() &&
                   renderLayout->Matches(layoutCore, layoutKey),
               "a render-layout snapshot atomically binds exact syntax identity, layout key, and every row geometry");

        note::NoteRenderLayoutKey changedKey = layoutKey;
        changedKey.dpi_y = 120;
        const std::shared_ptr<const note::NoteRenderLayoutSnapshot> preservedRenderLayout =
            renderLayout;
        Expect(!renderLayout->Matches(layoutCore, changedKey) &&
                   note::NoteRenderLayoutSnapshot::Build(
                       layoutSyntax, changedKey, note::NoteRenderLineLayoutMap::Snapshot{},
                       &renderLayout) == note::NoteRenderLayoutSnapshotBuildResult::LineCountMismatch &&
                   renderLayout == preservedRenderLayout,
               "a layout-key or row-count mismatch cannot reuse or replace the committed render-layout snapshot");

        note::NoteRenderLayoutKey incompatibleTabKey = layoutKey;
        incompatibleTabKey.tab_columns = 8;
        Expect(note::NoteRenderLayoutSnapshot::Build(
                   layoutSyntax, incompatibleTabKey, matchingLineLayouts, &renderLayout) ==
                   note::NoteRenderLayoutSnapshotBuildResult::InvalidLayoutKey &&
                   renderLayout == preservedRenderLayout,
               "a final render-layout key rejects a tab policy other than the canonical four-column policy");
    }

    {
        note::NoteTextCore renderPlanCore;
        renderPlanCore.Reset(
            note::NoteId{948}, note::NoteMetadata{L"render-plan.md", L"render plan"},
            L"# Heading\n"
            L"before **bold** [link](note.md) and `code` plus $x+y$.\n"
            L"$$\n"
            L"z\n"
            L"$$\n"
            L"\n"
            L"| A | B |\n"
            L"| --- | --- |\n"
            L"| one | two |",
            93, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> renderPlanSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> sourcePlan;
        const bool builtRenderSourcePlan = note::NoteSyntaxSnapshot::BuildFromTextCore(
                   renderPlanCore, note::NoteContentKind::Markdown, &renderPlanSyntax) ==
                   note::NoteSyntaxSnapshotBuildResult::Built &&
                   note::NoteRenderSourcePlan::Build(renderPlanSyntax, &sourcePlan) ==
                   note::NoteRenderSourcePlanBuildResult::Built;
        std::vector<note::NoteRenderSourceLinePlan> renderPlanLines;
        std::vector<note::NoteRenderAtomicGroup> renderPlanGroups;
        const bool materializedRenderSourcePlan = sourcePlan &&
            sourcePlan->CopyLinesForDifferentialTest(&renderPlanLines) &&
            sourcePlan->CopyAtomicGroups(&renderPlanGroups);
        Expect(builtRenderSourcePlan && sourcePlan && sourcePlan->valid() &&
                   sourcePlan->Matches(renderPlanCore) && materializedRenderSourcePlan &&
                   renderPlanLines.size() == renderPlanCore.logical_line_count(),
               "render source plan is derived from one exact immutable syntax snapshot and every source row");

        const auto hasRun = [&renderPlanLines](note::NoteRenderSourceRunKind wanted) {
            return std::any_of(renderPlanLines.begin(), renderPlanLines.end(),
                [wanted](const note::NoteRenderSourceLinePlan& line) {
                    return std::any_of(line.runs.begin(), line.runs.end(),
                        [wanted](const note::NoteRenderSourceRun& run) {
                            return run.kind == wanted;
                        });
                });
        };
        const bool hasBold = std::any_of(
            renderPlanLines.begin(), renderPlanLines.end(),
            [](const note::NoteRenderSourceLinePlan& line) {
                return std::any_of(line.runs.begin(), line.runs.end(),
                    [](const note::NoteRenderSourceRun& run) {
                        return std::any_of(run.styles.begin(), run.styles.end(),
                            [](const note::NoteRenderSourceStyleAttribute& style) {
                                return style.kind == note::StyleKind::Bold;
                            });
                    });
            });
        const bool hasHeadingText = !renderPlanLines.empty() &&
            std::any_of(renderPlanLines[0].runs.begin(), renderPlanLines[0].runs.end(),
                [](const note::NoteRenderSourceRun& run) {
                    return run.heading_level == 1 && run.source_span.start.value >= 2;
                });
        const bool hasTableGroup = std::any_of(
            renderPlanGroups.begin(), renderPlanGroups.end(),
            [](const note::NoteRenderAtomicGroup& group) {
                return group.kind == note::NoteRenderAtomicGroupKind::Table;
            });
        const bool hasBlockMathGroup = std::any_of(
            renderPlanGroups.begin(), renderPlanGroups.end(),
            [](const note::NoteRenderAtomicGroup& group) {
                return group.kind == note::NoteRenderAtomicGroupKind::BlockMath;
            });
        Expect(hasHeadingText && hasBold &&
                   hasRun(note::NoteRenderSourceRunKind::LinkText) &&
                   hasRun(note::NoteRenderSourceRunKind::InlineCode) &&
                   hasRun(note::NoteRenderSourceRunKind::InlineMath) &&
                   hasRun(note::NoteRenderSourceRunKind::BlockMath) &&
                   hasTableGroup && hasBlockMathGroup,
               "render source plan carries Markdown text, styles, code, links, math, and atomic table/block-math ownership without LineCache");

        note::NoteTextCore displayRunCore;
        const std::wstring displayRunText =
            L"escaped \\*mark\\* and `a\\*b` with $E = mc^2$";
        displayRunCore.Reset(
            note::NoteId{949}, note::NoteMetadata{L"display-run.md", L"display run"},
            displayRunText, 94, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> displayRunSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> displayRunSourcePlan;
        note::NoteRenderSourceLinePlan displayRunSourceLine;
        const bool builtDisplayRunSource =
            note::NoteSyntaxSnapshot::BuildFromTextCore(
                displayRunCore, note::NoteContentKind::Markdown, &displayRunSyntax) ==
                note::NoteSyntaxSnapshotBuildResult::Built &&
            note::NoteRenderSourcePlan::Build(displayRunSyntax, &displayRunSourcePlan) ==
                note::NoteRenderSourcePlanBuildResult::Built &&
            displayRunSourcePlan && displayRunSourcePlan->ResolveLine({0}, &displayRunSourceLine);
        const auto findDisplaySourceRun = [&displayRunSourceLine](note::NoteRenderSourceRunKind kind) {
            return std::find_if(
                displayRunSourceLine.runs.begin(), displayRunSourceLine.runs.end(),
                [kind](const note::NoteRenderSourceRun& run) { return run.kind == kind; });
        };
        const auto escapedTextRun = findDisplaySourceRun(note::NoteRenderSourceRunKind::Text);
        const auto inlineCodeRun = findDisplaySourceRun(note::NoteRenderSourceRunKind::InlineCode);
        const auto inlineMathRun = findDisplaySourceRun(note::NoteRenderSourceRunKind::InlineMath);
        note::NoteRenderFinalDisplayRun escapedDisplayRun;
        note::NoteRenderFinalDisplayRun inlineCodeDisplayRun;
        note::NoteRenderFinalDisplayRun inlineMathDisplayRun;
        const size_t escapeSourceOffset = displayRunText.find(L"\\*mark");
        const auto boundaryDisplayOffset = [](const note::NoteRenderFinalDisplayRun& run,
                                              size_t sourceOffset) {
            const auto found = std::find_if(
                run.boundaries.begin(), run.boundaries.end(),
                [sourceOffset](const note::NoteRenderFinalDisplayBoundary& boundary) {
                    return boundary.source_offset.value == sourceOffset;
                });
            return found == run.boundaries.end() ? static_cast<size_t>(-1) : found->display_offset;
        };
        const bool builtEscapedDisplayRun =
            builtDisplayRunSource && escapedTextRun != displayRunSourceLine.runs.end() &&
            note::BuildNoteRenderFinalDisplayRun(
                *displayRunSyntax, *escapedTextRun, &escapedDisplayRun) ==
                note::NoteRenderFinalDisplayRunBuildResult::Built;
        const bool builtInlineCodeDisplayRun =
            builtDisplayRunSource && inlineCodeRun != displayRunSourceLine.runs.end() &&
            note::BuildNoteRenderFinalDisplayRun(
                *displayRunSyntax, *inlineCodeRun, &inlineCodeDisplayRun) ==
                note::NoteRenderFinalDisplayRunBuildResult::Built;
        const bool builtInlineMathDisplayRun =
            builtDisplayRunSource && inlineMathRun != displayRunSourceLine.runs.end() &&
            note::BuildNoteRenderFinalDisplayRun(
                *displayRunSyntax, *inlineMathRun, &inlineMathDisplayRun) ==
                note::NoteRenderFinalDisplayRunBuildResult::Built;
        const size_t escapedDisplayStart = boundaryDisplayOffset(escapedDisplayRun, escapeSourceOffset);
        Expect(builtEscapedDisplayRun && builtInlineCodeDisplayRun && builtInlineMathDisplayRun &&
                   escapedDisplayRun.display_text.find(L"*mark*") != std::wstring::npos &&
                   escapedDisplayRun.display_text.find(L"\\*mark") == std::wstring::npos &&
                   escapeSourceOffset != std::wstring::npos &&
                   escapedDisplayStart != static_cast<size_t>(-1) &&
                   boundaryDisplayOffset(escapedDisplayRun, escapeSourceOffset + 1) ==
                       escapedDisplayStart &&
                   boundaryDisplayOffset(escapedDisplayRun, escapeSourceOffset + 2) ==
                       escapedDisplayStart + 1 &&
                   inlineCodeDisplayRun.display_text == L"a\\*b" &&
                   inlineMathDisplayRun.display_text == L"E = mc^2" &&
                   !inlineMathDisplayRun.boundaries.empty() &&
                   inlineMathDisplayRun.boundaries.front().source_offset ==
                       inlineMathRun->source_span.start &&
                   inlineMathDisplayRun.boundaries.front().display_offset == 0 &&
                   inlineMathDisplayRun.boundaries.back().source_offset ==
                       inlineMathRun->source_span.end &&
                   inlineMathDisplayRun.boundaries.back().display_offset ==
                       inlineMathDisplayRun.display_text.size(),
               "one final display-run transform decodes Markdown only where allowed and preserves source boundaries for escapes, code, and hidden math delimiters");

        note::NoteTextCore decoratorCore;
        decoratorCore.Reset(
            note::NoteId{9491}, note::NoteMetadata{L"decorator.md", L"decorator"},
            L"# Heading\n"
            L"- [x] task\n"
            L"> quote\n"
            L"\n"
            L"---\n"
            L"\n"
            L"```cpp\n"
            L"body\n"
            L"```\n"
            L"\n"
            L"| A |\n"
            L"| --- |\n"
            L"| B |\n"
            L"\n"
            L"**bold**",
            941, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> decoratorSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> decoratorPlan;
        std::vector<note::NoteRenderSourceLinePlan> decoratorLines;
        const bool builtDecoratorPlan =
            note::NoteSyntaxSnapshot::BuildFromTextCore(
                decoratorCore, note::NoteContentKind::Markdown, &decoratorSyntax) ==
                note::NoteSyntaxSnapshotBuildResult::Built &&
            note::NoteRenderSourcePlan::Build(decoratorSyntax, &decoratorPlan) ==
                note::NoteRenderSourcePlanBuildResult::Built &&
            decoratorPlan && decoratorPlan->CopyLinesForDifferentialTest(&decoratorLines);
        const auto hasDecoration = [&decoratorLines](size_t line,
                                                     note::NoteRenderSourceLineDecorationFlag flag) {
            return line < decoratorLines.size() && decoratorLines[line].decoration.has(flag);
        };
        const bool completeDecoratorCoverage = builtDecoratorPlan &&
            std::all_of(decoratorLines.begin(), decoratorLines.end(),
                        [](const note::NoteRenderSourceLinePlan& line) {
                            size_t cursor = line.content_span.start.value;
                            for (const note::NoteRenderSourceRun& run : line.runs) {
                                if (run.kind == note::NoteRenderSourceRunKind::BlockMath ||
                                    run.source_span.start.value != cursor ||
                                    run.source_span.end < run.source_span.start) {
                                    return false;
                                }
                                cursor = run.source_span.end.value;
                            }
                            return cursor == line.content_span.end.value;
                        });
        const note::NoteRenderSourceRun* headingHiddenSyntax = nullptr;
        if (builtDecoratorPlan && !decoratorLines.empty()) {
            const auto found = std::find_if(
                decoratorLines[0].runs.begin(), decoratorLines[0].runs.end(),
                [](const note::NoteRenderSourceRun& run) {
                    return run.kind == note::NoteRenderSourceRunKind::HiddenSyntax &&
                           run.source_span.start.value == 0 && run.source_span.end.value == 2;
                });
            if (found != decoratorLines[0].runs.end()) {
                headingHiddenSyntax = &*found;
            }
        }
        note::NoteRenderFinalDisplayRun hiddenPrefixDisplayRun;
        const bool builtHiddenPrefixDisplayRun =
            builtDecoratorPlan && headingHiddenSyntax &&
            note::BuildNoteRenderFinalDisplayRun(
                *decoratorSyntax, *headingHiddenSyntax, &hiddenPrefixDisplayRun) ==
                note::NoteRenderFinalDisplayRunBuildResult::Built;
        Expect(builtDecoratorPlan && completeDecoratorCoverage &&
                   hasDecoration(0, note::NoteRenderSourceLineDecorationHeading) &&
                   decoratorLines[0].decoration.heading_level == 1 &&
                   hasDecoration(1, note::NoteRenderSourceLineDecorationListItem) &&
                   hasDecoration(1, note::NoteRenderSourceLineDecorationTaskItem) &&
                   decoratorLines[1].decoration.task_checked &&
                   hasDecoration(2, note::NoteRenderSourceLineDecorationQuote) &&
                   hasDecoration(4, note::NoteRenderSourceLineDecorationHorizontalRule) &&
                   hasDecoration(6, note::NoteRenderSourceLineDecorationCodeFence) &&
                   hasDecoration(8, note::NoteRenderSourceLineDecorationCodeFence) &&
                   hasDecoration(11, note::NoteRenderSourceLineDecorationTableDivider) &&
                   builtHiddenPrefixDisplayRun && hiddenPrefixDisplayRun.display_text.empty() &&
                   hiddenPrefixDisplayRun.boundaries.size() == 2 &&
                   hiddenPrefixDisplayRun.boundaries.front().source_offset.value == 0 &&
                   hiddenPrefixDisplayRun.boundaries.back().source_offset.value == 2,
               "the final source plan owns structural decorators and zero-width Markdown syntax boundaries without a legacy line-cache lookup");

        const std::shared_ptr<const note::NoteRenderSourcePlan> preservedSourcePlan = sourcePlan;
        Expect(renderPlanCore.Apply({{2}, 0, L"revised "}) == note::NoteTextApplyResult::Applied &&
                   !preservedSourcePlan->Matches(renderPlanCore) &&
                   note::NoteRenderSourcePlan::Build(nullptr, &sourcePlan) ==
                   note::NoteRenderSourcePlanBuildResult::InvalidSyntaxSnapshot &&
                   sourcePlan == preservedSourcePlan,
               "stale or invalid source-plan candidates cannot replace the committed immutable render input");
    }

    {
        // Atomic-group lookup is a visible-range operation. A long document
        // must not scan every unrelated code/table/math owner merely to
        // expand one raw row to its shared-geometry boundary.
        constexpr size_t kAtomicGroupCount = 1024;
        std::wstring atomicGroupText;
        atomicGroupText.reserve(kAtomicGroupCount * 16);
        for (size_t index = 0; index < kAtomicGroupCount; ++index) {
            atomicGroupText += L"```\n";
            atomicGroupText += L"x\n";
            atomicGroupText += L"```\n";
        }
        note::NoteTextCore atomicIndexCore;
        atomicIndexCore.Reset(
            note::NoteId{9481}, note::NoteMetadata{L"atomic-index.md", L"atomic index"},
            std::move(atomicGroupText), 94, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> atomicIndexSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> atomicIndexPlan;
        std::vector<note::NoteRenderAtomicGroup> middleGroups;
        note::NoteRenderAtomicGroupQueryWork middleQueryWork;
        const size_t targetGroup = kAtomicGroupCount / 2;
        const note::LineIndex targetContentLine{targetGroup * 3 + 1};
        const bool builtAtomicIndex =
            note::NoteSyntaxSnapshot::BuildFromTextCore(
                atomicIndexCore, note::NoteContentKind::Markdown, &atomicIndexSyntax) ==
                note::NoteSyntaxSnapshotBuildResult::Built &&
            note::NoteRenderSourcePlan::Build(atomicIndexSyntax, &atomicIndexPlan) ==
                note::NoteRenderSourcePlanBuildResult::Built &&
            atomicIndexPlan->CopyAtomicGroupsIntersecting(
                targetContentLine, {targetContentLine.value + 1}, &middleGroups,
                &middleQueryWork);
        Expect(builtAtomicIndex,
               "long atomic-group fixture builds one immutable source plan");
        Expect(builtAtomicIndex && middleGroups.size() == 1,
               "atomic-group interval lookup returns exactly one local owner");
        if (middleGroups.size() == 1) {
            Expect(middleGroups.front().kind == note::NoteRenderAtomicGroupKind::CodeBlock,
                   "atomic-group interval lookup preserves the code-block owner kind");
            Expect(middleGroups.front().first_line <= targetContentLine &&
                       targetContentLine <= middleGroups.front().last_line,
                   "atomic-group interval lookup returns the local owner that contains the requested row");
        }
        Expect(builtAtomicIndex && middleQueryWork.visited_index_nodes < 64,
               "atomic-group interval lookup avoids scanning a long document");

        // The final owner resolver receives a full-document surface partition
        // but must still query only the raw-requested row's atomic groups.
        // Code blocks are deliberately not expanded by this owner policy; the
        // fixture therefore proves that unrelated groups are not enumerated
        // merely because the visible range covers the complete long note.
        note::NoteRenderFinalCompleteBuildInput atomicOwnerBuildInput;
        atomicOwnerBuildInput.content_kind = note::NoteContentKind::Markdown;
        atomicOwnerBuildInput.layout_key = {720, 96, 96, 94, 4, 12, true};
        atomicOwnerBuildInput.owner_input.render_active = true;
        atomicOwnerBuildInput.owner_input.editor_text_core_current = true;
        atomicOwnerBuildInput.owner_input.visible_lines = {
            {0}, {atomicIndexCore.logical_line_count()}};
        atomicOwnerBuildInput.owner_input.requested_editor_lines = {
            {targetContentLine, {targetContentLine.value + 1}}};
        atomicOwnerBuildInput.owner_input.caret_line = targetContentLine.value;
        DeterministicFinalMeasurementProvider atomicOwnerMeasurementProvider;
        std::shared_ptr<const note::NoteRenderFinalPublication> atomicOwnerPublication;
        std::shared_ptr<const note::NotePresentationOwnerPlan> atomicOwnerPlan;
        note::NotePresentationOwnerPlanBuildWork atomicOwnerWork;
        const bool builtAtomicOwnerPlan =
            note::NoteRenderFinalTransaction::BuildComplete(
                atomicIndexCore, atomicOwnerBuildInput, atomicOwnerMeasurementProvider,
                &atomicOwnerPublication) == note::NoteRenderFinalCompleteBuildResult::Built &&
            atomicOwnerPublication && atomicOwnerPublication->placement() &&
            note::NotePresentationOwnerPlan::Build(
                atomicOwnerBuildInput.owner_input, atomicIndexCore,
                atomicOwnerBuildInput.layout_key, atomicOwnerPublication->placement(),
                &atomicOwnerPlan, &atomicOwnerWork) ==
                note::NotePresentationOwnerPlanBuildResult::Built;
        Expect(builtAtomicOwnerPlan && atomicOwnerPlan && atomicOwnerPlan->valid() &&
                   atomicOwnerWork.atomic_group_queries == 1 &&
                   atomicOwnerWork.atomic_group_index_nodes < 64,
               "the final owner resolver expands only groups intersecting the raw requested row, not every group in a long visible document");
    }

    {
        note::NoteTextCore placementCore;
        placementCore.Reset(note::NoteId{949}, note::NoteMetadata{L"placement.md", L"placement"},
                            L"# Title\nalpha **bold** $x$", 94, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> placementSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> placementSource;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   placementCore, note::NoteContentKind::Markdown, &placementSyntax) ==
                   note::NoteSyntaxSnapshotBuildResult::Built &&
                   note::NoteRenderSourcePlan::Build(placementSyntax, &placementSource) ==
                   note::NoteRenderSourcePlanBuildResult::Built,
               "placement snapshot test creates one exact parser-derived source plan");

        std::vector<note::NoteRenderLineLayout> placementLineSpecs;
        placementLineSpecs.reserve(placementSource->line_count());
        for (size_t index = 0; index < placementSource->line_count(); ++index) {
            placementLineSpecs.push_back({16, static_cast<uint32_t>(80 + index * 10)});
        }
        note::NoteRenderLineLayoutMap::Snapshot placementLineLayouts;
        const note::NoteRenderLayoutKey placementKey{720, 96, 96, 2, 4, 12, true};
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> placementLayout;
        Expect(note::NoteRenderLineLayoutMap::Build(placementLineSpecs, &placementLineLayouts) &&
                   note::NoteRenderLayoutSnapshot::Build(
                       placementSyntax, placementKey, placementLineLayouts, &placementLayout) ==
                   note::NoteRenderLayoutSnapshotBuildResult::Built,
               "placement snapshot test binds measured row geometry to the same syntax publication");

        std::vector<note::NoteRenderLinePlacement> placements;
        placements.resize(placementSource->line_count());
        for (size_t line = 0; line < placementSource->line_count(); ++line) {
            note::NoteRenderSourceLinePlan sourceLine;
            if (!placementSource->ResolveLine({line}, &sourceLine)) continue;
            int x = 12;
            for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                const int width = 20 + static_cast<int>(run.source_span.end - run.source_span.start);
                placements[line].runs.push_back(note::NoteRenderRunPlacement{
                    run.source_span,
                    x,
                    width,
                    {{run.source_span.start, x}, {run.source_span.end, x + width}},
                });
                x += width + 2;
            }
        }
        std::shared_ptr<const note::NoteRenderPlacementSnapshot> placementSnapshot;
        note::NoteRenderLinePlacement resolvedInitialPlacementLine;
        Expect(note::NoteRenderPlacementSnapshot::Build(
                   placementSource, placementLayout, placements, &placementSnapshot) ==
                   note::NoteRenderPlacementSnapshotBuildResult::Built &&
                   placementSnapshot && placementSnapshot->valid() &&
                   placementSnapshot->Matches(placementCore, placementKey) &&
                   placementSnapshot->line_count() == placementSource->line_count() &&
                   placementSnapshot->ResolveLine({0}, &resolvedInitialPlacementLine),
               "placement snapshot publishes one shared run/boundary geometry for drawing and input consumers");

        std::shared_ptr<const note::NoteSyntaxSnapshot> separatelyParsedSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> separatelyParsedSource;
        Expect(note::NoteSyntaxSnapshot::BuildFromTextCore(
                   placementCore, note::NoteContentKind::Markdown, &separatelyParsedSyntax) ==
                   note::NoteSyntaxSnapshotBuildResult::Built &&
                   note::NoteRenderSourcePlan::Build(
                       separatelyParsedSyntax, &separatelyParsedSource) ==
                   note::NoteRenderSourcePlanBuildResult::Built &&
                   note::NoteRenderPlacementSnapshot::Build(
                       separatelyParsedSource, placementLayout, placements, &placementSnapshot) ==
                   note::NoteRenderPlacementSnapshotBuildResult::SourceLayoutIdentityMismatch,
               "placement rejects separately parsed but text-equal syntax so its source and layout cannot drift");

        const std::shared_ptr<const note::NoteRenderPlacementSnapshot> preservedPlacement =
            placementSnapshot;
        std::vector<note::NoteRenderLinePlacement> invalidVisualPlacements;
        bool brokeVisualFragment =
            preservedPlacement->CopyLinesForDifferentialTest(&invalidVisualPlacements);
        if (brokeVisualFragment) {
            brokeVisualFragment = false;
            for (note::NoteRenderLinePlacement& line : invalidVisualPlacements) {
                if (line.runs.empty()) continue;
                note::NoteRenderRunPlacement& run = line.runs.front();
                run.fragments = {{run.source_span, run.x_px, run.width_px, 16, 1}};
                brokeVisualFragment = true;
                break;
            }
        }
        Expect(brokeVisualFragment &&
                   note::NoteRenderPlacementSnapshot::Build(
                       placementSource, placementLayout, invalidVisualPlacements,
                       &placementSnapshot) == note::NoteRenderPlacementSnapshotBuildResult::BoundaryMismatch &&
                   placementSnapshot == preservedPlacement,
               "a visual fragment outside its measured logical row cannot publish inconsistent caret or selection geometry");
        bool brokeBoundary = false;
        for (note::NoteRenderLinePlacement& line : placements) {
            if (!line.runs.empty()) {
                ++line.runs.front().boundaries.back().source_offset.value;
                brokeBoundary = true;
                break;
            }
        }
        Expect(brokeBoundary &&
                   note::NoteRenderPlacementSnapshot::Build(
                       placementSource, placementLayout, placements, &placementSnapshot) ==
                   note::NoteRenderPlacementSnapshotBuildResult::BoundaryMismatch &&
                   placementSnapshot == preservedPlacement,
               "invalid source/display boundaries cannot replace a committed placement snapshot");

        note::NoteRenderLinePlacement remeasuredLine;
        int remeasuredX = 18;
        note::NoteRenderSourceLinePlan remeasuredSourceLine;
        const bool resolvedRemeasuredSourceLine =
            placementSource->ResolveLine({1}, &remeasuredSourceLine);
        for (const note::NoteRenderSourceRun& run : remeasuredSourceLine.runs) {
            const int width = 40 + static_cast<int>(run.source_span.end.value - run.source_span.start.value);
            remeasuredLine.runs.push_back(note::NoteRenderRunPlacement{
                run.source_span, remeasuredX, width,
                {{run.source_span.start, remeasuredX}, {run.source_span.end, remeasuredX + width}},
            });
            remeasuredX += width + 3;
        }
        std::shared_ptr<const note::NoteRenderPlacementSnapshot> locallySplicedPlacement;
        note::NoteRenderPlacementSnapshotLocalSpliceWork placementSpliceWork;
        note::NoteRenderLinePlacement resolvedSplicedPrefix;
        note::NoteRenderLinePlacement resolvedSplicedLine;
        Expect(resolvedRemeasuredSourceLine &&
                   note::NoteRenderPlacementSnapshot::BuildLocalSplice(
                   preservedPlacement, placementSource, placementLayout, {1}, {2},
                   {remeasuredLine}, &locallySplicedPlacement, &placementSpliceWork) ==
                   note::NoteRenderPlacementSnapshotLocalSpliceResult::Built &&
                   locallySplicedPlacement &&
                   placementSpliceWork.replacement_line_payloads == 1 &&
                   placementSpliceWork.tail_line_payload_rewrites == 0 &&
                   preservedPlacement->SharesLinePayloadForDifferentialTest(
                       *locallySplicedPlacement, {0}) &&
                   !preservedPlacement->SharesLinePayloadForDifferentialTest(
                       *locallySplicedPlacement, {1}) &&
                   placementSnapshot->ResolveLine({0}, &resolvedSplicedPrefix) &&
                   locallySplicedPlacement->ResolveLine({1}, &resolvedSplicedLine) &&
                   resolvedSplicedLine.runs.front().x_px == 18,
               "a persistent placement splice replaces only remeasured rows in one source/layout publication");
        const std::shared_ptr<const note::NoteRenderPlacementSnapshot> preservedSplicedPlacement =
            locallySplicedPlacement;
        Expect(note::NoteRenderPlacementSnapshot::BuildLocalSplice(
                   preservedPlacement, separatelyParsedSource, placementLayout, {1}, {2},
                   {remeasuredLine}, &locallySplicedPlacement) ==
                   note::NoteRenderPlacementSnapshotLocalSpliceResult::SourceLayoutIdentityMismatch &&
                   locallySplicedPlacement == preservedSplicedPlacement,
               "a placement splice rejects a source plan from another immutable syntax publication");
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> separatelyParsedLayout;
        Expect(note::NoteRenderLayoutSnapshot::Build(
                   separatelyParsedSyntax, placementKey, placementLineLayouts,
                   &separatelyParsedLayout) == note::NoteRenderLayoutSnapshotBuildResult::Built &&
                   note::NoteRenderPlacementSnapshot::BuildLocalSplice(
                       preservedPlacement, separatelyParsedSource, separatelyParsedLayout, {1}, {2},
                       {remeasuredLine}, &locallySplicedPlacement) ==
                       note::NoteRenderPlacementSnapshotLocalSpliceResult::ReplacementRejected &&
                   locallySplicedPlacement == preservedSplicedPlacement,
               "a separately rebuilt but layout-matching source plan cannot claim unchanged placement rows");

        note::NotePresentationOwnerInput ownerInput;
        ownerInput.render_active = true;
        ownerInput.editor_text_core_current = true;
        ownerInput.visible_lines = {{0}, {placementCore.logical_line_count()}};
        ownerInput.requested_editor_lines.push_back({{1}, {2}});
        ownerInput.caret_line = 1;
        std::shared_ptr<const note::NotePresentationOwnerPlan> ownerPlan;
        Expect(note::NotePresentationOwnerPlan::Build(
                   ownerInput, placementCore, placementKey, preservedPlacement, &ownerPlan) ==
                   note::NotePresentationOwnerPlanBuildResult::Built &&
                   ownerPlan && ownerPlan->valid() &&
                   ownerPlan->frame().frame_kind == note::NotePresentationFrameKind::DrawCommitted &&
                   ownerPlan->frame().caret_presenter == note::NoteCaretPresenter::NativeEditor &&
                   ownerPlan->owner_ranges().size() == 2 &&
                   ownerPlan->owner_ranges()[0].owner ==
                       note::NotePresentationLineOwner::CommittedPlacement &&
                   ownerPlan->owner_ranges()[1].owner ==
                       note::NotePresentationLineOwner::NativeEditor &&
                   ownerPlan->Matches(placementCore, placementKey),
               "presentation owner plan partitions visible rows into one committed or native owner and gives a raw caret its native presenter");

        std::shared_ptr<const note::NoteRenderFinalPublication> finalPublication;
        Expect(note::NoteRenderFinalPublication::Build(
                   ownerInput, placementCore, placementKey, placementSource, placementLayout,
                   preservedPlacement, &finalPublication) ==
                   note::NoteRenderFinalPublicationBuildResult::Built &&
                   finalPublication && finalPublication->valid() &&
                   finalPublication->Matches(placementCore, placementKey) &&
                   finalPublication->syntax() == placementSyntax &&
                   finalPublication->source_plan() == placementSource &&
                   finalPublication->layout() == placementLayout &&
                   finalPublication->placement() == preservedPlacement &&
                   finalPublication->owner_plan() &&
                   finalPublication->owner_plan()->placement() == preservedPlacement,
               "a final publication owns one exact syntax/source/layout/placement/owner chain for one structured frame");

        note::NoteRenderFinalCompleteBuildInput completeTransactionInput;
        completeTransactionInput.content_kind = note::NoteContentKind::Markdown;
        completeTransactionInput.layout_key = placementKey;
        completeTransactionInput.owner_input = ownerInput;
        DeterministicFinalMeasurementProvider completeMeasurementProvider;
        std::shared_ptr<const note::NoteRenderFinalPublication> completeTransactionPublication;
        Expect(note::NoteRenderFinalTransaction::BuildComplete(
                       placementCore, completeTransactionInput, completeMeasurementProvider,
                       &completeTransactionPublication) ==
                       note::NoteRenderFinalCompleteBuildResult::Built &&
                   completeTransactionPublication && completeTransactionPublication->valid() &&
                   completeTransactionPublication->Matches(placementCore, placementKey) &&
                   completeTransactionPublication->has_continuity_checkpoint() &&
                   completeTransactionPublication->owner_plan() &&
                   completeTransactionPublication->owner_plan()->frame().frame_kind ==
                       note::NotePresentationFrameKind::DrawCommitted,
               "one complete final transaction publishes only an exact structured frame chain");
        const std::shared_ptr<const note::NoteRenderFinalPublication>
            preservedCompleteTransactionPublication = completeTransactionPublication;
        note::NoteRenderFinalCompleteBuildInput pendingGeometryTransactionInput =
            completeTransactionInput;
        pendingGeometryTransactionInput.owner_input.geometry_may_change = true;
        Expect(note::NoteRenderFinalTransaction::BuildComplete(
                   placementCore, pendingGeometryTransactionInput, completeMeasurementProvider,
                   &completeTransactionPublication) ==
                   note::NoteRenderFinalCompleteBuildResult::RequiresNativeFallback &&
                   completeTransactionPublication == preservedCompleteTransactionPublication,
               "a rejected complete transaction never exposes its partial child snapshots");
        note::NoteRenderFinalCompleteBuildInput invalidPlacementTransactionInput =
            completeTransactionInput;
        InvalidPlacementFinalMeasurementProvider invalidPlacementMeasurementProvider;
        Expect(note::NoteRenderFinalTransaction::BuildComplete(
                   placementCore, invalidPlacementTransactionInput,
                   invalidPlacementMeasurementProvider,
                   &completeTransactionPublication) ==
                   note::NoteRenderFinalCompleteBuildResult::PlacementBuildFailed &&
                   completeTransactionPublication == preservedCompleteTransactionPublication,
               "an invalid complete-transaction measurement preserves the last complete publication");
        note::NoteRenderFinalCompleteBuildInput rawOnlyTransactionInput =
            completeTransactionInput;
        rawOnlyTransactionInput.content_kind = note::NoteContentKind::PlainText;
        Expect(note::NoteRenderFinalTransaction::BuildComplete(
                   placementCore, rawOnlyTransactionInput, completeMeasurementProvider,
                   &completeTransactionPublication) ==
                   note::NoteRenderFinalCompleteBuildResult::RequiresNativeFallback &&
                   completeTransactionPublication == preservedCompleteTransactionPublication,
               "plain-text content never publishes an empty structured final frame");

        const std::shared_ptr<const note::NoteRenderFinalPublication> preservedFinalPublication =
            finalPublication;
        Expect(note::NoteRenderFinalPublication::Build(
                   ownerInput, placementCore, placementKey, separatelyParsedSource, placementLayout,
                   preservedPlacement, &finalPublication) ==
                   note::NoteRenderFinalPublicationBuildResult::SourceLayoutIdentityMismatch &&
                   finalPublication == preservedFinalPublication,
               "a final publication rejects a separately parsed source plan instead of mixing it with current layout or placement");

        note::NotePresentationOwnerInput geometryPendingInput = ownerInput;
        geometryPendingInput.geometry_may_change = true;
        Expect(note::NoteRenderFinalPublication::Build(
                   geometryPendingInput, placementCore, placementKey, placementSource, placementLayout,
                   preservedPlacement, &finalPublication) ==
                   note::NoteRenderFinalPublicationBuildResult::RequiresNativeFallback &&
                   finalPublication == preservedFinalPublication,
               "a geometry-changing candidate never publishes a partial structured frame and requires whole-visible native fallback");

        Expect(placementCore.Apply({{2}, 0, L"changed "}) == note::NoteTextApplyResult::Applied &&
                   note::NotePresentationOwnerPlan::Build(
                       ownerInput, placementCore, placementKey, preservedPlacement, &ownerPlan) ==
                   note::NotePresentationOwnerPlanBuildResult::Built &&
                   ownerPlan->frame().frame_kind ==
                       note::NotePresentationFrameKind::NativeEditorFallback &&
                   ownerPlan->owner_ranges().size() == 1 &&
                   ownerPlan->owner_ranges().front().owner ==
                       note::NotePresentationLineOwner::NativeEditor,
               "a placement snapshot from another canonical revision cannot contribute structured rows to an owner plan");
        Expect(note::NoteRenderFinalPublication::Build(
                   ownerInput, placementCore, placementKey, placementSource, placementLayout,
                   preservedPlacement, &finalPublication) ==
                   note::NoteRenderFinalPublicationBuildResult::StaleCanonicalRevision &&
                   finalPublication == preservedFinalPublication,
               "a stale canonical revision cannot replace a committed final publication");
    }

    {
        note::NoteTextCore texFinalCore;
        texFinalCore.Reset(
            note::NoteId{951}, note::NoteMetadata{L"final.tex", L"final tex"},
            L"TeX source \\(E = mc^2\\)\n$$\n\\sum_{k=1}^{n} k\n$$", 96, 4);
        note::NoteRenderFinalCompleteBuildInput texFinalInput;
        texFinalInput.content_kind = note::NoteContentKind::TeXSource;
        texFinalInput.layout_key = {720, 96, 96, 4, 4, 12, true};
        texFinalInput.owner_input.render_active = true;
        texFinalInput.owner_input.editor_text_core_current = true;
        texFinalInput.owner_input.visible_lines = {
            {0}, {texFinalCore.logical_line_count()}};
        DeterministicFinalMeasurementProvider texFinalMeasurementProvider;
        std::shared_ptr<const note::NoteRenderFinalPublication> texFinalPublication;
        const note::NoteRenderFinalCompleteBuildResult texFinalResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                texFinalCore, texFinalInput, texFinalMeasurementProvider, &texFinalPublication);
        bool hasTeXTextRun = false;
        bool hasTeXInlineMathRun = false;
        bool hasTeXBlockMathRun = false;
        if (texFinalPublication && texFinalPublication->source_plan()) {
            for (size_t line = 0; line < texFinalPublication->source_plan()->line_count(); ++line) {
                note::NoteRenderSourceLinePlan sourceLine;
                if (!texFinalPublication->source_plan()->ResolveLine({line}, &sourceLine)) continue;
                for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                    hasTeXTextRun = hasTeXTextRun || run.kind == note::NoteRenderSourceRunKind::Text;
                    hasTeXInlineMathRun = hasTeXInlineMathRun ||
                        run.kind == note::NoteRenderSourceRunKind::InlineMath;
                    hasTeXBlockMathRun = hasTeXBlockMathRun ||
                        run.kind == note::NoteRenderSourceRunKind::BlockMath;
                }
            }
        }
        Expect(texFinalResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   texFinalPublication && texFinalPublication->valid() &&
                   texFinalPublication->Matches(texFinalCore, texFinalInput.layout_key) &&
                   texFinalPublication->has_continuity_checkpoint() &&
                   texFinalPublication->syntax()->content_kind() == note::NoteContentKind::TeXSource &&
                   hasTeXTextRun && hasTeXInlineMathRun && hasTeXBlockMathRun,
               "a complete final transaction preserves TeX text with inline and block math runs");
    }

    {
        note::NoteTextCore interactionCore;
        interactionCore.Reset(
            note::NoteId{996}, note::NoteMetadata{L"interaction.md", L"interaction"},
            L"prefix [jump](target-note.md) suffix\nsecond row", 420, 3);
        note::NoteRenderFinalCompleteBuildInput interactionInput;
        interactionInput.content_kind = note::NoteContentKind::Markdown;
        interactionInput.layout_key = {720, 96, 96, 5, 4, 12, true};
        interactionInput.owner_input.render_active = true;
        interactionInput.owner_input.editor_text_core_current = true;
        interactionInput.owner_input.visible_lines = {
            {0}, {interactionCore.logical_line_count()}};
        interactionInput.owner_input.requested_editor_lines.push_back({{1}, {2}});
        interactionInput.owner_input.caret_line = 1;
        DeterministicFinalMeasurementProvider interactionMeasurementProvider;
        std::shared_ptr<const note::NoteRenderFinalPublication> interactionPublication;
        const note::NoteRenderFinalCompleteBuildResult interactionBuildResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                interactionCore, interactionInput, interactionMeasurementProvider,
                &interactionPublication);
        note::NoteRenderFinalResolvedLine committedInteractionLine;
        note::NoteRenderFinalResolvedLine nativeInteractionLine;
        note::NoteRenderSourceLinePlan interactionSourceLine;
        note::NoteRenderLinePlacement interactionPlacementLine;
        size_t interactionLinkRun = static_cast<size_t>(-1);
        if (interactionPublication && interactionPublication->source_plan() &&
            interactionPublication->placement() &&
            interactionPublication->source_plan()->ResolveLine({0}, &interactionSourceLine) &&
            interactionPublication->placement()->ResolveLine({0}, &interactionPlacementLine)) {
            for (size_t index = 0; index < interactionSourceLine.runs.size(); ++index) {
                if (interactionSourceLine.runs[index].kind == note::NoteRenderSourceRunKind::LinkText &&
                    !interactionSourceLine.runs[index].link_target.empty()) {
                    interactionLinkRun = index;
                    break;
                }
            }
        }
        const bool interactionHasLinkGeometry =
            interactionLinkRun != static_cast<size_t>(-1) &&
            interactionLinkRun < interactionPlacementLine.runs.size() &&
            !interactionPlacementLine.runs[interactionLinkRun].boundaries.empty();
        note::NoteRenderFinalHit interactionHit;
        note::NoteRenderFinalCaretGeometry interactionCaret;
        std::vector<note::NoteRenderFinalRect> interactionSelection;
        uint64_t interactionHorizontalExtent = 0;
        const int interactionLinkX = interactionHasLinkGeometry
            ? interactionPlacementLine.runs[interactionLinkRun].x_px +
                interactionPlacementLine.runs[interactionLinkRun].width_px / 2
            : 0;
        const note::Span interactionLinkSpan = interactionHasLinkGeometry
            ? interactionSourceLine.runs[interactionLinkRun].source_span
            : note::Span{};
        const note::NoteRenderFinalInteractionResult committedLineResult = interactionPublication
            ? note::ResolveNoteRenderFinalCommittedLine(
                *interactionPublication, {0}, &committedInteractionLine)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult nativeLineResult = interactionPublication
            ? note::ResolveNoteRenderFinalCommittedLine(
                *interactionPublication, {1}, &nativeInteractionLine)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult hitResult = interactionPublication && interactionHasLinkGeometry
            ? note::HitTestNoteRenderFinalPublication(
                *interactionPublication, interactionLinkX, 8, &interactionHit)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult caretResult = interactionPublication && interactionHasLinkGeometry
            ? note::ResolveNoteRenderFinalCaret(
                *interactionPublication, interactionLinkSpan.start, &interactionCaret)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult selectionResult = interactionPublication && interactionHasLinkGeometry
            ? note::ResolveNoteRenderFinalSelection(
                *interactionPublication, interactionLinkSpan, &interactionSelection)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult horizontalExtentResult = interactionPublication
            ? note::ResolveNoteRenderFinalHorizontalExtent(
                *interactionPublication, &interactionHorizontalExtent)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        Expect(interactionBuildResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   interactionPublication && interactionHasLinkGeometry &&
                   committedLineResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   committedInteractionLine.line_index == note::LineIndex{0} &&
                   committedInteractionLine.layout.top_px == 0 &&
                   committedInteractionLine.layout.bottom_px == 16 &&
                   committedInteractionLine.source_line.runs.size() ==
                       committedInteractionLine.placement_line.runs.size() &&
                   nativeLineResult == note::NoteRenderFinalInteractionResult::NativeEditorOwner &&
                   hitResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   interactionHit.line_index == note::LineIndex{0} &&
                   interactionHit.is_link && !interactionHit.is_legacy_link_id &&
                   interactionHit.link_target == L"target-note.md" &&
                   interactionHit.source_span.start == interactionLinkSpan.start &&
                   interactionHit.source_span.end == interactionLinkSpan.end &&
                   caretResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   interactionCaret.line_index == note::LineIndex{0} &&
                   interactionCaret.top_px == 0 && interactionCaret.bottom_px == 16 &&
                   selectionResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   interactionSelection.size() == 1 &&
                   interactionSelection.front().top_px == 0 &&
                   interactionSelection.front().bottom_px == 16 &&
                   interactionSelection.front().right_px > interactionSelection.front().left_px &&
                   horizontalExtentResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   interactionHorizontalExtent == 93,
               "one final publication supplies paint, link hit test, caret, selection, and scroll geometry while native-owned rows expose none of its placement");
    }

    {
        // The final hit result must also preserve the application's persisted
        // <link=...> target.  The Win32 view uses this field directly after a
        // final publication, rather than consulting the previous legacy
        // LineCache after an accepted edit.
        note::NoteTextCore legacyLinkCore;
        legacyLinkCore.Reset(
            note::NoteId{9970}, note::NoteMetadata{L"legacy-link.md", L"legacy link"},
            L"<link=workspace-jump><lu>jump</></>", 420, 3);
        note::NoteRenderFinalCompleteBuildInput legacyLinkInput;
        legacyLinkInput.content_kind = note::NoteContentKind::Markdown;
        legacyLinkInput.layout_key = {720, 96, 96, 5, 4, 12, true};
        legacyLinkInput.owner_input.render_active = true;
        legacyLinkInput.owner_input.editor_text_core_current = true;
        legacyLinkInput.owner_input.visible_lines = {
            {0}, {legacyLinkCore.logical_line_count()}};
        DeterministicFinalMeasurementProvider legacyLinkMeasurementProvider;
        std::shared_ptr<const note::NoteRenderFinalPublication> legacyLinkPublication;
        const note::NoteRenderFinalCompleteBuildResult legacyLinkBuildResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                legacyLinkCore, legacyLinkInput, legacyLinkMeasurementProvider,
                &legacyLinkPublication);
        note::NoteRenderSourceLinePlan legacyLinkSourceLine;
        note::NoteRenderLinePlacement legacyLinkPlacementLine;
        size_t legacyLinkRun = static_cast<size_t>(-1);
        if (legacyLinkPublication && legacyLinkPublication->source_plan() &&
            legacyLinkPublication->placement() &&
            legacyLinkPublication->source_plan()->ResolveLine({0}, &legacyLinkSourceLine) &&
            legacyLinkPublication->placement()->ResolveLine({0}, &legacyLinkPlacementLine)) {
            for (size_t index = 0; index < legacyLinkSourceLine.runs.size(); ++index) {
                const auto& styles = legacyLinkSourceLine.runs[index].styles;
                if (std::any_of(styles.begin(), styles.end(), [](const auto& style) {
                        return style.kind == note::StyleKind::LinkId &&
                               style.value == L"workspace-jump";
                    })) {
                    legacyLinkRun = index;
                    break;
                }
            }
        }
        const bool legacyLinkHasGeometry =
            legacyLinkRun != static_cast<size_t>(-1) &&
            legacyLinkRun < legacyLinkPlacementLine.runs.size() &&
            !legacyLinkPlacementLine.runs[legacyLinkRun].boundaries.empty();
        note::NoteRenderFinalHit legacyLinkHit;
        const note::NoteRenderFinalInteractionResult legacyLinkHitResult =
            legacyLinkPublication && legacyLinkHasGeometry
                ? note::HitTestNoteRenderFinalPublication(
                    *legacyLinkPublication,
                    legacyLinkPlacementLine.runs[legacyLinkRun].x_px +
                        legacyLinkPlacementLine.runs[legacyLinkRun].width_px / 2,
                    8, &legacyLinkHit)
                : note::NoteRenderFinalInteractionResult::InvalidPublication;
        Expect(legacyLinkBuildResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   legacyLinkHasGeometry &&
                   legacyLinkHitResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   legacyLinkHit.is_link && legacyLinkHit.is_legacy_link_id &&
                   legacyLinkHit.link_target == L"workspace-jump",
               "a final publication resolves persisted link IDs from its own immutable source run");
    }

    {
        note::NoteTextCore wrappedInteractionCore;
        wrappedInteractionCore.Reset(
            note::NoteId{997}, note::NoteMetadata{L"wrapped.md", L"wrapped"}, L"abcdef", 421, 3);
        note::NoteRenderFinalCompleteBuildInput wrappedInteractionInput;
        wrappedInteractionInput.content_kind = note::NoteContentKind::Markdown;
        wrappedInteractionInput.layout_key = {80, 96, 96, 6, 4, 8, true};
        wrappedInteractionInput.owner_input.render_active = true;
        wrappedInteractionInput.owner_input.editor_text_core_current = true;
        wrappedInteractionInput.owner_input.visible_lines = {
            {0}, {wrappedInteractionCore.logical_line_count()}};
        WrappedFinalMeasurementProvider wrappedMeasurementProvider;
        std::shared_ptr<const note::NoteRenderFinalPublication> wrappedPublication;
        const note::NoteRenderFinalCompleteBuildResult wrappedBuildResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                wrappedInteractionCore, wrappedInteractionInput, wrappedMeasurementProvider,
                &wrappedPublication);
        note::NoteRenderFinalHit wrappedLowerHit;
        note::NoteRenderFinalCaretGeometry wrappedBeforeCaret;
        note::NoteRenderFinalCaretGeometry wrappedAfterCaret;
        std::vector<note::NoteRenderFinalRect> wrappedSelection;
        const note::NoteRenderFinalInteractionResult wrappedHitResult = wrappedPublication
            ? note::HitTestNoteRenderFinalPublication(*wrappedPublication, 12, 20, &wrappedLowerHit)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult wrappedBeforeCaretResult = wrappedPublication
            ? note::ResolveNoteRenderFinalCaret(
                *wrappedPublication, {3}, note::NoteRenderFinalCaretAffinity::BeforeVisualWrap,
                &wrappedBeforeCaret)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult wrappedAfterCaretResult = wrappedPublication
            ? note::ResolveNoteRenderFinalCaret(
                *wrappedPublication, {3}, note::NoteRenderFinalCaretAffinity::AfterVisualWrap,
                &wrappedAfterCaret)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult wrappedSelectionResult = wrappedPublication
            ? note::ResolveNoteRenderFinalSelection(*wrappedPublication, {{0}, {6}}, &wrappedSelection)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        Expect(wrappedBuildResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   wrappedPublication &&
                   wrappedHitResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   wrappedLowerHit.source_offset == note::Utf16CodeUnitOffset{3} &&
                   wrappedBeforeCaretResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   wrappedBeforeCaret.top_px == 0 && wrappedBeforeCaret.bottom_px == 16 &&
                   wrappedAfterCaretResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   wrappedAfterCaret.top_px == 16 && wrappedAfterCaret.bottom_px == 32 &&
                   wrappedSelectionResult == note::NoteRenderFinalInteractionResult::Resolved &&
                   wrappedSelection.size() == 2 &&
                   wrappedSelection[0].top_px == 0 && wrappedSelection[0].bottom_px == 16 &&
                   wrappedSelection[1].top_px == 16 && wrappedSelection[1].bottom_px == 32,
               "wrapped final placement supplies one shared fragment geometry for paint, click, caret affinity, and selection");
    }

    {
        HDC gdiMeasurementDc = CreateCompatibleDC(nullptr);
        HGDIOBJ previousFont = nullptr;
        if (gdiMeasurementDc) {
            previousFont = SelectObject(gdiMeasurementDc, GetStockObject(DEFAULT_GUI_FONT));
        }
        note::NoteTextCore gdiWrappedCore;
        gdiWrappedCore.Reset(
            note::NoteId{998}, note::NoteMetadata{L"gdi-wrap.md", L"gdi-wrap"},
            L"alpha beta gamma delta epsilon zeta eta theta iota kappa lambda", 422, 3);
        note::NoteRenderFinalCompleteBuildInput gdiInput;
        gdiInput.content_kind = note::NoteContentKind::Markdown;
        gdiInput.layout_key = {96, 96, 96, 7, 4, 8, true};
        gdiInput.owner_input.render_active = true;
        gdiInput.owner_input.editor_text_core_current = true;
        gdiInput.owner_input.visible_lines = {{0}, {gdiWrappedCore.logical_line_count()}};
        note::NoteRenderFinalGdiMeasurementProvider gdiProvider(gdiMeasurementDc);
        const bool gdiStableRawAllocations = [&]() {
            struct Fixture {
                const wchar_t* text;
                size_t edit_line;
            };
            const Fixture fixtures[] = {
                {L"prefix\n# heading\ntail", 1},
                {L"prefix\n<m=60>tall</>\ntail", 1},
                {L"prefix\ninline $\\frac{1}{2}$ tail\ntail", 1},
                {L"prefix\n::: note\n<m=60>body</>\n:::\ntail", 2},
                {L"prefix\n```\ncode\n```\ntail", 2},
                {L"prefix\n| <m=60>head</> | value |\n| --- | --- |\n| <m=60>cell</> | item |\ntail", 2},
                {L"prefix\n$$\\frac{\\frac{1}{2}}{\\frac{3}{4}}$$\ntail", 1},
                {L"prefix\n$$\n\\frac{\\frac{\\frac{1}{2}}{3}}{\\frac{4}{5}}\n$$\ntail", 2},
                {L"prefix\n<link=abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz>x</>\ntail", 1},
                {L"prefix\nalpha beta gamma delta epsilon zeta eta theta iota kappa lambda\ntail", 1},
            };
            bool sawRowPadding = false;
            bool sawGroupPadding = false;
            bool sawNecessaryGrowth = false;
            for (const uint32_t dpi : {96u, 144u, 192u}) {
                for (const bool wrap : {false, true}) {
                    for (const auto& fixture : fixtures) {
                        note::NoteTextCore core;
                        core.Reset(note::NoteId{1030}, note::NoteMetadata{L"stable.md", L"stable"},
                                   fixture.text, 460, 3);
                        auto input = gdiInput;
                        input.layout_key = {wrap ? 160u : 640u, dpi, dpi, 7, 4, 8, wrap};
                        input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                        std::shared_ptr<const note::NoteRenderFinalPublication> publication;
                        if (note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider,
                                &publication) != note::NoteRenderFinalCompleteBuildResult::Built) return false;
                        auto owner = input.owner_input;
                        owner.caret_line = fixture.edit_line;
                        owner.requested_editor_lines = {{{fixture.edit_line}, {fixture.edit_line + 1}}};
                        std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> frame;
                        if (note::NoteRenderFinalPresentationSnapshot::Build(core, publication, owner,
                                gdiProvider, &frame) !=
                            note::NoteRenderFinalPresentationSnapshotBuildResult::Built) return false;
                        // One edit unit, including complete atomic expansion. Compare
                        // against an independent natural raw measurement, not the
                        // padded surfaces returned by the presentation builder.
                        const auto range = std::find_if(frame->surface_ranges().begin(),
                            frame->surface_ranges().end(), [](const auto& candidate) {
                                return candidate.surface == note::NoteRenderFinalSurfaceKind::RawPaint;
                            });
                        if (range == frame->surface_ranges().end()) return false;
                        std::vector<note::NoteRenderFinalRawLineSurface> natural;
                        if (!gdiProvider.MeasureRawLines(core, input.layout_key, range->lines.first,
                                range->lines.last_exclusive, &natural) || natural.empty()) return false;
                        const auto structuredFirst = note::NoteRenderLineLayoutMap::LineAt(
                            publication->layout()->line_layouts(), range->lines.first);
                        const auto structuredTail = note::NoteRenderLineLayoutMap::LineAt(
                            publication->layout()->line_layouts(), range->lines.last_exclusive);
                        const auto hybridTail = note::NoteRenderLineLayoutMap::LineAt(
                            frame->line_layouts(), range->lines.last_exclusive);
                        if (!structuredFirst || !structuredTail || !hybridTail) return false;
                        uint64_t naturalHeight = 0;
                        for (const auto& row : natural) naturalHeight += row.layout.height_px;
                        const uint64_t structuredHeight = structuredTail->top_px - structuredFirst->top_px;
                        const uint64_t expectedHeight = std::max(naturalHeight, structuredHeight);
                        if (hybridTail->top_px != structuredFirst->top_px + expectedHeight ||
                            hybridTail->layout.height_px != structuredTail->layout.height_px ||
                            frame->SurfaceAt(range->lines.last_exclusive) !=
                                note::NoteRenderFinalSurfaceKind::StructuredPaint) return false;
                        if (naturalHeight < structuredHeight) {
                            if (natural.size() == 1) sawRowPadding = true;
                            else sawGroupPadding = true;
                            if (hybridTail->top_px != structuredTail->top_px) return false;
                        }
                        sawNecessaryGrowth = sawNecessaryGrowth || naturalHeight > structuredHeight;
                        for (size_t index = 0; index < natural.size(); ++index) {
                            const auto& measured = natural[index];
                            note::NoteRenderFinalRawLineSurface raw;
                            if (!frame->ResolveRawLine(measured.line_index, &raw) ||
                                raw.layout.height_px < measured.layout.height_px ||
                                raw.display.display_text != measured.display.display_text ||
                                raw.placement.runs.front().fragments.size() !=
                                    measured.placement.runs.front().fragments.size()) return false;
                            const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                                frame->line_layouts(), raw.line_index);
                            if (!layout) return false;
                            const auto& run = raw.placement.runs.front();
                            note::Utf16CodeUnitOffset nextVisual;
                            if (note::MoveNoteRenderFinalPresentationVertical(*frame,
                                    run.boundaries.back().source_offset, run.boundaries.back().x_px,
                                    note::NoteRenderFinalVerticalMove::Down, 1, &nextVisual) !=
                                    note::NoteRenderFinalPresentationInteractionResult::Resolved) return false;
                            const auto nextLine = note::NoteSourceLineMap::FindByOffset(core.source_line_map(), nextVisual);
                            if (!nextLine || nextLine->line_index.value != raw.line_index.value + 1) return false;
                            for (size_t fragmentIndex = 0; fragmentIndex < run.fragments.size(); ++fragmentIndex) {
                                const auto& fragment = run.fragments[fragmentIndex];
                                const auto& original = measured.placement.runs.front().fragments[fragmentIndex];
                                if (fragment.height_px != original.height_px ||
                                    fragment.top_offset_px != original.top_offset_px ||
                                    static_cast<uint64_t>(fragment.top_offset_px) + fragment.height_px >
                                        raw.layout.height_px) return false;
                                const auto boundary = std::find_if(run.boundaries.begin(), run.boundaries.end(),
                                    [fragmentIndex](const auto& candidate) {
                                        return candidate.fragment_index == fragmentIndex;
                                    });
                                if (boundary == run.boundaries.end()) return false;
                                note::NoteRenderFinalHit hit;
                                if (note::HitTestNoteRenderFinalPresentation(*frame, boundary->x_px,
                                        layout->top_px + fragment.top_offset_px, &hit) !=
                                        note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                                    hit.line_index != raw.line_index || hit.source_offset != boundary->source_offset) return false;
                            }
                            if (raw.layout.height_px > measured.layout.height_px) {
                                const auto& boundary = run.boundaries.back();
                                const auto& fragment = run.fragments[boundary.fragment_index];
                                note::NoteRenderFinalHit hit;
                                note::NoteRenderFinalCaretGeometry caret;
                                std::vector<note::NoteRenderFinalRect> selection;
                                if (note::HitTestNoteRenderFinalPresentation(*frame, boundary.x_px,
                                        layout->bottom_px - 1, &hit) !=
                                        note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                                    hit.line_index != raw.line_index || hit.source_offset != boundary.source_offset ||
                                    note::ResolveNoteRenderFinalPresentationCaret(*frame, hit.source_offset, &caret) !=
                                        note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                                    caret.bottom_px - caret.top_px != fragment.height_px ||
                                    note::ResolveNoteRenderFinalPresentationSelection(*frame, raw.source_span, &selection) !=
                                        note::NoteRenderFinalPresentationInteractionResult::Resolved) return false;
                                for (const auto& rect : selection) {
                                    if (rect.bottom_px > layout->top_px + measured.layout.height_px) return false;
                                }
                            }
                        }
                        // Repeated ownership-only rebuilds have no accumulated padding
                        // or stale structured Y coordinates; the canonical text is untouched.
                        std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> again, structured;
                        if (note::NoteRenderFinalPresentationSnapshot::Build(core, publication, owner,
                                gdiProvider, &again) != note::NoteRenderFinalPresentationSnapshotBuildResult::Built ||
                            note::NoteRenderFinalPresentationSnapshot::Build(core, publication,
                                input.owner_input, gdiProvider, &structured) !=
                                note::NoteRenderFinalPresentationSnapshotBuildResult::Built ||
                            again->line_layouts().total_height_px() != frame->line_layouts().total_height_px() ||
                            structured->line_layouts().total_height_px() !=
                                publication->layout()->line_layouts().total_height_px() ||
                            core.CopyRawRange({0}, core.text_length()) != fixture.text) return false;
                    }
                }
            }
            return sawRowPadding && sawGroupPadding && sawNecessaryGrowth;
        }();
        Expect(gdiStableRawAllocations,
               "raw ownership preserves row/atomic heights without stretching glyphs, and padding shares hit/caret/selection geometry at 96/144/192 DPI");
        const bool gdiVerticalNavigation = [&]() {
            note::NoteTextCore core;
            core.Reset(note::NoteId{1032}, note::NoteMetadata{L"vertical.md", L"vertical"},
                L"abcdefghijklmnop\n<m=60>x</>\nabcdefghijklmnop", 462, 3);
            auto input = gdiInput;
            input.layout_key = {640, 96, 96, 7, 4, 8, false};
            input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
            std::shared_ptr<const note::NoteRenderFinalPublication> publication;
            if (note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider, &publication) !=
                note::NoteRenderFinalCompleteBuildResult::Built) return false;
            auto owner = input.owner_input;
            owner.requested_editor_lines = {{{1}, {2}}};
            owner.caret_line = 1;
            std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> frame;
            if (note::NoteRenderFinalPresentationSnapshot::Build(core, publication, owner, gdiProvider, &frame) !=
                note::NoteRenderFinalPresentationSnapshotBuildResult::Built) return false;
            const auto middle = note::NoteSourceLineMap::LineAt(core.source_line_map(), {1});
            const auto last = note::NoteSourceLineMap::LineAt(core.source_line_map(), {2});
            if (!middle || !last) return false;
            note::NoteRenderFinalCaretGeometry firstCaret;
            if (note::ResolveNoteRenderFinalPresentationCaret(*frame, {8}, &firstCaret) !=
                note::NoteRenderFinalPresentationInteractionResult::Resolved) return false;
            note::Utf16CodeUnitOffset down, up, pageDown, pageUp;
            const auto move = [&](note::Utf16CodeUnitOffset from, note::NoteRenderFinalVerticalMove direction,
                                  uint64_t page, note::Utf16CodeUnitOffset* out) {
                return note::MoveNoteRenderFinalPresentationVertical(*frame, from, firstCaret.x_px,
                    direction, page, out) == note::NoteRenderFinalPresentationInteractionResult::Resolved;
            };
            if (!move({8}, note::NoteRenderFinalVerticalMove::Down, 1, &down) ||
                down < middle->start || down > middle->content_end ||
                !move(down, note::NoteRenderFinalVerticalMove::Down, 1, &pageDown) ||
                pageDown.value != last->start.value + 8 ||
                !move(pageDown, note::NoteRenderFinalVerticalMove::Up, 1, &up) || up != down ||
                !move(up, note::NoteRenderFinalVerticalMove::Up, 1, &pageUp) || pageUp.value != 8 ||
                !move({8}, note::NoteRenderFinalVerticalMove::PageDown, UINT64_MAX, &pageDown) ||
                pageDown.value != last->start.value + 8 ||
                !move(pageDown, note::NoteRenderFinalVerticalMove::PageUp, UINT64_MAX, &pageUp) ||
                pageUp.value != 8) return false;
            // Invalid source/output never changes the caller's destination.
            note::Utf16CodeUnitOffset sentinel{12345};
            return !move({core.text_length() + 1}, note::NoteRenderFinalVerticalMove::Up, 1, &sentinel) &&
                sentinel.value == 12345 &&
                note::MoveNoteRenderFinalPresentationVertical(*frame, {0}, 0,
                    note::NoteRenderFinalVerticalMove::Down, 1, nullptr) ==
                    note::NoteRenderFinalPresentationInteractionResult::InvalidOutput;
        }();
        Expect(gdiVerticalNavigation,
            "vertical/page navigation shares measured hybrid geometry, skips reserved raw padding, retains preferred X, and bounds huge pages");
        const bool gdiAlignedMathNavigation = [&]() {
            for (const uint32_t dpi : {96u, 144u, 192u}) {
                for (const auto alignment : {note::NoteRenderInlineMathVerticalAlignment::Top,
                        note::NoteRenderInlineMathVerticalAlignment::Center,
                        note::NoteRenderInlineMathVerticalAlignment::Bottom}) {
                    note::NoteTextCore core;
                    const std::wstring source = L"Inline dollar $x$\r\n"
                        L"Inline paren \\(a + b\\), tag <math display=inline>\\frac{1}{2}</>\r\ntail plain row";
                    core.Reset({1182}, {L"vim-math.md", L""}, source, 1, 1);
                    const auto middle = note::NoteSourceLineMap::LineAt(core.source_line_map(), {1});
                    if (!middle) return false;
                    auto input = gdiInput;
                    input.layout_key = {1600, dpi, dpi, 7, 4, 8, false};
                    input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                    note::NoteRenderFinalGdiMeasurementOptions options;
                    options.inline_math_vertical_alignment = alignment;
                    note::NoteRenderFinalGdiMeasurementProvider provider(gdiMeasurementDc, options);
                    std::shared_ptr<const note::NoteRenderFinalPublication> publication;
                    if (note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &publication) !=
                        note::NoteRenderFinalCompleteBuildResult::Built) return false;
                    for (const bool rawPreference : {false, true}) {
                        const auto buildFrame = [&](size_t row,
                            std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot>* out) {
                            auto owner = input.owner_input;
                            if (rawPreference) { owner.requested_editor_lines = {{{row}, {row + 1}}}; owner.caret_line = row; }
                            return note::NoteRenderFinalPresentationSnapshot::Build(core, publication, owner, provider, out) ==
                                note::NoteRenderFinalPresentationSnapshotBuildResult::Built;
                        };
                        std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> frame;
                        if (!buildFrame(1, &frame)) return false;
                        const note::Utf16CodeUnitOffset origin{middle->start.value + 6};
                        note::NoteRenderFinalCaretGeometry caret;
                        if (note::ResolveNoteRenderFinalPresentationCaret(*frame, origin, &caret) !=
                            note::NoteRenderFinalPresentationInteractionResult::Resolved) return false;
                        note::Utf16CodeUnitOffset up, down;
                        if (note::MoveNoteRenderFinalPresentationVertical(*frame, origin, caret.x_px,
                                note::NoteRenderFinalVerticalMove::Up, 1, &up) !=
                                note::NoteRenderFinalPresentationInteractionResult::Resolved || up.value != 6 ||
                            !buildFrame(0, &frame) ||
                            note::MoveNoteRenderFinalPresentationVertical(*frame, up, caret.x_px,
                                note::NoteRenderFinalVerticalMove::Down, 1, &down) !=
                                note::NoteRenderFinalPresentationInteractionResult::Resolved || down != origin) return false;
                    }
                }
            }
            return true;
        }();
        Expect(gdiAlignedMathNavigation,
            "Vim vertical math navigation groups aligned glyphs into one row and retains X across raw ownership at three DPI/alignments with CRLF");
        const bool gdiStableImeAllocations = [&]() {
            note::NoteTextCore core;
            core.Reset(note::NoteId{1031}, note::NoteMetadata{L"stable-ime.md", L"stable IME"},
                       L"prefix\n<m=60>文</>\ntail", 461, 3);
            auto input = gdiInput;
            input.layout_key = {160, 144, 144, 7, 4, 8, true};
            input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
            std::shared_ptr<const note::NoteRenderFinalPublication> publication;
            if (note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider,
                    &publication) != note::NoteRenderFinalCompleteBuildResult::Built) return false;
            const auto materializations = core.model_materialization_count();
            const auto source = note::NoteSourceLineMap::LineAt(core.source_line_map(), {1});
            const auto structured = note::NoteRenderLineLayoutMap::LineAt(
                publication->layout()->line_layouts(), {1});
            const auto tail = note::NoteRenderLineLayoutMap::LineAt(
                publication->layout()->line_layouts(), {2});
            if (!source || !structured || !tail) return false;
            bool sawPadding = false, sawGrowth = false;
            for (const size_t length : {0u, 1u, 400u, 1u, 0u}) {
                note::NoteRenderFinalImePreeditInput preeditInput;
                preeditInput.canonical_replacement_span = {source->start + 6, source->start + 7};
                preeditInput.composition_text.assign(length, L'変');
                const auto caretOffset = preeditInput.canonical_replacement_span.start + length;
                preeditInput.editor_selection = {caretOffset, caretOffset};
                note::NoteRenderFinalImePreeditPresentation preedit;
                if (note::NoteRenderFinalImePreeditPresentation::Build(core, preeditInput, &preedit) !=
                    note::NoteRenderFinalImePreeditPresentationBuildResult::Built) return false;
                auto owner = input.owner_input;
                owner.caret_line = 1;
                owner.requested_editor_lines = {{{1}, {2}}};
                owner.ime_preedit = true;
                owner.ime_preedit_can_reuse_committed_layout = true;
                std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> frame;
                if (note::NoteRenderFinalPresentationSnapshot::BuildWithImePreedit(core, publication,
                        owner, preedit, gdiProvider, &frame) !=
                    note::NoteRenderFinalPresentationSnapshotBuildResult::Built) return false;
                std::vector<note::NoteRenderFinalRawTextLine> textLines;
                textLines.push_back({{1}, preedit.editor_line_span(), preedit.temporary_raw_line()});
                std::vector<note::NoteRenderFinalRawLineSurface> natural;
                if (!gdiProvider.MeasureRawTextLines(input.layout_key, textLines, &natural) ||
                    natural.size() != 1) return false;
                const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(frame->line_layouts(), {1});
                const auto hybridTail = note::NoteRenderLineLayoutMap::LineAt(frame->line_layouts(), {2});
                if (!rawLayout || !hybridTail || rawLayout->top_px != structured->top_px ||
                    rawLayout->layout.height_px !=
                        std::max(structured->layout.height_px, natural.front().layout.height_px) ||
                    hybridTail->top_px != rawLayout->bottom_px) return false;
                note::NoteRenderFinalCaretGeometry caret;
                note::NoteRenderFinalHit hit;
                if (note::ResolveNoteRenderFinalPresentationEditorCaret(*frame, caretOffset, &caret) !=
                        note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                    note::HitTestNoteRenderFinalPresentationEditor(*frame, caret.x_px, caret.top_px, &hit) !=
                        note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                    hit.line_index != note::LineIndex{1} || hit.source_offset != caretOffset ||
                    caret.bottom_px > rawLayout->bottom_px ||
                    core.CopyRawRange({0}, core.text_length()) != L"prefix\n<m=60>文</>\ntail" ||
                    core.model_materialization_count() != materializations) return false;
                if (natural.front().layout.height_px < structured->layout.height_px) {
                    sawPadding = true;
                    if (hybridTail->top_px != tail->top_px) return false;
                } else if (natural.front().layout.height_px > structured->layout.height_px) sawGrowth = true;
            }
            return sawPadding && sawGrowth;
        }();
        Expect(gdiStableImeAllocations,
               "empty/short/long/shrinking IME preedit preserves structured allocation or grows without cropping and shares editor hit/caret coordinates");
        std::vector<note::NoteRenderFinalRawTextLine> gdiExplicitRawTextLines;
        gdiExplicitRawTextLines.push_back(
            {{17}, {{100}, {104}}, L"ab\t文"});
        std::vector<note::NoteRenderFinalRawLineSurface> gdiExplicitRawSurfaces;
        const bool gdiExplicitRawMeasurement = gdiProvider.MeasureRawTextLines(
            gdiInput.layout_key, gdiExplicitRawTextLines, &gdiExplicitRawSurfaces) &&
            gdiExplicitRawSurfaces.size() == 1 &&
            gdiExplicitRawSurfaces.front().line_index == note::LineIndex{17} &&
            gdiExplicitRawSurfaces.front().source_span.start == note::Utf16CodeUnitOffset{100} &&
            gdiExplicitRawSurfaces.front().source_span.end == note::Utf16CodeUnitOffset{104} &&
            gdiExplicitRawSurfaces.front().display.display_text == L"ab\t文" &&
            gdiExplicitRawSurfaces.front().display.boundaries.size() == 5 &&
            gdiExplicitRawSurfaces.front().layout.height_px > 0 &&
            gdiExplicitRawSurfaces.front().placement.runs.size() == 1 &&
            !gdiExplicitRawSurfaces.front().placement.runs.front().boundaries.empty() &&
            gdiExplicitRawSurfaces.front().placement.runs.front().boundaries.front().source_offset ==
                note::Utf16CodeUnitOffset{100} &&
            gdiExplicitRawSurfaces.front().placement.runs.front().boundaries.back().source_offset ==
                note::Utf16CodeUnitOffset{104};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiPublication;
        const note::NoteRenderFinalCompleteBuildResult gdiBuildResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiWrappedCore, gdiInput, gdiProvider, &gdiPublication);
        note::NoteRenderLinePlacement gdiPlacementLine;
        note::NoteRenderLineLayoutLocation gdiLayoutLine;
        bool gdiHasWrappedFragment = false;
        bool gdiFragmentsOwnAllSource = false;
        if (gdiPublication && gdiPublication->placement() && gdiPublication->layout() &&
            gdiPublication->placement()->ResolveLine({0}, &gdiPlacementLine)) {
            const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                gdiPublication->layout()->line_layouts(), {0});
            if (layout.has_value()) gdiLayoutLine = *layout;
            for (const note::NoteRenderRunPlacement& run : gdiPlacementLine.runs) {
                gdiHasWrappedFragment = gdiHasWrappedFragment || run.fragments.size() > 1;
                gdiFragmentsOwnAllSource = gdiFragmentsOwnAllSource ||
                    (!run.fragments.empty() && run.fragments.front().source_span.start == run.source_span.start &&
                     run.fragments.back().source_span.end == run.source_span.end);
            }
        }

        bool gdiEmptyLineInteraction = true;
        for (const std::wstring_view source : {std::wstring_view{L""},
                                               std::wstring_view{L"a\n\nb\n"}}) {
            note::NoteTextCore emptyLineCore;
            emptyLineCore.Reset(note::NoteId{1010},
                note::NoteMetadata{L"empty-lines.md", L"empty lines"}, std::wstring{source}, 434, 3);
            auto emptyLineInput = gdiInput;
            emptyLineInput.owner_input.visible_lines = {{0}, {emptyLineCore.logical_line_count()}};
            std::shared_ptr<const note::NoteRenderFinalPublication> publication;
            std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> presentation;
            if (note::NoteRenderFinalTransaction::BuildComplete(
                    emptyLineCore, emptyLineInput, gdiProvider, &publication) !=
                    note::NoteRenderFinalCompleteBuildResult::Built ||
                note::NoteRenderFinalPresentationSnapshot::Build(
                    emptyLineCore, publication, emptyLineInput.owner_input,
                    gdiProvider, &presentation) !=
                    note::NoteRenderFinalPresentationSnapshotBuildResult::Built) {
                gdiEmptyLineInteraction = false;
                continue;
            }
            for (size_t index = 0; index < emptyLineCore.logical_line_count(); ++index) {
                const auto location = note::NoteSourceLineMap::LineAt(
                    publication->syntax()->source_line_map(), {index});
                if (!location || location->start != location->content_end) continue;
                note::NoteRenderFinalCaretGeometry caret, hybridCaret;
                note::NoteRenderFinalHit hit, hybridHit;
                note::NoteRenderFinalGdiPaintOptions options;
                options.caret = location->start;
                const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                    presentation->line_layouts(), {index});
                const bool resolved = layout &&
                    note::ResolveNoteRenderFinalCaret(*publication, location->start, &caret) ==
                        note::NoteRenderFinalInteractionResult::Resolved &&
                    note::ResolveNoteRenderFinalPresentationEditorCaret(
                        *presentation, location->start, &hybridCaret) ==
                        note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                    note::HitTestNoteRenderFinalPublication(
                        *publication, 5, layout->top_px, &hit) ==
                        note::NoteRenderFinalInteractionResult::Resolved &&
                    note::HitTestNoteRenderFinalPresentationEditor(
                        *presentation, 5, layout->top_px, &hybridHit) ==
                        note::NoteRenderFinalPresentationInteractionResult::Resolved;
                gdiEmptyLineInteraction = gdiEmptyLineInteraction && resolved &&
                    caret.source_offset == location->start && caret.x_px == 0 &&
                    caret.top_px == layout->top_px && caret.bottom_px == layout->bottom_px &&
                    hybridCaret.top_px == caret.top_px && hybridCaret.bottom_px == caret.bottom_px &&
                    hit.source_offset == location->start && hybridHit.source_offset == location->start &&
                    note::NoteRenderFinalGdiPainter::Paint(gdiMeasurementDc, *publication, options) &&
                    note::NoteRenderFinalGdiPainter::Paint(gdiMeasurementDc, *presentation, options);
            }
        }
        Expect(gdiEmptyLineInteraction,
               "empty documents, interior blank lines and trailing blank lines share paint, caret and hit geometry");

        note::NoteTextCore gdiTabCore;
        gdiTabCore.Reset(
            note::NoteId{999}, note::NoteMetadata{L"gdi-tab.md", L"gdi-tab"}, L"a\tb", 423, 3);
        note::NoteRenderFinalCompleteBuildInput gdiTabInput = gdiInput;
        gdiTabInput.layout_key = {640, 96, 96, 7, 4, 8, false};
        gdiTabInput.owner_input.visible_lines = {{0}, {gdiTabCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiTabPublication;
        const note::NoteRenderFinalCompleteBuildResult gdiTabBuildResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiTabCore, gdiTabInput, gdiProvider, &gdiTabPublication);
        note::NoteRenderLinePlacement gdiTabPlacement;
        SIZE gdiSpace{};
        const bool measuredGdiSpace = gdiMeasurementDc &&
            GetTextExtentPoint32W(gdiMeasurementDc, L" ", 1, &gdiSpace) != FALSE;
        const auto tabBoundary = [&]() -> const note::NoteRenderPlacementBoundary* {
            if (!gdiTabPublication || !gdiTabPublication->placement() ||
                !gdiTabPublication->placement()->ResolveLine({0}, &gdiTabPlacement) ||
                gdiTabPlacement.runs.empty()) {
                return nullptr;
            }
            for (const note::NoteRenderPlacementBoundary& boundary :
                 gdiTabPlacement.runs.front().boundaries) {
                if (boundary.source_offset == note::Utf16CodeUnitOffset{2}) return &boundary;
            }
            return nullptr;
        }();
        note::NoteTextCore gdiMathCore;
        gdiMathCore.Reset(
            note::NoteId{1000}, note::NoteMetadata{L"gdi-math.md", L"gdi-math"},
            L"math $x^2$", 424, 3);
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiMathPublication;
        const note::NoteRenderFinalCompleteBuildResult gdiMathResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiMathCore, gdiInput, gdiProvider, &gdiMathPublication);
        const auto hasGdiMathGraphic = [&]() {
            if (!gdiMathPublication || !gdiMathPublication->placement()) return false;
            note::NoteRenderLinePlacement placement;
            if (!gdiMathPublication->placement()->ResolveLine({0}, &placement)) return false;
            return std::any_of(placement.decorations.begin(), placement.decorations.end(),
                               [](const note::NoteRenderVisualDecoration& decoration) {
                                   return decoration.kind ==
                                       note::NoteRenderVisualDecorationKind::InlineMathGraphic;
                               });
        };
        note::NoteTextCore gdiBlockMathCore;
        gdiBlockMathCore.Reset(
            note::NoteId{1003}, note::NoteMetadata{L"gdi-block-math.md", L"gdi-block-math"},
            L"$$\nx^2 + y^2\n$$", 427, 3);
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiBlockMathPublication;
        const note::NoteRenderFinalCompleteBuildResult gdiBlockMathResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiBlockMathCore, gdiInput, gdiProvider, &gdiBlockMathPublication);
        note::NoteRenderAtomicGroupPlacement gdiBlockMathGroup;
        const auto gdiBlockMathGeometry = [&]() {
            if (!gdiBlockMathPublication || !gdiBlockMathPublication->placement() ||
                !gdiBlockMathPublication->layout() ||
                !gdiBlockMathPublication->placement()->ResolveAtomicGroupContaining(
                    {1}, &gdiBlockMathGroup) ||
                gdiBlockMathGroup.kind != note::NoteRenderAtomicGroupKind::BlockMath ||
                gdiBlockMathGroup.first_line != note::LineIndex{0} ||
                gdiBlockMathGroup.last_line != note::LineIndex{2}) {
                return false;
            }
            const auto anchor = note::NoteRenderLineLayoutMap::LineAt(
                gdiBlockMathPublication->layout()->line_layouts(), {0});
            const auto collapsed = note::NoteRenderLineLayoutMap::LineAt(
                gdiBlockMathPublication->layout()->line_layouts(), {1});
            return anchor.has_value() && collapsed.has_value() &&
                   anchor->layout.height_px == gdiBlockMathGroup.height_px &&
                   !anchor->layout.collapsed_into_atomic_group &&
                   collapsed->layout.collapsed_into_atomic_group &&
                   collapsed->layout.height_px == 0 &&
                   collapsed->top_px == collapsed->bottom_px;
        }();
        // Closed/unclosed styles and unrelated inline markers must not turn
        // a proven plain-leaf edit into repeated whole-note measurement.
        for (const std::wstring& prefix : {std::wstring(L"<b>closed</b>\n\n"),
                std::wstring(L"<char=#ff0000>open\n\n"),
                std::wstring(L"**closed** and `code`\n\n"),
                std::wstring(L"<unknown>raw</unknown>\n\n")}) {
            const std::wstring source = prefix + L"middle plain row\n\ntail text";
            for (const std::wstring& target : {std::wstring(L"middle"), std::wstring(L"tail")}) {
                note::NoteTextCore core;
                core.Reset({1181}, {L"plain-proof.md", L""}, source, 1, 1);
                auto input = gdiInput;
                input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                std::shared_ptr<const note::NoteRenderFinalPublication> before, after, oracle;
                bool exact = note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider, &before) ==
                    note::NoteRenderFinalCompleteBuildResult::Built;
                const uint64_t models = core.model_materialization_count();
                const note::TextEdit edit{{source.find(target) + 2}, 1, L"QQ"};
                note::NoteRenderFinalLocalPatchInput patch;
                patch.layout_key = input.layout_key;
                patch.owner_input = input.owner_input;
                patch.edit = edit;
                exact = exact && core.Apply(edit) == note::NoteTextApplyResult::Applied &&
                    note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(before, core, patch, gdiProvider, &after) ==
                        note::NoteRenderFinalCompleteBuildResult::Built &&
                    core.model_materialization_count() == models;
                exact = exact && note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider, &oracle) ==
                    note::NoteRenderFinalCompleteBuildResult::Built;
                for (size_t line = 0; exact && line < core.logical_line_count(); ++line) {
                    note::NoteRenderSourceLinePlan a, b;
                    exact = after->source_plan()->ResolveLine({line}, &a) &&
                        oracle->source_plan()->ResolveLine({line}, &b) && a.runs.size() == b.runs.size() &&
                        a.decoration.flags == b.decoration.flags && a.content_span.start == b.content_span.start &&
                        a.content_span.end == b.content_span.end;
                    for (size_t run = 0; exact && run < a.runs.size(); ++run) {
                        const auto& x = a.runs[run]; const auto& y = b.runs[run];
                        exact = x.kind == y.kind && x.source_span.start == y.source_span.start &&
                            x.source_span.end == y.source_span.end && x.styles.size() == y.styles.size();
                        for (size_t style = 0; exact && style < x.styles.size(); ++style)
                            exact = x.styles[style].kind == y.styles[style].kind && x.styles[style].value == y.styles[style].value;
                    }
                }
                Expect(exact, "plain interior edits after closed or unclosed tags reuse locally and agree with a complete parse/style plan");
            }
        }
        {
            const std::wstring source =
                L"before\ntext <math display='inline'>x^2</> tail\n\n<math display=block>\n\\frac{1}{2}\n</math>\n\nafter plain\n";
            note::NoteTextCore core;
            core.Reset(note::NoteId{1054}, {L"tag-math.md", L""}, source, 480, 3);
            auto input = gdiInput;
            input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
            std::shared_ptr<const note::NoteRenderFinalPublication> publication;
            const bool built = note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider,
                &publication) == note::NoteRenderFinalCompleteBuildResult::Built && publication;
            note::NoteRenderLinePlacement inlineRow;
            note::NoteRenderAtomicGroupPlacement block;
            Expect(built && publication->placement()->ResolveLine({1}, &inlineRow) &&
                       std::count_if(inlineRow.decorations.begin(), inlineRow.decorations.end(), [](const auto& d) {
                           return d.kind == note::NoteRenderVisualDecorationKind::InlineMathGraphic;
                       }) == 1 &&
                       publication->placement()->ResolveAtomicGroupContaining({4}, &block) &&
                       block.kind == note::NoteRenderAtomicGroupKind::BlockMath &&
                       block.first_line.value == 3 && block.last_line.value == 5,
                   "named tag math produces one inline graphic and one existing atomic block in final GDI");
            bool ownerCorrect = built;
            for (const size_t caret : {size_t{1}, size_t{4}}) {
                auto owner = input.owner_input;
                owner.caret_line = caret;
                owner.requested_editor_lines = {{{caret}, {caret + 1}}};
                std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> hybrid;
                ownerCorrect = ownerCorrect && note::NoteRenderFinalPresentationSnapshot::Build(
                    core, publication, owner, gdiProvider, &hybrid) ==
                        note::NoteRenderFinalPresentationSnapshotBuildResult::Built;
                if (!ownerCorrect) break;
                for (size_t line = 0; line < core.logical_line_count(); ++line) {
                    const bool raw = caret == 1 ? line == 1 : line >= 3 && line <= 5;
                    ownerCorrect = ownerCorrect && hybrid->SurfaceAt({line}) == (raw ?
                        note::NoteRenderFinalSurfaceKind::RawPaint : note::NoteRenderFinalSurfaceKind::StructuredPaint);
                }
            }
            Expect(ownerCorrect, "editing named math selects only the inline row or the complete block, never duplicate surfaces or whole-note raw");
            if (built) {
                const auto materializations = core.model_materialization_count();
                const note::TextEdit edit{{source.find(L"after plain") + 6}, 1, L"QQ"};
                const bool applied = core.Apply(edit) == note::NoteTextApplyResult::Applied;
                note::NoteRenderFinalLocalPatchInput patch;
                patch.layout_key = input.layout_key;
                patch.owner_input = input.owner_input;
                patch.edit = edit;
                std::shared_ptr<const note::NoteRenderFinalPublication> after;
                const bool patched = applied && note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                    publication, core, patch, gdiProvider, &after) == note::NoteRenderFinalCompleteBuildResult::Built;
                Expect(patched && after && core.model_materialization_count() == materializations &&
                           after->source_plan()->SharesLinePayloadForDifferentialTest(*publication->source_plan(), {1}) &&
                           after->source_plan()->SharesLinePayloadForDifferentialTest(*publication->source_plan(), {4}),
                       "plain edits after closed math tags reuse prior math without materializing the whole note");
            }
            core.Reset(note::NoteId{1054}, {L"tag-math.md", L""}, source, 480, 3);
            const note::TextEdit deleteTagLetter{{source.find(L"</math>") + 3}, 1, L""};
            const bool deleted = core.Apply(deleteTagLetter) == note::NoteTextApplyResult::Applied;
            note::NoteRenderFinalLocalPatchInput patch;
            patch.layout_key = input.layout_key;
            patch.owner_input = input.owner_input;
            patch.edit = deleteTagLetter;
            std::shared_ptr<const note::NoteRenderFinalPublication> rejected;
            std::shared_ptr<const note::NoteRenderFinalPublication> rebuilt;
            Expect(built && deleted && note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                       publication, core, patch, gdiProvider, &rejected) != note::NoteRenderFinalCompleteBuildResult::Built &&
                       !rejected && note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider, &rebuilt) ==
                           note::NoteRenderFinalCompleteBuildResult::Built,
                   "deleting a normal letter in a math closer revokes local reuse and safely rebuilds current source");
        }
        for (const std::wstring source : {L"x <math>y</math>\ntail\n", L"x <math>y</>\ntail\n",
                                          L"x <math future='a>b'>y</>\ntail\n"}) {
            note::NoteTextCore core;
            core.Reset(note::NoteId{1055}, {L"tag-edge.md", L""}, source, 490, 3);
            auto input = gdiInput;
            input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
            std::shared_ptr<const note::NoteRenderFinalPublication> before, after, rebuilt;
            const bool built = note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider, &before) ==
                note::NoteRenderFinalCompleteBuildResult::Built;
            const note::TextEdit edit{{0}, 1, L""};
            const bool applied = core.Apply(edit) == note::NoteTextApplyResult::Applied;
            note::NoteRenderFinalLocalPatchInput patch;
            patch.layout_key = input.layout_key;
            patch.owner_input = input.owner_input;
            patch.edit = edit;
            note::NoteRenderAtomicGroupPlacement block;
            Expect(built && applied && note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                       before, core, patch, gdiProvider, &after) != note::NoteRenderFinalCompleteBuildResult::Built &&
                       !after && note::NoteRenderFinalTransaction::BuildComplete(core, input, gdiProvider, &rebuilt) ==
                           note::NoteRenderFinalCompleteBuildResult::Built &&
                       rebuilt->placement()->ResolveAtomicGroupContaining({0}, &block) &&
                       block.kind == note::NoteRenderAtomicGroupKind::BlockMath,
                   "deleting surrounding plain text reclassifies tag math as a block instead of retaining an old inline layout");
        }
        // This is the exact mixed Markdown/TeX fixture used by the UI
        // automation.  Keep its final GDI route here: a structural build is
        // insufficient if the published hybrid frame cannot paint the same
        // content with the current selection and caret.
        note::NoteTextCore gdiMixedMathCore;
        gdiMixedMathCore.Reset(
            note::NoteId{1008}, note::NoteMetadata{L"gdi-mixed-math.md", L"gdi mixed math"},
            L"# Math UI Fixture\n\n"
            L"Inline dollar $x$\n"
            L"Inline paren \\(a + b\\), tag <math display=inline>x^2</>\n"
            L"::: note\n"
            L"- **Bold marker regression**\n"
            L":::\n\n"
            L"$$z$$\n\n"
            L"\\[w + 1\\]\n\n"
            L"<math display='block'>\\frac{1}{2}</math>\n\n"
            L"| Item | Example | State |\n"
            L"| :--- | :-----: | ---: |\n"
            L"| Markdown | `value` | <link=ui-final-frame-link><lu>jump</></> |\n",
            432, 3);
        note::NoteRenderFinalCompleteBuildInput gdiMixedMathInput = gdiInput;
        gdiMixedMathInput.layout_key = {640, 96, 96, 7, 4, 8, true};
        gdiMixedMathInput.owner_input.visible_lines = {
            {0}, {gdiMixedMathCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiMixedMathPublication;
        const note::NoteRenderFinalCompleteBuildResult gdiMixedMathBuildResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiMixedMathCore, gdiMixedMathInput, gdiProvider, &gdiMixedMathPublication);
        std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> gdiMixedMathPresentation;
        const note::NoteRenderFinalPresentationSnapshotBuildResult gdiMixedMathPresentationResult =
            note::NoteRenderFinalPresentationSnapshot::Build(
                gdiMixedMathCore, gdiMixedMathPublication, gdiMixedMathInput.owner_input,
                gdiProvider, &gdiMixedMathPresentation);
        note::NoteTextCore gdiTableCore;
        gdiTableCore.Reset(
            note::NoteId{1001}, note::NoteMetadata{L"gdi-table.md", L"gdi-table"},
            L"| left | right |\n| :--- | ---: |\n| a | b |", 425, 3);
        note::NoteRenderFinalCompleteBuildInput gdiTableInput = gdiInput;
        gdiTableInput.owner_input.visible_lines = {{0}, {gdiTableCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiTablePublication;
        const note::NoteRenderFinalCompleteBuildResult gdiTableResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiTableCore, gdiTableInput, gdiProvider, &gdiTablePublication);
        note::NoteRenderAtomicGroupPlacement gdiTableGroup;
        const auto gdiTableGeometry = [&]() {
            if (!gdiTablePublication || !gdiTablePublication->placement() ||
                !gdiTablePublication->layout() ||
                !gdiTablePublication->placement()->ResolveAtomicGroupContaining(
                    {1}, &gdiTableGroup) ||
                gdiTableGroup.kind != note::NoteRenderAtomicGroupKind::Table ||
                gdiTableGroup.table_columns.size() != 2 ||
                gdiTableGroup.table_rows.size() != 3 ||
                gdiTableGroup.table_rows[0].divider ||
                !gdiTableGroup.table_rows[0].header ||
                !gdiTableGroup.table_rows[1].divider ||
                gdiTableGroup.table_rows[2].header ||
                gdiTableGroup.table_rows[2].divider) {
                return false;
            }
            const auto header = note::NoteRenderLineLayoutMap::LineAt(
                gdiTablePublication->layout()->line_layouts(), {0});
            const auto divider = note::NoteRenderLineLayoutMap::LineAt(
                gdiTablePublication->layout()->line_layouts(), {1});
            const auto body = note::NoteRenderLineLayoutMap::LineAt(
                gdiTablePublication->layout()->line_layouts(), {2});
            const auto& first = gdiTableGroup.table_columns[0];
            const auto& second = gdiTableGroup.table_columns[1];
            return header.has_value() && divider.has_value() && body.has_value() &&
                   first.left_border_x_px == 0 &&
                   first.right_border_x_px == second.left_border_x_px &&
                   second.right_border_x_px < gdiTableGroup.width_px &&
                   divider->layout.height_px == gdiTableGroup.table_rows[1].height_px &&
                   divider->layout.height_px == 1 &&
                   body->top_px == header->bottom_px + divider->layout.height_px;
        }();
        const bool gdiTableAllCommitted = gdiTablePublication && gdiTablePublication->owner_plan() &&
            gdiTablePublication->owner_plan()->owner_ranges().size() == 1 &&
            gdiTablePublication->owner_plan()->owner_ranges().front().owner ==
                note::NotePresentationLineOwner::CommittedPlacement &&
            gdiTablePublication->owner_plan()->owner_ranges().front().lines.first.value == 0 &&
            gdiTablePublication->owner_plan()->owner_ranges().front().lines.last_exclusive.value == 3;
        note::NoteTextCore gdiContainerCore;
        const std::wstring containerSource =
            L"before\n::: note\nplain body\n::: inner\n# Heading\n`chip` $x$\n:::\n"
            L"| Item | Value |\n| --- | --- |\n| a | b |\n\n$$\nx^2\n$$\n:::\nafter";
        gdiContainerCore.Reset(note::NoteId{1050},
            note::NoteMetadata{L"container.md", L"container"}, containerSource, 450, 3);
        auto gdiContainerInput = gdiInput;
        gdiContainerInput.layout_key = {640, 96, 96, 7, 4, 8, true};
        gdiContainerInput.owner_input.visible_lines = {{0}, {gdiContainerCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiContainerPublication;
        const auto containerBuild = note::NoteRenderFinalTransaction::BuildComplete(
            gdiContainerCore, gdiContainerInput, gdiProvider, &gdiContainerPublication);
        bool containerGeometry = containerBuild == note::NoteRenderFinalCompleteBuildResult::Built;
        if (containerGeometry) {
            for (size_t line = 1; line <= 14; ++line) {
                note::NoteRenderSourceLinePlan source;
                note::NoteRenderLinePlacement placement;
                const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                    gdiContainerPublication->layout()->line_layouts(), {line});
                containerGeometry = layout && gdiContainerPublication->source_plan()->ResolveLine({line}, &source) &&
                    gdiContainerPublication->placement()->ResolveLine({line}, &placement) &&
                    source.decoration.container_depth == (line >= 3 && line <= 6 ? 2u : 1u);
                if (!containerGeometry) break;
                if (layout->layout.collapsed_into_atomic_group) continue;
                containerGeometry = std::any_of(placement.decorations.begin(), placement.decorations.end(),
                    [](const auto& decoration) {
                        return decoration.kind == note::NoteRenderVisualDecorationKind::ContainerSurface;
                    });
                if (!containerGeometry) break;
            }
            note::NoteRenderSourceLinePlan opening;
            containerGeometry = containerGeometry &&
                gdiContainerPublication->source_plan()->ResolveLine({1}, &opening) &&
                std::all_of(opening.runs.begin(), opening.runs.end(), [](const auto& run) {
                    return run.kind == note::NoteRenderSourceRunKind::HiddenSyntax;
                });
        }
        Expect(containerGeometry, "final GDI container surfaces retain nested depths, hidden fences, tables and block math");
        auto containerRawOwner = gdiContainerInput.owner_input;
        containerRawOwner.caret_line = 2;
        containerRawOwner.requested_editor_lines = {{{2}, {3}}};
        std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> gdiContainerRawPresentation;
        const bool containerHybrid = containerGeometry &&
            note::NoteRenderFinalPresentationSnapshot::Build(gdiContainerCore, gdiContainerPublication,
                containerRawOwner, gdiProvider, &gdiContainerRawPresentation) ==
                    note::NoteRenderFinalPresentationSnapshotBuildResult::Built &&
            gdiContainerRawPresentation->SurfaceAt({2}) == note::NoteRenderFinalSurfaceKind::RawPaint &&
            gdiContainerRawPresentation->SurfaceAt({4}) == note::NoteRenderFinalSurfaceKind::StructuredPaint;
        Expect(containerHybrid, "editing a container body switches only the caret row to raw, not its whole enclosing block");
        bool containerClickMatchesSource = containerGeometry;
        if (containerClickMatchesSource) {
            const auto offset = note::Utf16CodeUnitOffset{containerSource.find(L"plain body") + 3};
            note::NoteRenderFinalCaretGeometry caret;
            note::NoteRenderFinalHit hit;
            containerClickMatchesSource = note::ResolveNoteRenderFinalCaret(*gdiContainerPublication, offset,
                note::NoteRenderFinalCaretAffinity::AfterVisualWrap, &caret) ==
                    note::NoteRenderFinalInteractionResult::Resolved &&
                note::HitTestNoteRenderFinalPublication(*gdiContainerPublication,
                    caret.x_px, caret.top_px, &hit) == note::NoteRenderFinalInteractionResult::Resolved &&
                hit.source_offset == offset;
        }
        Expect(containerClickMatchesSource, "container inset geometry shares the exact source position with caret and hit testing");
        {
            const std::wstring source = L"::: my_note\nplain body\n:::\ntail\n";
            note::NoteTextCore editCore;
            editCore.Reset(note::NoteId{1051}, {L"container.md", L""}, source, 460, 3);
            auto input = gdiContainerInput;
            input.owner_input.visible_lines = {{0}, {editCore.logical_line_count()}};
            std::shared_ptr<const note::NoteRenderFinalPublication> before;
            const bool built = note::NoteRenderFinalTransaction::BuildComplete(
                editCore, input, gdiProvider, &before) == note::NoteRenderFinalCompleteBuildResult::Built;
            const note::TextEdit edit{{source.find(L"body") + 1}, 1, L"QQ"};
            const auto materializations = editCore.model_materialization_count();
            const bool applied = editCore.Apply(edit) == note::NoteTextApplyResult::Applied;
            note::NoteRenderFinalLocalPatchInput patch;
            patch.layout_key = input.layout_key;
            patch.owner_input = input.owner_input;
            patch.edit = edit;
            std::shared_ptr<const note::NoteRenderFinalPublication> after;
            const auto result = built && applied ? note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                before, editCore, patch, gdiProvider, &after) : note::NoteRenderFinalCompleteBuildResult::InvalidEdit;
            Expect(result == note::NoteRenderFinalCompleteBuildResult::Built && after &&
                   editCore.model_materialization_count() == materializations &&
                   after->source_plan()->SharesLinePayloadForDifferentialTest(*before->source_plan(), {0}) &&
                   after->source_plan()->SharesLinePayloadForDifferentialTest(*before->source_plan(), {3}),
                   "plain container-body edits reuse unchanged prefix/suffix without whole-document materialization");
            std::shared_ptr<const note::NoteRenderFinalPublication> oracle;
            bool samePlan = after && note::NoteRenderFinalTransaction::BuildComplete(
                editCore, input, gdiProvider, &oracle) == note::NoteRenderFinalCompleteBuildResult::Built;
            if (samePlan) {
                for (size_t line = 0; line < editCore.logical_line_count(); ++line) {
                    note::NoteRenderSourceLinePlan a, b;
                    samePlan = after->source_plan()->ResolveLine({line}, &a) &&
                        oracle->source_plan()->ResolveLine({line}, &b) &&
                        a.content_span.start == b.content_span.start && a.content_span.end == b.content_span.end &&
                        a.decoration.flags == b.decoration.flags &&
                        a.decoration.container_depth == b.decoration.container_depth && a.runs.size() == b.runs.size();
                    if (!samePlan) break;
                    for (size_t index = 0; index < a.runs.size(); ++index) {
                        samePlan = samePlan && a.runs[index].kind == b.runs[index].kind &&
                            a.runs[index].source_span.start == b.runs[index].source_span.start &&
                            a.runs[index].source_span.end == b.runs[index].source_span.end;
                    }
                    if (!samePlan) break;
                }
            }
            Expect(samePlan, "shifted container-body local source plans match a complete parse including unchanged closing fences");
        }
        for (const std::wstring source : {L"x::: note\nbody\ntail\n",
                                           L"::: note\nplain body\n:::\ntail\n"}) {
            note::NoteTextCore editCore;
            editCore.Reset(note::NoteId{1052}, {L"container.md", L""}, source, 470, 3);
            auto input = gdiContainerInput;
            input.owner_input.visible_lines = {{0}, {editCore.logical_line_count()}};
            std::shared_ptr<const note::NoteRenderFinalPublication> before;
            const bool built = note::NoteRenderFinalTransaction::BuildComplete(
                editCore, input, gdiProvider, &before) == note::NoteRenderFinalCompleteBuildResult::Built;
            const note::TextEdit edit = source.front() == L'x' ? note::TextEdit{{0}, 1, L""}
                : note::TextEdit{{source.find(L"\n:::\n") + 1}, 1, L""};
            const bool applied = editCore.Apply(edit) == note::NoteTextApplyResult::Applied;
            note::NoteRenderFinalLocalPatchInput patch{input.layout_key, input.owner_input, edit};
            std::shared_ptr<const note::NoteRenderFinalPublication> retained = before;
            const auto result = built && applied ? note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                before, editCore, patch, gdiProvider, &retained) : note::NoteRenderFinalCompleteBuildResult::InvalidEdit;
            std::shared_ptr<const note::NoteRenderFinalPublication> complete;
            Expect(result == note::NoteRenderFinalCompleteBuildResult::RequiresNativeFallback && retained == before &&
                   note::NoteRenderFinalTransaction::BuildComplete(editCore, input, gdiProvider, &complete) ==
                       note::NoteRenderFinalCompleteBuildResult::Built,
                   "revealing or deleting a container boundary revokes local reuse and rebuilds a complete current frame");
        }
        note::NoteTextCore gdiDecoratorCore;
        gdiDecoratorCore.Reset(
            note::NoteId{1002}, note::NoteMetadata{L"gdi-decorator.md", L"gdi-decorator"},
            L"# heading\n- bullet\n- [x] task\n> quote `code`\n\n---\n\n```txt\ncode\n```",
            426, 3);
        note::NoteRenderFinalCompleteBuildInput gdiDecoratorInput = gdiInput;
        gdiDecoratorInput.layout_key = {640, 96, 96, 7, 4, 8, true};
        gdiDecoratorInput.owner_input.visible_lines = {
            {0}, {gdiDecoratorCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiDecoratorPublication;
        const note::NoteRenderFinalCompleteBuildResult gdiDecoratorResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiDecoratorCore, gdiDecoratorInput, gdiProvider, &gdiDecoratorPublication);
        note::NoteTextCore gdiTaskCore;
        gdiTaskCore.Reset(
            note::NoteId{1030}, note::NoteMetadata{L"gdi-task.md", L"gdi-task"},
            L"- [x] ノートを開く\n- [ ] 見出しを追加する\n    - [ ] nested\n"
            L"- [ ] <m=60>tall</>\n- [x] <s=32>large</>\n"
            L"- [ ] a long task with enough text to wrap onto several visual rows without stretching the checkbox",
            426, 3);
        note::NoteRenderFinalCompleteBuildInput gdiTaskInput = gdiDecoratorInput;
        gdiTaskInput.layout_key.client_width_px = 240;
        gdiTaskInput.owner_input.visible_lines = {{0}, {gdiTaskCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiTaskPublication;
        const auto gdiTaskResult = note::NoteRenderFinalTransaction::BuildComplete(
            gdiTaskCore, gdiTaskInput, gdiProvider, &gdiTaskPublication);
        std::vector<note::NoteRenderVisualDecoration> gdiTaskBoxes;
        std::vector<uint64_t> gdiTaskTops;
        std::vector<uint32_t> gdiTaskHeights;
        const bool gdiTaskGeometry = [&]() {
            if (gdiTaskResult != note::NoteRenderFinalCompleteBuildResult::Built ||
                !gdiTaskPublication || !gdiTaskPublication->placement() ||
                !gdiTaskPublication->layout()) return false;
            for (size_t line = 0; line < 6; ++line) {
                note::NoteRenderLinePlacement placement;
                const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                    gdiTaskPublication->layout()->line_layouts(), {line});
                if (!layout || !gdiTaskPublication->placement()->ResolveLine({line}, &placement)) {
                    return false;
                }
                size_t boxCount = 0;
                for (const auto& decoration : placement.decorations) {
                    if (decoration.kind != note::NoteRenderVisualDecorationKind::TaskCheckbox) continue;
                    ++boxCount;
                    const int side = decoration.right_px - decoration.left_px;
                    if (side <= 0 || static_cast<uint32_t>(side) !=
                            decoration.bottom_offset_px - decoration.top_offset_px ||
                        decoration.left_px < 8 || decoration.top_offset_px == 0 ||
                        decoration.bottom_offset_px > layout->layout.height_px) return false;
                    bool hasText = false;
                    for (const auto& run : placement.runs) {
                        for (const auto& fragment : run.fragments) {
                            if (fragment.width_px <= 0) continue;
                            hasText = true;
                            if (fragment.x_px - decoration.right_px < std::max(1, side / 3)) return false;
                        }
                    }
                    if (!hasText) return false;
                    gdiTaskBoxes.push_back(decoration);
                    gdiTaskTops.push_back(layout->top_px);
                    gdiTaskHeights.push_back(layout->layout.height_px);
                }
                if (boxCount != 1) return false;
            }
            const auto side = [&](size_t index) {
                return gdiTaskBoxes[index].right_px - gdiTaskBoxes[index].left_px;
            };
            return gdiTaskBoxes[0].checked && !gdiTaskBoxes[1].checked &&
                   side(0) == side(1) && side(0) == side(2) && side(0) == side(3) &&
                   side(0) == side(5) && side(4) > side(0) &&
                   gdiTaskBoxes[2].left_px > gdiTaskBoxes[1].left_px &&
                   gdiTaskHeights[3] > gdiTaskHeights[0] &&
                   gdiTaskHeights[5] > gdiTaskHeights[0];
        }();
        const auto visibleClickSurvivesRawTransition = [&](const note::NoteTextCore& core,
                                                          const auto& publication,
                                                          const auto& ownerInput) {
            if (!publication || !publication->source_plan() || !publication->placement()) return false;
            note::NoteRenderFinalWin32AdapterInput input;
            input.text_core = &core;
            input.structural_publication = publication;
            input.owner_input = ownerInput;
            input.measurement_dc = gdiMeasurementDc;
            std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> structured;
            if (note::NoteRenderFinalWin32AdapterFrame::Build(input, &structured) !=
                    note::NoteRenderFinalWin32AdapterBuildResult::Built || !structured) return false;
            for (size_t line = 0; line < publication->source_plan()->line_count(); ++line) {
                note::NoteRenderSourceLinePlan source;
                note::NoteRenderLinePlacement placement;
                const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                    publication->layout()->line_layouts(), {line});
                if (!layout || !publication->source_plan()->ResolveLine({line}, &source) ||
                    !publication->placement()->ResolveLine({line}, &placement)) return false;
                for (size_t index = 0; index < source.runs.size(); ++index) {
                    const auto& run = source.runs[index];
                    if (run.kind == note::NoteRenderSourceRunKind::HiddenSyntax ||
                        run.display_source_span.start == run.display_source_span.end) continue;
                    const auto expected = run.display_source_span.start;
                    const auto& measured = placement.runs[index];
                    const auto boundary = std::find_if(measured.boundaries.begin(), measured.boundaries.end(),
                        [&](const auto& value) { return value.source_offset == expected; });
                    if (boundary == measured.boundaries.end() ||
                        boundary->fragment_index >= measured.fragments.size()) return false;
                    const auto& fragment = measured.fragments[boundary->fragment_index];
                    const auto y = layout->top_px + fragment.top_offset_px;
                    note::NoteRenderFinalHit committedHit;
                    note::NoteRenderFinalHit presentedHit;
                    if (note::HitTestNoteRenderFinalPublication(*publication, boundary->x_px, y,
                            &committedHit) != note::NoteRenderFinalInteractionResult::Resolved ||
                        structured->HitTest(boundary->x_px, y, &presentedHit) !=
                            note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                        committedHit.source_offset != expected || presentedHit.source_offset != expected) {
                        return false;
                    }
                    auto rawInput = input;
                    rawInput.owner_input.caret_line = line;
                    rawInput.owner_input.requested_editor_lines = {{{line}, {line + 1}}};
                    rawInput.editor_selection = {expected, expected};
                    rawInput.editor_caret = expected;
                    std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> raw;
                    note::NoteRenderFinalCaretGeometry caret;
                    if (note::NoteRenderFinalWin32AdapterFrame::Build(rawInput, &raw) !=
                            note::NoteRenderFinalWin32AdapterBuildResult::Built || !raw ||
                        raw->Caret(&caret) != note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                        caret.source_offset != expected ||
                        raw->presentation()->SurfaceAt({line}) != note::NoteRenderFinalSurfaceKind::RawPaint) {
                        return false;
                    }
                    note::NoteRenderFinalHit rawHit;
                    if (raw->HitTest(caret.x_px, caret.top_px, &rawHit) !=
                            note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                        rawHit.source_offset != expected) return false;
                    break; // First visible text boundary, including hidden list/style prefixes.
                }
            }
            return true;
        };
        const bool gdiVisibleClicksKeepSource =
            visibleClickSurvivesRawTransition(gdiDecoratorCore, gdiDecoratorPublication,
                                              gdiDecoratorInput.owner_input) &&
            visibleClickSurvivesRawTransition(gdiTaskCore, gdiTaskPublication, gdiTaskInput.owner_input);
        const auto hasGdiDecoration = [&](note::NoteRenderVisualDecorationKind kind) {
            if (!gdiDecoratorPublication || !gdiDecoratorPublication->placement()) return false;
            for (size_t line = 0; line < gdiDecoratorPublication->placement()->line_count(); ++line) {
                note::NoteRenderLinePlacement placement;
                if (!gdiDecoratorPublication->placement()->ResolveLine({line}, &placement)) {
                    return false;
                }
                if (std::any_of(placement.decorations.begin(), placement.decorations.end(),
                                [kind](const note::NoteRenderVisualDecoration& decoration) {
                                    return decoration.kind == kind;
                                })) {
                    return true;
                }
            }
            return false;
        };
        note::NoteRenderFinalCompleteBuildInput gdiRawTableInput = gdiTableInput;
        gdiRawTableInput.owner_input.requested_editor_lines = {{{1}, {2}}};
        gdiRawTableInput.owner_input.caret_line = 1;
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiRawTablePublication;
        const note::NoteRenderFinalCompleteBuildResult gdiRawTableResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiTableCore, gdiRawTableInput, gdiProvider, &gdiRawTablePublication);
        note::NoteTextCore gdiStyleCore;
        gdiStyleCore.Reset(
            note::NoteId{1003}, note::NoteMetadata{L"gdi-style.md", L"gdi-style"},
            L"<char=ff0000>red</> <back=00ff00>back</> <u>under</> <x>strike</> [link](jump) <font=Consolas><s=32>large</></> <m=60>tall</>",
            427, 3);
        note::NoteRenderFinalCompleteBuildInput gdiStyleInput = gdiInput;
        gdiStyleInput.layout_key = {640, 96, 96, 7, 4, 8, true};
        gdiStyleInput.owner_input.visible_lines = {{0}, {gdiStyleCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiStylePublication;
        const note::NoteRenderFinalCompleteBuildResult gdiStyleResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiStyleCore, gdiStyleInput, gdiProvider, &gdiStylePublication);
        const bool gdiStyleSourceRuns = [&]() {
            note::NoteRenderSourceLinePlan line;
            if (!gdiStylePublication || !gdiStylePublication->source_plan() ||
                !gdiStylePublication->source_plan()->ResolveLine({0}, &line)) {
                return false;
            }
            bool textColour = false;
            bool backgroundColour = false;
            bool underline = false;
            bool strike = false;
            bool link = false;
            bool fontFamily = false;
            bool fontSize = false;
            bool lineHeight = false;
            for (const note::NoteRenderSourceRun& run : line.runs) {
                for (const note::NoteRenderSourceStyleAttribute& style : run.styles) {
                    textColour = textColour || style.kind == note::StyleKind::TextColor;
                    backgroundColour = backgroundColour || style.kind == note::StyleKind::BackgroundColor;
                    underline = underline || style.kind == note::StyleKind::Underline;
                    strike = strike || style.kind == note::StyleKind::Strike;
                    link = link || style.kind == note::StyleKind::LinkAccent ||
                        style.kind == note::StyleKind::LinkUnderline;
                    fontFamily = fontFamily || style.kind == note::StyleKind::FontFamily;
                    fontSize = fontSize || style.kind == note::StyleKind::FontSize;
                    lineHeight = lineHeight || style.kind == note::StyleKind::LineHeight;
                }
            }
            return textColour && backgroundColour && underline && strike && link &&
                fontFamily && fontSize && lineHeight;
        }();
        const bool gdiStyleFontGeometry = [&]() {
            if (!gdiStylePublication || !gdiStylePublication->layout()) return false;
            const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                gdiStylePublication->layout()->line_layouts(), {0});
            return layout.has_value() && layout->layout.height_px >= 80;
        }();
        note::NoteTextCore gdiPositionStyleCore;
        gdiPositionStyleCore.Reset(
            note::NoteId{1007}, note::NoteMetadata{L"gdi-position-style.md", L"gdi-position-style"},
            L"<d=3>indent</>\nplain <d=center>center</> tail\n<d=\"right -1\">right</><d=\"right -1\"> pair</>",
            431, 3);
        note::NoteRenderFinalCompleteBuildInput gdiPositionStyleInput = gdiInput;
        gdiPositionStyleInput.layout_key = {640, 96, 96, 7, 4, 8, false};
        gdiPositionStyleInput.owner_input.visible_lines = {
            {0}, {gdiPositionStyleCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiPositionStylePublication;
        const note::NoteRenderFinalCompleteBuildResult gdiPositionStyleResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiPositionStyleCore, gdiPositionStyleInput, gdiProvider,
                &gdiPositionStylePublication);
        bool gdiPositionStyleRunMapping = false;
        bool gdiPositionStyleSyntaxHidden = false;
        bool gdiPositionStyleIndentGeometry = false;
        bool gdiPositionStyleCenterGeometry = false;
        bool gdiPositionStyleRightGeometry = false;
        const bool gdiPositionStyleGeometry = [&]() {
            if (!gdiMeasurementDc || !gdiPositionStylePublication ||
                !gdiPositionStylePublication->source_plan() ||
                !gdiPositionStylePublication->placement()) {
                return false;
            }
            note::NoteRenderSourceLinePlan indentLine;
            note::NoteRenderSourceLinePlan centeredLine;
            note::NoteRenderSourceLinePlan rightLine;
            note::NoteRenderLinePlacement indentPlacement;
            note::NoteRenderLinePlacement centeredPlacement;
            note::NoteRenderLinePlacement rightPlacement;
            if (!gdiPositionStylePublication->source_plan()->ResolveLine({0}, &indentLine) ||
                !gdiPositionStylePublication->source_plan()->ResolveLine({1}, &centeredLine) ||
                !gdiPositionStylePublication->source_plan()->ResolveLine({2}, &rightLine) ||
                !gdiPositionStylePublication->placement()->ResolveLine({0}, &indentPlacement) ||
                !gdiPositionStylePublication->placement()->ResolveLine({1}, &centeredPlacement) ||
                !gdiPositionStylePublication->placement()->ResolveLine({2}, &rightPlacement) ||
                indentLine.runs.size() != indentPlacement.runs.size() ||
                centeredLine.runs.size() != centeredPlacement.runs.size() ||
                rightLine.runs.size() != rightPlacement.runs.size()) {
                return false;
            }
            SIZE space{};
            if (!GetTextExtentPoint32W(gdiMeasurementDc, L" ", 1, &space)) return false;
            const int spaceWidth = std::max<LONG>(1, space.cx);
            const auto hasStyle = [](const note::NoteRenderSourceRun& run,
                                     note::StyleKind kind) {
                return std::any_of(run.styles.begin(), run.styles.end(),
                                   [kind](const note::NoteRenderSourceStyleAttribute& style) {
                                       return style.kind == kind;
                                   });
            };
            const auto findStyledRun = [&](const note::NoteRenderSourceLinePlan& line,
                                           const note::NoteRenderLinePlacement& placement,
                                           note::StyleKind kind,
                                           size_t ordinal) -> const note::NoteRenderRunPlacement* {
                size_t found = 0;
                for (size_t index = 0; index < line.runs.size(); ++index) {
                    if (line.runs[index].kind == note::NoteRenderSourceRunKind::HiddenSyntax ||
                        !hasStyle(line.runs[index], kind)) {
                        continue;
                    }
                    if (found++ == ordinal) return &placement.runs[index];
                }
                return nullptr;
            };
            const note::NoteRenderRunPlacement* indent =
                findStyledRun(indentLine, indentPlacement, note::StyleKind::Indent, 0);
            const note::NoteRenderRunPlacement* centered =
                findStyledRun(centeredLine, centeredPlacement, note::StyleKind::Anchor, 0);
            const note::NoteRenderRunPlacement* rightFirst =
                findStyledRun(rightLine, rightPlacement, note::StyleKind::Anchor, 0);
            const note::NoteRenderRunPlacement* rightSecond =
                findStyledRun(rightLine, rightPlacement, note::StyleKind::Anchor, 1);
            size_t markupTokenCount = 0;
            const auto tagsAreHidden = [&](const note::NoteRenderSourceLinePlan& line) {
                for (const note::NoteRenderSourceRun& run : line.runs) {
                    if (run.source_span.end <= run.source_span.start) return false;
                    const std::wstring first = gdiPositionStyleCore.CopyRawRange(
                        run.source_span.start, 1);
                    if (first != L"<") continue;
                    ++markupTokenCount;
                    if (run.kind != note::NoteRenderSourceRunKind::HiddenSyntax) return false;
                }
                return true;
            };
            gdiPositionStyleSyntaxHidden = tagsAreHidden(indentLine) &&
                tagsAreHidden(centeredLine) && tagsAreHidden(rightLine) && markupTokenCount == 8;
            gdiPositionStyleRunMapping = indent && centered && rightFirst && rightSecond;
            if (!gdiPositionStyleRunMapping) return false;
            gdiPositionStyleIndentGeometry = indent->x_px >= 8 + 3 * spaceWidth;
            gdiPositionStyleCenterGeometry = centered->x_px > 8 &&
                centered->x_px + centered->width_px < 632;
            gdiPositionStyleRightGeometry = rightFirst->x_px + rightFirst->width_px <= 640 &&
                rightSecond->x_px == rightFirst->x_px + rightFirst->width_px;
            return gdiPositionStyleIndentGeometry && gdiPositionStyleCenterGeometry &&
                gdiPositionStyleRightGeometry;
        }();
        note::NoteTextCore gdiHybridCore;
        gdiHybridCore.Reset(
            note::NoteId{1004}, note::NoteMetadata{L"gdi-hybrid.md", L"gdi-hybrid"},
            L"prefix\n\n| left | right |\n| :--- | ---: |\n| a | b |\n\ntail", 428, 3);
        note::NoteRenderFinalCompleteBuildInput gdiHybridInput = gdiInput;
        gdiHybridInput.owner_input.visible_lines = {{0}, {gdiHybridCore.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> gdiHybridStructuralPublication;
        const note::NoteRenderFinalCompleteBuildResult gdiHybridStructuralResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                gdiHybridCore, gdiHybridInput, gdiProvider, &gdiHybridStructuralPublication);
        note::NotePresentationOwnerInput gdiHybridOwnerInput = gdiHybridInput.owner_input;
        gdiHybridOwnerInput.requested_editor_lines = {{{3}, {4}}};
        gdiHybridOwnerInput.caret_line = 3;
        std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> gdiHybridPresentation;
        const note::NoteRenderFinalPresentationSnapshotBuildResult gdiHybridPresentationResult =
            note::NoteRenderFinalPresentationSnapshot::Build(
                gdiHybridCore, gdiHybridStructuralPublication, gdiHybridOwnerInput,
                gdiProvider, &gdiHybridPresentation);
        note::NoteRenderFinalRawLineSurface gdiHybridRawDivider;
        const bool gdiHybridSnapshotMatches = gdiHybridPresentation &&
            gdiHybridStructuralPublication &&
            gdiHybridPresentation->Matches(gdiHybridCore, gdiHybridInput.layout_key);
        const bool gdiHybridSurfaceRanges = gdiHybridPresentation &&
            gdiHybridPresentation->surface_ranges().size() == 3 &&
            gdiHybridPresentation->surface_ranges()[0].surface ==
                note::NoteRenderFinalSurfaceKind::StructuredPaint &&
            gdiHybridPresentation->surface_ranges()[0].lines.first == note::LineIndex{0} &&
            gdiHybridPresentation->surface_ranges()[0].lines.last_exclusive == note::LineIndex{2} &&
            gdiHybridPresentation->surface_ranges()[1].surface ==
                note::NoteRenderFinalSurfaceKind::RawPaint &&
            gdiHybridPresentation->surface_ranges()[1].lines.first == note::LineIndex{2} &&
            gdiHybridPresentation->surface_ranges()[1].lines.last_exclusive == note::LineIndex{5} &&
            gdiHybridPresentation->surface_ranges()[2].surface ==
                note::NoteRenderFinalSurfaceKind::StructuredPaint &&
            gdiHybridPresentation->surface_ranges()[2].lines.first == note::LineIndex{5} &&
            gdiHybridPresentation->surface_ranges()[2].lines.last_exclusive == note::LineIndex{7};
        const bool gdiHybridRawDividerResolved = gdiHybridPresentation &&
            gdiHybridPresentation->ResolveRawLine({3}, &gdiHybridRawDivider) &&
            gdiHybridRawDivider.display.display_text == L"| :--- | ---: |" &&
            gdiHybridRawDivider.layout.height_px > 1;
        note::NoteRenderFinalImePreeditPresentation gdiImePreedit;
        std::shared_ptr<const note::NoteRenderFinalPresentationSnapshot> gdiImePresentation;
        note::NoteRenderFinalImePreeditPresentationBuildResult gdiImePreeditResult =
            note::NoteRenderFinalImePreeditPresentationBuildResult::InvalidTextCore;
        note::NoteRenderFinalPresentationSnapshotBuildResult gdiImePresentationResult =
            note::NoteRenderFinalPresentationSnapshotBuildResult::InvalidTextCore;
        note::NoteRenderFinalRawLineSurface gdiImeRawDivider;
        if (gdiHybridRawDividerResolved) {
            const size_t replacementStart = gdiHybridRawDivider.source_span.start.value + 2;
            note::NoteRenderFinalImePreeditInput gdiImeInput;
            gdiImeInput.canonical_replacement_span = {{replacementStart}, {replacementStart + 1}};
            gdiImeInput.composition_text = L"変換";
            gdiImeInput.editor_selection = {{replacementStart}, {replacementStart + 2}};
            gdiImePreeditResult = note::NoteRenderFinalImePreeditPresentation::Build(
                gdiHybridCore, gdiImeInput, &gdiImePreedit);
            note::NotePresentationOwnerInput gdiImeOwnerInput = gdiHybridOwnerInput;
            gdiImeOwnerInput.ime_preedit = true;
            gdiImeOwnerInput.ime_preedit_can_reuse_committed_layout = true;
            gdiImePresentationResult =
                note::NoteRenderFinalPresentationSnapshot::BuildWithImePreedit(
                    gdiHybridCore, gdiHybridStructuralPublication, gdiImeOwnerInput,
                    gdiImePreedit, gdiProvider, &gdiImePresentation);
        }
        const bool gdiImePresentationGeometry =
            gdiImePreeditResult ==
                note::NoteRenderFinalImePreeditPresentationBuildResult::Built &&
            gdiImePresentationResult ==
                note::NoteRenderFinalPresentationSnapshotBuildResult::Built &&
            gdiImePresentation && gdiImePresentation->has_ime_preedit() &&
            gdiImePresentation->Matches(gdiHybridCore, gdiHybridInput.layout_key) &&
            gdiImePresentation->SurfaceAt({3}) == note::NoteRenderFinalSurfaceKind::RawPaint &&
            gdiImePresentation->ResolveRawLine({3}, &gdiImeRawDivider) &&
            gdiImeRawDivider.display.display_text == L"| 変換--- | ---: |" &&
            gdiImeRawDivider.source_span.start == gdiImePreedit.editor_line_span().start &&
            gdiImeRawDivider.source_span.end == gdiImePreedit.editor_line_span().end;
        std::vector<note::NoteRenderFinalRect> gdiImeSelectionRects;
        note::NoteRenderFinalCaretGeometry gdiImeCaret;
        note::NoteRenderFinalHit gdiImeHit;
        const bool gdiImeInteractionGeometry = [&]() {
            if (!gdiImePresentationGeometry || gdiImeRawDivider.placement.runs.size() != 1) {
                return false;
            }
            const note::NoteRenderRunPlacement& rawRun =
                gdiImeRawDivider.placement.runs.front();
            const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(
                gdiImePresentation->line_layouts(), {3});
            if (!rawLayout.has_value() || rawRun.boundaries.empty() || rawRun.fragments.empty()) {
                return false;
            }
            const note::NoteRenderFinalPresentationInteractionResult selectionResult =
                note::ResolveNoteRenderFinalPresentationEditorSelection(
                    *gdiImePresentation, gdiImePreedit.editor_selection(), &gdiImeSelectionRects);
            const note::NoteRenderFinalPresentationInteractionResult caretResult =
                note::ResolveNoteRenderFinalPresentationEditorCaret(
                    *gdiImePresentation, gdiImePreedit.editor_selection().end, &gdiImeCaret);
            const note::NoteRenderFinalPresentationInteractionResult hitResult =
                note::HitTestNoteRenderFinalPresentationEditor(
                    *gdiImePresentation, rawRun.boundaries.front().x_px,
                    rawLayout->top_px + rawRun.fragments.front().top_offset_px, &gdiImeHit);
            return selectionResult ==
                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                caretResult == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                hitResult == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                !gdiImeSelectionRects.empty() &&
                gdiImeCaret.line_index == note::LineIndex{3} &&
                gdiImeCaret.source_offset == gdiImePreedit.editor_selection().end &&
                gdiImeHit.line_index == note::LineIndex{3} &&
                gdiImeHit.source_span.start == gdiImeRawDivider.source_span.start &&
                gdiImeHit.source_span.end == gdiImeRawDivider.source_span.end;
        }();
        std::vector<note::NoteRenderFinalRect> gdiImeCrossSurfaceSelectionRects;
        const bool gdiImeCrossSurfaceSelection = [&]() {
            if (!gdiImePresentationGeometry) return false;
            const auto canonicalTail = note::NoteSourceLineMap::LineAt(
                gdiHybridCore.source_line_map(), {6});
            const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(
                gdiImePresentation->line_layouts(), {3});
            const auto tailLayout = note::NoteRenderLineLayoutMap::LineAt(
                gdiImePresentation->line_layouts(), {6});
            note::Utf16CodeUnitOffset editorTailStart{};
            if (!canonicalTail.has_value() || !rawLayout.has_value() || !tailLayout.has_value() ||
                canonicalTail->line.content_length < 2 ||
                !gdiImePreedit.MapCanonicalUnchangedBoundary(
                    canonicalTail->start, note::NoteSourceEditBoundarySide::AfterReplacement,
                    &editorTailStart)) {
                return false;
            }
            const note::Span selection{
                gdiImePreedit.editor_selection().start, editorTailStart + 2};
            if (note::ResolveNoteRenderFinalPresentationEditorSelection(
                    *gdiImePresentation, selection, &gdiImeCrossSurfaceSelectionRects) !=
                    note::NoteRenderFinalPresentationInteractionResult::Resolved ||
                gdiImeCrossSurfaceSelectionRects.size() < 2) {
                return false;
            }
            bool hasRawRect = false;
            bool hasTailRect = false;
            for (const note::NoteRenderFinalRect& rect : gdiImeCrossSurfaceSelectionRects) {
                hasRawRect = hasRawRect ||
                    (rect.top_px >= rawLayout->top_px && rect.top_px < rawLayout->bottom_px);
                hasTailRect = hasTailRect ||
                    (rect.top_px >= tailLayout->top_px && rect.top_px < tailLayout->bottom_px);
            }
            return hasRawRect && hasTailRect;
        }();
        note::NoteRenderFinalWin32AdapterInput gdiAdapterInput;
        gdiAdapterInput.text_core = &gdiHybridCore;
        gdiAdapterInput.structural_publication = gdiHybridStructuralPublication;
        gdiAdapterInput.owner_input = gdiHybridOwnerInput;
        gdiAdapterInput.owner_input.ime_preedit = true;
        gdiAdapterInput.owner_input.ime_preedit_can_reuse_committed_layout = true;
        gdiAdapterInput.ime_preedit = gdiImePreedit;
        gdiAdapterInput.editor_selection = gdiImePreedit.editor_selection();
        gdiAdapterInput.editor_caret = gdiImePreedit.editor_selection().end;
        gdiAdapterInput.measurement_dc = gdiMeasurementDc;
        std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> gdiAdapterFrame;
        const note::NoteRenderFinalWin32AdapterBuildResult gdiAdapterBuildResult =
            note::NoteRenderFinalWin32AdapterFrame::Build(gdiAdapterInput, &gdiAdapterFrame);
        std::vector<note::NoteRenderFinalRect> gdiAdapterSelectionRects;
        note::NoteRenderFinalCaretGeometry gdiAdapterCaret;
        note::NoteRenderFinalHit gdiAdapterHit;
        uint64_t gdiAdapterHorizontalExtent = 0;
        const bool gdiAdapterGeometry = [&]() {
            if (gdiAdapterBuildResult != note::NoteRenderFinalWin32AdapterBuildResult::Built ||
                !gdiAdapterFrame || !gdiImeInteractionGeometry ||
                gdiImeRawDivider.placement.runs.empty() ||
                gdiImeRawDivider.placement.runs.front().boundaries.empty()) {
                return false;
            }
            const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(
                gdiAdapterFrame->presentation()->line_layouts(), {3});
            const note::NoteRenderRunPlacement& rawRun =
                gdiImeRawDivider.placement.runs.front();
            return rawLayout.has_value() &&
                gdiAdapterFrame->Selection(&gdiAdapterSelectionRects) ==
                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                gdiAdapterFrame->Caret(&gdiAdapterCaret) ==
                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                gdiAdapterFrame->HitTest(
                    rawRun.boundaries.front().x_px,
                    rawLayout->top_px + rawRun.fragments.front().top_offset_px,
                    &gdiAdapterHit) == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                gdiAdapterFrame->HorizontalExtent(&gdiAdapterHorizontalExtent) ==
                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                !gdiAdapterSelectionRects.empty() &&
                gdiAdapterCaret.source_offset == gdiAdapterInput.editor_caret &&
                gdiAdapterHit.line_index == note::LineIndex{3} &&
                gdiAdapterHorizontalExtent > 0;
        }();
        note::NoteRenderFinalWin32AdapterInput invalidGdiAdapterInput = gdiAdapterInput;
        invalidGdiAdapterInput.editor_caret = {
            gdiImePreedit.coordinate_map().new_source_length() + 1};
        std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> invalidGdiAdapterFrame;
        const bool gdiAdapterRejectsInvalidEditorState =
            note::NoteRenderFinalWin32AdapterFrame::Build(
                invalidGdiAdapterInput, &invalidGdiAdapterFrame) ==
                note::NoteRenderFinalWin32AdapterBuildResult::SelectionOutsideEditorText &&
            !invalidGdiAdapterFrame;

        // The per-view state is the actual publication boundary used by the
        // runtime adapter: it may retain a structural candidate privately,
        // but it exposes a frame only after the source/placement and hybrid
        // presentation both describe the same editor state.
        note::NoteTextCore gdiStateCore;
        gdiStateCore.Reset(
            note::NoteId{1006}, note::NoteMetadata{L"state.md", L"state"},
            L"first\nsecond\nthird", 430, 3);
        note::NoteRenderFinalWin32AdapterStateInput gdiStateInput;
        gdiStateInput.text_core = &gdiStateCore;
        gdiStateInput.content_kind = note::NoteContentKind::Markdown;
        gdiStateInput.layout_key = {320, 96, 96, 7, 4, 8, true};
        gdiStateInput.owner_input.render_active = true;
        gdiStateInput.owner_input.editor_text_core_current = true;
        gdiStateInput.owner_input.visible_lines = {{0}, {gdiStateCore.logical_line_count()}};
        gdiStateInput.owner_input.requested_editor_lines = {{{1}, {2}}};
        gdiStateInput.owner_input.caret_line = 1;
        gdiStateInput.editor_selection = {{7}, {7}};
        gdiStateInput.editor_caret = {7};
        gdiStateInput.measurement_dc = gdiMeasurementDc;
        note::NoteRenderFinalWin32AdapterState gdiState;
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiStateInitial =
            gdiState.Refresh(gdiStateInput);
        const bool gdiStateInitialFrame = gdiState.has_frame() && gdiState.frame() &&
            gdiState.frame()->presentation()->SurfaceAt({1}) ==
                note::NoteRenderFinalSurfaceKind::RawPaint;
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiStateReused =
            gdiState.Refresh(gdiStateInput);
        gdiState.ClearFrame();
        const bool gdiStateEventWithdrawal = !gdiState.has_frame() && !gdiState.frame();
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiStateAfterWithdrawal =
            gdiState.Refresh(gdiStateInput);
        const bool gdiStateWithdrawalReusedStructure = gdiState.has_frame() && gdiState.frame();
        const note::TextEdit gdiStateLocalEdit{{7}, 0, L"X"};
        const bool gdiStateAppliedLocalEdit =
            gdiStateCore.Apply(gdiStateLocalEdit) == note::NoteTextApplyResult::Applied;
        gdiStateInput.editor_selection = {{8}, {8}};
        gdiStateInput.editor_caret = {8};
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiStateLocal =
            gdiState.Refresh(gdiStateInput, gdiStateLocalEdit);
        const bool gdiStateLocalFrame = gdiState.has_frame() && gdiState.frame() &&
            gdiState.frame()->presentation()->Matches(gdiStateCore, gdiStateInput.layout_key) &&
            gdiState.frame()->presentation()->SurfaceAt({1}) ==
                note::NoteRenderFinalSurfaceKind::RawPaint;
        const note::TextEdit gdiStateStructuralEdit{{0}, 0, L"# "};
        const bool gdiStateAppliedStructuralEdit =
            gdiStateCore.Apply(gdiStateStructuralEdit) == note::NoteTextApplyResult::Applied;
        gdiStateInput.editor_selection = {{2}, {2}};
        gdiStateInput.editor_caret = {2};
        gdiStateInput.owner_input.requested_editor_lines = {{{0}, {1}}};
        gdiStateInput.owner_input.caret_line = 0;
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiStateStructural =
            gdiState.Refresh(gdiStateInput, gdiStateStructuralEdit);
        const bool gdiStateStructuralFrame = gdiState.has_frame() && gdiState.frame() &&
            gdiState.frame()->presentation()->Matches(gdiStateCore, gdiStateInput.layout_key) &&
            gdiState.frame()->presentation()->SurfaceAt({0}) ==
                note::NoteRenderFinalSurfaceKind::RawPaint;
        note::NoteRenderFinalWin32AdapterStateInput gdiStateInvalidInput = gdiStateInput;
        gdiStateInvalidInput.editor_selection.end = {gdiStateCore.text_length() + 1};
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiStateInvalid =
            gdiState.Refresh(gdiStateInvalidInput);
        const bool gdiStateFailureClearsFrame =
            gdiStateInvalid ==
                note::NoteRenderFinalWin32AdapterStateRefreshResult::RequiresNativeFallback &&
            !gdiState.has_frame() && !gdiState.frame();
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiStateRecovered =
            gdiState.Refresh(gdiStateInput);
        const bool gdiStateRecoveredFrame = gdiState.has_frame() && gdiState.frame() &&
            gdiState.frame()->presentation()->Matches(gdiStateCore, gdiStateInput.layout_key);
#if !defined(NDEBUG)
        const note::NoteRenderFinalWin32AdapterStateTestWork gdiStateWork =
            gdiState.test_work();
        const bool gdiStateWorkUsesOnlyProvenTransactions =
            gdiStateWork.refresh_calls == 7 &&
            gdiStateWork.structural_reuse_attempts == 4 &&
            gdiStateWork.local_transaction_attempts == 2 &&
            gdiStateWork.local_transaction_builds == 1 &&
            gdiStateWork.complete_transaction_attempts == 2 &&
            gdiStateWork.complete_transaction_builds == 2;
#else
        const bool gdiStateWorkUsesOnlyProvenTransactions = true;
#endif

        // Exercise the runtime publication boundary itself on the long
        // fixture already used for persistent source/placement tests.  The
        // adapter must make the exact same decision: a marker-free edit in
        // the middle may use one local transaction, but may not silently add
        // a second complete transaction merely because the document is long.
        constexpr size_t kLongAdapterRows = 4096;
        constexpr size_t kLongAdapterEditedRow = kLongAdapterRows / 2;
        const std::wstring longAdapterHeading = L"# Long adapter section\n\n";
        std::wstring longAdapterText = longAdapterHeading;
        longAdapterText.reserve(kLongAdapterRows * 6);
        for (size_t row = 0; row < kLongAdapterRows; ++row) {
            longAdapterText.append(L"plain");
            if (row + 1 < kLongAdapterRows) longAdapterText.push_back(L'\n');
        }
        note::NoteTextCore longAdapterCore;
        longAdapterCore.Reset(
            note::NoteId{1009}, note::NoteMetadata{L"long-adapter.md", L"long adapter"},
            std::move(longAdapterText), 433, 3);
        note::NoteRenderFinalWin32AdapterStateInput longAdapterInput;
        longAdapterInput.text_core = &longAdapterCore;
        longAdapterInput.content_kind = note::NoteContentKind::Markdown;
        longAdapterInput.layout_key = {640, 96, 96, 7, 4, 8, true};
        longAdapterInput.owner_input.render_active = true;
        longAdapterInput.owner_input.editor_text_core_current = true;
        longAdapterInput.owner_input.visible_lines = {
            {0}, {longAdapterCore.logical_line_count()}};
        longAdapterInput.editor_selection = {{0}, {0}};
        longAdapterInput.editor_caret = {0};
        longAdapterInput.measurement_dc = gdiMeasurementDc;
        note::NoteRenderFinalWin32AdapterState longAdapterState;
        const note::NoteRenderFinalWin32AdapterStateRefreshResult longAdapterInitial =
            longAdapterState.Refresh(longAdapterInput);
        const std::shared_ptr<const note::NoteRenderFinalPublication> longAdapterBefore = [&]() {
            const auto& frame = longAdapterState.frame();
            return frame && frame->presentation()
                ? frame->presentation()->structural_publication()
                : std::shared_ptr<const note::NoteRenderFinalPublication>{};
        }();
        const note::TextEdit longAdapterEdit{{longAdapterHeading.size() + kLongAdapterEditedRow * 6 + 2}, 0, L"X"};
        const bool longAdapterAppliedEdit =
            longAdapterCore.Apply(longAdapterEdit) == note::NoteTextApplyResult::Applied;
        longAdapterInput.editor_selection = {
            {longAdapterEdit.start.value + 1}, {longAdapterEdit.start.value + 1}};
        longAdapterInput.editor_caret = {longAdapterEdit.start.value + 1};
        const note::NoteRenderFinalWin32AdapterStateRefreshResult longAdapterLocal =
            longAdapterState.Refresh(longAdapterInput, longAdapterEdit);
        const std::shared_ptr<const note::NoteRenderFinalPublication> longAdapterAfter = [&]() {
            const auto& frame = longAdapterState.frame();
            return frame && frame->presentation()
                ? frame->presentation()->structural_publication()
                : std::shared_ptr<const note::NoteRenderFinalPublication>{};
        }();
#if !defined(NDEBUG)
        const note::NoteRenderFinalWin32AdapterStateTestWork longAdapterWork =
            longAdapterState.test_work();
        const bool longAdapterUsesBoundedWork =
            longAdapterWork.refresh_calls == 2 &&
            longAdapterWork.structural_reuse_attempts == 0 &&
            longAdapterWork.local_transaction_attempts == 1 &&
            longAdapterWork.local_transaction_builds == 1 &&
            longAdapterWork.complete_transaction_attempts == 1 &&
            longAdapterWork.complete_transaction_builds == 1;
#else
        const bool longAdapterUsesBoundedWork = true;
#endif
        const bool longAdapterKeepsUnchangedPayloads =
            longAdapterBefore && longAdapterAfter &&
            longAdapterBefore->source_plan() && longAdapterAfter->source_plan() &&
            longAdapterBefore->placement() && longAdapterAfter->placement() &&
            longAdapterBefore->source_plan()->SharesLinePayloadForDifferentialTest(
                *longAdapterAfter->source_plan(), {0}) &&
            longAdapterBefore->source_plan()->SharesLinePayloadForDifferentialTest(
                *longAdapterAfter->source_plan(), {kLongAdapterRows + 1}) &&
            longAdapterBefore->placement()->SharesLinePayloadForDifferentialTest(
                *longAdapterAfter->placement(), {0}) &&
            longAdapterBefore->placement()->SharesLinePayloadForDifferentialTest(
                *longAdapterAfter->placement(), {kLongAdapterRows + 1});

        // The actual Win32 adapter must preserve the same persisted link ID
        // at its presentation hit-test edge.  This is the route used by a
        // click after final-frame publication, not merely the structural
        // publication helper tested above.
        note::NoteTextCore gdiLegacyLinkCore;
        gdiLegacyLinkCore.Reset(
            note::NoteId{1008}, note::NoteMetadata{L"final-link.md", L"final link"},
            L"<link=workspace-final-jump><lu>jump</></>", 432, 3);
        note::NoteRenderFinalWin32AdapterStateInput gdiLegacyLinkInput;
        gdiLegacyLinkInput.text_core = &gdiLegacyLinkCore;
        gdiLegacyLinkInput.content_kind = note::NoteContentKind::Markdown;
        gdiLegacyLinkInput.layout_key = {640, 96, 96, 7, 4, 8, true};
        gdiLegacyLinkInput.owner_input.render_active = true;
        gdiLegacyLinkInput.owner_input.editor_text_core_current = true;
        gdiLegacyLinkInput.owner_input.visible_lines = {
            {0}, {gdiLegacyLinkCore.logical_line_count()}};
        gdiLegacyLinkInput.measurement_dc = gdiMeasurementDc;
        note::NoteRenderFinalWin32AdapterState gdiLegacyLinkState;
        const note::NoteRenderFinalWin32AdapterStateRefreshResult gdiLegacyLinkRefresh =
            gdiLegacyLinkState.Refresh(gdiLegacyLinkInput);
        const bool gdiLegacyLinkPaint = gdiLegacyLinkState.frame() &&
            gdiLegacyLinkState.frame()->Paint(gdiMeasurementDc, {});
        const bool gdiLegacyLinkPresentationHit = [&]() {
            const auto& frame = gdiLegacyLinkState.frame();
            if (gdiLegacyLinkRefresh !=
                    note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedComplete ||
                !frame || !frame->presentation() ||
                !frame->presentation()->structural_publication()) {
                return false;
            }
            const auto& structural = frame->presentation()->structural_publication();
            note::NoteRenderSourceLinePlan sourceLine;
            note::NoteRenderLinePlacement placementLine;
            if (!structural->source_plan() || !structural->placement() ||
                !structural->source_plan()->ResolveLine({0}, &sourceLine) ||
                !structural->placement()->ResolveLine({0}, &placementLine)) {
                return false;
            }
            size_t legacyRun = static_cast<size_t>(-1);
            for (size_t index = 0; index < sourceLine.runs.size(); ++index) {
                if (std::any_of(sourceLine.runs[index].styles.begin(),
                                sourceLine.runs[index].styles.end(),
                                [](const auto& style) {
                                    return style.kind == note::StyleKind::LinkId &&
                                        style.value == L"workspace-final-jump";
                                })) {
                    legacyRun = index;
                    break;
                }
            }
            const auto layout = note::NoteRenderLineLayoutMap::LineAt(
                frame->presentation()->line_layouts(), {0});
            if (legacyRun == static_cast<size_t>(-1) ||
                legacyRun >= placementLine.runs.size() || !layout ||
                placementLine.runs[legacyRun].boundaries.empty()) {
                return false;
            }
            note::NoteRenderFinalHit hit;
            return frame->HitTest(
                       placementLine.runs[legacyRun].x_px +
                           placementLine.runs[legacyRun].width_px / 2,
                       layout->top_px, &hit) ==
                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                hit.is_link && hit.is_legacy_link_id &&
                hit.link_target == L"workspace-final-jump";
        }();
        const auto gdiHybridTailGeometry = [&]() {
            if (!gdiHybridPresentation || !gdiHybridStructuralPublication) return false;
            const auto structuralTail = note::NoteRenderLineLayoutMap::LineAt(
                gdiHybridStructuralPublication->layout()->line_layouts(), {6});
            const auto hybridTail = note::NoteRenderLineLayoutMap::LineAt(
                gdiHybridPresentation->line_layouts(), {6});
            return structuralTail.has_value() && hybridTail.has_value() &&
                hybridTail->top_px > structuralTail->top_px;
        }();
        const bool gdiHybridSurfaceQueries = gdiHybridPresentation &&
            gdiHybridPresentation->SurfaceAt({0}) ==
                note::NoteRenderFinalSurfaceKind::StructuredPaint &&
            gdiHybridPresentation->SurfaceAt({3}) ==
                note::NoteRenderFinalSurfaceKind::RawPaint &&
            gdiHybridPresentation->SurfaceAt({6}) ==
                note::NoteRenderFinalSurfaceKind::StructuredPaint;
        const bool gdiHybridGeometry = gdiHybridSnapshotMatches && gdiHybridSurfaceRanges &&
            gdiHybridRawDividerResolved && gdiHybridTailGeometry && gdiHybridSurfaceQueries;
        note::Span gdiHybridRawSelection{};
        std::vector<note::NoteRenderFinalRect> gdiHybridSelectionRects;
        note::NoteRenderFinalCaretGeometry gdiHybridCaret;
        note::NoteRenderFinalHit gdiHybridRawHit;
        uint64_t gdiHybridHorizontalExtent = 0;
        const bool gdiHybridInteractionGeometry = [&]() {
            if (!gdiHybridPresentation || !gdiHybridRawDividerResolved ||
                gdiHybridRawDivider.placement.runs.size() != 1) {
                return false;
            }
            const note::NoteRenderRunPlacement& rawRun =
                gdiHybridRawDivider.placement.runs.front();
            const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(
                gdiHybridPresentation->line_layouts(), {3});
            if (!rawLayout.has_value() || rawRun.boundaries.empty() ||
                rawRun.fragments.empty() || gdiHybridRawDivider.source_span.end.value -
                    gdiHybridRawDivider.source_span.start.value < 3) {
                return false;
            }
            gdiHybridRawSelection = {
                gdiHybridRawDivider.source_span.start,
                gdiHybridRawDivider.source_span.start + 3};
            const note::NoteRenderFinalPresentationInteractionResult selectionResult =
                note::ResolveNoteRenderFinalPresentationSelection(
                    *gdiHybridPresentation, gdiHybridRawSelection, &gdiHybridSelectionRects);
            const note::NoteRenderFinalPresentationInteractionResult caretResult =
                note::ResolveNoteRenderFinalPresentationCaret(
                    *gdiHybridPresentation, gdiHybridRawSelection.end, &gdiHybridCaret);
            const note::NoteRenderFinalPresentationInteractionResult hitResult =
                note::HitTestNoteRenderFinalPresentation(
                    *gdiHybridPresentation, rawRun.boundaries.front().x_px,
                    rawLayout->top_px + rawRun.fragments.front().top_offset_px,
                    &gdiHybridRawHit);
            const note::NoteRenderFinalPresentationInteractionResult extentResult =
                note::ResolveNoteRenderFinalPresentationHorizontalExtent(
                    *gdiHybridPresentation, &gdiHybridHorizontalExtent);
            return selectionResult ==
                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                caretResult == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                hitResult == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                extentResult == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                !gdiHybridSelectionRects.empty() &&
                gdiHybridSelectionRects.front().top_px >= rawLayout->top_px &&
                gdiHybridSelectionRects.front().bottom_px <= rawLayout->bottom_px &&
                gdiHybridCaret.line_index == note::LineIndex{3} &&
                gdiHybridCaret.source_offset == gdiHybridRawSelection.end &&
                gdiHybridRawHit.line_index == note::LineIndex{3} &&
                gdiHybridRawHit.source_span.start == gdiHybridRawDivider.source_span.start &&
                gdiHybridRawHit.source_span.end == gdiHybridRawDivider.source_span.end &&
                !gdiHybridRawHit.is_link &&
                gdiHybridHorizontalExtent >= static_cast<uint64_t>(
                    std::max(0, rawRun.x_px + rawRun.width_px));
        }();
        std::vector<note::NoteRenderFinalRect> gdiTabSelectionRects;
        note::NoteRenderFinalCaretGeometry gdiTabCaret;
        const note::NoteRenderFinalInteractionResult gdiTabSelectionResult = gdiTabPublication
            ? note::ResolveNoteRenderFinalSelection(
                *gdiTabPublication, {{0}, {3}}, &gdiTabSelectionRects)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const note::NoteRenderFinalInteractionResult gdiTabCaretResult = gdiTabPublication
            ? note::ResolveNoteRenderFinalCaret(*gdiTabPublication, {3}, &gdiTabCaret)
            : note::NoteRenderFinalInteractionResult::InvalidPublication;
        const bool gdiTabSelectionGeometry =
            gdiTabSelectionResult == note::NoteRenderFinalInteractionResult::Resolved &&
            gdiTabCaretResult == note::NoteRenderFinalInteractionResult::Resolved &&
            gdiTabSelectionRects.size() == 1 &&
            gdiTabSelectionRects.front().right_px > gdiTabSelectionRects.front().left_px &&
            gdiTabSelectionRects.front().bottom_px > gdiTabSelectionRects.front().top_px;
        BITMAPINFO gdiPaintBitmapInfo{};
        gdiPaintBitmapInfo.bmiHeader.biSize = sizeof(gdiPaintBitmapInfo.bmiHeader);
        gdiPaintBitmapInfo.bmiHeader.biWidth = 800;
        gdiPaintBitmapInfo.bmiHeader.biHeight = -600; // top-down, exact client coordinates
        gdiPaintBitmapInfo.bmiHeader.biPlanes = 1;
        gdiPaintBitmapInfo.bmiHeader.biBitCount = 32;
        gdiPaintBitmapInfo.bmiHeader.biCompression = BI_RGB;
        HBITMAP gdiPaintBitmap = gdiMeasurementDc
            ? CreateDIBSection(gdiMeasurementDc, &gdiPaintBitmapInfo, DIB_RGB_COLORS,
                               nullptr, nullptr, 0) : nullptr;
        HGDIOBJ previousPaintBitmap = gdiPaintBitmap && gdiMeasurementDc
            ? SelectObject(gdiMeasurementDc, gdiPaintBitmap) : nullptr;
        if (gdiMeasurementDc && gdiPaintBitmap && previousPaintBitmap &&
            previousPaintBitmap != HGDI_ERROR) {
            (void)SelectClipRgn(gdiMeasurementDc, nullptr);
            (void)IntersectClipRect(gdiMeasurementDc, 0, 0, 800, 600);
        }
        bool gdiPainterSetup = gdiMeasurementDc && gdiPaintBitmap && previousPaintBitmap &&
            previousPaintBitmap != HGDI_ERROR && gdiTablePublication && gdiRawTablePublication;
        RECT gdiPainterClip{};
        const int gdiPainterClipKind = gdiMeasurementDc ? GetClipBox(gdiMeasurementDc, &gdiPainterClip) : ERROR;
        bool gdiStructuredPaintResult = false;
        bool gdiRawOwnerPaintResult = false;
        COLORREF gdiStructuredPaintPixel = CLR_INVALID;
        COLORREF gdiRawOwnerPaintPixel = CLR_INVALID;
        bool gdiPainterStructured = false;
        bool gdiPainterRawOwner = false;
        bool gdiSelectionCaretPaintResult = false;
        bool gdiPainterSelection = false;
        bool gdiPainterCaret = false;
        bool gdiHybridPaintResult = false;
        bool gdiHybridRawGlyphPainted = false;
        bool gdiHybridRawGroupHasNoStructuredGrid = false;
        bool gdiHybridSelectionPainted = false;
        bool gdiHybridCaretPainted = false;
        bool gdiImePaintResult = false;
        bool gdiImeSelectionPainted = false;
        bool gdiImeCaretPainted = false;
        bool gdiAdapterPaintResult = false;
        bool gdiAdapterSingleCaretOwner = false;
        bool gdiEmptyClipPaintSuccess = false;
        bool gdiEmptyPreeditFrameSuccess = false;
        bool gdiAttributedPreeditFrameSuccess = false;
        bool gdiAttributedPreeditPaintSuccess = false;
        bool gdiAllImeAttributesPaintSuccess = false;
        bool gdiStylePaintResult = false;
        bool gdiStyleBackgroundPainted = false;
        bool gdiMixedMathStructuralPaintResult = false;
        bool gdiMixedMathPresentationPaintResult = false;
        bool gdiTaskPaintResult = false;
        if (gdiPainterSetup) {
            RECT paintBounds{0, 0, 800, 600};
            HBRUSH white = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            bool containerPaint = containerGeometry && containerHybrid;
            if (containerPaint) {
                note::NoteRenderFinalGdiPaintOptions options;
                (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                containerPaint = note::NoteRenderFinalGdiPainter::Paint(
                    gdiMeasurementDc, *gdiContainerPublication, options);
                const auto bodyLayout = note::NoteRenderLineLayoutMap::LineAt(
                    gdiContainerPublication->layout()->line_layouts(), {2});
                note::NoteRenderAtomicGroupPlacement table;
                containerPaint = containerPaint && bodyLayout &&
                    GetPixel(gdiMeasurementDc, 8, static_cast<int>(bodyLayout->top_px + 1)) == options.theme.container_border &&
                    GetPixel(gdiMeasurementDc, 10, static_cast<int>(bodyLayout->top_px + 1)) == options.theme.container_surface &&
                    gdiContainerPublication->placement()->ResolveAtomicGroupContaining({7}, &table);
                const auto tableLayout = note::NoteRenderLineLayoutMap::LineAt(
                    gdiContainerPublication->layout()->line_layouts(), {7});
                containerPaint = containerPaint && tableLayout &&
                    GetPixel(gdiMeasurementDc, table.x_px, static_cast<int>(tableLayout->top_px)) == options.theme.table_border;
                (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                containerPaint = containerPaint && note::NoteRenderFinalGdiPainter::Paint(
                    gdiMeasurementDc, *gdiContainerRawPresentation, options);
            }
            Expect(containerPaint, "container backgrounds, distinct chips and table borders paint without erasing the shared grid in structured/hybrid frames");
            if (gdiTaskGeometry) {
                note::NoteRenderFinalGdiPaintOptions taskPaintOptions;
                (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                gdiTaskPaintResult = note::NoteRenderFinalGdiPainter::Paint(
                    gdiMeasurementDc, *gdiTaskPublication, taskPaintOptions);
                for (size_t index = 0; index < 2 && gdiTaskPaintResult; ++index) {
                    const auto& box = gdiTaskBoxes[index];
                    const int top = static_cast<int>(gdiTaskTops[index] + box.top_offset_px);
                    const int bottom = static_cast<int>(gdiTaskTops[index] + box.bottom_offset_px);
                    size_t interiorInk = 0;
                    for (int y = top + 1; y < bottom - 1; ++y) {
                        for (int x = box.left_px + 1; x < box.right_px - 1; ++x) {
                            const COLORREF pixel = GetPixel(gdiMeasurementDc, x, y);
                            if (pixel == taskPaintOptions.theme.text) ++interiorInk;
                        }
                    }
                    gdiTaskPaintResult = (box.checked ? interiorInk > 0 : interiorInk == 0) &&
                        GetPixel(gdiMeasurementDc, box.left_px, top) == taskPaintOptions.theme.text &&
                        GetPixel(gdiMeasurementDc, box.right_px, top + 1) == RGB(255, 255, 255) &&
                        GetPixel(gdiMeasurementDc, box.left_px + 1, top - 1) == RGB(255, 255, 255) &&
                        GetPixel(gdiMeasurementDc, box.left_px + 1, bottom) == RGB(255, 255, 255);
                }
            }
            note::NoteRenderFinalGdiPaintOptions gdiPaintOptions;
            gdiPaintOptions.horizontal_scroll_px = gdiTableGroup.x_px;
            const auto gdiTableAnchor = note::NoteRenderLineLayoutMap::LineAt(
                gdiTablePublication->layout()->line_layouts(), gdiTableGroup.first_line);
            if (!gdiTableAnchor.has_value()) {
                gdiPainterSetup = false;
            } else {
                // The pixel check deliberately puts the exact table origin at the client
                // origin.  This covers both scroll-coordinate transforms and the single
                // shared grid paint, rather than assuming the first logical line begins
                // at content Y=0.
                gdiPaintOptions.vertical_scroll_px = gdiTableAnchor->top_px;
            }
            if (gdiPainterSetup) {
                const auto hasNonWhiteTablePixel = [&]() {
                    const int right = std::max(1, std::min(800, gdiTableGroup.width_px));
                    const int bottom = static_cast<int>(std::max<uint32_t>(
                        1U, std::min<uint32_t>(600U, gdiTableGroup.height_px)));
                    for (int y = 0; y < bottom; ++y) {
                        for (int x = 0; x < right; ++x) {
                            const COLORREF pixel = GetPixel(gdiMeasurementDc, x, y);
                            if (pixel != CLR_INVALID && pixel != RGB(255, 255, 255)) return true;
                        }
                    }
                    return false;
                };
                (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                gdiStructuredPaintResult = note::NoteRenderFinalGdiPainter::Paint(
                    gdiMeasurementDc, *gdiTablePublication, gdiPaintOptions);
                gdiStructuredPaintPixel = GetPixel(gdiMeasurementDc, 0, 0);
                gdiPainterStructured = gdiStructuredPaintResult &&
                    gdiStructuredPaintPixel != CLR_INVALID && hasNonWhiteTablePixel();
                (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                gdiRawOwnerPaintResult = note::NoteRenderFinalGdiPainter::Paint(
                    gdiMeasurementDc, *gdiRawTablePublication, gdiPaintOptions);
                gdiRawOwnerPaintPixel = GetPixel(gdiMeasurementDc, 0, 0);
                gdiPainterRawOwner = gdiRawOwnerPaintResult &&
                    gdiRawOwnerPaintPixel != CLR_INVALID && !hasNonWhiteTablePixel();

                if (gdiMixedMathPublication && gdiMixedMathPresentation) {
                    const auto replacedLine = note::NoteSourceLineMap::LineAt(
                        gdiMixedMathCore.source_line_map(), {3});
                    if (replacedLine) {
                        note::NoteRenderFinalImePreeditInput emptyInput;
                        emptyInput.canonical_replacement_span = {
                            replacedLine->start, replacedLine->content_end};
                        emptyInput.editor_selection = {replacedLine->start, replacedLine->start};
                        note::NoteRenderFinalImePreeditPresentation emptyPreedit;
                        note::NoteRenderFinalWin32AdapterInput emptyFrameInput;
                        emptyFrameInput.text_core = &gdiMixedMathCore;
                        emptyFrameInput.structural_publication = gdiMixedMathPublication;
                        emptyFrameInput.owner_input = gdiMixedMathInput.owner_input;
                        emptyFrameInput.owner_input.ime_preedit = true;
                        emptyFrameInput.owner_input.ime_preedit_can_reuse_committed_layout = true;
                        emptyFrameInput.owner_input.requested_editor_lines = {{{3}, {4}}};
                        emptyFrameInput.editor_selection = emptyInput.editor_selection;
                        emptyFrameInput.editor_caret = replacedLine->start;
                        emptyFrameInput.measurement_dc = gdiMeasurementDc;
                        std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> emptyFrame;
                        if (note::NoteRenderFinalImePreeditPresentation::Build(
                                gdiMixedMathCore, emptyInput, &emptyPreedit) ==
                                note::NoteRenderFinalImePreeditPresentationBuildResult::Built) {
                            emptyFrameInput.ime_preedit = std::move(emptyPreedit);
                            note::NoteRenderFinalRawLineSurface emptyRaw;
                            note::NoteRenderFinalCaretGeometry emptyCaret;
                            gdiEmptyPreeditFrameSuccess =
                                note::NoteRenderFinalWin32AdapterFrame::Build(emptyFrameInput, &emptyFrame) ==
                                    note::NoteRenderFinalWin32AdapterBuildResult::Built &&
                                emptyFrame && emptyFrame->presentation()->ResolveRawLine({3}, &emptyRaw) &&
                                emptyRaw.display.display_text.empty() &&
                                emptyFrame->presentation()->SurfaceAt({2}) ==
                                    note::NoteRenderFinalSurfaceKind::StructuredPaint &&
                                emptyFrame->Caret(&emptyCaret) ==
                                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                                emptyCaret.bottom_px > emptyCaret.top_px &&
                                emptyFrame->Paint(gdiMeasurementDc, {});
                        }
                        using ImeAttribute = note::NoteImeCharacterAttribute;
                        const auto buildAttributedFrame = [&](const std::wstring& text,
                            const std::vector<ImeAttribute>& attributes,
                            const std::vector<note::Utf16CodeUnitOffset>& clauses) {
                            auto input = emptyInput;
                            input.composition_text = text;
                            input.composition_attributes = attributes;
                            input.composition_clauses = clauses;
                            note::NoteRenderFinalImePreeditPresentation preedit;
                            std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> frame;
                            if (note::NoteRenderFinalImePreeditPresentation::Build(gdiMixedMathCore, input, &preedit) ==
                                note::NoteRenderFinalImePreeditPresentationBuildResult::Built) {
                                auto adapter = emptyFrameInput;
                                adapter.ime_preedit = std::move(preedit);
                                if (note::NoteRenderFinalWin32AdapterFrame::Build(adapter, &frame) !=
                                    note::NoteRenderFinalWin32AdapterBuildResult::Built) frame.reset();
                            }
                            return frame;
                        };
                        const auto attributedFrame = buildAttributedFrame(L"abcdef",
                            {ImeAttribute::Input, ImeAttribute::Input,
                             ImeAttribute::TargetConverted, ImeAttribute::TargetConverted,
                             ImeAttribute::Converted, ImeAttribute::Converted}, {{0}, {2}, {4}, {6}});
                        const auto wrappedFrame = buildAttributedFrame(std::wstring(240, L'a'),
                            std::vector<ImeAttribute>(240, ImeAttribute::TargetNotConverted),
                            {{0}, {60}, {120}, {180}, {240}});
                        gdiAttributedPreeditFrameSuccess = attributedFrame && wrappedFrame &&
                            attributedFrame->ime_decorations().size() == 3 &&
                            wrappedFrame->ime_decorations().size() > 4 &&
                            attributedFrame->ime_decorations()[1].attribute == ImeAttribute::TargetConverted &&
                            attributedFrame->presentation()->SurfaceAt({2}) == note::NoteRenderFinalSurfaceKind::StructuredPaint;
                        if (gdiAttributedPreeditFrameSuccess) {
                            // Each wrap/clause rectangle must agree with the
                            // same selection geometry, not font-width guesses.
                            std::vector<note::NoteRenderFinalRect> expected;
                            const auto& span = attributedFrame->presentation()->ime_preedit()->segments()[1].editor_span;
                            const auto& target = attributedFrame->ime_decorations()[1].rect;
                            const auto& inputRect = attributedFrame->ime_decorations()[0].rect;
                            gdiAttributedPreeditFrameSuccess =
                                note::ResolveNoteRenderFinalPresentationEditorSelection(*attributedFrame->presentation(), span, &expected) ==
                                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                                expected.size() == 1 && expected[0].left_px == target.left_px &&
                                expected[0].right_px == target.right_px && expected[0].top_px == target.top_px &&
                                expected[0].bottom_px == target.bottom_px;
                            (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                            note::NoteRenderFinalWin32PaintState state;
                            state.paint_caret = false;
                            state.horizontal_scroll_px = std::max(0, inputRect.left_px - 2);
                            state.vertical_scroll_px = inputRect.top_px;
                            state.theme.text = RGB(17, 90, 23);
                            const COLORREF brushBefore = GetDCBrushColor(gdiMeasurementDc);
                            const int targetX = target.left_px + 2 - state.horizontal_scroll_px;
                            const int targetBottom = static_cast<int>(target.bottom_px - state.vertical_scroll_px);
                            const int inputX = inputRect.left_px + 1 - state.horizontal_scroll_px;
                            const int inputBottom = static_cast<int>(inputRect.bottom_px - state.vertical_scroll_px);
                            gdiAttributedPreeditPaintSuccess = attributedFrame->Paint(gdiMeasurementDc, state) &&
                                GetDCBrushColor(gdiMeasurementDc) == brushBefore &&
                                GetPixel(gdiMeasurementDc, targetX, targetBottom - 1) == state.theme.text &&
                                GetPixel(gdiMeasurementDc, targetX, targetBottom - 2) == state.theme.text &&
                                GetPixel(gdiMeasurementDc, inputX, inputBottom - 1) == state.theme.text &&
                                GetPixel(gdiMeasurementDc, inputX + 2, inputBottom - 1) == RGB(255, 255, 255);
                            gdiAllImeAttributesPaintSuccess = true;
                            for (const auto attribute : {ImeAttribute::Input, ImeAttribute::TargetConverted,
                                ImeAttribute::Converted, ImeAttribute::TargetNotConverted,
                                ImeAttribute::InputError, ImeAttribute::FixedConverted}) {
                                const auto frame = buildAttributedFrame(L"abcdef",
                                    std::vector<ImeAttribute>(6, attribute), {{0}, {6}});
                                if (!frame || frame->ime_decorations().size() != 1) {
                                    gdiAllImeAttributesPaintSuccess = false;
                                    break;
                                }
                                (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                                const auto& rect = frame->ime_decorations().front().rect;
                                state.horizontal_scroll_px = std::max(0, rect.left_px - 2);
                                state.vertical_scroll_px = rect.top_px;
                                const int x = rect.left_px + 1 - state.horizontal_scroll_px;
                                const int bottom = static_cast<int>(rect.bottom_px - state.vertical_scroll_px);
                                const bool target = attribute == ImeAttribute::TargetConverted ||
                                                    attribute == ImeAttribute::TargetNotConverted;
                                const bool error = attribute == ImeAttribute::InputError;
                                const bool painted = frame->Paint(gdiMeasurementDc, state) &&
                                    GetPixel(gdiMeasurementDc, x, bottom - (target || error ? 2 : 1)) == state.theme.text &&
                                    (attribute != ImeAttribute::Input ||
                                     GetPixel(gdiMeasurementDc, x + 2, bottom - 1) == RGB(255, 255, 255)) &&
                                    (!error || GetPixel(gdiMeasurementDc, x + 1, bottom - 1) == state.theme.text) &&
                                    (!target || GetPixel(gdiMeasurementDc, x, bottom - 1) == state.theme.text);
                                gdiAllImeAttributesPaintSuccess = gdiAllImeAttributesPaintSuccess && painted;
                            }
                        }
                    }
                    const int emptyClipState = SaveDC(gdiMeasurementDc);
                    if (emptyClipState != 0) {
                        gdiEmptyClipPaintSuccess =
                            IntersectClipRect(gdiMeasurementDc, 0, 0, 0, 0) == NULLREGION &&
                            note::NoteRenderFinalGdiPainter::Paint(
                                gdiMeasurementDc, *gdiMixedMathPublication) &&
                            note::NoteRenderFinalGdiPainter::Paint(
                                gdiMeasurementDc, *gdiMixedMathPresentation);
                        (void)RestoreDC(gdiMeasurementDc, emptyClipState);
                    }
                    note::NoteRenderFinalGdiPaintOptions mixedMathPaintOptions;
                    mixedMathPaintOptions.caret = note::Utf16CodeUnitOffset{0};
                    (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                    gdiMixedMathStructuralPaintResult = note::NoteRenderFinalGdiPainter::Paint(
                        gdiMeasurementDc, *gdiMixedMathPublication, mixedMathPaintOptions);
                    (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                    gdiMixedMathPresentationPaintResult = note::NoteRenderFinalGdiPainter::Paint(
                        gdiMeasurementDc, *gdiMixedMathPresentation, mixedMathPaintOptions);
                }

                if (gdiTabSelectionGeometry) {
                    const note::NoteRenderFinalRect& selection = gdiTabSelectionRects.front();
                    note::NoteRenderFinalGdiPaintOptions selectionPaintOptions;
                    selectionPaintOptions.horizontal_scroll_px = selection.left_px;
                    selectionPaintOptions.vertical_scroll_px = selection.top_px;
                    selectionPaintOptions.selection = note::Span{{0}, {3}};
                    selectionPaintOptions.caret = note::Utf16CodeUnitOffset{3};
                    (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                    gdiSelectionCaretPaintResult = note::NoteRenderFinalGdiPainter::Paint(
                        gdiMeasurementDc, *gdiTabPublication, selectionPaintOptions);
                    const int selectionRight = static_cast<int>(std::min<int64_t>(
                        800, static_cast<int64_t>(selection.right_px) - selection.left_px));
                    const int selectionBottom = static_cast<int>(std::min<uint64_t>(
                        600U, selection.bottom_px - selection.top_px));
                    for (int y = 0; y < selectionBottom && !gdiPainterSelection; ++y) {
                        for (int x = 0; x < selectionRight; ++x) {
                            if (GetPixel(gdiMeasurementDc, x, y) ==
                                selectionPaintOptions.theme.selection_surface) {
                                gdiPainterSelection = true;
                                break;
                            }
                        }
                    }
                    const int caretX = gdiTabCaret.x_px - selection.left_px;
                    const int caretY = static_cast<int>(
                        gdiTabCaret.top_px - selection.top_px);
                    gdiPainterCaret = caretX >= 0 && caretX < 800 && caretY >= 0 && caretY < 600 &&
                        GetPixel(gdiMeasurementDc, caretX, caretY) ==
                            selectionPaintOptions.theme.caret;
                }

                if (gdiHybridPresentation && gdiHybridInteractionGeometry &&
                    gdiHybridRawDivider.placement.runs.size() == 1) {
                    const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(
                        gdiHybridPresentation->line_layouts(), {3});
                    const note::NoteRenderRunPlacement& rawRun =
                        gdiHybridRawDivider.placement.runs.front();
                    if (rawLayout.has_value()) {
                        note::NoteRenderFinalGdiPaintOptions hybridPaintOptions;
                        hybridPaintOptions.horizontal_scroll_px = rawRun.x_px;
                        hybridPaintOptions.vertical_scroll_px = rawLayout->top_px;
                        hybridPaintOptions.theme.table_border = RGB(255, 0, 0);
                        hybridPaintOptions.selection = gdiHybridRawSelection;
                        hybridPaintOptions.caret = gdiHybridCaret.source_offset;
                        (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                        gdiHybridPaintResult = note::NoteRenderFinalGdiPainter::Paint(
                            gdiMeasurementDc, *gdiHybridPresentation, hybridPaintOptions);
                        const int scanRight = static_cast<int>(std::min<int64_t>(
                            800, static_cast<int64_t>(rawRun.width_px) + 1));
                        const int scanBottom = static_cast<int>(std::min<uint32_t>(
                            600U, rawLayout->layout.height_px));
                        bool hasRedGridPixel = false;
                        for (int y = 0; y < scanBottom; ++y) {
                            for (int x = 0; x < scanRight; ++x) {
                                const COLORREF pixel = GetPixel(gdiMeasurementDc, x, y);
                                gdiHybridRawGlyphPainted = gdiHybridRawGlyphPainted ||
                                    pixel == hybridPaintOptions.theme.text;
                                hasRedGridPixel = hasRedGridPixel ||
                                    pixel == hybridPaintOptions.theme.table_border;
                                gdiHybridSelectionPainted = gdiHybridSelectionPainted ||
                                    pixel == hybridPaintOptions.theme.selection_surface;
                            }
                        }
                        gdiHybridRawGroupHasNoStructuredGrid = !hasRedGridPixel;
                        const int caretX = gdiHybridCaret.x_px - rawRun.x_px;
                        const int caretY = static_cast<int>(
                            gdiHybridCaret.top_px - rawLayout->top_px);
                        gdiHybridCaretPainted = caretX >= 0 && caretX < 800 &&
                            caretY >= 0 && caretY < 600 &&
                            GetPixel(gdiMeasurementDc, caretX, caretY) ==
                                hybridPaintOptions.theme.caret;
                    }
                }

                if (gdiImePresentation && gdiImeInteractionGeometry &&
                    gdiImeRawDivider.placement.runs.size() == 1) {
                    const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(
                        gdiImePresentation->line_layouts(), {3});
                    const note::NoteRenderRunPlacement& rawRun =
                        gdiImeRawDivider.placement.runs.front();
                    if (rawLayout.has_value()) {
                        note::NoteRenderFinalGdiPaintOptions imePaintOptions;
                        imePaintOptions.horizontal_scroll_px = rawRun.x_px;
                        imePaintOptions.vertical_scroll_px = rawLayout->top_px;
                        imePaintOptions.selection = gdiImePreedit.editor_selection();
                        imePaintOptions.caret = gdiImePreedit.editor_selection().end;
                        (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                        gdiImePaintResult = note::NoteRenderFinalGdiPainter::Paint(
                            gdiMeasurementDc, *gdiImePresentation, imePaintOptions);
                        const int scanRight = static_cast<int>(std::min<int64_t>(
                            800, static_cast<int64_t>(rawRun.width_px) + 1));
                        const int scanBottom = static_cast<int>(std::min<uint32_t>(
                            600U, rawLayout->layout.height_px));
                        for (int y = 0; y < scanBottom; ++y) {
                            for (int x = 0; x < scanRight; ++x) {
                                gdiImeSelectionPainted = gdiImeSelectionPainted ||
                                    GetPixel(gdiMeasurementDc, x, y) ==
                                        imePaintOptions.theme.selection_surface;
                            }
                        }
                        const int caretX = gdiImeCaret.x_px - rawRun.x_px;
                        const int caretY = static_cast<int>(
                            gdiImeCaret.top_px - rawLayout->top_px);
                        gdiImeCaretPainted = caretX >= 0 && caretX < 800 &&
                            caretY >= 0 && caretY < 600 &&
                            GetPixel(gdiMeasurementDc, caretX, caretY) ==
                                imePaintOptions.theme.caret;
                    }
                }

                if (gdiAdapterFrame && gdiAdapterGeometry &&
                    gdiImeRawDivider.placement.runs.size() == 1) {
                    const auto rawLayout = note::NoteRenderLineLayoutMap::LineAt(
                        gdiAdapterFrame->presentation()->line_layouts(), {3});
                    const note::NoteRenderRunPlacement& rawRun =
                        gdiImeRawDivider.placement.runs.front();
                    if (rawLayout.has_value()) {
                        note::NoteRenderFinalWin32PaintState adapterPaintState;
                        adapterPaintState.horizontal_scroll_px = rawRun.x_px;
                        adapterPaintState.vertical_scroll_px = rawLayout->top_px;
                        adapterPaintState.theme.caret = RGB(255, 0, 255);
                        (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                        gdiAdapterPaintResult = gdiAdapterFrame->Paint(
                            gdiMeasurementDc, adapterPaintState);
                        const int caretX = gdiAdapterCaret.x_px - adapterPaintState.horizontal_scroll_px;
                        const int caretY = static_cast<int>(gdiAdapterCaret.top_px - rawLayout->top_px);
                        const bool paintedCaret = gdiAdapterPaintResult &&
                            GetPixel(gdiMeasurementDc, caretX, caretY) == adapterPaintState.theme.caret;
                        (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                        adapterPaintState.paint_caret = false;
                        gdiAdapterSingleCaretOwner = paintedCaret &&
                            gdiAdapterFrame->Paint(gdiMeasurementDc, adapterPaintState) &&
                            GetPixel(gdiMeasurementDc, caretX, caretY) != adapterPaintState.theme.caret;
                    }
                }

                if (gdiStylePublication && gdiStyleSourceRuns) {
                    note::NoteRenderSourceLinePlan styleLine;
                    note::NoteRenderLinePlacement stylePlacement;
                    const auto styleLayout = note::NoteRenderLineLayoutMap::LineAt(
                        gdiStylePublication->layout()->line_layouts(), {0});
                    if (styleLayout.has_value() &&
                        gdiStylePublication->source_plan()->ResolveLine({0}, &styleLine) &&
                        gdiStylePublication->placement()->ResolveLine({0}, &stylePlacement) &&
                        styleLine.runs.size() == stylePlacement.runs.size()) {
                        note::NoteRenderFinalGdiPaintOptions stylePaintOptions;
                        stylePaintOptions.vertical_scroll_px = styleLayout->top_px;
                        (void)FillRect(gdiMeasurementDc, &paintBounds, white);
                        gdiStylePaintResult = note::NoteRenderFinalGdiPainter::Paint(
                            gdiMeasurementDc, *gdiStylePublication, stylePaintOptions);
                        for (size_t index = 0; index < styleLine.runs.size(); ++index) {
                            const bool isBackgroundRun = std::any_of(
                                styleLine.runs[index].styles.begin(),
                                styleLine.runs[index].styles.end(),
                                [](const note::NoteRenderSourceStyleAttribute& style) {
                                    return style.kind == note::StyleKind::BackgroundColor;
                            });
                            if (!isBackgroundRun) continue;
                            for (const note::NoteRenderRunPlacementFragment& fragment :
                                 stylePlacement.runs[index].fragments) {
                                if (fragment.width_px <= 0 || fragment.top_offset_px >= 600 ||
                                    fragment.x_px >= 800 ||
                                    fragment.x_px > std::numeric_limits<int>::max() -
                                        fragment.width_px) {
                                    continue;
                                }
                                const int right = std::min(800, fragment.x_px + fragment.width_px);
                                const uint64_t fragmentBottom =
                                    static_cast<uint64_t>(fragment.top_offset_px) + fragment.height_px;
                                const int bottom = static_cast<int>(std::min<uint64_t>(
                                    600, fragmentBottom));
                                for (int y = static_cast<int>(fragment.top_offset_px);
                                     y < bottom && !gdiStyleBackgroundPainted; ++y) {
                                    for (int x = std::max(0, fragment.x_px); x < right; ++x) {
                                        gdiStyleBackgroundPainted =
                                            GetPixel(gdiMeasurementDc, x, y) == RGB(0, 255, 0);
                                        if (gdiStyleBackgroundPainted) break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (previousPaintBitmap && previousPaintBitmap != HGDI_ERROR && gdiMeasurementDc) {
            (void)SelectObject(gdiMeasurementDc, previousPaintBitmap);
        }
        if (gdiPaintBitmap) (void)DeleteObject(gdiPaintBitmap);
        if (gdiMeasurementDc && previousFont) (void)SelectObject(gdiMeasurementDc, previousFont);
        if (gdiMeasurementDc) (void)DeleteDC(gdiMeasurementDc);

        Expect(gdiPainterSetup,
               "the GDI painter test owns a selected memory bitmap and both table publications");
        Expect(gdiExplicitRawMeasurement,
               "the GDI provider measures one explicit transient raw line without a temporary whole-document TextCore");
        Expect(gdiTableAllCommitted,
               "the structured table publication gives every table row one committed owner");
        Expect(gdiPainterClipKind != ERROR && gdiPainterClip.bottom > gdiPainterClip.top,
               "the GDI painter receives a nonempty memory-DC client clip");
        Expect(gdiStructuredPaintResult && gdiStructuredPaintPixel != CLR_INVALID,
               "the final GDI painter accepts the complete structured table publication");
        Expect(gdiRawOwnerPaintResult && gdiRawOwnerPaintPixel != CLR_INVALID,
               "the final GDI painter accepts the native-owned table publication without painting it");
        Expect(gdiMixedMathBuildResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiMixedMathPresentationResult ==
                       note::NoteRenderFinalPresentationSnapshotBuildResult::Built &&
                   gdiMixedMathStructuralPaintResult && gdiMixedMathPresentationPaintResult,
               "the UI mixed Markdown/TeX fixture paints through both complete and published hybrid final frames");
        Expect(gdiPainterStructured,
               "the final GDI painter consumes one committed table placement and paints its shared grid");
        Expect(gdiPainterRawOwner,
               "the final GDI painter leaves a native-owned table group entirely to the raw editor");
        Expect(gdiSelectionCaretPaintResult && gdiPainterSelection && gdiPainterCaret,
               "the final GDI painter derives structured selection and caret pixels from the same placement");
        Expect(gdiHybridPaintResult && gdiHybridRawGlyphPainted &&
                   gdiHybridRawGroupHasNoStructuredGrid && gdiHybridSelectionPainted &&
                   gdiHybridCaretPainted,
               "the hybrid GDI painter draws raw source, selection, and caret at hybrid coordinates without a structured grid underneath");
        Expect(gdiHybridStructuralResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiHybridStructuralPublication,
               "the hybrid presentation test starts from one current structural publication");
        Expect(gdiHybridPresentationResult ==
                   note::NoteRenderFinalPresentationSnapshotBuildResult::Built &&
                   gdiHybridPresentation,
               "the hybrid presentation snapshot accepts exact raw GDI surfaces");
        Expect(gdiHybridSnapshotMatches,
               "the hybrid snapshot retains one exact structural revision and layout identity");
        Expect(gdiHybridSurfaceRanges,
               "a table edit expands one requested raw row to the full table surface range");
        Expect(gdiHybridRawDividerResolved,
               "the raw table divider preserves its literal source and receives normal raw line height");
        Expect(gdiImePresentationGeometry,
               "an IME preedit replaces only its raw owner line with temporary editor coordinates while retaining one hybrid snapshot");
        Expect(gdiImeInteractionGeometry && gdiImePaintResult && gdiImeSelectionPainted &&
                   gdiImeCaretPainted,
               "IME preedit selection, caret, hit-test, and custom paint use one temporary raw coordinate surface");
        Expect(gdiImeCrossSurfaceSelection,
               "an IME editor-coordinate selection crosses raw preedit and unchanged structured suffixes without coordinate mixing");
        Expect(gdiAdapterGeometry && gdiAdapterPaintResult &&
                   gdiAdapterRejectsInvalidEditorState,
               "the final Win32 adapter publishes one complete IME frame or rejects invalid editor coordinates before paint");
        Expect(gdiAdapterSingleCaretOwner,
               "a final frame omits its painted caret when Windows owns the visible caret");
        Expect(gdiEmptyClipPaintSuccess,
               "an empty Windows paint region is a successful no-op, not a native fallback trigger");
        Expect(gdiEmptyPreeditFrameSuccess,
               "an empty provisional IME row retains its final caret and paint while unrelated rows stay structured");
        Expect(gdiAttributedPreeditFrameSuccess,
               "IME clauses and wrapped candidate segments resolve through exactly the same final geometry as selection");
        Expect(gdiAttributedPreeditPaintSuccess,
               "the final painter distinguishes dotted input from thick candidate underlines after scrolling and restores GDI state");
        Expect(gdiAllImeAttributesPaintSuccess,
               "all six IME character statuses paint their expected dotted, thick, thin, or error underline without changing glyph geometry");
        Expect(gdiStyleResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiStyleSourceRuns && gdiStyleFontGeometry && gdiStylePaintResult &&
                   gdiStyleBackgroundPainted,
               "the final painter and measurer apply legacy color, decoration, link, family, size, and line-height styles from final source runs without a legacy overlay");
        Expect(gdiStyleResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiStyleSourceRuns && gdiStyleFontGeometry,
               "final source styles retain legacy visible-run semantics and their measured font geometry");
        Expect(gdiStyleSourceRuns,
               "legacy visible source runs retain every parsed color, decoration, link, font, and line-height style");
        Expect(gdiStyleFontGeometry,
               "a legacy line-height style contributes its measured final row height");
        Expect(gdiStylePaintResult && gdiStyleBackgroundPainted,
               "the final painter applies a legacy background style after legacy syntax is hidden");
        Expect(gdiPositionStyleResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiPositionStylePublication,
               "the final measurer accepts a document containing legacy indent and anchor source styles");
        Expect(gdiPositionStyleRunMapping,
               "legacy position styles remain attached to their final placement runs");
        Expect(gdiPositionStyleSyntaxHidden,
               "parser-proven legacy position tags become zero-width final syntax rather than visible text");
        Expect(gdiPositionStyleIndentGeometry,
               "a legacy indent offsets the first visible final run by its space columns");
        Expect(gdiPositionStyleCenterGeometry,
               "a legacy center anchor receives a viewport-relative final X coordinate");
        Expect(gdiPositionStyleRightGeometry,
               "adjacent legacy right-anchor runs share one non-flow placement origin");
        Expect(gdiPositionStyleResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiPositionStyleGeometry,
               "the final measurer gives legacy indent and contiguous left/center/right anchors one immutable placement geometry");
        Expect(gdiStateInitial ==
                   note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedComplete &&
                   gdiStateInitialFrame &&
                   gdiStateReused ==
                       note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedReusedStructural &&
                   gdiStateEventWithdrawal &&
                   gdiStateAfterWithdrawal ==
                       note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedReusedStructural &&
                   gdiStateWithdrawalReusedStructure &&
                   gdiStateAppliedLocalEdit &&
                   gdiStateLocal ==
                       note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedLocalPatch &&
                   gdiStateLocalFrame && gdiStateAppliedStructuralEdit &&
                   gdiStateStructural ==
                       note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedComplete &&
                   gdiStateStructuralFrame && gdiStateFailureClearsFrame &&
                   gdiStateRecovered ==
                       note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedReusedStructural &&
                   gdiStateRecoveredFrame && gdiStateWorkUsesOnlyProvenTransactions,
               "the final view state withdraws a public frame before an event, then atomically publishes complete, local, and recovered hybrid frames without exposing stale glyphs");
        Expect(gdiLegacyLinkPresentationHit,
               "the published Win32 final frame resolves persisted link IDs without legacy cache lookup");
        Expect(gdiLegacyLinkPaint,
               "a published legacy underlined link paints its hidden syntax and visible text without falling back");
        Expect(longAdapterInitial ==
                   note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedComplete &&
                   longAdapterAppliedEdit &&
                   longAdapterLocal ==
                       note::NoteRenderFinalWin32AdapterStateRefreshResult::PublishedLocalPatch &&
                   longAdapterAfter &&
                   longAdapterAfter->Matches(longAdapterCore, longAdapterInput.layout_key) &&
                   longAdapterUsesBoundedWork && longAdapterKeepsUnchangedPayloads,
               "a 4096-row final adapter edit uses one local transaction, no second complete transaction, and shared unchanged payloads");
        Expect(gdiHybridTailGeometry,
               "the hybrid layout moves the following structured row after raw table geometry changes");
        Expect(gdiHybridSurfaceQueries,
               "hybrid surface lookup resolves every source row to exactly one paint kind");
        Expect(gdiHybridInteractionGeometry,
               "hybrid interaction resolves raw-source hit, selection, caret, and horizontal extent from the same layout map");
        Expect(gdiHybridStructuralResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiHybridStructuralPublication &&
                   gdiHybridPresentationResult ==
                       note::NoteRenderFinalPresentationSnapshotBuildResult::Built &&
                   gdiHybridGeometry,
               "a raw table group gets measured raw surfaces and shifts following structured rows through one hybrid layout map");

        Expect(gdiTaskGeometry,
               "task checkboxes are square, font-sized and separated from text across nested, tall and wrapped rows");
        Expect(gdiVisibleClicksKeepSource,
               "visible heading, task and styled text starts own clicks instead of hidden syntax and retain their source boundary after raw transition");
        Expect(gdiTaskPaintResult,
               "task checkbox borders and ticks stay inside their measured bounds and distinguish checked from unchecked");
        Expect(gdiBuildResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiPublication && gdiHasWrappedFragment && gdiFragmentsOwnAllSource &&
                   gdiLayoutLine.layout.height_px > 0 &&
                   gdiTabBuildResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiTabPublication && measuredGdiSpace && tabBoundary &&
                   tabBoundary->x_px == 8 + 4 * std::max<LONG>(1, gdiSpace.cx) &&
                   gdiMathResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiMathPublication && hasGdiMathGraphic() &&
                   gdiBlockMathResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiBlockMathPublication && gdiBlockMathGeometry &&
                   gdiTableResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiTablePublication && gdiTableGeometry &&
                   gdiRawTableResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   gdiDecoratorResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   hasGdiDecoration(note::NoteRenderVisualDecorationKind::ListMarker) &&
                   hasGdiDecoration(note::NoteRenderVisualDecorationKind::TaskCheckbox) &&
                   hasGdiDecoration(note::NoteRenderVisualDecorationKind::QuoteBar) &&
                   hasGdiDecoration(note::NoteRenderVisualDecorationKind::CodeBlockSurface) &&
                   hasGdiDecoration(note::NoteRenderVisualDecorationKind::HorizontalRule) &&
                   hasGdiDecoration(note::NoteRenderVisualDecorationKind::InlineCodeSurface),
               "the final GDI provider measures wrapped source/display fragments, tab stops, inline/block math, shared table grids, and structural decoration geometry");
    }

    {
        note::NoteMetadata localTransactionMeta;
        localTransactionMeta.file_name = L"final-local.md";
        note::NoteTextCore localTransactionCore;
        localTransactionCore.Reset(
            note::NoteId{995}, localTransactionMeta, L"first\nsecond\nthird", 400, 3);
        note::NoteRenderFinalCompleteBuildInput localCompleteInput;
        localCompleteInput.content_kind = note::NoteContentKind::Markdown;
        localCompleteInput.layout_key = {720, 96, 96, 4, 4, 12, true};
        localCompleteInput.owner_input.render_active = true;
        localCompleteInput.owner_input.editor_text_core_current = true;
        localCompleteInput.owner_input.visible_lines = {
            {0}, {localTransactionCore.logical_line_count()}};
        DeterministicFinalMeasurementProvider localMeasurementProvider;
        std::shared_ptr<const note::NoteRenderFinalPublication> beforeLocalTransaction;
        const note::NoteRenderFinalCompleteBuildResult beforeLocalTransactionResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                localTransactionCore, localCompleteInput, localMeasurementProvider,
                &beforeLocalTransaction);
        const note::TextEdit localTransactionEdit{{8}, 0, L"X"};
        const bool appliedLocalTransactionEdit =
            localTransactionCore.Apply(localTransactionEdit) == note::NoteTextApplyResult::Applied;
        note::NoteRenderFinalLocalPatchInput localPatchInput;
        localPatchInput.layout_key = localCompleteInput.layout_key;
        localPatchInput.owner_input = localCompleteInput.owner_input;
        localPatchInput.edit = localTransactionEdit;
        std::shared_ptr<const note::NoteRenderFinalPublication> locallyPatchedTransaction =
            beforeLocalTransaction;
        const note::NoteRenderFinalCompleteBuildResult localPatchTransactionResult =
            note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                beforeLocalTransaction, localTransactionCore, localPatchInput,
                localMeasurementProvider, &locallyPatchedTransaction);
        note::NoteRenderSourceLinePlan locallyPatchedSourceLine;
        note::NoteRenderSourceLinePlan fullyRebuiltSourceLine;
        note::NoteRenderLinePlacement locallyPatchedPlacementLine;
        note::NoteRenderLinePlacement fullyRebuiltPlacementLine;
        std::shared_ptr<const note::NoteRenderFinalPublication> fullyRebuiltTransaction;
        const note::NoteRenderFinalCompleteBuildResult fullRebuildTransactionResult =
            note::NoteRenderFinalTransaction::BuildComplete(
                localTransactionCore, localCompleteInput, localMeasurementProvider,
                &fullyRebuiltTransaction);
        const bool localFinalSourceMatchesFull =
            locallyPatchedTransaction && fullyRebuiltTransaction &&
            locallyPatchedTransaction->source_plan()->ResolveLine({1}, &locallyPatchedSourceLine) &&
            fullyRebuiltTransaction->source_plan()->ResolveLine({1}, &fullyRebuiltSourceLine) &&
            locallyPatchedSourceLine.content_span.start == fullyRebuiltSourceLine.content_span.start &&
            locallyPatchedSourceLine.content_span.end == fullyRebuiltSourceLine.content_span.end &&
            locallyPatchedSourceLine.runs.size() == fullyRebuiltSourceLine.runs.size() &&
            locallyPatchedTransaction->placement()->ResolveLine({1}, &locallyPatchedPlacementLine) &&
            fullyRebuiltTransaction->placement()->ResolveLine({1}, &fullyRebuiltPlacementLine) &&
            locallyPatchedPlacementLine.runs.size() == fullyRebuiltPlacementLine.runs.size();
        Expect(beforeLocalTransactionResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   beforeLocalTransaction && beforeLocalTransaction->has_continuity_checkpoint() &&
                   appliedLocalTransactionEdit &&
                   localPatchTransactionResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   locallyPatchedTransaction && locallyPatchedTransaction != beforeLocalTransaction &&
                   locallyPatchedTransaction->Matches(
                       localTransactionCore, localCompleteInput.layout_key) &&
                   locallyPatchedTransaction->has_continuity_checkpoint() &&
                   locallyPatchedTransaction->source_plan()->SharesLinePayloadForDifferentialTest(
                       *beforeLocalTransaction->source_plan(), {0}) &&
                   locallyPatchedTransaction->source_plan()->SharesLinePayloadForDifferentialTest(
                       *beforeLocalTransaction->source_plan(), {2}) &&
                   locallyPatchedTransaction->placement()->SharesLinePayloadForDifferentialTest(
                       *beforeLocalTransaction->placement(), {0}) &&
                   locallyPatchedTransaction->placement()->SharesLinePayloadForDifferentialTest(
                       *beforeLocalTransaction->placement(), {2}) &&
                   fullRebuildTransactionResult == note::NoteRenderFinalCompleteBuildResult::Built &&
                   localFinalSourceMatchesFull,
               "a local final transaction publishes one checkpoint-continuous chain and shares only unchanged rows");

        const note::TextEdit invalidMeasuredEdit{{9}, 0, L"Y"};
        const bool appliedInvalidMeasuredEdit =
            localTransactionCore.Apply(invalidMeasuredEdit) == note::NoteTextApplyResult::Applied;
        note::NoteRenderFinalLocalPatchInput invalidMeasurementInput = localPatchInput;
        invalidMeasurementInput.edit = invalidMeasuredEdit;
        InvalidPlacementFinalMeasurementProvider invalidLocalMeasurementProvider;
        const std::shared_ptr<const note::NoteRenderFinalPublication> preservedLocalTransaction =
            locallyPatchedTransaction;
        Expect(appliedInvalidMeasuredEdit &&
                   note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                       locallyPatchedTransaction, localTransactionCore, invalidMeasurementInput,
                       invalidLocalMeasurementProvider, &locallyPatchedTransaction) ==
                       note::NoteRenderFinalCompleteBuildResult::PlacementBuildFailed &&
                   locallyPatchedTransaction == preservedLocalTransaction,
               "a failed local final measurement cannot expose a partial replacement chain");
    }

    {
        note::NoteTextCore atomicGroupCore;
        atomicGroupCore.Reset(
            note::NoteId{950}, note::NoteMetadata{L"atomic-groups.md", L"atomic groups"},
            L"top\n"
            L"$$\n"
            L"a+b\n"
            L"$$\n"
            L"middle\n"
            L"\n"
            L"| A | B |\n"
            L"| --- | --- |\n"
            L"| one | two |\n"
            L"\n"
            L"tail",
            95, 4);
        std::shared_ptr<const note::NoteSyntaxSnapshot> atomicGroupSyntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> atomicGroupSource;
        const note::NoteRenderLayoutKey atomicGroupKey{720, 96, 96, 3, 4, 12, true};
        std::vector<note::NoteRenderLineLayout> atomicGroupLineSpecs(
            atomicGroupCore.logical_line_count(), {16, 120});
        std::vector<note::NoteRenderAtomicGroup> atomicGroupSourceGroups;
        const bool builtAtomicGroupSource =
            note::NoteSyntaxSnapshot::BuildFromTextCore(
                atomicGroupCore, note::NoteContentKind::Markdown, &atomicGroupSyntax) ==
                note::NoteSyntaxSnapshotBuildResult::Built &&
            note::NoteRenderSourcePlan::Build(atomicGroupSyntax, &atomicGroupSource) ==
                note::NoteRenderSourcePlanBuildResult::Built &&
            atomicGroupSource && atomicGroupSource->CopyAtomicGroups(&atomicGroupSourceGroups);
        if (builtAtomicGroupSource) {
            for (const note::NoteRenderAtomicGroup& group : atomicGroupSourceGroups) {
                if (group.kind != note::NoteRenderAtomicGroupKind::BlockMath ||
                    group.first_line > group.last_line ||
                    group.last_line.value >= atomicGroupLineSpecs.size()) {
                    continue;
                }
                const size_t sourceRows = group.last_line.value - group.first_line.value + 1;
                if (sourceRows > std::numeric_limits<uint32_t>::max() / 16u) {
                    atomicGroupSourceGroups.clear();
                    break;
                }
                atomicGroupLineSpecs[group.first_line.value] = {
                    static_cast<uint32_t>(sourceRows) * 16u, 120, false};
                for (size_t line = group.first_line.value + 1; line <= group.last_line.value;
                     ++line) {
                    atomicGroupLineSpecs[line] = {0, 0, true};
                    if (line == std::numeric_limits<size_t>::max()) break;
                }
            }
        }
        note::NoteRenderLineLayoutMap::Snapshot atomicGroupLineLayouts;
        std::shared_ptr<const note::NoteRenderLayoutSnapshot> atomicGroupLayout;
        Expect(builtAtomicGroupSource && !atomicGroupSourceGroups.empty() &&
                   note::NoteRenderLineLayoutMap::Build(
                       atomicGroupLineSpecs, &atomicGroupLineLayouts) &&
                   note::NoteRenderLayoutSnapshot::Build(
                       atomicGroupSyntax, atomicGroupKey, atomicGroupLineLayouts,
                       &atomicGroupLayout) == note::NoteRenderLayoutSnapshotBuildResult::Built,
               "owner-plan atomic-group test creates one current syntax, layout, and source plan");

        std::vector<note::NoteRenderLinePlacement> atomicGroupPlacements(
            atomicGroupCore.logical_line_count());
        for (size_t line = 0; line < atomicGroupSource->line_count(); ++line) {
            note::NoteRenderSourceLinePlan sourceLine;
            if (!atomicGroupSource->ResolveLine({line}, &sourceLine)) continue;
            int x = 12;
            for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                const int width = 20 + static_cast<int>(
                    run.source_span.end.value - run.source_span.start.value);
                atomicGroupPlacements[line].runs.push_back(note::NoteRenderRunPlacement{
                    run.source_span, x, width,
                    {{run.source_span.start, x}, {run.source_span.end, x + width}},
                });
                x += width + 2;
            }
        }
        std::vector<note::NoteRenderAtomicGroupPlacement> atomicGroupGeometry;
        if (atomicGroupSource) {
            for (const note::NoteRenderAtomicGroup& group : atomicGroupSourceGroups) {
                if (group.kind != note::NoteRenderAtomicGroupKind::Table &&
                    group.kind != note::NoteRenderAtomicGroupKind::BlockMath) {
                    continue;
                }
                uint64_t height = 0;
                for (size_t line = group.first_line.value; line <= group.last_line.value; ++line) {
                    const auto layoutLine = note::NoteRenderLineLayoutMap::LineAt(
                        atomicGroupLineLayouts, {line});
                    if (!layoutLine.has_value() ||
                        height > std::numeric_limits<uint32_t>::max() -
                                     layoutLine->layout.height_px) {
                        atomicGroupGeometry.clear();
                        break;
                    }
                    height += layoutLine->layout.height_px;
                    if (line == std::numeric_limits<size_t>::max()) break;
                }
                if (atomicGroupGeometry.empty() && height == 0) break;
                note::NoteRenderAtomicGroupPlacement groupPlacement{
                    group.kind, group.source_span, group.first_line, group.last_line,
                    12, 120, static_cast<uint32_t>(height)};
                if (group.kind == note::NoteRenderAtomicGroupKind::Table) {
                    size_t columnCount = 0;
                    note::NoteTableLayoutInput tableInput;
                    for (size_t line = group.first_line.value; line <= group.last_line.value;
                         ++line) {
                        note::NoteRenderSourceLinePlan sourceLine;
                        if (!atomicGroupSource->ResolveLine({line}, &sourceLine)) {
                            atomicGroupGeometry.clear();
                            break;
                        }
                        for (const note::NoteRenderSourceRun& run : sourceLine.runs) {
                            if (run.table_column == note::NoteRenderSourceRun::kNoTableColumn) {
                                continue;
                            }
                            columnCount = std::max(columnCount, run.table_column_count);
                            tableInput.cell_measures.push_back({run.table_column, 24});
                        }
                        if (line == std::numeric_limits<size_t>::max()) break;
                    }
                    tableInput.column_count = columnCount;
                    tableInput.minimum_content_width_px = 8;
                    tableInput.cell_horizontal_padding_px = 2;
                    tableInput.border_width_px = 1;
                    const note::NoteTableLayout tableLayout =
                        note::ResolveNoteTableLayout(tableInput);
                    if (!tableLayout.valid || tableLayout.total_width_px <= 0) {
                        atomicGroupGeometry.clear();
                        break;
                    }
                    groupPlacement.width_px = tableLayout.total_width_px;
                    uint32_t top = 0;
                    for (size_t line = group.first_line.value; line <= group.last_line.value;
                         ++line) {
                        const auto layoutLine = note::NoteRenderLineLayoutMap::LineAt(
                            atomicGroupLineLayouts, {line});
                        note::NoteRenderSourceLinePlan sourceLine;
                        if (!layoutLine.has_value() ||
                            !atomicGroupSource->ResolveLine({line}, &sourceLine) ||
                            top > std::numeric_limits<uint32_t>::max() -
                                      layoutLine->layout.height_px) {
                            atomicGroupGeometry.clear();
                            break;
                        }
                        groupPlacement.table_rows.push_back(
                            {{line}, top, layoutLine->layout.height_px,
                             sourceLine.decoration.has(
                                 note::NoteRenderSourceLineDecorationTableHeader),
                             sourceLine.decoration.has(
                                 note::NoteRenderSourceLineDecorationTableDivider)});
                        top += layoutLine->layout.height_px;
                        if (line == std::numeric_limits<size_t>::max()) break;
                    }
                    for (const note::NoteTableColumnLayout& column : tableLayout.columns) {
                        groupPlacement.table_columns.push_back(
                            {column.left_border_x_px, column.content_x_px,
                             column.content_width_px, column.right_border_x_px});
                    }
                    if (atomicGroupGeometry.empty() && groupPlacement.table_rows.empty()) break;
                }
                atomicGroupGeometry.push_back(std::move(groupPlacement));
            }
        }
        std::shared_ptr<const note::NoteRenderPlacementSnapshot> atomicGroupPlacement;
        Expect(note::NoteRenderPlacementSnapshot::Build(
                   atomicGroupSource, atomicGroupLayout, atomicGroupPlacements,
                   atomicGroupGeometry,
                   &atomicGroupPlacement) == note::NoteRenderPlacementSnapshotBuildResult::Built,
               "owner-plan atomic-group test creates one current placement snapshot");

        note::NotePresentationOwnerInput mathRawInput;
        mathRawInput.render_active = true;
        mathRawInput.editor_text_core_current = true;
        mathRawInput.visible_lines = {{0}, {atomicGroupCore.logical_line_count()}};
        mathRawInput.requested_editor_lines.push_back({{2}, {3}});
        mathRawInput.caret_line = 2;
        std::shared_ptr<const note::NotePresentationOwnerPlan> mathRawPlan;
        Expect(note::NotePresentationOwnerPlan::Build(
                   mathRawInput, atomicGroupCore, atomicGroupKey, atomicGroupPlacement,
                   &mathRawPlan) == note::NotePresentationOwnerPlanBuildResult::Built &&
                   mathRawPlan->frame().caret_presenter == note::NoteCaretPresenter::NativeEditor &&
                   mathRawPlan->owner_ranges().size() == 3 &&
                   mathRawPlan->owner_ranges()[0].lines.first.value == 0 &&
                   mathRawPlan->owner_ranges()[0].lines.last_exclusive.value == 1 &&
                   mathRawPlan->owner_ranges()[0].owner ==
                       note::NotePresentationLineOwner::CommittedPlacement &&
                   mathRawPlan->owner_ranges()[1].lines.first.value == 1 &&
                   mathRawPlan->owner_ranges()[1].lines.last_exclusive.value == 4 &&
                   mathRawPlan->owner_ranges()[1].owner ==
                       note::NotePresentationLineOwner::NativeEditor &&
                   mathRawPlan->owner_ranges()[2].lines.first.value == 4 &&
                   mathRawPlan->owner_ranges()[2].lines.last_exclusive.value ==
                       atomicGroupCore.logical_line_count(),
               "editing one block-math row gives its complete delimiter-to-delimiter group one native owner");

        note::NotePresentationOwnerInput tableRawInput = mathRawInput;
        tableRawInput.requested_editor_lines = {{{8}, {9}}};
        tableRawInput.caret_line = 8;
        std::shared_ptr<const note::NotePresentationOwnerPlan> tableRawPlan;
        std::vector<note::NoteRenderAtomicGroup> atomicGroups;
        const bool copiedAtomicGroups = atomicGroupSource->CopyAtomicGroups(&atomicGroups);
        const auto tableGroup = std::find_if(
            atomicGroups.begin(), atomicGroups.end(),
            [](const note::NoteRenderAtomicGroup& group) {
                return group.kind == note::NoteRenderAtomicGroupKind::Table;
            });
        Expect(copiedAtomicGroups && tableGroup != atomicGroups.end(),
               "the parsed table retains an explicit shared-geometry atomic group");
        if (copiedAtomicGroups && tableGroup != atomicGroups.end()) {
            Expect(tableGroup->first_line.value == 6,
                   "the parsed table atomic group begins at its header row");
            Expect(tableGroup->last_line.value == 8,
                   "the parsed table atomic group ends at its final body row");
        }
        note::NoteRenderSourceLinePlan tableHeaderSourceLine;
        note::NoteRenderSourceLinePlan tableBodySourceLine;
        const bool resolvedTableCellRuns = atomicGroupSource &&
            atomicGroupSource->ResolveLine({6}, &tableHeaderSourceLine) &&
            atomicGroupSource->ResolveLine({8}, &tableBodySourceLine);
        const auto headerFirstCell = resolvedTableCellRuns && !tableHeaderSourceLine.runs.empty()
            ? std::find_if(tableHeaderSourceLine.runs.begin(), tableHeaderSourceLine.runs.end(),
                           [](const note::NoteRenderSourceRun& run) {
                               return run.table_column != note::NoteRenderSourceRun::kNoTableColumn;
                           })
            : tableHeaderSourceLine.runs.end();
        const auto headerSecondCell = resolvedTableCellRuns && headerFirstCell != tableHeaderSourceLine.runs.end()
            ? std::find_if(std::next(headerFirstCell), tableHeaderSourceLine.runs.end(),
                           [](const note::NoteRenderSourceRun& run) {
                               return run.table_column != note::NoteRenderSourceRun::kNoTableColumn;
                           })
            : tableHeaderSourceLine.runs.end();
        const auto bodyFirstCell = resolvedTableCellRuns && !tableBodySourceLine.runs.empty()
            ? std::find_if(tableBodySourceLine.runs.begin(), tableBodySourceLine.runs.end(),
                           [](const note::NoteRenderSourceRun& run) {
                               return run.table_column != note::NoteRenderSourceRun::kNoTableColumn;
                           })
            : tableBodySourceLine.runs.end();
        Expect(resolvedTableCellRuns &&
                   headerFirstCell != tableHeaderSourceLine.runs.end() &&
                   headerSecondCell != tableHeaderSourceLine.runs.end() &&
                   bodyFirstCell != tableBodySourceLine.runs.end() &&
                   headerFirstCell->table_block == headerSecondCell->table_block &&
                   headerFirstCell->table_block == bodyFirstCell->table_block &&
                   headerFirstCell->table_row_block != bodyFirstCell->table_row_block &&
                   headerFirstCell->table_column_count == 2 &&
                   headerSecondCell->table_column_count == 2 &&
                   bodyFirstCell->table_column_count == 2 &&
                   headerFirstCell->table_column == 0 &&
                   headerSecondCell->table_column == 1 &&
                   bodyFirstCell->table_column == 0 &&
                   headerFirstCell->table_header && headerSecondCell->table_header &&
                   !bodyFirstCell->table_header,
               "final source runs retain parser-derived table, row, column, alignment, and header ownership for one shared grid");
        const note::NotePresentationOwnerPlanBuildResult tableRawBuild =
            note::NotePresentationOwnerPlan::Build(
                tableRawInput, atomicGroupCore, atomicGroupKey, atomicGroupPlacement,
                &tableRawPlan);
        Expect(tableRawBuild == note::NotePresentationOwnerPlanBuildResult::Built &&
                   tableRawPlan != nullptr,
               "a current placement snapshot admits a table-row native owner request");
        Expect(tableRawPlan != nullptr && tableRawPlan->owner_ranges().size() == 3 &&
                   tableRawPlan->owner_ranges()[0].lines.last_exclusive.value == 6 &&
                   tableRawPlan->owner_ranges()[1].lines.first.value == 6 &&
                   tableRawPlan->owner_ranges()[1].lines.last_exclusive.value == 9 &&
                   tableRawPlan->owner_ranges()[1].owner ==
                       note::NotePresentationLineOwner::NativeEditor &&
                   tableRawPlan->owner_ranges()[2].lines.first.value == 9 &&
                   tableRawPlan->owner_ranges()[2].lines.last_exclusive.value == 11,
               "editing one table row gives the complete shared grid one native owner without duplicate structured cells");
    }

    {
        note::NoteLineSequence sequence;
        const std::vector<note::NoteLineSequenceLineSpec> initialLines{
            {5, 1},  // alpha\n
            {4, 2},  // beta\r\n
            {3, 0},  // EOF row
        };
        Expect(sequence.Reset(initialLines) == note::NoteLineSequenceMutationResult::Applied &&
                   sequence.line_count() == 3 && sequence.text_length() == 15,
               "line sequence records aggregate canonical text length without absolute tail offsets");

        const auto first = sequence.LineAt({0});
        const auto middle = sequence.LineAt({1});
        const auto last = sequence.LineAt({2});
        Expect(first.has_value() && middle.has_value() && last.has_value() &&
                   first->start.value == 0 && first->content_end.value == 5 &&
                   first->next_start.value == 6 && middle->start.value == 6 &&
                   middle->content_end.value == 10 && middle->next_start.value == 12 &&
                   last->start.value == 12 && last->next_start.value == 15,
               "line sequence resolves line-local content and break boundaries from prefix aggregates");
        Expect(sequence.FindByOffset({5}).has_value() &&
                   sequence.FindByOffset({5})->line_index.value == 0 &&
                   sequence.FindByOffset({6}).has_value() &&
                   sequence.FindByOffset({6})->line_index.value == 1 &&
                   sequence.FindByOffset({11}).has_value() &&
                   sequence.FindByOffset({11})->line_index.value == 1 &&
                   sequence.FindByOffset({12}).has_value() &&
                   sequence.FindByOffset({12})->line_index.value == 2 &&
                   sequence.FindByOffset({15}).has_value() &&
                   sequence.FindByOffset({15})->line_index.value == 2,
               "line sequence maps content, CRLF, and EOF offsets without rewriting later rows");

        const note::NoteLineId firstId = first->line.id;
        const note::NoteLineId middleId = middle->line.id;
        const note::NoteLineId lastId = last->line.id;
        const note::NoteLineSequenceReplaceResult replaced = sequence.Replace(
            {1}, {2}, std::vector<note::NoteLineSequenceLineSpec>{{6, 1}});
        const auto replacedMiddle = sequence.LineAt({1});
        const auto preservedFirst = sequence.LineAt({0});
        const auto preservedLast = sequence.LineAt({2});
        const std::vector<note::NoteLineSequenceLocation> ordered = sequence.SnapshotLocations();
        Expect(replaced.applied() && replaced.removed_line_ids == std::vector<note::NoteLineId>{middleId} &&
                   replaced.inserted_line_ids.size() == 1 &&
                   replacedMiddle.has_value() && replacedMiddle->line.id != middleId &&
                   preservedFirst.has_value() && preservedFirst->line.id == firstId &&
                   preservedLast.has_value() && preservedLast->line.id == lastId &&
                   sequence.text_length() == 16 && preservedLast->start.value == 13,
               "line sequence splices one row while preserving unaffected stable identities and lazy tail offsets");
        Expect(ordered.size() == 3 &&
                   ordered[0].line_index.value == 0 && ordered[0].line.id == firstId &&
                   replacedMiddle.has_value() &&
                   ordered[1].line_index.value == 1 && ordered[1].line.id == replacedMiddle->line.id &&
                   ordered[2].line_index.value == 2 && ordered[2].line.id == lastId &&
                   ordered[2].start.value == 13,
               "line sequence supplies an O(n) ordered location snapshot without changing stable IDs");

        const note::NoteLineSequenceReplaceResult invalidRange = sequence.Replace(
            {3}, {4}, std::vector<note::NoteLineSequenceLineSpec>{});
        Expect(!invalidRange.applied() &&
                   invalidRange.result == note::NoteLineSequenceMutationResult::InvalidRange &&
                   sequence.line_count() == 3 && sequence.text_length() == 16 &&
                   sequence.LineAt({0})->line.id == firstId && sequence.LineAt({2})->line.id == lastId,
               "line sequence rejects an invalid splice without changing the published ordering");

        const note::NoteLineSequenceMutationResult overflow = sequence.Reset(
            std::vector<note::NoteLineSequenceLineSpec>{{std::numeric_limits<size_t>::max(), 1}});
        Expect(overflow == note::NoteLineSequenceMutationResult::LengthOverflow &&
                   sequence.line_count() == 3 && sequence.text_length() == 16,
               "line sequence rejects aggregate overflow without discarding its prior snapshot");
    }

    {
        note::NoteTextPieceSequence sequence;
        Expect(sequence.Reset(L"alpha\nbeta\ngamma") ==
                   note::NoteTextPieceSequenceMutationResult::Applied &&
                   sequence.initialized() && sequence.text_length() == 16 &&
                   sequence.Equals(L"alpha\nbeta\ngamma"),
               "piece sequence accepts an initial canonical UTF-16 text snapshot");

        const note::NoteTextPieceSequence::Snapshot before = sequence.TakeSnapshot();
        Expect(sequence.Apply(note::TextEdit{{6}, 0, L"++"}) ==
                   note::NoteTextPieceSequenceMutationResult::Applied &&
                   sequence.Equals(L"alpha\n++beta\ngamma") &&
                   sequence.CopyRange({6}, 6) == L"++beta" &&
                   sequence.SharesExactRange(before, {0}, {0}, 6) &&
                   sequence.SharesExactRange(before, {6}, {8}, 10),
               "piece sequence preserves unchanged prefix and suffix backing identity across an insertion");

        const note::NoteTextPieceSequence::Snapshot afterInsertion = sequence.TakeSnapshot();
        Expect(sequence.Apply(note::TextEdit{{6}, 2, L""}) ==
                   note::NoteTextPieceSequenceMutationResult::Applied &&
                   sequence.Equals(L"alpha\nbeta\ngamma") &&
                   sequence.SharesExactRange(afterInsertion, {8}, {6}, 10),
               "piece sequence removes a local inserted piece without rewriting the shared suffix");

        const note::NoteTextPieceSequence::Snapshot beforeInvalid = sequence.TakeSnapshot();
        Expect(sequence.Apply(note::TextEdit{{17}, 0, L"x"}) ==
                   note::NoteTextPieceSequenceMutationResult::InvalidRange &&
                   sequence.Equals(L"alpha\nbeta\ngamma") &&
                   sequence.SharesExactRange(beforeInvalid, {0}, {0}, 16),
               "piece sequence rejects invalid edits without publishing a partial text state");

        note::NoteTextPieceSequence anotherEmptySequence;
        Expect(sequence.Reset(L"") == note::NoteTextPieceSequenceMutationResult::Applied &&
                   anotherEmptySequence.Reset(L"") ==
                       note::NoteTextPieceSequenceMutationResult::Applied &&
                   !sequence.MatchesSnapshot(anotherEmptySequence.TakeSnapshot()),
               "piece sequence distinguishes separately published empty roots");
    }

    {
        note::NoteTextCore textCore;
        Expect(textCore.Apply(note::TextEdit{0, 0, L"x"}) ==
                   note::NoteTextApplyResult::InvalidOwner,
               "text core rejects edits without an owning note");

        note::NoteMetadata metadata;
        metadata.file_name = L"core.md";
        textCore.Reset(note::NoteId{77}, metadata, L"a\r\nb", 7, 3);
        Expect(textCore.valid() && textCore.note_id() == note::NoteId{77} &&
                   textCore.content_revision() == 7 &&
                   textCore.persistence_revision() == 3 &&
                   textCore.model().line_starts == std::vector<size_t>({0, 3}),
               "text core owns raw text, revision, persistence generation, and line index");
        Expect(textCore.Apply(note::TextEdit{1, 0, L"x"}) ==
                   note::NoteTextApplyResult::Applied &&
                   textCore.MatchesRaw(L"ax\r\nb") &&
                   textCore.content_revision() == 8 &&
                   textCore.model().line_starts == std::vector<size_t>({0, 4}) &&
                   textCore.BuildStorageTextCrlf() == L"ax\r\nb",
               "text core applies an edit atomically and advances its line index");

        note::NoteTextCore lineCountCore;
        lineCountCore.Reset(note::NoteId{78}, metadata, L"ab", 1, 0);
        Expect(lineCountCore.logical_line_count() == 1 &&
                   lineCountCore.model_materialization_count() == 0 &&
                   lineCountCore.Apply(note::TextEdit{1, 0, L"\n"}) ==
                       note::NoteTextApplyResult::Applied &&
                   lineCountCore.logical_line_count() == 2 &&
                   lineCountCore.model_materialization_count() == 0 &&
                   lineCountCore.Apply(note::TextEdit{1, 1, L""}) ==
                       note::NoteTextApplyResult::Applied &&
                   lineCountCore.logical_line_count() == 1 &&
                   lineCountCore.model_materialization_count() == 0,
               "text core updates logical line count locally without materializing its model");

        lineCountCore.Reset(note::NoteId{78}, metadata, L"a\rb", 2, 0);
        Expect(lineCountCore.logical_line_count() == 2 &&
                   lineCountCore.Apply(note::TextEdit{2, 0, L"\n"}) ==
                       note::NoteTextApplyResult::Applied &&
                   lineCountCore.MatchesRaw(L"a\r\nb") &&
                   lineCountCore.logical_line_count() == 2 &&
                   lineCountCore.Apply(note::TextEdit{1, 2, L""}) ==
                       note::NoteTextApplyResult::Applied &&
                   lineCountCore.MatchesRaw(L"ab") &&
                   lineCountCore.logical_line_count() == 1 &&
                   lineCountCore.model_materialization_count() == 0,
               "text core counts CRLF pairs that are formed or removed across an edit boundary");

        textCore.Reset(note::NoteId{77}, metadata, L"a\rb\nc\r\nd", 9, 4);
        Expect(textCore.BuildStorageTextCrlf() == L"a\r\nb\r\nc\r\nd",
               "text core emits one CRLF for every logical line-break representation");

        const uint64_t revisionBeforeInvalid = textCore.content_revision();
        Expect(textCore.Apply(note::TextEdit{99, 1, L"bad"}) ==
                   note::NoteTextApplyResult::InvalidRange &&
                   textCore.content_revision() == revisionBeforeInvalid &&
                   textCore.MatchesRaw(L"a\rb\nc\r\nd"),
               "text core rejects an invalid range without clamping or mutation");
        textCore.SetPersistenceRevision(4);
        Expect(textCore.persistence_revision() == 4,
               "text core advances persistence identity independently from content revision");

        textCore.Reset(note::NoteId{77}, metadata, L"z",
                       std::numeric_limits<uint64_t>::max(), 4);
        Expect(textCore.Apply(note::TextEdit{1, 0, L"x"}) ==
                   note::NoteTextApplyResult::RevisionExhausted &&
                   textCore.MatchesRaw(L"z"),
               "text core rejects mutation when its monotonic revision is exhausted");

        note::LocalNoteKernel historyKernel;
        historyKernel.Reset(note::NoteId{78}, metadata, L"", 1, 0,
                            note::NoteContentKind::PlainText);
        const note::NoteTextSelection emptySelection{{0}, {0}};
        const note::NoteTextSelection afterA{{1}, {1}};
        const note::NoteTextSelection afterB{{2}, {2}};
        (void)historyKernel.ApplyUserEdit(note::TextEdit{0, 0, L"a"}, emptySelection,
                                          afterA, note::NoteHistoryOperationKind::Typing,
                                          100, false);
        (void)historyKernel.ApplyUserEdit(note::TextEdit{1, 0, L"b"}, afterA,
                                          afterB, note::NoteHistoryOperationKind::Typing,
                                          200, false);
        Expect(historyKernel.CanUndo() && historyKernel.text_core().MatchesRaw(L"ab"),
               "kernel history owns user edits for one note id");
        const auto undo = historyKernel.Undo(false);
        Expect(undo.has_value() && historyKernel.text_core().MatchesRaw(L""),
               "merged typing undo restores text without prescribing UI selection");
        const auto redo = historyKernel.Redo(false);
        Expect(redo.has_value() && historyKernel.text_core().MatchesRaw(L"ab"),
               "kernel history redo restores text without prescribing UI selection");

        note::LocalNoteKernel nonEmptyHistoryKernel;
        nonEmptyHistoryKernel.Reset(note::NoteId{781}, metadata, L"base", 1, 0,
                                    note::NoteContentKind::PlainText);
        const note::NoteTextSelection beforeBaseInsert{{4}, {4}};
        const note::NoteTextSelection afterBaseInsert{{5}, {5}};
        (void)nonEmptyHistoryKernel.ApplyUserEdit(
            note::TextEdit{4, 0, L"+"}, beforeBaseInsert, afterBaseInsert,
            note::NoteHistoryOperationKind::Typing, 100, false);
        const auto nonEmptyUndo = nonEmptyHistoryKernel.Undo(false);
        const auto nonEmptyRedo = nonEmptyHistoryKernel.Redo(false);
        const auto nonEmptySecondUndo = nonEmptyHistoryKernel.Undo(false);
        Expect(nonEmptyUndo.has_value() && nonEmptyRedo.has_value() &&
                   nonEmptySecondUndo.has_value() &&
                   nonEmptyHistoryKernel.text_core().MatchesRaw(L"base"),
               "history refreshes immutable replay roots after undo and redo on a non-empty note");

        note::LocalNoteKernel guardedHistoryKernel;
        guardedHistoryKernel.Reset(note::NoteId{780}, metadata, L"", 1, 0,
                                   note::NoteContentKind::PlainText);
        (void)guardedHistoryKernel.ApplyUserEdit(
            note::TextEdit{0, 0, L"a"}, emptySelection, afterA,
            note::NoteHistoryOperationKind::Typing, 100, false);
        const auto guardedUndo = guardedHistoryKernel.Undo(false);
        const auto unrelatedInsert = guardedHistoryKernel.Apply(
            note::TextEdit{0, 0, L"b"}, false);
        const auto rejectedRedo = guardedHistoryKernel.Redo(false);
        Expect(guardedUndo.has_value() && unrelatedInsert.applied() &&
                   !rejectedRedo.has_value() && guardedHistoryKernel.CanRedo() &&
                   !guardedHistoryKernel.CanUndo() &&
                   guardedHistoryKernel.text_core().MatchesRaw(L"b"),
               "history replay rejects a mismatched current range without moving stacks or text");
        historyKernel.ClearHistory();
        Expect(!historyKernel.CanUndo() && !historyKernel.CanRedo() &&
                   historyKernel.text_core().MatchesRaw(L"ab"),
               "clearing kernel history preserves saved text while disabling undo and redo");

        note::LocalNoteKernel lineHistoryKernel;
        lineHistoryKernel.Reset(note::NoteId{79}, metadata, L"", 1, 0,
                               note::NoteContentKind::PlainText);
        const note::NoteTextSelection afterFirstLine{{2}, {2}};
        const note::NoteTextSelection afterBreak{{3}, {3}};
        const note::NoteTextSelection afterSecondLine{{4}, {4}};
        (void)lineHistoryKernel.ApplyUserEdit(note::TextEdit{0, 0, L"ab"}, emptySelection,
                                              afterFirstLine, note::NoteHistoryOperationKind::Typing,
                                              100, false);
        (void)lineHistoryKernel.ApplyUserEdit(note::TextEdit{2, 0, L"\n"}, afterFirstLine,
                                              afterBreak, note::NoteHistoryOperationKind::Typing,
                                              200, false);
        (void)lineHistoryKernel.ApplyUserEdit(note::TextEdit{3, 0, L"c"}, afterBreak,
                                              afterSecondLine, note::NoteHistoryOperationKind::Typing,
                                              300, false);
        const auto undoSecondLine = lineHistoryKernel.Undo(false);
        const auto undoLineBreak = lineHistoryKernel.Undo(false);
        const auto undoFirstLine = lineHistoryKernel.Undo(false);
        Expect(undoSecondLine.has_value() && undoLineBreak.has_value() && undoFirstLine.has_value() &&
                   lineHistoryKernel.text_core().MatchesRaw(L""),
               "history undo stops at a line boundary before resuming on the preceding line");

        note::NoteTextCoreRegistry textCores;
        note::NoteTextCore* firstCore = textCores.Reset(
            note::NoteId{101}, metadata, L"first", 1, 0);
        note::NoteTextCore* sameCore = textCores.Find(note::NoteId{101});
        note::NoteTextCore* secondCore = textCores.Reset(
            note::NoteId{202}, metadata, L"second", 1, 0);
        Expect(firstCore && firstCore == sameCore && secondCore &&
                   secondCore != firstCore && textCores.size() == 2,
               "text core registry shares one stable core per note id");
        const note::ViewIdentity firstView{
            note::ViewId{301}, note::NoteId{101}, 1};
        const note::ViewIdentity reboundView{
            note::ViewId{301}, note::NoteId{202}, 2};
        Expect(textCores.FindForView(firstView) == firstCore &&
                   textCores.FindForView(reboundView) == secondCore &&
                   textCores.FindForView(note::ViewIdentity{}) == nullptr,
               "text core registry resolves the model from a valid view binding");
        note::NoteTextCore* resetFirst = textCores.Reset(
            note::NoteId{101}, metadata, L"updated", 2, 1);
        Expect(resetFirst == firstCore && firstCore->MatchesRaw(L"updated") &&
                   firstCore->persistence_revision() == 1,
               "text core registry resets one note without replacing its shared instance");
        Expect(textCores.Forget(note::NoteId{202}) &&
                   !textCores.Find(note::NoteId{202}) && textCores.size() == 1,
               "text core registry forgets one note without affecting other cores");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"# Heading\n\nbody");
        Expect(parsed.second.source_identity.source_revision == parsed.first.revision &&
                   !parsed.second.source_identity.note_id.valid(),
               "parsed NoteDocument records its source revision");
        Expect(note::NoteDocumentMatchesTextModel(parsed.second, parsed.first),
               "parsed NoteDocument matches its source text model");

        note::NoteLayout layout;
        const note::NoteDerivedSnapshotIdentity layoutIdentity{
            note::NoteId{0x3301ULL}, parsed.first.revision};
        note::SetNoteLayoutSourceIdentity(&layout, layoutIdentity);
        Expect(note::NoteLayoutMatchesTextModel(layout, parsed.first) &&
                   note::NoteLayoutMatchesSourceIdentity(layout, layoutIdentity),
               "NoteLayout records one typed source identity");

        note::ApplyTextEdit(&parsed.first, note::TextEdit{0, 0, L"edited "});
        Expect(!note::NoteDocumentMatchesTextModel(parsed.second, parsed.first),
               "stale NoteDocument is rejected after a text revision change");
        Expect(!note::NoteLayoutMatchesTextModel(layout, parsed.first),
               "stale NoteLayout is rejected after a text revision change");
    }

    {
        Expect(note::NormalizeSemanticSearchTerm(L" ＡＢＣ, ﾊﾟﾋﾟ ") == L"abcパピ",
               "semantic search term normalizes width, case, separators, and voice marks");
        const note::SemanticNormalizedTextIndex normalized =
            note::BuildSemanticNormalizedTextIndex(L"Ａ B，ﾊﾟ");
        Expect(normalized.text == L"abパ",
               "semantic normalized text removes separators and normalizes width");
        Expect(normalized.source_start == std::vector<size_t>({0, 2, 4}) &&
                   normalized.source_end == std::vector<size_t>({1, 3, 6}),
                   "semantic normalized text maps combined voice marks to the full raw range");


        note::SemanticSearchOptions strictOptions;
        strictOptions.normalizeWidthKana = false;
        strictOptions.ignoreCase = false;
        strictOptions.ignoreSeparators = false;
        Expect(note::NormalizeSemanticSearchTerm(L" ＡＢＣ, ﾊﾟﾋﾟ ", strictOptions) == L" ＡＢＣ, ﾊﾟﾋﾟ ",
               "semantic search options can disable width, case, kana, and separator normalization");
        const note::NoteQueryGroups strictQuery{{
            note::NormalizeSemanticSearchTerm(L"ABC", strictOptions)}};
        Expect(!note::MatchNoteText(L"abc", strictQuery, strictOptions).matched &&
                   note::MatchNoteText(L"ABC", strictQuery, strictOptions).matched,
               "semantic search options can require case-sensitive matches");
    }

    {
        const note::NoteId owner{0x5401ULL};
        const std::wstring raw =
            L"# Head\r\n"
            L"Ａ B\r\n"
            L"<link=jump>target</>\r\n";
        const std::string bytes = TestWideToUTF8(raw);
        const note::SnapshotIdentity identity =
            note::BuildSnapshotIdentity(owner, 0, 7, bytes);
        const note::WorkspaceNoteIndexSnapshot index =
            note::BuildWorkspaceNoteIndex(
                identity,
                L"workspace\\indexed.md",
                note::NoteMetadata{L"indexed.md", L"indexed"},
                raw,
                note::NoteContentKind::Markdown);
        Expect(index.valid && note::SameSnapshotIdentity(index.snapshot_identity, identity) &&
                   index.lines.size() == 4 && index.semantic_index.valid &&
                   index.semantic_index.headings.size() == 1,
               "workspace note index binds raw lines and semantic data to one snapshot identity");

        const note::NoteQueryGroups query{{
            note::NormalizeSemanticSearchTerm(L"a b")}};
        const std::vector<note::WorkspaceNoteLineMatch> matches =
            note::SearchWorkspaceNoteIndex(index, query);
        const size_t expectedStart = raw.find(L'Ａ');
        Expect(matches.size() == 1 && matches[0].line_number == 2 &&
                   matches[0].match.firstPos == expectedStart &&
                   matches[0].match.firstLen == 3,
               "workspace note search maps normalized hits back to absolute raw offsets");

        const std::optional<size_t> anchor =
            note::FindWorkspaceLinkIdAnchor(index, L"jump");
        Expect(anchor.has_value() &&
                   !note::FindWorkspaceLinkIdAnchor(index, L"jump", anchor).has_value(),
               "workspace note index resolves link ids and excludes their source span");

        note::LocalNoteKernel currentKernel;
        currentKernel.Reset(owner, note::NoteMetadata{L"indexed.md", L"indexed"},
                            raw, 9, 7, note::NoteContentKind::Markdown);
        (void)currentKernel.RefreshDerived();
        const note::SnapshotIdentity currentIdentity =
            note::BuildSnapshotIdentity(owner, 9, 7, bytes);
        Expect(note::BuildWorkspaceNoteIndexFromKernel(
                   currentIdentity, L"workspace\\indexed.md", currentKernel).valid &&
                   !note::BuildWorkspaceNoteIndexFromKernel(
                       note::BuildSnapshotIdentity(note::NoteId{0x5402ULL}, 9, 7, bytes),
                       L"workspace\\indexed.md", currentKernel).valid,
               "workspace note index accepts only a matching active Kernel snapshot owner");

        const note::WorkspaceNoteIndexSnapshot plain =
            note::BuildWorkspaceNoteIndex(
                identity,
                L"workspace\\indexed.txt",
                note::NoteMetadata{L"indexed.txt", L"indexed"},
                L"Ａ B",
                note::NoteContentKind::PlainText);
        Expect(plain.valid && !plain.semantic_index.valid &&
                   note::SearchWorkspaceNoteIndex(plain, query).size() == 1,
               "workspace note index searches plain text without fabricating syntax semantics");
    }

    {
        note::NoteWorkspaceService workspace;
        const note::NoteId owner{0x5481ULL};
        note::LocalNoteKernel* kernel = workspace.ResetKernel(
            owner,
            note::NoteMetadata{L"service.md", L"service"},
            L"# Service\nbody",
            5,
            2,
            note::NoteContentKind::Markdown);
        Expect(kernel != nullptr &&
                   workspace.FindKernelForView(
                       note::ViewIdentity{note::ViewId{0x81ULL}, owner, 1}) == kernel,
               "workspace service owns one Kernel resolved by view identity");
        (void)kernel->RefreshDerived();
        const std::string bytes = TestWideToUTF8(kernel->text_core().model().raw);
        const note::SnapshotIdentity identity =
            note::BuildSnapshotIdentity(owner, 5, 2, bytes);
        const auto activeIndex = workspace.ResolveIndexFromKernel(
            identity, L"workspace\\service.md", *kernel);
        const auto reusedIndex = workspace.ResolveIndex(
            identity,
            L"workspace\\service.md",
            note::NoteMetadata{L"service.md", L"service"},
            kernel->text_core().model().raw,
            note::NoteContentKind::Markdown);
        Expect(activeIndex && activeIndex == reusedIndex &&
                   workspace.index_cache_size() == 1,
               "workspace service reuses one immutable index across active and resolved snapshots");

        note::MarkupExportConfig exportConfig{};
        exportConfig.format = note::ExportMarkupFormat::Html;
        const note::WorkspaceNoteExportResult exported = activeIndex
            ? note::ExportWorkspaceHtml(*activeIndex, exportConfig)
            : note::WorkspaceNoteExportResult{};
        Expect(exported.ok &&
                   note::SameSnapshotIdentity(exported.snapshot_identity, identity) &&
                   exported.bytes.find("<h1>Service</h1>") != std::string::npos,
               "workspace export preserves the resolved snapshot identity and derived document");

        note::WorkspaceNoteIndexSnapshot mismatchedExport = activeIndex
            ? *activeIndex
            : note::WorkspaceNoteIndexSnapshot{};
        ++mismatchedExport.snapshot_identity.content_fingerprint;
        Expect(!note::ExportWorkspaceHtml(
                    mismatchedExport, exportConfig).ok,
               "workspace export rejects a fingerprint that does not identify its raw text");

        (void)kernel->Apply(note::TextEdit{kernel->text_core().model().raw.size(),
                                     0, L" pending"}, true);
        const std::string pendingBytes =
            TestWideToUTF8(kernel->text_core().model().raw);
        const note::SnapshotIdentity pendingIdentity =
            note::BuildSnapshotIdentity(owner, 6, 2, pendingBytes);
        Expect(!workspace.ResolveIndexFromKernel(
                   pendingIdentity, L"workspace\\service.md", *kernel) &&
                   !note::BuildWorkspaceNoteIndexFromKernel(
                       pendingIdentity, L"workspace\\service.md", *kernel).valid,
               "workspace service rejects stale derived state while a Kernel edit is pending");
        (void)kernel->RefreshDerived();

        const std::wstring changedRaw = L"# Service\nchanged";
        const note::SnapshotIdentity changedIdentity = note::BuildSnapshotIdentity(
            owner, 6, 2, TestWideToUTF8(changedRaw));
        const auto changedIndex = workspace.ResolveIndex(
            changedIdentity,
            L"workspace\\service.md",
            note::NoteMetadata{L"service.md", L"service"},
            changedRaw,
            note::NoteContentKind::Markdown);
        Expect(changedIndex && changedIndex != activeIndex &&
                   workspace.index_cache_size() == 2,
               "workspace service never reuses an index across content revisions");

        workspace.ResetKernel(
            owner,
            note::NoteMetadata{L"service.md", L"service"},
            changedRaw,
            6,
            2,
            note::NoteContentKind::Markdown);
        Expect(workspace.index_cache_size() == 0,
               "workspace service invalidates every cached generation when a Kernel resets");

        for (uint64_t value = 1; value <= 130; ++value) {
            const note::NoteId cacheOwner{0x6000ULL + value};
            const std::string rawBytes = "cache" + std::to_string(value);
            workspace.ResolveIndex(
                note::BuildSnapshotIdentity(cacheOwner, 1, 0, rawBytes),
                L"cache\\" + std::to_wstring(value) + L".txt",
                note::NoteMetadata{},
                L"cache" + std::to_wstring(value),
                note::NoteContentKind::PlainText);
        }
        Expect(workspace.index_cache_size() == 128,
               "workspace service bounds immutable index retention");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(
            L"# First\r\n"
            L"\r\n"
            L"### **Second** [jump](target)\r\n"
            L"\r\n"
            L"<h2>Legacy Head</>\r\n"
            L"\r\n"
             L"<link=internal>local</> and $x+1$\r\n");
        const note::NoteId semanticOwner{0x5501ULL};
        const note::SemanticIndexSnapshot unboundIndex =
            note::BuildSemanticIndexSnapshot(semanticOwner, parsed.first, parsed.second);
        Expect(!unboundIndex.valid,
               "semantic index rejects a syntax snapshot without a bound owner");
        note::SetNoteDocumentSourceIdentity(
            &parsed.second,
            note::NoteDerivedSnapshotIdentity{semanticOwner, parsed.first.revision});
        const note::SemanticIndexSnapshot index =
            note::BuildSemanticIndexSnapshot(semanticOwner, parsed.first, parsed.second);
        Expect(index.valid &&
                   index.source_identity == note::NoteDerivedSnapshotIdentity{
                       semanticOwner, parsed.first.revision},
               "semantic index is valid for a matching owner and source revision");
        Expect(note::SemanticIndexMatchesTextModel(index, semanticOwner, parsed.first),
               "semantic index records its owner and source revision");
        Expect(!note::SemanticIndexMatchesTextModel(
                   index, note::NoteId{0x5502ULL}, parsed.first),
               "semantic index rejects another note with the same revision");
        Expect(!index.normalized_text.text.empty(),
               "semantic index contains normalized searchable text");
        Expect(index.headings.size() == 3,
               "semantic index contains Markdown and legacy headings");
        if (index.headings.size() == 3) {
            Expect(index.headings[0].level == 1 && index.headings[0].text == L"First",
                   "semantic index extracts first Markdown heading");
            Expect(index.headings[1].level == 3 && index.headings[1].text == L"Second jump",
                   "semantic index flattens styled Markdown heading text");
            Expect(index.headings[2].level == 2 && index.headings[2].text == L"Legacy Head",
                   "semantic index extracts legacy heading content");
            Expect(index.headings[0].line_span.end.value < parsed.first.raw.size() &&
                       parsed.first.raw[index.headings[0].line_span.end.value] == L'\r',
                   "semantic heading line span excludes CRLF");
        }

        bool foundMarkdownLink = false;
        bool foundLinkId = false;
        for (const auto& link : index.links) {
            if (link.kind == note::SemanticLinkKind::MarkdownTarget &&
                link.target == L"target") {
                foundMarkdownLink = true;
            }
            if (link.kind == note::SemanticLinkKind::LinkId &&
                link.target == L"internal" && link.text == L"local") {
                foundLinkId = true;
            }
        }
        Expect(foundMarkdownLink, "semantic index contains Markdown link targets");
        Expect(foundLinkId, "semantic index contains internal link ids");
        Expect(index.math.size() == 1 && index.math[0].normalized_tex == L"x+1",
               "semantic index contains normalized math spans");

        note::NoteTextModel newer = parsed.first;
        ++newer.revision;
        const note::SemanticIndexSnapshot stale =
            note::BuildSemanticIndexSnapshot(semanticOwner, newer, parsed.second);
        Expect(!stale.valid &&
                   !note::SemanticIndexMatchesTextModel(index, semanticOwner, newer),
               "semantic index rejects a stale NoteDocument revision");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$hit$");
        Expect(doc.math_spans.size() == 1, "visible inline math is still extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].normalized_tex == L"hit",
                   "visible inline math keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$x$");
        Expect(doc.math_spans.size() == 1, "$...$ inline math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Inline,
                   "$...$ is classified as inline math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::Dollar,
                   "$...$ keeps the dollar delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"x",
                   "$...$ keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$$x + y$$");
        Expect(doc.math_spans.size() == 1, "$$...$$ block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "$$...$$ is classified as block math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::DoubleDollar,
                   "$$...$$ keeps the double-dollar delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"x + y",
                   "$$...$$ keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"$$\n"
            L"a^2 + b^2 = c^2\n"
            L"$$");
        Expect(doc.math_spans.size() == 1, "multiline $$...$$ block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "multiline $$...$$ is classified as block math");
            Expect(doc.math_spans[0].normalized_tex == L"\na^2 + b^2 = c^2\n",
                   "multiline $$...$$ keeps its full normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"\\(a + b\\)");
        Expect(doc.math_spans.size() == 1, "\\(...\\) inline math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Inline,
                   "\\(...\\) is classified as inline math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::BackslashParen,
                   "\\(...\\) keeps the backslash-paren delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"a + b",
                   "\\(...\\) keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"\\[c + d\\]");
        Expect(doc.math_spans.size() == 1, "\\[...\\] block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "\\[...\\] is classified as block math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::BackslashBracket,
                   "\\[...\\] keeps the backslash-bracket delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"c + d",
                   "\\[...\\] keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"\\[\n"
            L"\\frac{1}{1 + x}\n"
            L"= 1 - x + x^2\n"
            L"\\]");
        Expect(doc.math_spans.size() == 1, "multiline \\[...\\] block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "multiline \\[...\\] is classified as block math");
            Expect(doc.math_spans[0].normalized_tex == L"\n\\frac{1}{1 + x}\n= 1 - x + x^2\n",
                   "multiline \\[...\\] keeps its full normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"\\[\n"
            L"\\begin{pmatrix}\n"
            L"1 & 2 \\\\\n"
            L"3 & 4\n"
            L"\\end{pmatrix}\n"
            L"\\]");
        Expect(FindDiagnostic(doc, L"NOTE-W-MATH-UNSUPPORTED") == nullptr,
               "supported display math environment does not emit unsupported warning");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"\\[\n"
            L"\\begin{array}{cc} 1 & 2 \\end{array}\n"
            L"\\]");
        const note::Diagnostic* diag = FindDiagnostic(doc, L"NOTE-W-MATH-UNSUPPORTED");
        Expect(diag != nullptr, "unsupported math environment emits warning");
        if (diag) {
            Expect(diag->severity == note::DiagnosticSeverity::Warning,
                   "unsupported math environment diagnostic is a warning");
        }
        Expect(doc.math_spans.size() == 1 && doc.math_spans[0].diagnostic_ids.size() == 1,
               "unsupported math environment warning is attached to its math span");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$\\overbrace{x}$");
        const note::Diagnostic* diag = FindDiagnostic(doc, L"NOTE-W-MATH-UNSUPPORTED");
        Expect(diag != nullptr, "unsupported math command emits warning");
        if (diag) {
            Expect(diag->severity == note::DiagnosticSeverity::Warning,
                   "unsupported math command diagnostic is a warning");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"# Math UI Fixture\n\n"
            L"Inline dollar $x$\n"
            L"Inline paren \\(a + b\\)\n\n"
            L"$$z$$\n\n"
            L"\\[w + 1\\]\n");
        Expect(doc.math_spans.size() == 4,
               "mixed math fixture extracts every supported delimiter");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(
            L"# Math UI Fixture\r\r"
            L"Inline dollar $x$\r"
            L"Inline paren \\(a + b\\)\r\r"
            L"$$z$$\r\r"
            L"\\[w + 1\\]\r");
        Expect(parsed.first.line_starts.size() == 9,
               "RichEdit CR lines are indexed as logical lines");
        Expect(parsed.second.math_spans.size() == 4,
               "mixed math fixture with RichEdit CR lines extracts every delimiter");
    }

    {
        Expect(ApplyTextEditKeepsLineStartsInSync(
                   L"alpha\r\nbeta\r\ngamma",
                   note::TextEdit{2, 0, L"XYZ"}),
               "incremental note line starts stay in sync for same-line insertion");
        Expect(ApplyTextEditKeepsLineStartsInSync(
                   L"alpha\r\nbeta\r\ngamma",
                   note::TextEdit{7, 4, L"one\r\ntwo\r\nthree"}),
               "incremental note line starts stay in sync for multiline paste");
        Expect(ApplyTextEditKeepsLineStartsInSync(
                   L"alpha\r\nbeta\r\ngamma",
                   note::TextEdit{5, 2, L""}),
               "incremental note line starts stay in sync when deleting a CRLF boundary");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"`$skip$`");
        Expect(doc.math_spans.empty(), "math extraction skips inline code");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"```\r\n$$omit$$\r\n\\(omit\\)\r\n```");
        Expect(doc.math_spans.empty(), "math extraction skips fenced code");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Broken \\(x + y");
        Expect(FindDiagnostic(doc, L"NOTE-E-MATH-UNCLOSED-PAREN") != nullptr,
               "unclosed \\( ... \\) math emits a diagnostic");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Before\n$$\nx + y");
        Expect(doc.math_spans.empty(), "unclosed $$ block stays normal text");
        Expect(FindDiagnostic(doc, L"NOTE-E-MATH-UNCLOSED-DOUBLE-DOLLAR") == nullptr,
               "unclosed $$ block does not emit a diagnostic");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Before\n\\[\nx + y");
        Expect(doc.math_spans.empty(), "unclosed \\[ block stays normal text");
        Expect(FindDiagnostic(doc, L"NOTE-E-MATH-UNCLOSED-BRACKET") == nullptr,
               "unclosed \\[ block does not emit a diagnostic");
    }

    {
        const auto parsed = BuildMd4cModelAndDoc(L"前 <math>x^2 + y</math> 後");
        const auto& doc = parsed.second;
        Expect(doc.math_spans.size() == 1 && doc.math_spans[0].kind == note::MathKind::Inline &&
                   doc.math_spans[0].delimiter == note::MathDelimiter::MathTag &&
                   doc.math_spans[0].normalized_tex == L"x^2 + y" &&
                   doc.math_spans[0].span.start.value == 2 &&
                   doc.math_spans[0].span.end.value == parsed.first.raw.find(L" 後"),
               "named math tags produce one inline TeX span with canonical UTF-16 ownership");
        note::TextExportConfig config;
        config.mathMode = note::ExportTextMathMode::Raw;
        Expect(note::ExportPlainText(parsed.first, doc, config).find("<math>x^2 + y</math>") != std::string::npos,
               "TXT raw-math export preserves named wrapper source and body");
        config.mathMode = note::ExportTextMathMode::Simplified;
        Expect(note::ExportPlainText(parsed.first, doc, config).find("x^2 + y") != std::string::npos,
               "TXT simplified math keeps the TeX body instead of dropping it");
    }
    {
        for (const std::wstring newline : {L"\n", L"\r", L"\r\n"}) {
            const auto doc = ParseMd4c(L"before" + newline + L"<MATH>" + newline +
                                      L"\\frac{1}{2}" + newline + L"</MaTh>" + newline + L"after");
            Expect(doc.math_spans.size() == 1 && doc.math_spans[0].kind == note::MathKind::Block &&
                       doc.math_spans[0].normalized_tex == L"\n\\frac{1}{2}\n",
                   "standalone named math tags share block math with LF, RichEdit CR and CRLF");
        }
        const auto standalone = ParseMd4c(L"  <math>x</math>  ");
        Expect(standalone.math_spans.size() == 1 && standalone.math_spans[0].kind == note::MathKind::Block,
               "a standalone single-row math wrapper is display math");
        for (const std::wstring source : {
                L"<math>x", L"<math>outer <math>x</math></math>",
                L"text <math>\nx\n</math> tail", L"<math>x\n```\ny\n```\n</math>"}) {
            const auto doc = ParseMd4c(source);
            Expect(doc.math_spans.empty() && FindDiagnostic(doc, L"NOTE-E-MATH-TAG"),
                   "unclosed, nested, mixed multiline and code-crossing wrappers retain source without math ownership");
        }
        for (const std::wstring source : {
                L"`<math>x</math>`", L"```\n<math>x</math>\n```", L"\\<math>x</math>",
                L"<math/>x</math>"}) {
            Expect(ParseMd4c(source).math_spans.empty(),
                   "math tags in code, escaped tags and self-closing tags are not interpreted");
        }
        const std::wstring styledSource = L"<b>before <math>x < y</math> after</b>";
        const auto doc = ParseMd4c(styledSource);
        const auto* bold = FindStyleSpan(doc, note::StyleKind::Bold);
        Expect(doc.math_spans.size() == 1 && bold && bold->span.end.value == styledSource.rfind(L"</b>"),
               "a named math closer does not pop an enclosing markup style");
        const auto container = ParseMd4c(L"::: note\n<math>\nx\n::: literal\n</math>\n:::\n");
        Expect(container.math_spans.size() == 1 &&
                   std::count_if(container.blocks.begin(), container.blocks.end(), [](const auto& block) {
                       return block.kind == note::BlockKind::FencedContainer;
                   }) == 1,
               "completed named block math protects its body from container fence interpretation");
        const auto texModel = note::MakeNoteTextModel({L"math.tex", L""}, L"<math>x</math> $y$", 1);
        const auto texDoc = note::ParseTeXMathDocument(texModel);
        Expect(texDoc.math_spans.size() == 1 && texDoc.math_spans[0].delimiter == note::MathDelimiter::Dollar,
               "TeX source continues to interpret TeX delimiters only, not note-specific wrappers");
        const auto unsupported = ParseMd4c(L"text <math>\\overbrace{x}</math>");
        Expect(unsupported.math_spans.size() == 1 && FindDiagnostic(unsupported, L"NOTE-W-MATH-UNSUPPORTED"),
               "tag math retains the existing unsupported-TeX diagnostic policy");
        std::wstring repeated;
        for (size_t index = 0; index < 4096; ++index) repeated += L"<math>";
        const auto malformed = ParseMd4c(repeated);
        Expect(malformed.math_spans.empty() && malformed.diagnostics.size() == 1,
               "many unclosed math openings produce one bounded rejection without suffix rescanning");
    }

    {
        for (const std::wstring closing : {L"</math>", L"</>"}) {
            for (const std::wstring attr : {L" display=inline", L" display = 'INLINE'", L" DISPLAY=\"inline\""}) {
                const auto doc = ParseMd4c(L"<math" + attr + L">x^2" + closing);
                Expect(doc.math_spans.size() == 1 && doc.math_spans[0].kind == note::MathKind::Inline &&
                           doc.math_spans[0].normalized_tex == L"x^2" &&
                           doc.math_spans[0].delimiter == (closing == L"</>" ?
                               note::MathDelimiter::LegacyMathTag : note::MathDelimiter::MathTag),
                       "display attributes override standalone inference with either math closing form");
            }
            const std::wstring raw = L"<u>before <math display=inline future='a>b'>x" + closing + L" after</>";
            const auto parsed = BuildMd4cModelAndDoc(raw);
            const auto& doc = parsed.second;
            const auto* underline = FindStyleSpan(doc, note::StyleKind::Underline);
            note::MarkupExportConfig config;
            Expect(doc.math_spans.size() == 1 && underline && underline->span.end.value == raw.rfind(L"</>") &&
                       FindDiagnostic(doc, L"NOTE-W-MATH-TAG-ATTRIBUTE") &&
                       note::ExportMarkdown(parsed.first, doc, config).find("future='a>b'") != std::string::npos,
                   "math owns its closer without closing outer underline and preserves unknown quoted attributes in Markdown");
        }
        for (const std::wstring raw : {L"text <math display=block>x</> tail",
                L"<math display=inline>\nx\n</>", L"<math display=other>x</>",
                L"<math display=block display=inline>x</>", L"<math display>x</>",
                L"<math display='inline'x=y>x</>", L"<math display='inline>x</>",
                L"<math>outer <math display=inline>x</></>"}) {
            const auto doc = ParseMd4c(raw);
            Expect(doc.math_spans.empty() && FindDiagnostic(doc, L"NOTE-E-MATH-TAG"),
                   "invalid display values, duplicate or malformed attributes, unsafe placement and nesting retain raw source");
        }
        const auto block = ParseMd4c(L"<math display=block>\nx\n</>");
        Expect(block.math_spans.size() == 1 && block.math_spans[0].kind == note::MathKind::Block,
               "explicit block display supports multiline math and universal closing");
        const auto escaped = ParseMd4c(L"text <math display=inline>x \\</> y</>");
        Expect(escaped.math_spans.size() == 1 && escaped.math_spans[0].normalized_tex == L"x \\</> y",
               "escaped universal closers remain literal inside TeX math");
        const auto analysis = note::AnalyzeMathBoxInput(L"<math display=block>x</>");
        Expect(analysis.kind == note::MathKind::Block && analysis.content_text == L"x" && analysis.diagnostics.empty(),
               "MathBox shares the attribute lexer and explicit display mode");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"<link=dest><lu>jump</></>");
        Expect(FindStyleSpan(doc, note::StyleKind::LinkId, L"dest") != nullptr,
               "markdown route keeps legacy link-id style spans");
        Expect(FindStyleSpan(doc, note::StyleKind::LinkUnderline) != nullptr,
               "markdown route keeps legacy link underline style spans");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"`<link=dest>jump</>`");
        Expect(FindStyleSpan(doc, note::StyleKind::LinkId) == nullptr,
               "legacy link-id markup is ignored inside inline code");
    }

    {
        const std::wstring text =
            L"# Top\n"
            L"<u>top\n"
            L"## Mid\n"
            L"<char=#ff0000>mid\n"
            L"### Deep\n"
            L"<back=#00ff00>deep\n"
            L"</##>\n"
            L"after\n";
        const note::NoteDocument doc = ParseMd4c(text);
        const size_t closePos = text.find(L"</##>");
        const size_t afterPos = text.find(L"after");
        const note::StyleSpan* underline = FindStyleSpan(doc, note::StyleKind::Underline);
        const note::StyleSpan* textColor = FindStyleSpan(doc, note::StyleKind::TextColor, L"#ff0000");
        const note::StyleSpan* backColor = FindStyleSpan(doc, note::StyleKind::BackgroundColor, L"#00ff00");
        Expect(underline != nullptr && underline->span.end.value > afterPos,
               "hash heading close keeps higher-level tag effects active");
        Expect(textColor != nullptr && textColor->span.end.value == closePos,
               "</##> closes same-level tag effects");
        Expect(backColor != nullptr && backColor->span.end.value == closePos,
               "</##> closes deeper-level tag effects");
    }
    {
        const note::NoteDocument doc = ParseMd4c(L"<h2>Legacy Heading</>\n\nParagraph\n");
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading, 0);
        Expect(heading != nullptr, "standalone legacy heading tag is promoted to heading block");
        if (heading != nullptr) {
            Expect(heading->origin == note::BlockOrigin::LegacyHeadingTag,
                   "promoted legacy heading keeps legacy origin");
            Expect(heading->level == 2, "promoted legacy heading keeps level");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Paragraph <h2>Inline</> tail\n");
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading, 0);
        Expect(heading == nullptr, "inline legacy heading tag is not promoted to heading block");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"# Head\n\n"
            L"3. one\n"
            L"4. two\n\n"
            L"```cpp\n"
            L"code();\n"
            L"```\n");
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading);
        const note::BlockNode* list = FindBlock(doc, note::BlockKind::List);
        const note::BlockNode* firstItem = FindBlock(doc, note::BlockKind::ListItem, 0);
        const note::BlockNode* secondItem = FindBlock(doc, note::BlockKind::ListItem, 1);
        const note::BlockNode* codeBlock = FindBlock(doc, note::BlockKind::CodeBlock);
        const size_t listIndex = (list != nullptr)
            ? static_cast<size_t>(list - doc.blocks.data())
            : static_cast<size_t>(-1);
        Expect(heading != nullptr && heading->level == 1,
               "md4c adapter keeps heading blocks with level");
        Expect(list != nullptr && list->ordered && list->start_number == 3,
               "md4c adapter keeps ordered-list metadata");
        Expect(firstItem != nullptr && secondItem != nullptr &&
                   listIndex != static_cast<size_t>(-1) &&
                   firstItem->parent == listIndex &&
                   secondItem->parent == listIndex,
               "md4c adapter keeps list-item parent links");
        Expect(codeBlock != nullptr && codeBlock->info_string == L"cpp",
               "md4c adapter keeps fenced code info strings");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"<char=1a73e8>blue</> <back=fff2cc>highlight</>");
        Expect(FindStyleSpan(doc, note::StyleKind::TextColor, L"#1a73e8") != nullptr,
               "bare six-digit text color receives the CSS hash prefix");
        Expect(FindStyleSpan(doc, note::StyleKind::BackgroundColor, L"#fff2cc") != nullptr,
               "bare six-digit background color receives the CSS hash prefix");
    }

    {
        const note::NoteDocument doc = ParseTeXSource(
            L"# TeX source stays literal\n"
            L"$x$ % $ignored$\n"
            L"\\(y + 1\\)\n");
        Expect(doc.blocks.size() == 1 && doc.inlines.size() == 1 &&
                   doc.style_spans.empty(),
               "TeX source parser does not create Markdown structure or styles");
        Expect(doc.math_spans.size() == 2,
               "TeX source parser extracts completed TeX math but ignores comments");
        if (doc.math_spans.size() == 2) {
            Expect(doc.math_spans[0].normalized_tex == L"x" &&
                       doc.math_spans[1].normalized_tex == L"y + 1",
                   "TeX source parser preserves math bodies without Markdown rewriting");
        }
    }

    {
        const note::NoteDocument doc = ParseTeXSource(
            L"$$\n"
            L"\\sum_{k=1}^{n} k = \\frac{n(n+1)}{2}\n"
            L"$$\n");
        Expect(doc.math_spans.size() == 1 && doc.math_spans[0].diagnostic_ids.empty(),
               "TeX source sample formula is a supported display-math span");
        if (doc.math_spans.size() == 1) {
            const auto node = mathrender::Parse(doc.math_spans[0].normalized_tex);
            Expect(node != nullptr,
                   "TeX source sample formula is accepted by the shared math renderer");
        }
    }

    {
        // The note editor uses two spaces to nest a '-' list item.  Four
        // leading spaces would instead be interpreted as a code block.
        const note::NoteDocument doc = ParseMd4c(
            L"- parent\n"
            L"  - child\n");
        const note::BlockNode* parentItem = FindBlock(doc, note::BlockKind::ListItem, 0);
        const note::BlockNode* childItem = FindBlock(doc, note::BlockKind::ListItem, 1);
        Expect(parentItem != nullptr && childItem != nullptr &&
                   FindBlock(doc, note::BlockKind::CodeBlock) == nullptr,
               "two-space unordered-list indent remains Markdown list content");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"Before\n\n"
            L"---\n\n"
            L"After\n");
        const note::BlockNode* rule = FindBlock(doc, note::BlockKind::HorizontalRule);
        Expect(rule != nullptr, "md4c adapter keeps thematic breaks as horizontal-rule blocks");
        Expect(FindDiagnostic(doc, L"NOTE-W-MD4C-BLOCK") == nullptr,
               "thematic breaks do not emit unsupported-block diagnostics");
    }

    {
        const std::wstring source = L"Before\n\n---\n\nAfter\n";
        const note::NoteDocument doc = ParseMd4c(source);
        const note::BlockNode* rule = FindBlock(doc, note::BlockKind::HorizontalRule);
        const size_t ruleStart = source.find(L"---");
        Expect(rule != nullptr && ruleStart != std::wstring::npos &&
                   rule->span.start.value == ruleStart &&
                   rule->span.end.value == ruleStart + 4,
               "thematic-break span is limited to its source line");
    }

    {
        const std::wstring source = L"Text before the rule\n---\nText after the rule\n";
        const note::NoteDocument doc = ParseMd4c(source);
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading);
        const note::BlockNode* rule = FindBlock(doc, note::BlockKind::HorizontalRule);
        const size_t ruleStart = source.find(L"---");
        Expect(heading == nullptr,
               "dash thematic break does not promote the preceding text to a heading");
        Expect(rule != nullptr && ruleStart != std::wstring::npos &&
                   rule->span.start.value == ruleStart && rule->span.end.value == ruleStart + 4,
               "dash thematic break after text remains a horizontal-rule block");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"Text before a pending item\n"
            L"- \n");
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading);
        const note::BlockNode* paragraph = FindBlock(doc, note::BlockKind::Paragraph);
        Expect(heading == nullptr,
               "empty unordered-list marker does not promote the preceding text to a heading");
        Expect(paragraph != nullptr,
               "empty unordered-list marker keeps the preceding text as a paragraph");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"| A | B |\n"
            L"|:--|--:|\n"
            L"| 1 | 2 |\n");
        const note::BlockNode* table = FindBlock(doc, note::BlockKind::Table);
        const note::BlockNode* headCell = FindBlock(doc, note::BlockKind::TableHeaderCell);
        const note::BlockNode* bodyCell = FindBlock(doc, note::BlockKind::TableCell);
        size_t tableIndex = std::numeric_limits<size_t>::max();
        size_t headCellIndex = std::numeric_limits<size_t>::max();
        size_t bodyCellIndex = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < doc.blocks.size(); ++index) {
            if (doc.blocks[index].kind == note::BlockKind::Table && tableIndex == std::numeric_limits<size_t>::max()) {
                tableIndex = index;
            } else if (doc.blocks[index].kind == note::BlockKind::TableHeaderCell &&
                       headCellIndex == std::numeric_limits<size_t>::max()) {
                headCellIndex = index;
            } else if (doc.blocks[index].kind == note::BlockKind::TableCell &&
                       bodyCellIndex == std::numeric_limits<size_t>::max()) {
                bodyCellIndex = index;
            }
        }
        Expect(table != nullptr && table->table_column_count == 2,
               "md4c adapter keeps table blocks with column count");
        Expect(headCell != nullptr && headCell->table_cell_align == note::TableCellAlign::Left,
               "md4c adapter keeps table header cell alignment");
        Expect(bodyCell != nullptr,
               "md4c adapter keeps table body cells");
        const bool headerHierarchy =
            headCellIndex < doc.blocks.size() &&
            doc.blocks[headCellIndex].parent < doc.blocks.size() &&
            doc.blocks[doc.blocks[headCellIndex].parent].kind == note::BlockKind::TableRow &&
            doc.blocks[doc.blocks[headCellIndex].parent].parent < doc.blocks.size() &&
            doc.blocks[doc.blocks[doc.blocks[headCellIndex].parent].parent].kind == note::BlockKind::TableHead &&
            doc.blocks[doc.blocks[doc.blocks[headCellIndex].parent].parent].parent == tableIndex;
        const bool bodyHierarchy =
            bodyCellIndex < doc.blocks.size() &&
            doc.blocks[bodyCellIndex].parent < doc.blocks.size() &&
            doc.blocks[doc.blocks[bodyCellIndex].parent].kind == note::BlockKind::TableRow &&
            doc.blocks[doc.blocks[bodyCellIndex].parent].parent < doc.blocks.size() &&
            doc.blocks[doc.blocks[doc.blocks[bodyCellIndex].parent].parent].kind == note::BlockKind::TableBody &&
            doc.blocks[doc.blocks[doc.blocks[bodyCellIndex].parent].parent].parent == tableIndex;
        Expect(headerHierarchy && bodyHierarchy,
               "md4c adapter preserves table cell-to-row-to-section hierarchy for structured rendering");
        bool headerInlineHasCellParent = false;
        for (const note::InlineNode& inlineNode : doc.inlines) {
            if (inlineNode.parent_block == headCellIndex) {
                headerInlineHasCellParent = true;
                break;
            }
        }
        Expect(headerInlineHasCellParent,
               "md4c adapter assigns table header text to its cell parent");
    }

    {
        const std::wstring source =
            L"| `code` | 用途 | `value` |\n"
            L"|:--|:--|:--|\n"
            L"| `name` | インラインコード | `sample` |\n";
        const note::NoteDocument doc = ParseMd4c(source);
        const note::InlineNode* headerCode = FindInline(doc, note::InlineKind::Code, 0);
        const note::InlineNode* bodyCode = FindInline(doc, note::InlineKind::Code, 2);
        auto hasTableCellParent = [&doc](const note::InlineNode* inlineNode) {
            if (!inlineNode || inlineNode->parent_block >= doc.blocks.size()) return false;
            const note::BlockKind kind = doc.blocks[inlineNode->parent_block].kind;
            return kind == note::BlockKind::TableHeaderCell || kind == note::BlockKind::TableCell;
        };
        const bool headerLiteral = headerCode && headerCode->span.end.value <= source.size() &&
            source.substr(headerCode->span.start.value,
                          headerCode->span.end.value - headerCode->span.start.value) == L"code";
        const bool bodyLiteral = bodyCode && bodyCode->span.end.value <= source.size() &&
            source.substr(bodyCode->span.start.value,
                          bodyCode->span.end.value - bodyCode->span.start.value) == L"name";
        Expect(hasTableCellParent(headerCode) && hasTableCellParent(bodyCode) &&
                   headerLiteral && bodyLiteral,
               "inline code in table cells retains its literal content and cell ownership");
    }

    {
        const std::wstring source =
            L"| ` | 用途 | `value` |\n"
            L"|:--|:--|:--|\n"
            L"| raw | インラインコード | sample |\n";
        const note::NoteDocument doc = ParseMd4c(source);
        bool loneBacktickHasTableCellText = false;
        for (const note::InlineNode& inlineNode : doc.inlines) {
            if (inlineNode.kind != note::InlineKind::Text ||
                inlineNode.parent_block >= doc.blocks.size() ||
                inlineNode.span.end.value > source.size()) {
                continue;
            }
            const note::BlockKind kind = doc.blocks[inlineNode.parent_block].kind;
            if ((kind == note::BlockKind::TableHeaderCell || kind == note::BlockKind::TableCell) &&
                source.substr(inlineNode.span.start.value,
                              inlineNode.span.end.value - inlineNode.span.start.value) == L"`") {
                loneBacktickHasTableCellText = true;
                break;
            }
        }
        const note::InlineNode* pairedValue = FindInline(doc, note::InlineKind::Code, 0);
        const bool pairedValueRemainsCode = pairedValue &&
            pairedValue->parent_block < doc.blocks.size() &&
            pairedValue->span.end.value <= source.size() &&
            source.substr(pairedValue->span.start.value,
                          pairedValue->span.end.value - pairedValue->span.start.value) == L"value" &&
            doc.blocks[pairedValue->parent_block].kind == note::BlockKind::TableHeaderCell;
        Expect(loneBacktickHasTableCellText && pairedValueRemainsCode,
               "an unmatched table-cell backtick stays literal without disabling paired inline code");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"- [x] done\n- [ ] todo\n");
        const note::BlockNode* firstItem = FindBlock(doc, note::BlockKind::ListItem, 0);
        const note::BlockNode* secondItem = FindBlock(doc, note::BlockKind::ListItem, 1);
        Expect(firstItem != nullptr && firstItem->task_item && firstItem->task_checked,
               "md4c adapter keeps checked task-list item metadata");
        Expect(secondItem != nullptr && secondItem->task_item && !secondItem->task_checked,
               "md4c adapter keeps unchecked task-list item metadata");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"Paragraph with *em* **strong** ~~strike~~ [jump](dest), ![alt](image.png), and `code`.\n");
        const note::InlineNode* em = FindInline(doc, note::InlineKind::Emphasis);
        const note::InlineNode* strong = FindInline(doc, note::InlineKind::Strong);
        const note::InlineNode* strike = FindInline(doc, note::InlineKind::Strike);
        const note::InlineNode* link = FindInline(doc, note::InlineKind::Link);
        const note::InlineNode* image = FindInline(doc, note::InlineKind::Image);
        const note::InlineNode* code = FindInline(doc, note::InlineKind::Code);
        Expect(em != nullptr, "md4c adapter keeps emphasis overlays");
        Expect(strong != nullptr, "md4c adapter keeps strong overlays");
        Expect(strike != nullptr, "md4c adapter keeps strike overlays");
        Expect(link != nullptr && link->target == L"dest",
               "md4c adapter keeps markdown link targets");
        Expect(image != nullptr && image->target == L"image.png",
               "md4c adapter keeps markdown image targets");
        Expect(code != nullptr, "md4c adapter keeps inline code overlays");
    }

    {
        const note::NoteDocument afterIdeographicSpace = ParseMd4c(L"前　*空白後*　後\n");
        const note::NoteDocument besideJapanesePunctuation = ParseMd4c(L"前、*強調*。後\n");
        Expect(FindInline(afterIdeographicSpace, note::InlineKind::Emphasis) != nullptr,
               "MD4C local patch treats ideographic space as Markdown whitespace");
        Expect(FindInline(besideJapanesePunctuation, note::InlineKind::Emphasis) != nullptr,
               "MD4C local patch keeps Japanese punctuation flanking behavior");
    }

    {
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"  $$x + 1$$  ");
        Expect(analysis.has_input, "MathBox analysis accepts non-empty input");
        Expect(analysis.has_wrapping, "MathBox analysis recognizes wrapped latex");
        Expect(analysis.flavor == note::MathInputFlavor::Latex,
               "MathBox analysis classifies $$...$$ as latex");
        Expect(analysis.kind == note::MathKind::Block,
               "MathBox analysis classifies $$...$$ as block math");
        Expect(analysis.delimiter == note::MathDelimiter::DoubleDollar,
               "MathBox analysis keeps the double-dollar delimiter");
        Expect(analysis.content_text == L"x + 1",
               "MathBox analysis strips $$ wrappers");
        Expect(analysis.diagnostics.empty(),
               "valid wrapped latex has no diagnostics");
    }

    {
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"\\[x + 1");
        Expect(HasError(analysis, L"NOTE-E-MATHBOX-UNCLOSED-BRACKET"),
               "MathBox analysis reports unclosed \\[ ... \\]");
    }

    {
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"<math>a + b</>");
        Expect(analysis.has_wrapping, "MathBox analysis recognizes <math> wrapper");
        Expect(analysis.flavor == note::MathInputFlavor::Markup,
               "MathBox analysis classifies <math> wrapper as markup");
        Expect(analysis.content_text == L"a + b",
               "MathBox analysis strips the legacy wrapper body");
        Expect(analysis.diagnostics.empty(),
               "closed markup wrapper has no diagnostics");
    }

    {
        const auto named = note::AnalyzeMathBoxInput(L"<math>x^2</math>");
        Expect(named.delimiter == note::MathDelimiter::MathTag && named.content_text == L"x^2" &&
                   note::MathDelimiterLabel(named.delimiter) == L"<math>...</math>",
               "MathBox identifies the explicit named wrapper separately from its legacy input helper");
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"x^2 + y^2");
        Expect(analysis.has_input && !analysis.has_wrapping,
               "bare MathBox text stays valid without wrappers");
        Expect(analysis.content_text == L"x^2 + y^2",
               "bare MathBox text is preserved as content");
        Expect(analysis.diagnostics.empty(),
               "bare MathBox text does not emit delimiter diagnostics");
    }

    {
        auto node = mathrender::Parse(L"q = \\dfrac{a+b}{c}");
        Expect(ContainsMathNodeType(node.get(), mathrender::Node::Type::Fraction),
               "math renderer parses \\dfrac as a fraction node");
    }

    {
        using Alignment = note::NoteRenderInlineMathVerticalAlignment;
        const uint32_t top = note::ResolveNoteRenderInlineMathTopOffset(20, 8, Alignment::Top);
        const uint32_t center = note::ResolveNoteRenderInlineMathTopOffset(20, 8, Alignment::Center);
        const uint32_t bottom = note::ResolveNoteRenderInlineMathTopOffset(20, 8, Alignment::Bottom);
        Expect(top == 0 && center == 6 && bottom == 12 &&
                   note::ResolveNoteRenderInlineMathTopOffset(20, 20, Alignment::Bottom) == 0 &&
                   note::ResolveNoteRenderInlineMathTopOffset(8, 20, Alignment::Bottom) == 0,
               "inline math alignment also positions shorter text when tall math consumes the whole row");
    }

    {
        auto node = mathrender::Parse(L"\\vec{x} + \\bar{y} + \\hat{z}");
        Expect(ContainsMathNodeType(node.get(), mathrender::Node::Type::Accent),
               "math renderer parses common accent commands as accent nodes");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"# Title\n\nText with <u>mark</> and $x$.\n");
        note::TextExportConfig config{};
        config.mathMode = note::ExportTextMathMode::Simplified;
        config.markupMode = note::ExportTextMarkupMode::Simplified;
        const std::string out = note::ExportPlainText(parsed.first, parsed.second, config);
        Expect(out.find("Title") != std::string::npos, "plain text export keeps heading text");
        Expect(out.find("mark") != std::string::npos, "plain text export keeps styled text content");
        Expect(out.find("<u>") == std::string::npos, "plain text export strips legacy style tags");
        Expect(out.find("$x$") == std::string::npos && out.find("x") != std::string::npos,
               "plain text export simplifies math text");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(
            L"# Heading text\n\n"
            L"- List text\n"
            L"- [x] Completed task\n"
            L"> Quote text\n"
            L"[Link text](link-target) and **strong text**.\n"
            L"![Image alternative text](image.png)\n"
            L"| Column heading | Column value |\n"
            L"| --- | --- |\n"
            L"| Cell text | Cell value |\n"
            L"```cpp\n"
            L"code text\n"
            L"```\n"
            L"---\n");
        note::TextExportConfig config{};
        config.mathMode = note::ExportTextMathMode::Raw;
        config.markupMode = note::ExportTextMarkupMode::Simplified;
        const std::string out = note::ExportPlainText(parsed.first, parsed.second, config);
        for (const std::string_view visibleText : {
                 "Heading text", "List text", "Quote text", "Link text",
                 "strong text", "Image alternative text", "Completed task",
                 "Column heading", "Column value", "Cell text", "Cell value", "code text"}) {
            Expect(out.find(visibleText) != std::string::npos,
                   "plain text export preserves visible Markdown text");
        }
        Expect(out.find("# Heading text") == std::string::npos &&
                   out.find("- List text") == std::string::npos &&
                   out.find("> Quote text") == std::string::npos &&
                   out.find("link-target") == std::string::npos &&
                   out.find("**strong text**") == std::string::npos &&
                   out.find("![Image alternative text]") == std::string::npos &&
                   out.find("```cpp") == std::string::npos &&
                   out.find("[x] Completed task") == std::string::npos &&
                   out.find("-----") == std::string::npos,
               "plain text export removes Markdown syntax without removing its text");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"# Head\n\nBody $x$.\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Markdown;
        config.mathMode = note::ExportMarkupMathMode::Placeholder;
        config.mathPlaceholder = "[math]";
        config.includeTitleHeading = true;
        config.shiftHeadingLevels = true;
        config.title = L"Doc";
        const std::string out = note::ExportMarkdown(parsed.first, parsed.second, config);
        Expect(out.find("# Doc") != std::string::npos, "markdown export can prepend title heading");
        Expect(out.find("## Head") != std::string::npos, "markdown export can shift existing headings");
        Expect(out.find("[math]") != std::string::npos, "markdown export can replace math spans");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"Text with **bold** and <u>mark</> and $x$.\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Html;
        config.title = L"Doc";
        const std::string out = note::ExportHtml(parsed.first, parsed.second, config);
        Expect(out.find("<strong>bold</strong>") != std::string::npos, "html export keeps strong emphasis");
        Expect(out.find("text-decoration:underline;") != std::string::npos, "html export maps underline style spans");
        Expect(out.find("class=\"math inline\"") != std::string::npos, "html export renders math spans");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"$$x + y$$\n\n\\[z + 1\\]\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Html;
        config.title = L"Doc";
        const std::string out = note::ExportHtml(parsed.first, parsed.second, config);
        Expect(out.find("class=\"math block\"") != std::string::npos,
               "html export renders block math spans");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"escaped \\* mark, escaped \\\\ slash, and yen \u00A5*.\n");
        note::TextExportConfig plainConfig{};
        const std::string plain = note::ExportPlainText(parsed.first, parsed.second, plainConfig);
        Expect(plain.find("escaped * mark, escaped \\ slash") != std::string::npos,
               "plain text export decodes U+005C Markdown escapes including a literal backslash");
        Expect(plain.find("yen \xC2\xA5*") != std::string::npos,
               "plain text export keeps U+00A5 yen sign as ordinary text");

        note::MarkupExportConfig htmlConfig{};
        htmlConfig.format = note::ExportMarkupFormat::Html;
        const std::string html = note::ExportHtml(parsed.first, parsed.second, htmlConfig);
        Expect(html.find("escaped * mark, escaped \\ slash") != std::string::npos,
               "HTML export matches rendered Markdown backslash escapes");
        Expect(html.find("yen \xC2\xA5*") != std::string::npos,
               "HTML export keeps U+00A5 yen sign as ordinary text");

        note::MarkupExportConfig markdownConfig{};
        markdownConfig.format = note::ExportMarkupFormat::Markdown;
        const std::string markdown = note::ExportMarkdown(parsed.first, parsed.second, markdownConfig);
        Expect(markdown.find("escaped \\* mark, escaped \\\\ slash") != std::string::npos,
               "Markdown export preserves raw U+005C escape source");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"PDF に<b>マーカー</b>を引いたり、<i>斜体</i>にする\n");
        const auto* boldSpan = FindStyleSpan(doc, note::StyleKind::Bold);
        const auto* italicSpan = FindStyleSpan(doc, note::StyleKind::Italic);
        Expect(boldSpan != nullptr, "<b> tag creates StyleKind::Bold span");
        Expect(italicSpan != nullptr, "<i> tag creates StyleKind::Italic span");
    }

    {
        auto [model, doc] = BuildMd4cModelAndDoc(L"<b>bold</b> <i>italic</i> <x>strike</x>\n");
        const auto* boldSpan = FindStyleSpan(doc, note::StyleKind::Bold);
        const auto* strikeSpan = FindStyleSpan(doc, note::StyleKind::Strike);
        Expect(boldSpan != nullptr, "<b>...</b> creates StyleKind::Bold span");
        Expect(strikeSpan != nullptr, "<x>...</x> creates StyleKind::Strike span");

        std::string html = note::ExportHtml(model, doc, note::MarkupExportConfig{});
        Expect(html.find("font-weight:bold;") != std::string::npos, "ExportHtml renders StyleKind::Bold as CSS font-weight:bold;");
        Expect(html.find("text-decoration:line-through;") != std::string::npos, "ExportHtml renders StyleKind::Strike as CSS text-decoration:line-through;");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"<b>Universal closing</b> </>\n");
        const auto* boldSpan = FindStyleSpan(doc, note::StyleKind::Bold);
        Expect(boldSpan != nullptr, "<b>...</> creates StyleKind::Bold span with universal closing tag");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"");
        Expect(doc.blocks.empty(), "empty note parses without blocks or crash");
        Expect(doc.math_spans.empty(), "empty note has no math spans");
    }

    {
        std::wstring longLine(1000 * 1000, L'a');
        longLine += L" $x$";
        const note::NoteDocument doc = ParseMd4c(std::move(longLine));
        Expect(!doc.blocks.empty(), "1MB single-line note parses without hang/crash");
        Expect(doc.math_spans.size() == 1, "1MB single-line note still extracts trailing math");
    }

    {
        std::wstring text = L"before ";
        text.push_back(L'\0');
        text += L" after $x$";
        const note::NoteDocument doc = ParseMd4c(std::move(text));
        Expect(doc.math_spans.size() == 1, "NUL-containing note parses and preserves later math scan");
    }

    {
        std::wstring text = L"bad surrogate ";
        text.push_back(static_cast<wchar_t>(0xD800));
        text += L" tail";
        const note::NoteDocument doc = ParseMd4c(std::move(text));
        Expect(!doc.diagnostics.empty() || doc.blocks.size() <= 1,
               "isolated UTF-16 surrogate is handled without parser crash");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"<script>alert('x')</script> & text\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Html;
        config.title = L"<Unsafe>";
        const std::string out = note::ExportHtml(parsed.first, parsed.second, config);
        Expect(out.find("<script>alert") == std::string::npos,
               "html export escapes script-like input");
        Expect(out.find("&lt;script&gt;alert") != std::string::npos,
               "html export keeps script-like input only as escaped text");
        Expect(out.find("<title>&lt;Unsafe&gt;</title>") != std::string::npos,
               "html export escapes title text");
    }

    std::cout << "Summary: failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
