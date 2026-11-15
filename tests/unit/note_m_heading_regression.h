#pragma once

#include "note/note_parser.h"
#include "note/note_export.h"
#include "note/note_syntax_lexical.h"
#include "note/note_render_final_win32_adapter.h"
#include <algorithm>
#include <iostream>

namespace note_m_heading_test {
struct Result {
    bool parsing = true, literals = true, lineHeight = true, geometry = true, exports = true;
    size_t cases = 0;
};
inline const note::BlockNode* Heading(const note::NoteDocument& doc) {
    const auto found = std::find_if(doc.blocks.begin(), doc.blocks.end(), [](const auto& block) {
        return block.kind == note::BlockKind::Heading;
    });
    return found == doc.blocks.end() ? nullptr : &*found;
}
inline void Check(bool& group, bool condition, const std::wstring& label) {
    if (!condition) std::wcerr << L"m heading: " << label << L'\n';
    group = group && condition;
}
inline Result Run(HDC dc) {
    Result result;
    for (int tier = 1; tier <= 6; ++tier) {
        const std::wstring name = tier == 1 ? L"m" : L"m" + std::to_wstring(tier - 1);
        for (const auto& close : {std::wstring{}, std::wstring(L"</>"), L"</" + name + L">"}) {
            ++result.cases;
            const auto raw = L"<" + name + L">Heading" + close + L"\r\nBody";
            const auto model = note::MakeNoteTextModel({L"alias.clro", L""}, raw, 1);
            const auto doc = note::ParseNoteDocument(model);
            const auto* heading = Heading(doc);
            Check(result.parsing, heading && heading->level == tier &&
                heading->origin == note::BlockOrigin::LegacyHeadingTag, name + L" tier");
            const size_t body = raw.find(L"Body");
            Check(result.parsing, std::any_of(doc.style_spans.begin(), doc.style_spans.end(), [&](const auto& style) {
                return style.kind == note::StyleKind::Indent && style.value == std::to_wstring(tier - 1) &&
                    style.span.start.value <= body && style.span.end.value >= body + 4;
            }), name + L" body indent");
            Check(result.lineHeight, std::none_of(doc.style_spans.begin(), doc.style_spans.end(), [](const auto& style) {
                return style.kind == note::StyleKind::LineHeight;
            }), name + L" is not line height");
            const auto text = note::ExportPlainText(model, doc, {});
            const auto html = note::ExportHtml(model, doc, {});
            Check(result.exports, text.find("Heading") < text.find("Body") &&
                text.find("Heading", text.find("Heading") + 1) == std::string::npos &&
                text.find('<') == std::string::npos && html.find("<h" + std::to_string(tier) + ">Heading</h") != std::string::npos,
                name + L" text/HTML content");

            note::NoteTextCore core;
            core.Reset(note::NoteId{static_cast<uint64_t>(2100 + tier)}, model.meta, raw, 500, 1);
            note::NoteRenderFinalCompleteBuildInput input;
            input.content_kind = note::NoteContentKind::Markdown;
            input.layout_key = {640, 96, 96, 1, 4, 8, false};
            input.owner_input.render_active = true;
            input.owner_input.editor_text_core_current = true;
            input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
            note::NoteRenderFinalGdiMeasurementProvider provider(dc);
            std::shared_ptr<const note::NoteRenderFinalPublication> publication;
            const auto built = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &publication);
            bool geometry = built == note::NoteRenderFinalCompleteBuildResult::Built && publication;
            if (geometry) {
                note::NoteRenderFinalWin32AdapterInput adapter;
                adapter.text_core = &core;
                adapter.structural_publication = publication;
                adapter.owner_input = input.owner_input;
                adapter.editor_selection = {{body}, {body}};
                adapter.editor_caret = {body};
                adapter.measurement_dc = dc;
                for (bool rawLine : {false, true}) {
                    if (rawLine) {
                        adapter.owner_input.requested_editor_lines = {{{1}, {2}}};
                        adapter.owner_input.caret_line = 1;
                    }
                    std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> frame;
                    note::NoteRenderFinalCaretGeometry caret{};
                    note::NoteRenderFinalHit hit{};
                    geometry = geometry && note::NoteRenderFinalWin32AdapterFrame::Build(adapter, &frame) ==
                        note::NoteRenderFinalWin32AdapterBuildResult::Built && frame &&
                        frame->Caret(&caret) == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                        (tier != 1 || caret.x_px == 8) &&
                        frame->HitTest(caret.x_px, caret.top_px, &hit) == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                        hit.source_offset.value == body;
                }
            }
            Check(result.geometry, geometry && core.CopyRawRange({0}, core.text_length()) == raw, name + L" final frame/raw handoff");
        }
    }
    for (const auto& raw : {L"<m>\nHeading</m>\nBody", L"<M1>Heading</M1>\nBody"}) {
        ++result.cases;
        const auto model = note::MakeNoteTextModel({L"alias.clro", L""}, raw, 1);
        const auto doc = note::ParseNoteDocument(model);
        Check(result.parsing, Heading(doc) != nullptr && note::ExportPlainText(model, doc, {}).find("Heading") != std::string::npos,
            L"next-line/case-insensitive heading");
    }
    for (const auto& raw : {L"<m=5>height</>", L"<m+5>height</>", L"before <m>height</>"}) {
        ++result.cases;
        const auto doc = note::ParseNoteDocument(note::MakeNoteTextModel({L"alias.clro", L""}, raw, 1));
        Check(result.lineHeight, !Heading(doc) && std::any_of(doc.style_spans.begin(), doc.style_spans.end(), [](const auto& style) {
            return style.kind == note::StyleKind::LineHeight;
        }), L"existing line-height syntax");
    }
    for (const auto& raw : {L"<m0>literal</>", L"<m6>literal</>", L"<m999999999999999>literal</>",
            L"text <m1>literal</>", L"`<m>literal</>`", L"```\n<m>literal</>\n```",
            L"$$\n<m>literal</>\n$$", L"\\<m>literal</>"}) {
        ++result.cases;
        const auto doc = note::ParseNoteDocument(note::MakeNoteTextModel({L"alias.clro", L""}, raw, 1));
        Check(result.literals, !Heading(doc), raw);
    }
    result.cases += 2;
    auto fullwidth = note::MakeNoteTextModel({L"alias.clro", L""}, L"＜ｍ１＞見出し＜／ｍ１＞\n本文", 1);
    auto doc = note::ParseNoteDocument(fullwidth);
    Check(result.parsing, Heading(doc) && Heading(doc)->level == 2 && doc.uses_clro_fullwidth_markdown, L"fullwidth tag");
    fullwidth.meta.clro_ascii_only = true;
    Check(result.literals, !Heading(note::ParseNoteDocument(fullwidth)), L"ASCII-only setting");
    ++result.cases;
    const auto numericValue = note::ParseNoteDocument(note::MakeNoteTextModel(
        {L"alias.clro", L""}, L"<m=５>height</>", 1));
    Check(result.lineHeight, std::any_of(numericValue.style_spans.begin(), numericValue.style_spans.end(), [](const auto& style) {
        return style.kind == note::StyleKind::LineHeight && style.value == L"５";
    }), L"attribute numbers are not folded");
    for (const auto* raw : {
            L"# Top\ntop\n## Mid\nmid\n<d=3>explicit</>\n</##>\nback\n###### Deep\ndeep\n</#>\nplain\n<h1>Legacy</>\nlegacy",
            L"::: note\n<m1>Heading</>\nBody\n:::\nTail",
            L"<m>**Heading** $x$</>\nBody",
            L"<m>\nHeading</m>\nBody"}) {
        ++result.cases;
        note::NoteTextCore core;
        core.Reset(note::NoteId{2200}, note::NoteMetadata{L"mixed.clro", L""}, raw, 501, 1);
        note::NoteRenderFinalCompleteBuildInput input;
        input.content_kind = note::NoteContentKind::Markdown;
        input.layout_key = {640, 96, 96, 1, 4, 8, false};
        input.owner_input.render_active = true;
        input.owner_input.editor_text_core_current = true;
        input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
        note::NoteRenderFinalGdiMeasurementProvider provider(dc);
        std::shared_ptr<const note::NoteRenderFinalPublication> publication;
        Check(result.geometry, note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &publication) ==
            note::NoteRenderFinalCompleteBuildResult::Built && publication,
            std::wstring(L"mixed/container/math/next-line frame: ") + raw);
    }
    return result;
}
} // namespace note_m_heading_test
