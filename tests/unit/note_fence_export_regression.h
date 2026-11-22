#pragma once

#include "note/note_export.h"
#include "note/note_kernel.h"
#include "note/note_parser.h"
#include "note/note_render_final_win32_adapter.h"

#include <algorithm>
#include <cwchar>
#include <iostream>

namespace note_fence_export_test {
inline bool Run(HDC dc) {
    if (!dc) return false;
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label, size_t index) {
        ++checks;
        if (!value) std::cerr << "fence/export: " << label << " case=" << index << "\n";
        ok = ok && value;
    };
    struct FenceCase { const wchar_t* source; bool closed; const wchar_t* body; };
    const FenceCase fences[] = {
        {L"```\n```", true, L""}, {L"~~~\n~~~", true, L""},
        {L"```", false, L""}, {L"```\n", false, L""},
        {L"```cpp title=EMPTY\n```", true, L""},
        {L"``` title=EMPTY\n````", true, L""},
        {L"```cpp\ncpp\n```", true, L"cpp"},
        {L"```\n\n```", true, L""},
        {L"```\n\n", false, L""},
        {L"> ```\n> ```", true, L""},
        {L"- ```\n  ```", true, L""},
        {L"> - ```\n>   ```", true, L""},
        {L"::: box\n```\n```\n:::", true, L""},
        {L"<m>HEAD</m>\n```\n```", true, L""},
        {L"```cpp title=EMPTY\ncpp\n", false, L"cpp"},
    };
    size_t index = 0;
    for (const auto& test : fences) {
        for (const auto* file : {L"fence.clro", L"fence.md"}) {
            for (const auto* newline : {L"\n", L"\r", L"\r\n"}) {
                std::wstring raw = L"BEFORE\n\n";
                raw += test.source;
                if (test.closed) raw += L"\n\nAFTER";
                std::wstring converted;
                for (wchar_t ch : raw) {
                    if (ch == L'\n') converted += newline;
                    else converted.push_back(ch);
                }
                raw = std::move(converted);
                note::NoteTextCore core;
                core.Reset({3201}, {file, L""}, raw, 1, 1);
                const auto& model = core.model();
                const auto doc = note::ParseNoteDocument(model);
                size_t codeCount = 0;
                for (const auto& block : doc.blocks) {
                    if (block.kind != note::BlockKind::CodeBlock) continue;
                    ++codeCount;
                    check(block.code_fence_span.start.value == raw.find(L"```", raw.find(L"BEFORE")) ||
                          raw[block.code_fence_span.start.value] == L'~', "exact opening fence position", index);
                    check(block.code_fence_span.end > block.code_fence_span.start &&
                          block.code_fence_span.end.value <= raw.size() && block.fence_closed == test.closed,
                          "complete parser-owned syntax extent", index);
                    if (!test.closed) check(block.code_fence_span.end.value == raw.size(),
                        "unclosed fence extent includes final line transport", index);
                    const auto body = raw.substr(block.span.start.value, block.span.end - block.span.start);
                    check(body.find(L"```") == std::wstring::npos && body.find(L"title=") == std::wstring::npos &&
                          (std::wstring(test.body).empty() || body.find(test.body) != std::wstring::npos),
                          "literal body excludes fences and title", index);
                }
                check(codeCount == 1 && !note::NoteDocumentHasFatalParseFailure(doc), "one code owner", index);
                note::MarkupExportConfig config;
                const auto html = note::ExportHtml(model, doc, config);
                check(html.find("<pre>") != std::string::npos && html.find("```") == std::string::npos,
                      "HTML exports the literal body", index);
                if (raw.find(L"title=EMPTY") != std::wstring::npos) {
                    const auto title = html.find("EMPTY");
                    check(title != std::string::npos && html.find("EMPTY", title + 1) == std::string::npos,
                          "empty code title is exported once", index);
                }
                for (bool wrap : {false, true}) {
                    note::NoteRenderFinalWin32AdapterState state;
                    note::NoteRenderFinalWin32AdapterStateInput input;
                    input.text_core = &core;
                    input.layout_key = {wrap ? 160u : 640u, 96, 96, 1, 4, 8, wrap};
                    input.measurement_dc = dc;
                    input.owner_input.render_active = true;
                    input.owner_input.editor_text_core_current = true;
                    input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                    (void)state.Refresh(input);
                    check(state.has_frame(), "empty fence publishes a complete composed frame", index);
                    if (state.has_frame()) {
                        const auto plan = state.frame()->presentation()->structural_publication()->source_plan();
                        for (const auto& block : doc.blocks) {
                            if (block.kind != note::BlockKind::CodeBlock) continue;
                            const size_t first = note::ResolveLineColumn(model, block.code_fence_span.start.value).line - 1;
                            const size_t last = note::ResolveLineColumn(model, block.code_fence_span.end.value - 1).line - 1;
                            for (size_t line = first; line <= last; ++line) {
                                note::NoteRenderSourceLinePlan row;
                                const bool resolved = plan->ResolveLine({line}, &row);
                                check(resolved && row.decoration.has(note::NoteRenderSourceLineDecorationCodeBlock) &&
                                      row.decoration.has(note::NoteRenderSourceLineDecorationCodeFence) ==
                                        (line == first || (line == last && test.closed)),
                                      "source rows distinguish delimiters from literal body", index);
                            }
                        }
                    }
                }
                check(core.MatchesRaw(raw) && core.content_revision() == 1, "render and export preserve source", index);
                ++index;
            }
        }
    }

    struct ExportCase { const wchar_t* file; const wchar_t* source; const char* shifted; };
    const ExportCase exports[] = {
        {L"export.md", L"FIRST\nTARGET\n=\nTAIL", "# DOC\n\n## FIRST TARGET\nTAIL"},
        {L"export.md", L"FIRST\nTARGET\n--\nTAIL", "# DOC\n\n### FIRST TARGET\nTAIL"},
        {L"export.clro", L"FIRST\nTARGET\n=\nTAIL", "# DOC\n\nFIRST\nTARGET\n=\nTAIL"},
        {L"export.md", L"# **TARGET** ###\nTAIL", "# DOC\n\n## **TARGET** ###\nTAIL"},
        {L"export.md", L"> # **TARGET**\n> TAIL", "# DOC\n\n> ## **TARGET**\n> TAIL"},
        {L"export.md", L"- # **TARGET**\n  TAIL", "# DOC\n\n- ## **TARGET**\n  TAIL"},
        {L"export.md", L"> FIRST\n> **TARGET**\n> =\n> TAIL", "# DOC\n\n> ## FIRST **TARGET**\n> TAIL"},
        {L"export.md", L"###### TARGET\nTAIL", "# DOC\n\n###### TARGET\nTAIL"},
        {L"export.clro", L"<m>FIRST\n**TARGET**</m>\nTAIL", "# DOC\n\n## FIRST **TARGET**\nTAIL"},
        {L"export.clro", L"<m>FIRST\nTARGET\n</>\nTAIL", "# DOC\n\n## FIRST TARGET\nTAIL"},
        {L"export.clro", L"<m>\nFIRST\nTARGET</m>\nTAIL", "# DOC\n\n## FIRST TARGET\nTAIL"},
        {L"export.md", L"FIRST\nTARGET\n=", "# DOC\n\n## FIRST TARGET"},
        {L"export.md", L"1234567890. TARGET\n=", "# DOC\n\n## 1234567890. TARGET"},
        {L"export.md", L"# **# TARGET**", "# DOC\n\n## **# TARGET**"},
        {L"export.clro", L"<m><b>TARGET</b>\nTAIL", "# DOC\n\n## <b>TARGET</b>\nTAIL"},
    };
    index = 0;
    for (const auto& test : exports) {
        for (const auto* newline : {L"\n", L"\r", L"\r\n"}) {
            std::wstring raw;
            std::string expected;
            const std::string outputNewline(newline, newline + std::wcslen(newline));
            for (const wchar_t ch : std::wstring(test.source)) {
                if (ch == L'\n') raw += newline;
                else raw.push_back(ch);
            }
            // The prepended title has a stable LF separator. Existing source
            // line endings stay intact after the single rewritten heading row.
            for (size_t i = 0; test.shifted[i]; ++i) {
                if (test.shifted[i] == '\n' && i >= 7) expected += outputNewline;
                else expected.push_back(test.shifted[i]);
            }
            const auto model = note::MakeNoteTextModel({test.file, L""}, raw, 1);
            const auto doc = note::ParseNoteDocument(model);
            note::MarkupExportConfig config;
            config.includeTitleHeading = true;
            config.title = L"DOC";
            config.shiftHeadingLevels = true;
            const auto shifted = note::ExportMarkdown(model, doc, config);
            check(shifted && *shifted == expected, "shifted heading consumes its source once", index);
            if (shifted && *shifted != expected) std::cerr << "actual output=\n" << *shifted << "\n";
            config.shiftHeadingLevels = false;
            const auto preserved = note::ExportMarkdown(model, doc, config);
            check(preserved && *preserved == "# DOC\n\n" + std::string(raw.begin(), raw.end()),
                  "title without level shift preserves original rows", index);
            check(model.raw == raw && model.revision == 1, "Markdown export preserves source", index);
            ++index;
        }
    }

    note::LocalNoteKernel kernel;
    const std::wstring raw = L"BEFORE\n\n```cpp title=EMPTY\n```\n\nAFTER";
    kernel.Reset({3202}, {L"edit.clro", L""}, raw, 1, 1, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    const auto exactFence = [&]() {
        const auto& source = kernel.text_core().model().raw;
        const auto found = std::find_if(kernel.document().blocks.begin(), kernel.document().blocks.end(), [](const auto& block) {
            return block.kind == note::BlockKind::CodeBlock;
        });
        return kernel.CanReadSyntax() && found != kernel.document().blocks.end() &&
            found->code_fence_span.start.value == source.find(L"```") &&
            found->code_fence_span.end.value == source.rfind(L"```") + 3;
    };
    check(exactFence(), "persistent syntax stores empty fence extent", 0);
    const auto inserted = kernel.ApplyUserEdit({{raw.find(L"\n```") + 1}, 0, L"BODY\n"}, {}, {},
        note::NoteHistoryOperationKind::Typing, 1, true);
    (void)kernel.RefreshDerived();
    check(inserted.applied() && exactFence(), "empty-to-nonempty edit retains exact extent", 0);
    check(kernel.Undo(true).has_value(), "undo body insertion", 0);
    (void)kernel.RefreshDerived();
    check(exactFence() && kernel.text_core().MatchesRaw(raw), "undo restores empty fence", 0);
    check(kernel.Redo(true).has_value(), "redo body insertion", 0);
    (void)kernel.RefreshDerived();
    check(exactFence(), "redo restores fence coordinates", 0);
    const auto prefix = kernel.ApplyUserEdit({{0}, 0, L"PREFIX\n"}, {}, {}, note::NoteHistoryOperationKind::Typing, 2, true);
    (void)kernel.RefreshDerived();
    check(prefix.applied() && exactFence(), "prefix edit translates persistent fence coordinates", 0);
    std::cout << "Fence/export checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_fence_export_test
