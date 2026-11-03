#pragma once

#include "note/note_clro_markdown.h"
#include "note/note_parser.h"
#include "note/note_export.h"
#include "note/note_kernel.h"
#include "note/note_workspace_service.h"
#include "note/note_parser_checkpoint.h"
#include "note/note_render_source_plan.h"
#include "note/note_syntax_snapshot.h"
#include <iostream>

namespace note_fullwidth_test {

struct Results { size_t cases = 0; bool parsing = true; bool literals = true; bool coordinates = true; bool editing = true; };

inline bool VerifySourcePlan(const note::NoteTextModel& model) {
    note::NoteTextCore core;
    core.Reset(note::NoteId{982}, model.meta, model.raw, model.revision, 1);
    std::shared_ptr<const note::NoteSyntaxSnapshot> snapshot;
    std::shared_ptr<const note::NoteRenderSourcePlan> plan;
    if (note::NoteSyntaxSnapshot::BuildFromTextCore(core, note::NoteContentKind::Markdown, &snapshot) !=
        note::NoteSyntaxSnapshotBuildResult::Built || !snapshot ||
        note::NoteRenderSourcePlan::Build(snapshot, &plan) != note::NoteRenderSourcePlanBuildResult::Built || !plan) return false;
    for (size_t i = 0; i < plan->line_count(); ++i) {
        note::NoteRenderSourceLinePlan line;
        if (!plan->ResolveLine({i}, &line)) return false;
        size_t cursor = line.content_span.start.value;
        for (const auto& run : line.runs) {
            if (run.source_span.start.value != cursor || run.source_span.end < run.source_span.start ||
                run.source_span.end > line.content_span.end) return false;
            cursor = run.source_span.end.value;
        }
        if (cursor != line.content_span.end.value) return false;
    }
    note::NoteDocument copied;
    return snapshot->CopyDocumentForCompleteBuild(&copied) && copied.uses_clro_fullwidth_markdown;
}

inline Results Run() {
    Results result;
    const wchar_t* tags[] = {L"<c\uff1dred>text</>", L"<\uff43=red>text</>", L"\uff1cu\uff1etext\uff1c\uff0f\uff1e",
        L"<\uff42>text</\uff42>", L"<\uff53=24>text</>", L"<\uff43har=red>text</\uff43har>"};
    for (const auto* text : tags) {
        const auto model = note::MakeNoteTextModel({L"test.clro", L""}, text, 1);
        const auto doc = note::ParseNoteDocument(model);
        result.parsing = result.parsing && !doc.style_spans.empty() && !note::NoteDocumentHasFatalParseFailure(doc);
        result.coordinates = result.coordinates && VerifySourcePlan(model);
        ++result.cases;
    }
    const wchar_t* literals[] = {L"\uff5e(example)\uff5e(other)", L"\uff5e\uff5ecancel\uff5e\uff5e", L"\uff03Heading",
        L"\uff0a\uff0abold\uff0a\uff0a", L"\uff1e quote", L"\uff0d item", L"\uff11\uff0e item",
        L"\uff3blink\uff3d\uff08local.txt\uff09", L"\uff40code\uff40", L"\uff1a\uff1a\uff1a title",
        L"\uff04x\uff04", L"\uff3c\uff08x\uff3c\uff09", L"\uff1cunknown\uff1etext\uff1c\uff0funknown\uff1e",
        L"\uff21\uff22\uff23", L"\uff5e\uff08example\uff09", L"\uff1cmath\uff1ex\uff1c/math\uff1e"};
    for (const auto* text : literals) for (const auto* name : {L"test.clro", L"test.md", L"test.txt"}) {
        const auto model = note::MakeNoteTextModel({name, L""}, text, 2);
        const auto doc = note::ParseNoteDocument(model);
        result.literals = result.literals && !doc.uses_clro_fullwidth_markdown && doc.style_spans.empty() && doc.math_spans.empty();
        for (const auto& node : doc.inlines) result.literals = result.literals && node.kind == note::InlineKind::Text;
        ++result.cases;
    }
    for (const auto* text : {L"`<\uff43=red>text</>`", L"```\n<\uff43=red>text</>\n```", L"$<\uff43=red>x</>$"}) {
        const auto model = note::MakeNoteTextModel({L"test.clro", L""}, text, 3);
        result.literals = result.literals && note::ParseNoteDocument(model).style_spans.empty();
        ++result.cases;
    }
    note::LocalNoteKernel kernel;
    kernel.Reset(note::NoteId{983}, {L"edit.clro", L""}, L"<\uff43=red>text</>", 1, 0, note::NoteContentKind::Markdown);
    result.editing = kernel.RefreshDerived(true).current;
    const auto edit = kernel.ApplyUserEdit({{9}, 0, L"x"}, {}, {}, note::NoteHistoryOperationKind::Typing, 100, false);
    const auto undo = kernel.Undo(false);
    const auto redo = kernel.Redo(false);
    result.editing = result.editing && edit.applied() && undo && undo->applied() && redo && redo->applied();
    struct StrictCase { const wchar_t* text; bool styled; };
    for (const auto& item : {StrictCase{L"＜ｃ＝赤＞本文＜／＞", false},
         StrictCase{L"<c＝red>本文</>", false}, StrictCase{L"<c=赤>本文</>", false},
         StrictCase{L"<b　>本文</>", false}, StrictCase{L"<font=＂Arial＂>本文</>", false},
         StrictCase{L"<back=薄黄>本文</>", false}, StrictCase{L"<c=red>本文</>", true},
         StrictCase{L"<c=#ff0000>本文</>", true}, StrictCase{L"<back=lightyellow>本文</>", true},
         StrictCase{L"<font=游ゴシック>本文</>", true}}) {
        auto strict = note::MakeNoteTextModel({L"strict.clro", L""}, item.text, 1);
        strict.meta.clro_ascii_only = true;
        const auto parsed = note::ParseNoteDocument(strict);
        result.parsing = result.parsing && !note::NoteDocumentHasFatalParseFailure(parsed) &&
            (!parsed.style_spans.empty() == item.styled) && strict.raw == item.text;
        ++result.cases;
    }
    auto md = note::MakeNoteTextModel({L"ordinary.md", L""}, L"<c=赤>本文</>", 1);
    md.meta.clro_ascii_only = true;
    result.parsing = result.parsing && !note::ParseNoteDocument(md).style_spans.empty();
    const auto retained = kernel.text_core().TakeSnapshot();
    const auto revision = kernel.text_core().content_revision();
    result.editing = result.editing && kernel.SetClroAsciiOnly(true) && kernel.CanUndo() &&
        kernel.text_core().MatchesSnapshot(retained) && kernel.text_core().content_revision() == revision &&
        kernel.RefreshDerived(true).current && kernel.document().style_spans.empty() &&
        kernel.SetClroAsciiOnly(false) && kernel.RefreshDerived(true).current && !kernel.document().style_spans.empty();
    note::NoteWorkspaceService workspace;
    const auto identity = note::BuildSnapshotIdentity(note::NoteId{991}, 1, 0, u8"<ｃ=red>x</>");
    note::NoteMetadata metadata{L"cache.clro", L""};
    const auto permissive = workspace.ResolveIndex(identity, L"cache.clro", metadata, L"<ｃ=red>x</>", note::NoteContentKind::Markdown);
    metadata.clro_ascii_only = true;
    const auto strict = workspace.ResolveIndex(identity, L"cache.clro", metadata, L"<ｃ=red>x</>", note::NoteContentKind::Markdown);
    result.parsing = result.parsing && permissive && strict && permissive != strict &&
        !permissive->document.style_spans.empty() && strict->document.style_spans.empty();
    return result;
}

inline bool RunExport(std::string (*encode)(const std::wstring&)) {
    struct Case { const wchar_t* raw; const wchar_t* converted; };
    // TXT removes the wrappers, never the text those wrappers decorate.
    const Case plainCases[] = {
        {L"<c=赤>赤い文字</>", L"赤い文字\n"},
        {L"＜ｃ＝赤＞赤い文字＜／＞", L"赤い文字\n"},
        {L"＜ｃ＝赤＞前＜ｂ＞太字＜／＞後＜／＞", L"前太字後\n"},
        {L"# 見出し\n\n**太字**と<u>下線</>", L"見出し\n太字と下線\n"},
        {L"`＜ｃ＝赤＞コード＜／＞`", L"＜ｃ＝赤＞コード＜／＞\n"},
        {L"<math>x^2</math>", L"x^2\n"}
    };
    bool plainPass = true;
    for (const auto& item : plainCases) {
        const auto model = note::MakeNoteTextModel({L"plain.clro", L""}, item.raw, 9);
        const auto doc = note::ParseNoteDocument(model);
        note::TextExportConfig config;
        config.mathMode = note::ExportTextMathMode::Simplified;
        const bool exact = note::ExportPlainText(model, doc, config) == encode(item.converted);
        if (!exact) std::cerr << "CLRO TXT wrapper removal lost or changed visible content\n";
        plainPass = plainPass && exact && model.raw == item.raw;
    }
    const Case cases[] = {
        {L"＃Heading", L"# Heading"}, {L"＃#Heading\r\n", L"## Heading\r\n"},
        {L"＃＊＊Heading＊＊", L"# **Heading**"},
        {L"＊＊bold＊＊ and ＡＢ１２！＃＠＋", L"**bold** and ＡＢ１２！＃＠＋"},
        {L"－　項目\n１． item", L"- 項目\n1. item"},
        {L"－ ［ｘ］ done", L"- [x] done"},
        {L"｀＊＊literal＊＊｀", L"`＊＊literal＊＊`"},
        {L"｀｀｀\n＃literal\n｀｀｀", L"```\n＃literal\n```"},
        {L"```\n＃literal\n```", L"```\n＃literal\n```"},
        {L"$a＊b$\n\n＃H", L"$a＊b$\n\n# H"},
        {L"＼$a＊b$\n\n＃H", L"＼$a＊b$\n\n# H"},
        {L"＄x＄\n\n＊＊bold＊＊", L"＄x＄\n\n**bold**"},
        {L"［link］（local（１）.txt）", L"[link](local（１）.txt)"},
        {L"＜font＝＂Ａ＊Ｂ＂＞text＜／＞", L"<font=\"Ａ＊Ｂ\">text</>"},
        {L"<ｃ＝赤>text</>", L"<c=赤>text</>"},
        {L"<ｃ=赤>text</ｃ>", L"<c=赤>text</c>"},
        {L"＜ＣＨＡＲ＝赤＞text＜／ＣＨＡＲ＞", L"<CHAR=赤>text</CHAR>"},
        {L"<ｆｏｎｔ=\"Ａ＊Ｂ\">text</>", L"<font=\"Ａ＊Ｂ\">text</>"},
        {L"<ｌ=idＡＢ>text</>", L"<l=idＡＢ>text</>"},
        {L"<ｕｎｋｎｏｗｎ>literal</ｕｎｋｎｏｗｎ>", L"<ｕｎｋｎｏｗｎ>literal</ｕｎｋｎｏｗｎ>"},
        {L"`<ｃ＝赤>literal</>`", L"`<ｃ＝赤>literal</>`"},
        {L"＜unknown＞literal＜／unknown＞", L"＜unknown＞literal＜／unknown＞"},
        {L"：：： title＊literal\n＊＊body＊＊\n：：：", L"::: title＊literal\n**body**\n:::"},
        {L"｀｀｀cpp title＝＂file＊name＂\nx\n｀｀｀", L"```cpp title=\"file＊name\"\nx\n```"},
        {L"［shown］［ref＊id］\n\n［ref＊id］： local（１）.txt", L"[shown][ref＊id]\n\n[ref＊id]: local（１）.txt"},
        {L"#literal\n\n＊＊body＊＊", L"#literal\n\n**body**"},
        {L"＼＊literal＼＊", L"＊literal＊"}, {L"\\＊literal\\＊", L"＊literal＊"},
        {L"＼*literal＼*", L"\\*literal\\*"}
    };
    bool pass = plainPass && note::MarkupExportConfig{}.normalizeClroSyntax;
    size_t index = 0;
    for (const auto& item : cases) {
        const auto model = note::MakeNoteTextModel({L"export.clro", L""}, item.raw, 10);
        const auto doc = note::ParseNoteDocument(model);
        const auto converted = note::ExportMarkdown(model, doc, note::MarkupExportConfig{});
        note::MarkupExportConfig preserve;
        preserve.normalizeClroSyntax = false;
        const auto original = note::ExportMarkdown(model, doc, preserve);
        const bool exact = converted && *converted == encode(doc.uses_clro_fullwidth_markdown ? item.converted : item.raw) && original && *original == encode(item.raw) && model.raw == item.raw;
        if (!exact) std::cerr << "CLRO export case failed: " << index << " output=" << (converted ? *converted : "<failed>") << "\n";
        pass = pass && exact;
        ++index;
    }
    const auto model = note::MakeNoteTextModel({L"export.clro", L""}, L"# Heading\n\n$x＊y$", 11);
    const auto doc = note::ParseNoteDocument(model);
    note::MarkupExportConfig config;
    config.includeTitleHeading = true;
    config.title = L"Title";
    config.mathMode = note::ExportMarkupMathMode::Placeholder;
    const auto adjusted = note::ExportMarkdown(model, doc, config);
    pass = pass && adjusted && *adjusted == "# Title\n\n## Heading\n\n[math]";
    const auto md = note::MakeNoteTextModel({L"export.md", L""}, L"＃Heading\n＊＊bold＊＊", 12);
    const auto unchanged = note::ExportMarkdown(md, note::ParseNoteDocument(md), {});
    return pass && unchanged && *unchanged == encode(md.raw);
}

} // namespace note_fullwidth_test
