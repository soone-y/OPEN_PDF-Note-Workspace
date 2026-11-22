#pragma once

#include "note/note_export.h"
#include "note/note_kernel.h"
#include "note/note_parser.h"
#include "note/note_render_final_win32_adapter.h"

#include <algorithm>
#include <iostream>

namespace note_clro_line_boundary_test {
inline bool HeadingAt(const note::NoteDocument& document, size_t offset) {
    return std::any_of(document.blocks.begin(), document.blocks.end(), [offset](const auto& block) {
        return block.kind == note::BlockKind::Heading &&
            block.span.start.value <= offset && offset < block.span.end.value;
    });
}

inline bool Run(HDC dc) {
    if (!dc) return false;
    struct Case { const wchar_t* raw; bool clro_heading; bool md_heading; };
    const Case cases[] = {
        {L"TARGET\n-", false, true},
        {L"TARGET\n--", false, true},
        {L"TARGET\n=", false, true},
        {L"TARGET\n===", false, true},
        {L"FIRST\nTARGET\n--", false, true},
        {L"FIRST\nTARGET\n===", false, true},
        {L"TARGET\n---", false, false},
        {L"# FIRST\nTARGET", false, false},
        {L"<m>FIRST\nTARGET", false, false},
        {L"<m1>FIRST\nTARGET", false, false},
        {L"<m>\nTARGET", false, true},
        {L"<h2>\nTARGET", false, true},
        {L"<m>\nTARGET</m>\nTAIL", true, true},
        {L"<m>\nFIRST\nTARGET</m>\nTAIL", true, false},
        {L"<m>FIRST\nTARGET\n</>\nTAIL", true, false},
        {L"<m><b>FIRST\nTARGET</></m>\nTAIL", true, false},
        {L"::: box\nTARGET\n--\n:::", false, true},
        {L"```\nTARGET\n===\n```", false, false},
        {L"`TARGET\n===`", false, false},
        {L"TARGET\n\n===", false, false},
    };
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label, size_t index) {
        ++checks;
        if (!value) std::cerr << "CLRO line boundary: " << label << " case=" << index << "\n";
        ok = ok && value;
    };
    for (const auto* file : {L"lines.clro", L"lines.CLRO", L"lines.md", L"lines.markdown"}) {
        const bool clro = std::wstring_view(file) == L"lines.clro" || std::wstring_view(file) == L"lines.CLRO";
        for (const auto* newline : {L"\n", L"\r\n", L"\r"}) {
            size_t index = 0;
            for (const auto& test : cases) {
                std::wstring raw;
                for (wchar_t ch : std::wstring_view(test.raw)) {
                    if (ch == L'\n') raw += newline;
                    else raw += ch;
                }
                const auto model = note::MakeNoteTextModel({file, L""}, raw, 1);
                const auto document = note::ParseNoteDocument(model);
                const size_t target = raw.find(L"TARGET");
                const bool heading = clro ? test.clro_heading : test.md_heading;
                check(HeadingAt(document, target) == heading, "only explicit headings affect the target row", index);
                const auto text = note::ExportPlainText(model, document, {});
                const auto html = note::ExportHtml(model, document, {});
                const auto markdown = note::ExportMarkdown(model, document, {});
                const size_t first = text.find("TARGET");
                check(first != std::string::npos && text.find("TARGET", first + 1) == std::string::npos &&
                      html.find("TARGET") != std::string::npos && markdown.has_value() && model.raw == raw,
                      "display and exports preserve source content", index);
                note::NoteTextCore core;
                core.Reset({3101}, model.meta, raw, 1, 1);
                note::NoteRenderFinalCompleteBuildInput input;
                input.layout_key = {640, 96, 96, 1, 4, 8, true};
                input.owner_input.render_active = true;
                input.owner_input.editor_text_core_current = true;
                input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                note::NoteRenderFinalGdiMeasurementProvider provider(dc);
                std::shared_ptr<const note::NoteRenderFinalPublication> publication;
                const auto result = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &publication);
                note::NoteRenderSourceLinePlan line;
                const auto starts = note::BuildLineStarts(raw);
                const size_t row = static_cast<size_t>(std::upper_bound(starts.begin(), starts.end(), target) - starts.begin()) - 1;
                check(result == note::NoteRenderFinalCompleteBuildResult::Built && publication &&
                      publication->source_plan()->ResolveLine({row}, &line) &&
                      line.decoration.has(note::NoteRenderSourceLineDecorationHeading) == heading,
                      "structured geometry uses the same row boundary", index);
                if (clro && index < 6) {
                    check(document.blocks.size() > 0 && document.blocks.front().kind == note::BlockKind::Paragraph,
                          "underline text keeps its preceding paragraph", index);
                }
                ++index;
            }
        }
        if (clro) {
            std::wstring sections;
            for (size_t section = 0; section < 128; ++section) sections += L"<m1>Title\nBody\n</##>\n";
            const auto sectionModel = note::MakeNoteTextModel({file, L""}, sections);
            const auto sectionDocument = note::ParseNoteDocument(sectionModel);
            check(std::count_if(sectionDocument.blocks.begin(), sectionDocument.blocks.end(), [](const auto& block) {
                return block.kind == note::BlockKind::Heading;
            }) == 128 && !HeadingAt(sectionDocument, sections.find(L"Body")),
                  "repeated unpaired headings and section closes remain row-scoped", 0);
        }
        const auto plain = note::MakeNoteTextModel({file, L""}, L"FIRST\r\nTARGET");
        const auto plainHtml = note::ExportHtml(plain, note::ParseNoteDocument(plain), {});
        check((plainHtml.find("FIRST<br>\nTARGET") != std::string::npos) == clro,
              "CLRO HTML renders a physical newline once", 0);
        const auto code = note::MakeNoteTextModel({file, L""}, L"`FIRST\nTARGET`");
        const auto codeHtml = note::ExportHtml(code, note::ParseNoteDocument(code), {});
        check((codeHtml.find("<code>FIRST<br>\nTARGET</code>") != std::string::npos) == clro,
              "CLRO multiline inline code displays physical row boundaries", 0);
        const auto unfinished = note::MakeNoteTextModel({file, L""}, L"<c=red>FIRST\nTARGET");
        const auto unfinishedDocument = note::ParseNoteDocument(unfinished);
        check(std::any_of(unfinishedDocument.style_spans.begin(), unfinishedDocument.style_spans.end(), [&](const auto& style) {
            return style.kind == note::StyleKind::TextColor && style.span.end.value == unfinished.raw.size();
        }), "an unfinished decoration tag has a provisional scope through document end", 0);
        const auto paired = note::MakeNoteTextModel({file, L""}, L"**FIRST\nTARGET**\n\n<c=red>FIRST\nTARGET</>");
        const auto pairedDocument = note::ParseNoteDocument(paired);
        const size_t firstTarget = paired.raw.find(L"TARGET");
        const size_t secondTarget = paired.raw.rfind(L"TARGET");
        check(std::any_of(pairedDocument.inlines.begin(), pairedDocument.inlines.end(), [&](const auto& node) {
            return node.kind == note::InlineKind::Strong && node.span.start.value <= firstTarget && firstTarget < node.span.end.value;
        }) && std::any_of(pairedDocument.style_spans.begin(), pairedDocument.style_spans.end(), [&](const auto& style) {
            return style.kind == note::StyleKind::TextColor && style.span.start.value <= secondTarget && secondTarget < style.span.end.value;
        }), "paired markup and tags explicitly span physical lines", 0);
    }
    note::LocalNoteKernel kernel;
    const std::wstring raw = L"FIRST\nTARGET\n";
    kernel.Reset({3102}, {L"edit.clro", L""}, raw, 1, 1, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    const auto edit = kernel.ApplyUserEdit({{raw.size()}, 0, L"="}, {}, {},
        note::NoteHistoryOperationKind::Typing, 1, true);
    (void)kernel.RefreshDerived();
    check(edit.applied() && kernel.CanReadSyntax() && !HeadingAt(kernel.document(), raw.find(L"TARGET")),
          "adding an underline does not promote prior text during editing", 0);
    check(kernel.Undo(true).has_value() && kernel.text_core().MatchesRaw(raw), "undo retains original lines", 0);
    const auto redone = kernel.Redo(true);
    (void)kernel.RefreshDerived();
    check(redone.has_value() && kernel.CanReadSyntax() && kernel.text_core().MatchesRaw(raw + L"=") &&
          !HeadingAt(kernel.document(), raw.find(L"TARGET")), "redo retains the CLRO boundary policy", 0);
    const std::wstring draft = L"<c=red>FIRST\nTARGET\nTAIL";
    kernel.Reset({3103}, {L"draft.clro", L""}, draft, 1, 1, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    const auto tailColored = [&]() {
        const size_t tail = kernel.text_core().model().raw.find(L"TAIL");
        return std::any_of(kernel.document().style_spans.begin(), kernel.document().style_spans.end(), [&](const auto& style) {
            return style.kind == note::StyleKind::TextColor && style.span.start.value <= tail && tail < style.span.end.value;
        });
    };
    check(tailColored(), "draft opening tag provisionally reaches the tail", 0);
    const auto closed = kernel.ApplyUserEdit({{draft.find(L"TAIL")}, 0, L"</c>"}, {}, {},
        note::NoteHistoryOperationKind::Typing, 2, true);
    (void)kernel.RefreshDerived();
    check(closed.applied() && kernel.CanReadSyntax() && !tailColored(), "typing the close contracts the provisional scope", 0);
    const auto reopen = kernel.Undo(true);
    (void)kernel.RefreshDerived();
    check(reopen.has_value() && kernel.CanReadSyntax() && tailColored(), "undo restores the unfinished tag scope", 0);
    const auto reclose = kernel.Redo(true);
    (void)kernel.RefreshDerived();
    check(reclose.has_value() && kernel.CanReadSyntax() && !tailColored(), "redo contracts the scope again", 0);
    std::cout << "CLRO line boundary checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
inline bool RunLongVowelRule(HDC dc) {
    bool ok = dc != nullptr;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) std::cerr << "CLRO long-vowel rule: " << label << " check=" << checks << "\n";
        ok = ok && value;
    };
    const auto hasRule = [](const note::NoteDocument& doc) {
        return std::any_of(doc.blocks.begin(), doc.blocks.end(), [](const auto& b) {
            return b.kind == note::BlockKind::HorizontalRule;
        });
    };
    struct Case { const wchar_t* raw; bool rule; };
    const Case cases[] = {
        {L"ーーー", true}, {L"BEFORE\nーーー\nAFTER", true},
        {L"::: box\nーーー\n:::", true},
        {L"ーー", false}, {L"ーーーー", false}, {L"－－－", false},
        {L"-ーー", false}, {L"本文ーーー", false}, {L"ーーー本文", false},
        {L" ーーー", false}, {L"ーーー ", false}, {L"　ーーー", false},
        {L"\tーーー", false}, {L"> ーーー", false}, {L"- ーーー", false},
        {L"`ーーー`", false}, {L"`FIRST\nーーー\nLAST`", false},
        {L"```\nーーー\n```", false}, {L"~~~\nーーー", false},
        {L"$$\nーーー\n$$", false}, {L"<math>\nーーー\n</math>", false},
    };
    for (const auto* file : {L"rule.clro", L"rule.CLRO", L"rule.md"}) {
        const bool clro = std::wstring_view(file) != L"rule.md";
        for (bool asciiOnly : {false, true}) for (const auto* newline : {L"\n", L"\r\n", L"\r"}) {
            for (const auto& test : cases) {
                std::wstring raw;
                for (wchar_t ch : std::wstring_view(test.raw)) {
                    if (ch == L'\n') raw += newline; else raw += ch;
                }
                note::NoteMetadata meta{file, L""};
                meta.clro_ascii_only = asciiOnly;
                const auto model = note::MakeNoteTextModel(meta, raw, 1);
                const auto doc = note::ParseNoteDocument(model);
                const bool expected = clro && test.rule;
                check(!note::NoteDocumentHasFatalParseFailure(doc) && hasRule(doc) == expected,
                      "exact row only, CLRO formats and literal protection");
                const auto projected = note::BuildClroMarkdownExportSource(model);
                std::wstring expectedExport = raw;
                if (expected) expectedExport.replace(raw.find(L"ーーー"), 3, L"---");
                check(projected && *projected == expectedExport && model.raw == raw && model.revision == 1,
                      "normalized Markdown, preserved literal text and original source");
                if (!expected) continue;
                check(note::ExportHtml(model, doc, {}).find("<hr") != std::string::npos,
                      "HTML horizontal rule");
                note::NoteTextCore core;
                core.Reset({2701}, meta, raw, 1, 1);
                note::NoteRenderFinalCompleteBuildInput input;
                input.content_kind = note::NoteContentKind::Markdown;
                input.layout_key = {480, 96, 96, 1, 4, 8, false};
                input.owner_input.render_active = true;
                input.owner_input.editor_text_core_current = true;
                input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                note::NoteRenderFinalGdiMeasurementProvider provider(dc);
                std::shared_ptr<const note::NoteRenderFinalPublication> publication;
                const auto result = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &publication);
                const size_t ruleLine = raw.find(L"ーーー") == 0 ? 0 : 1;
                note::NoteRenderLinePlacement placement;
                check(result == note::NoteRenderFinalCompleteBuildResult::Built && publication &&
                      publication->placement()->ResolveLine({ruleLine}, &placement) &&
                      std::any_of(placement.decorations.begin(), placement.decorations.end(), [](const auto& d) {
                          return d.kind == note::NoteRenderVisualDecorationKind::HorizontalRule &&
                              d.left_px >= 8 && d.right_px > d.left_px;
                      }), "final rendering owns a horizontal-rule decoration");
            }
        }
    }
    // Exercise the final-renderer shortcut independently of the editor kernel.
    for (const auto* raw : {L"ーー", L"ーーーx", L"ーーー"}) {
        note::NoteTextCore core;
        core.Reset({2705}, {L"patch.clro", L""}, raw, 1, 1);
        note::NoteRenderFinalCompleteBuildInput input;
        input.content_kind = note::NoteContentKind::Markdown;
        input.layout_key = {480, 96, 96, 1, 4, 8, false};
        input.owner_input.render_active = true;
        input.owner_input.editor_text_core_current = true;
        input.owner_input.visible_lines = {{0}, {1}};
        note::NoteRenderFinalGdiMeasurementProvider provider(dc);
        std::shared_ptr<const note::NoteRenderFinalPublication> previous, patched;
        const bool built = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &previous) ==
            note::NoteRenderFinalCompleteBuildResult::Built;
        const size_t length = std::wstring_view(raw).size();
        const note::TextEdit edit = length == 2 ? note::TextEdit{{2}, 0, L"ー"} :
            length == 4 ? note::TextEdit{{3}, 1, L""} : note::TextEdit{{3}, 0, L"x"};
        note::NoteRenderFinalLocalPatchInput patch;
        patch.layout_key = input.layout_key;
        patch.owner_input = input.owner_input;
        patch.edit = edit;
        check(built && core.Apply(edit) == note::NoteTextApplyResult::Applied &&
              note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(previous, core, patch, provider, &patched) !=
                  note::NoteRenderFinalCompleteBuildResult::Built && !patched,
              "renderer shortcut rejects delimiter creation and removal");
    }
    note::LocalNoteKernel kernel;
    kernel.Reset({2702}, {L"edit.clro", L""}, L"BEFORE\nーー\nAFTER", 1, 1, note::NoteContentKind::Markdown);
    check(kernel.RefreshDerived().current, "initial editing snapshot");
    const auto editAndCheck = [&](note::TextEdit edit, bool expected) {
        const auto applied = kernel.ApplyUserEdit(edit, {}, {}, note::NoteHistoryOperationKind::Typing, checks, true);
        const auto refreshed = kernel.RefreshDerived();
        check(applied.applied() && refreshed.current && hasRule(kernel.document()) == expected,
              "typing and deletion update block ownership");
    };
    editAndCheck({{9}, 0, L"ー"}, true);
    editAndCheck({{10}, 0, L"x"}, false);
    editAndCheck({{10}, 1, L""}, true);
    editAndCheck({{9}, 1, L""}, false);
    const auto undone = kernel.Undo(true);
    check(undone && undone->applied() && kernel.RefreshDerived().current && hasRule(kernel.document()), "undo restores rule");
    const auto redone = kernel.Redo(true);
    check(redone && redone->applied() && kernel.RefreshDerived().current && !hasRule(kernel.document()), "redo removes rule");
    kernel.Reset({2703}, {L"batch.clro", L""}, L"ー", 1, 1, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    check(kernel.Apply({{1}, 0, L"ー"}, true).applied() &&
          kernel.Apply({{2}, 0, L"ー"}, true).applied() &&
          kernel.RefreshDerived().current && hasRule(kernel.document()), "batched input enters rule");
    kernel.Reset({2704}, {L"plain.clro", L""}, L"ノート", 1, 1, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    const auto materializations = kernel.text_core().model_materialization_count();
    check(kernel.Apply({{3}, 0, L"ー"}, true).applied() &&
          kernel.text_core().model_materialization_count() == materializations &&
          kernel.RefreshDerived().current && !hasRule(kernel.document()), "ordinary long vowel keeps cheap text path");
    std::cout << "CLRO long-vowel rule checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_clro_line_boundary_test
