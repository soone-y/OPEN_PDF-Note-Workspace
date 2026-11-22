#pragma once

#include "note/note_export.h"
#include "note/note_kernel.h"
#include "note/note_parser.h"
#include "note/note_render_source_plan.h"
#include "note/note_syntax_snapshot.h"

#include <algorithm>
#include <iostream>

namespace note_table_continuation_test {
inline bool InTable(const note::NoteDocument& document, size_t position) {
    return std::any_of(document.blocks.begin(), document.blocks.end(), [position](const auto& block) {
        return block.kind == note::BlockKind::Table &&
            block.span.start.value <= position && position < block.span.end.value;
    });
}

inline bool Run() {
    struct Case { const wchar_t* tail; bool clro; bool markdown; };
    const Case cases[] = {
        {L"TARGET", false, true},
        {L"   TARGET", false, true},
        {L"\tTARGET", false, true},
        {L"**TARGET**", false, true},
        {L"`TARGET`", false, true},
        {L"<c=red>TARGET</>", false, true},
        {L"first\nTARGET", false, true},
        {L"\nTARGET", false, false},
        {L"TARGET | value", true, true},
        {L"| TARGET", true, true},
        {L"TARGET |", true, true},
        {L"TARGET \\| literal", true, true},
        {L"`TARGET | literal`", true, true},
        {L"| second | row |\nTARGET", false, true},
        {L"TARGET ｜ fullwidth", false, true},
        {L"# TARGET", false, true},
        {L"- TARGET", false, false},
        {L"> TARGET", false, false},
        {L"```\nTARGET\n```", false, true},
        {L"::: box\nTARGET\n:::", false, false},
    };
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label, size_t index) {
        ++checks;
        if (!value) std::cerr << "table continuation: " << label << " case=" << index << "\n";
        ok = ok && value;
    };
    for (const auto* file : {L"table.clro", L"table.CLRO", L"table.md", L"table.markdown"}) {
        const bool clro = std::wstring_view(file) == L"table.clro" || std::wstring_view(file) == L"table.CLRO";
        for (const auto* newline : {L"\n", L"\r\n", L"\r"}) {
            const std::wstring prefix = std::wstring(L"| H | H2 |") + newline +
                L"|---|---|" + newline + L"| a | b |" + newline;
            size_t index = 0;
            for (const auto& test : cases) {
                std::wstring tail;
                for (wchar_t ch : std::wstring_view(test.tail)) {
                    if (ch == L'\n') tail += newline;
                    else tail += ch;
                }
                const std::wstring raw = prefix + tail;
                const auto model = note::MakeNoteTextModel({file, L""}, raw, 1);
                const auto document = note::ParseNoteDocument(model);
                const size_t target = raw.find(L"TARGET");
                const bool expected = clro ? test.clro : test.markdown;
                check(InTable(document, target) == expected, "table boundary follows file type", index);
                const auto text = note::ExportPlainText(model, document, {});
                const auto html = note::ExportHtml(model, document, {});
                const size_t first = text.find("TARGET");
                const size_t htmlFirst = html.find("TARGET");
                check(first != std::string::npos && text.find("TARGET", first + 1) == std::string::npos &&
                      htmlFirst != std::string::npos && html.find("TARGET", htmlFirst + 1) == std::string::npos &&
                      model.raw == raw, "source and exported content survive once", index);
                if (index == 0) {
                    check((html.find("TARGET") < html.find("</table>")) == expected,
                          "HTML places following text outside the closed table", index);
                    note::NoteTextCore core;
                    core.Reset({2801}, {file, L""}, raw, 1, 1);
                    std::shared_ptr<const note::NoteSyntaxSnapshot> syntax;
                    std::shared_ptr<const note::NoteRenderSourcePlan> plan;
                    note::NoteRenderSourceLinePlan line;
                    const bool built = note::NoteSyntaxSnapshot::BuildFromTextCore(core, note::NoteContentKind::Markdown, &syntax) ==
                        note::NoteSyntaxSnapshotBuildResult::Built && syntax &&
                        note::NoteRenderSourcePlan::Build(syntax, &plan) == note::NoteRenderSourcePlanBuildResult::Built && plan &&
                        plan->ResolveLine({3}, &line);
                    check(built && line.decoration.has(note::NoteRenderSourceLineDecorationTable) == expected,
                          "render source plan ends table decoration at the same row", index);
                }
                ++index;
            }
        }
        // A row probe starts after the quote/list prefix already consumed by MD4C.
        for (const auto* raw : {
                 L"> | H | H2 |\n> |---|---|\n> | a | b |\n> TARGET",
                 L"- | H | H2 |\n  |---|---|\n  | a | b |\n  TARGET"}) {
            note::NoteMetadata metadata{file, L""};
            metadata.clro_list_implicit_continuation = true;
            const auto model = note::MakeNoteTextModel(metadata, raw);
            check(InTable(note::ParseNoteDocument(model), model.raw.find(L"TARGET")) == !clro,
                  "nested quote/list table keeps its independent row boundary", 0);
        }
        struct RangeCase { const wchar_t* raw; note::InlineKind kind; };
        const RangeCase ranges[] = {
            {L"**first\nTARGET**", note::InlineKind::Strong},
            {L"*first\nTARGET*", note::InlineKind::Emphasis},
            {L"__first\nTARGET__", clro ? note::InlineKind::Underline : note::InlineKind::Strong},
            {L"~~first\nTARGET~~", note::InlineKind::Strike},
            {L"`first\nTARGET`", note::InlineKind::Code},
            {L"[first\nTARGET](local.md)", note::InlineKind::Link},
        };
        for (const auto& range : ranges) {
            const auto model = note::MakeNoteTextModel({file, L""}, range.raw);
            const auto document = note::ParseNoteDocument(model);
            const size_t target = model.raw.find(L"TARGET");
            check(std::any_of(document.inlines.begin(), document.inlines.end(), [&](const auto& node) {
                return node.kind == range.kind && node.span.start.value <= target && target < node.span.end.value;
            }), "paired inline markup still spans lines", 0);
        }
        for (const auto* raw : {L"<c=red>first\n\nTARGET</>", L"<b>first\nTARGET</>",
                               L"<d=center>first\nTARGET</>"}) {
            const auto model = note::MakeNoteTextModel({file, L""}, raw);
            const auto document = note::ParseNoteDocument(model);
            const size_t target = model.raw.find(L"TARGET");
            check(std::any_of(document.style_spans.begin(), document.style_spans.end(), [&](const auto& style) {
                return style.span.start.value <= target && target < style.span.end.value;
            }), "paired tags still span lines and blank lines", 0);
        }
    }
    const std::wstring row = L"| TARGET | value |";
    const std::wstring source = L"| H | H2 |\n|---|---|\n" + row + L"\nTAIL";
    const size_t rowStart = source.find(row);
    note::LocalNoteKernel kernel;
    kernel.Reset({2802}, {L"edit.clro", L""}, source, 1, 1, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    const auto edit = kernel.ApplyUserEdit({{rowStart}, row.size(), L"TARGET value"}, {}, {},
        note::NoteHistoryOperationKind::Typing, 1, true);
    (void)kernel.RefreshDerived();
    check(edit.applied() && kernel.CanReadSyntax() && !InTable(kernel.document(), rowStart),
          "removing row pipes updates the table boundary through the edit path", 0);
    const auto undone = kernel.Undo(true);
    (void)kernel.RefreshDerived();
    check(undone.has_value() && kernel.CanReadSyntax() && kernel.text_core().MatchesRaw(source) &&
          InTable(kernel.document(), rowStart + 2), "undo restores the row and table membership", 0);
    const auto redone = kernel.Redo(true);
    (void)kernel.RefreshDerived();
    check(redone.has_value() && kernel.CanReadSyntax() && !InTable(kernel.document(), rowStart),
          "redo reapplies the table boundary", 0);
    std::cout << "Table continuation checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_table_continuation_test
