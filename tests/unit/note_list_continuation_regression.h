#pragma once

#include "note/note_export.h"
#include "note/note_kernel.h"
#include "note/note_parser.h"
#include "note/note_render_source_plan.h"
#include "note/note_syntax_snapshot.h"
#include "note/note_workspace_service.h"

#include <algorithm>
#include <iostream>

namespace note_list_continuation_test {
inline int ListDepth(const note::NoteDocument& document, size_t position) {
    return static_cast<int>(std::count_if(document.blocks.begin(), document.blocks.end(), [&](const auto& block) {
        return block.kind == note::BlockKind::List && block.span.start.value <= position && position < block.span.end.value;
    }));
}
inline bool Run() {
    struct Case { const wchar_t* source; int strict_depth; int standard_depth; };
    const Case cases[] = {
        {L"- item\nTARGET", 0, 1},
        {L"* item\nTARGET", 0, 1},
        {L"+ item\nTARGET", 0, 1},
        {L"1. item\nTARGET", 0, 1},
        {L"3) item\nTARGET", 0, 1},
        {L"- [ ] item\nTARGET", 0, 1},
        {L"- [x] item\n  TARGET", 0, 1},
        {L"- item\n  TARGET", 0, 1},
        {L"- item\n\tTARGET", 0, 1},
        {L"- item\r\nTARGET", 0, 1},
        {L"- item\rTARGET", 0, 1},
        {L"- item\n\nTARGET", 0, 0},
        {L"- item\n\n  TARGET", 0, 1},
        {L"- outer\n  - inner\n    TARGET", 0, 2},
        {L"- outer\n  - TARGET", 2, 2},
        {L"- first\n- TARGET", 1, 1},
        {L"9. first\n10. TARGET", 1, 1},
        {L"- [x] first\n- [ ] TARGET", 1, 1},
        {L"- first\nplain\n- TARGET", 1, 1},
        {L"paragraph\nTARGET", 0, 0},
        {L"- item\n\\- TARGET", 0, 1},
        {L"- item\n  > TARGET", 0, 1},
        {L"> - item\n> TARGET", 0, 1},
        {L"- item\n# TARGET", 0, 0},
        {L"```\n- TARGET\n```", 0, 0},
        {L"- item\n---\nTARGET", 0, 0},
        {L"- [link][ref]\nTARGET\n\n[ref]: local.md", 0, 1},
    };
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label, size_t index) {
        ++checks;
        if (!value) std::cerr << "list continuation: " << label << " case=" << index << "\n";
        ok = ok && value;
    };
    for (const auto* file : {L"list.clro", L"list.CLRO", L"list.md"}) {
        const bool markdown = std::wstring_view(file) == L"list.md";
        for (bool enabled : {false, true}) {
            note::NoteMetadata metadata{file, L""};
            metadata.clro_list_implicit_continuation = enabled;
            size_t index = 0;
            for (const auto& test : cases) {
                const auto model = note::MakeNoteTextModel(metadata, test.source, 1);
                const auto document = note::ParseNoteDocument(model);
                const size_t target = model.raw.find(L"TARGET");
                const int depth = markdown || enabled ? test.standard_depth : test.strict_depth;
                check(ListDepth(document, target) == depth, "explicit, lazy and indented list membership", index);
                const auto text = note::ExportPlainText(model, document, {});
                const size_t first = text.find("TARGET");
                check(first != std::string::npos && text.find("TARGET", first + 1) == std::string::npos &&
                      model.raw == test.source, "export keeps all source content once", index);
                const auto html = note::ExportHtml(model, document, {});
                const size_t htmlTarget = html.find("TARGET");
                check(htmlTarget != std::string::npos && html.find("TARGET", htmlTarget + 1) == std::string::npos,
                      "HTML retains list and continuation text once", index);
                ++index;
            }
        }
    }
    const std::wstring source = L"- first\nTARGET [link][ref]\n\n[ref]: local.md";
    note::NoteMetadata metadata{L"policy.clro", L""};
    note::LocalNoteKernel kernel;
    kernel.Reset({2701}, metadata, source, 10, 2, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    const auto edit = kernel.ApplyUserEdit({{source.size()}, 0, L"!"}, {}, {},
        note::NoteHistoryOperationKind::Typing, 1, true);
    const auto snapshot = kernel.text_core().TakeSnapshot();
    const auto revision = kernel.text_core().content_revision();
    check(edit.applied() && kernel.SetClroListImplicitContinuation(true) && !kernel.CanReadSyntax(),
          "policy change invalidates derived state", 0);
    (void)kernel.RefreshDerived();
    check(kernel.CanReadSyntax() && ListDepth(kernel.document(), source.find(L"TARGET")) == 1 &&
          kernel.text_core().MatchesSnapshot(snapshot) && kernel.text_core().content_revision() == revision,
          "switch keeps canonical root and revision", 0);
    check(kernel.Undo(true).has_value() && kernel.text_core().MatchesRaw(source), "undo survives policy switch", 0);
    check(kernel.SetClroListImplicitContinuation(false) && kernel.Redo(true).has_value(), "redo survives policy switch", 0);
    (void)kernel.RefreshDerived();
    check(ListDepth(kernel.document(), source.find(L"TARGET")) == 0 && kernel.text_core().MatchesRaw(source + L"!"),
          "redo renders under the current policy", 0);

    note::NoteWorkspaceService workspace;
    const auto identity = note::BuildSnapshotIdentity({2702}, 10, 2, "list-policy-fixture");
    const auto strict = workspace.ResolveIndex(identity, L"policy.clro", metadata, source, note::NoteContentKind::Markdown);
    metadata.clro_list_implicit_continuation = true;
    const auto standard = workspace.ResolveIndex(identity, L"policy.clro", metadata, source, note::NoteContentKind::Markdown);
    check(strict && standard && strict != standard && ListDepth(strict->document, source.find(L"TARGET")) == 0 &&
          ListDepth(standard->document, source.find(L"TARGET")) == 1, "index cache separates policies", 0);
    if (strict && standard) {
        const auto strictHtml = note::ExportHtml(strict->text_model, strict->document, {});
        const auto standardHtml = note::ExportHtml(standard->text_model, standard->document, {});
        check(strictHtml.find("</ul>") < strictHtml.find("TARGET") &&
              standardHtml.find("TARGET") < standardHtml.find("</ul>"), "HTML uses the selected list boundaries", 0);
        check(std::any_of(strict->document.inlines.begin(), strict->document.inlines.end(), [](const auto& node) {
            return node.kind == note::InlineKind::Link && node.target == L"local.md";
        }), "reference links resolve across strict list boundaries", 0);
    }

    for (const auto* raw : {L"- item\n  TARGET", L"- ```\n  TARGET\n  ```"}) {
        note::NoteTextCore core;
        core.Reset({2703}, {L"layout.clro", L""}, raw, 1, 1);
        std::shared_ptr<const note::NoteSyntaxSnapshot> syntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> plan;
        note::NoteRenderSourceLinePlan line;
        const bool built = note::NoteSyntaxSnapshot::BuildFromTextCore(core, note::NoteContentKind::Markdown, &syntax) ==
            note::NoteSyntaxSnapshotBuildResult::Built && syntax &&
            note::NoteRenderSourcePlan::Build(syntax, &plan) == note::NoteRenderSourcePlanBuildResult::Built && plan &&
            plan->ResolveLine({1}, &line);
        check(built && !line.decoration.has(note::NoteRenderSourceLineDecorationListItem),
              "unmarked body and fenced content have no list decoration", 0);
    }
    for (bool enabled : {false, true}) {
        metadata.clro_list_implicit_continuation = enabled;
        const auto nestedModel = note::MakeNoteTextModel(metadata,
            L"- FIRST **BOLD** [LINK](local.md)\n  - NEST\n  AFTER\n\nTAIL");
        const auto nestedDocument = note::ParseNoteDocument(nestedModel);
        const auto html = note::ExportHtml(nestedModel, nestedDocument, {});
        size_t previous = 0;
        for (const auto* word : {"FIRST", "BOLD", "LINK", "NEST", "AFTER", "TAIL"}) {
            const size_t position = html.find(word);
            check(position != std::string::npos && position >= previous &&
                  html.find(word, position + 1) == std::string::npos,
                  "HTML interleaves direct inline text and nested lists in source order", enabled);
            previous = position;
        }
        check(html.find("<strong>BOLD</strong>") != std::string::npos &&
              html.find("<a href=\"local.md\">LINK</a>") != std::string::npos,
              "HTML retains formatting on tight list text", enabled);
    }
    std::wstring quoted(256, L'>');
    quoted += L" initial\n";
    for (int line = 0; line < 2048; ++line) quoted += L"continuation\n";
    quoted += L"TARGET";
    metadata.quote_lazy_continuation = true;
    metadata.clro_list_implicit_continuation = false;
    const auto deep = note::ParseNoteDocument(note::MakeNoteTextModel(metadata, quoted));
    check(std::count_if(deep.blocks.begin(), deep.blocks.end(), [](const auto& block) {
        return block.kind == note::BlockKind::Quote;
    }) == 256, "deep implicit quotes retain independent cached continuation", 0);
    std::cout << "List continuation checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_list_continuation_test
