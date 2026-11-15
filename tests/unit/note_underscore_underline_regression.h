#pragma once

#include "note/note_export.h"
#include "note/note_kernel.h"
#include "note/note_parser.h"
#include "note/note_render_source_plan.h"
#include "note/note_syntax_snapshot.h"

#include <algorithm>
#include <cwchar>
#include <iostream>

namespace note_underscore_test {
inline bool Run() {
    struct Case {
        const wchar_t* raw;
        int underline;
        int bold;
        int italic;
    };
    const Case cases[] = {
        {L"__text__", 1, 0, 0},
        {L"_text_", 0, 0, 1},
        {L"**text**", 0, 1, 0},
        {L"___text___", 1, 0, 1},
        {L"***text***", 0, 1, 1},
        {L"**__text__**", 1, 1, 0},
        {L"__**text**__", 1, 1, 0},
        {L"__*text*__", 1, 0, 1},
        {L"*__text__*", 1, 0, 1},
        {L"__left__ __right__", 2, 0, 0},
        {L"__first\nsecond__", 1, 0, 0},
        {L"- __item__", 1, 0, 0},
        {L"> __quote__", 1, 0, 0},
        {L"| Head |\n| --- |\n| __cell__ |", 1, 0, 0},
        {L"[__link__](local.md)", 1, 0, 0},
        {L"__`code`__", 1, 0, 0},
        {L"`__literal__`", 0, 0, 0},
        {L"```\n__literal__\n```", 0, 0, 0},
        {L"\\_\\_literal\\_\\_", 0, 0, 0},
        {L"__unclosed", 0, 0, 0},
        {L"word__inside__word", 0, 0, 0},
    };
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label, size_t index) {
        ++checks;
        if (!value) std::cerr << "underscore underline: " << label << " case=" << index << "\n";
        ok = ok && value;
    };
    for (const auto* file : {L"underline.clro", L"underline.CLRO", L"underline.md"}) {
        size_t index = 0;
        const bool clro = std::wstring_view(file) != L"underline.md";
        for (const auto& test : cases) {
            const auto model = note::MakeNoteTextModel({file, L""}, test.raw, 5);
            const auto doc = note::ParseNoteDocument(model);
            const auto count = [&](note::InlineKind kind) {
                return std::count_if(doc.inlines.begin(), doc.inlines.end(), [&](const auto& node) {
                    return node.kind == kind;
                });
            };
            check(count(note::InlineKind::Underline) == (clro ? test.underline : 0) &&
                  count(note::InlineKind::Strong) == test.bold + (clro ? 0 : test.underline) &&
                  count(note::InlineKind::Emphasis) == test.italic,
                  "CLRO underline and Markdown emphasis grammar", index);
            check(model.raw == test.raw && model.revision == 5 &&
                  std::all_of(doc.inlines.begin(), doc.inlines.end(), [&](const auto& node) {
                      return node.span.start <= node.span.end && node.span.end.value <= model.raw.size();
                  }), "original text and UTF-16 ranges", index);
            const auto markdown = note::ExportMarkdown(model, doc, {});
            check(markdown && *markdown == std::string(test.raw, test.raw + std::wcslen(test.raw)),
                  "Markdown source export", index);
            ++index;
        }
    }

    for (bool asciiOnly : {false, true}) {
        note::NoteMetadata metadata{L"underline.clro", L""};
        metadata.clro_ascii_only = asciiOnly;
        const auto model = note::MakeNoteTextModel(metadata, L"__plain__ **bold** _italic_", 8);
        const auto doc = note::ParseNoteDocument(model);
        check(note::ExportPlainText(model, doc, {}) == "plain bold italic\n" &&
              note::ExportHtml(model, doc, {}).find("<u>plain</u>") != std::string::npos,
              "TXT keeps content and HTML carries underline", asciiOnly);
        note::NoteTextCore core;
        core.Reset({2401}, metadata, model.raw, 8, 1);
        std::shared_ptr<const note::NoteSyntaxSnapshot> syntax;
        std::shared_ptr<const note::NoteRenderSourcePlan> plan;
        bool built = note::NoteSyntaxSnapshot::BuildFromTextCore(core, note::NoteContentKind::Markdown, &syntax) ==
            note::NoteSyntaxSnapshotBuildResult::Built && syntax &&
            note::NoteRenderSourcePlan::Build(syntax, &plan) == note::NoteRenderSourcePlanBuildResult::Built && plan;
        note::NoteRenderSourceLinePlan line;
        built = built && plan->ResolveLine({0}, &line);
        bool underline = false;
        for (const auto& run : line.runs) {
            if (run.source_span.start.value > 2 || run.source_span.end.value < 7) continue;
            underline = std::any_of(run.styles.begin(), run.styles.end(), [](const auto& style) {
                return style.kind == note::StyleKind::Underline;
            }) && std::none_of(run.styles.begin(), run.styles.end(), [](const auto& style) {
                return style.kind == note::StyleKind::Bold;
            });
        }
        check(built && underline, "final source plan applies underline without bold", asciiOnly);
    }
    {
        const auto model = note::MakeNoteTextModel({L"unicode.clro", L""}, L"__文字\U0001F600__", 1);
        const auto doc = note::ParseNoteDocument(model);
        check(note::ExportPlainText(model, doc, {}) == u8"文字\U0001F600\n" &&
              note::ExportHtml(model, doc, {}).find(u8"<u>文字\U0001F600</u>") != std::string::npos,
              "Japanese and surrogate-pair content", 0);
        note::LocalNoteKernel kernel;
        kernel.Reset({2402}, {L"editing.clro", L""}, L"__text__", 1, 1, note::NoteContentKind::Markdown);
        (void)kernel.RefreshDerived();
        const auto removed = kernel.Apply({{0}, 1, L""}, true);
        const auto afterRemove = kernel.RefreshDerived();
        check(removed.applied() && afterRemove.current &&
              std::none_of(kernel.document().inlines.begin(), kernel.document().inlines.end(), [](const auto& node) {
                  return node.kind == note::InlineKind::Underline;
              }), "deleting one delimiter updates derived formatting", 0);
        const auto restored = kernel.Apply({{0}, 0, L"_"}, true);
        const auto afterRestore = kernel.RefreshDerived();
        check(restored.applied() && afterRestore.current &&
              std::any_of(kernel.document().inlines.begin(), kernel.document().inlines.end(), [](const auto& node) {
                  return node.kind == note::InlineKind::Underline;
              }), "restoring delimiter restores underline", 0);
    }
    std::cout << "Underscore underline checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_underscore_test
