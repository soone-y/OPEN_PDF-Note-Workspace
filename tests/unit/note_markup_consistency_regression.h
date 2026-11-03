#pragma once

#include "note_font_size_regression.h"
#include "note/note_export.h"
#include "note/note_parser.h"
#include "note/note_render_final_display_run.h"
#include "note/note_render_final_win32_adapter.h"

namespace note_markup_test {
struct Result { bool text = true, html = true, links = true, math = true, indent = true; };

[[nodiscard]] inline Result Run() {
    Result result;
    const auto exported = [&](const wchar_t* raw, bool html) {
        note::NoteTextModel model;
        model.raw = raw;
        model.line_starts = note::BuildLineStarts(model.raw);
        const auto doc = note::ParseNoteDocument(model);
        return html ? note::ExportHtml(model, doc, {}) : note::ExportPlainText(model, doc, {});
    };
    for (const auto* raw : {L"abc <unknown>text</unknown> xyz", L"abc </char> xyz", L"abc </> xyz"}) {
        const auto txt = exported(raw, false);
        const auto html = exported(raw, true);
        result.text = result.text && txt.find("</") != std::string::npos;
        result.html = result.html && html.find("&lt;/") != std::string::npos;
    }
    const std::pair<const wchar_t*, const char*> textCases[] = {
        {L"<u>mark</u>", "mark\n"}, {L"<u>open", "open\n"},
        {L"`<u>literal</u>`", "<u>literal</u>\n"}, {L"```\n<u>literal</u>\n```", "<u>literal</u>\n"},
        {L"a `code` b `more` c", "a code b more c\n"},
        {L"`line1\nline2`", "line1\nline2\n"}
    };
    for (const auto& test : textCases) {
        const auto txt = exported(test.first, false);
        if (txt != test.second) std::cerr << "markup: text mismatch actual=[" << txt << "] expected=[" << test.second << "]\n";
        result.text = result.text && txt == test.second;
    }
    const auto code = exported(L"```\n<u>literal</u>\n```", true);
    result.html = result.html && code.find("&lt;u&gt;literal&lt;/u&gt;") != std::string::npos;
    struct CssCase { const wchar_t* raw; const char* wanted; const char* forbidden; };
    const CssCase cssCases[] = {
        {L"<s=24>size</>", "font-size:24px;", "font-size:24;"},
        {L"<s=-9>size</>", "font-size:7px;", "font-size:-9;"},
        {L"<s=30><s=-9>size</></>", "font-size:21px;", "font-size:30px;"},
        {L"<s=+3><s=+3>size</></>", "font-size:22px;", "font-size:19px;"},
        {L"<s=-999>size</>", "font-size:6px;", "font-size:-999;"},
        {L"<s=999999999999999>size</>", "font-size:256px;", "font-size:999"},
        {L"<char=ff0000>A<char=00ff00>B</>C</>", "color:#00ff00;\">B", "color:#00ff00;color:#ff0000;"},
        {L"<back=ff0000>A<back=00ff00>B</>C</>", "background-color:#00ff00;\">B", "background-color:#00ff00;background-color:#ff0000;"},
        {L"<u,x>both</>", "text-decoration:underline line-through;", "text-decoration:underline;text-decoration:"},
        {L"<m=5>tall</>", "line-height:2.47500;", "line-height:5;"},
        {L"<back=\"url(local.png)\">safe</>", "safe", "url(local.png)"},
        {L"<char=HICONT>contrast</>", "color:CanvasText;", "color:HICONT"},
        {L"<char=\"red;position:fixed\">safe</>", "safe", "position:fixed"},
        {L"<font=\"Yu Gothic\">font</>", "font-family:&quot;Yu Gothic&quot;;", "font-family:Yu Gothic;"}
    };
    for (const auto& test : cssCases) {
        const auto html = exported(test.raw, true);
        if (html.find(test.wanted) == std::string::npos || html.find(test.forbidden) != std::string::npos) {
            std::cerr << "markup: CSS mismatch wanted=" << test.wanted << '\n';
            result.html = false;
        }
    }
    note_font_test::Surface surface;
    LOGFONTW description{};
    description.lfHeight = -24;
    description.lfWeight = FW_NORMAL;
    description.lfQuality = NONANTIALIASED_QUALITY;
    std::copy_n(L"Segoe UI", 9, description.lfFaceName);
    note_font_test::Font font(surface.dc, description);
    if (!surface.valid() || !font.valid()) return {false, false, false, false, false};
    note::NoteTextCore core;
    note::NoteRenderFinalCompleteBuildInput input;
    input.content_kind = note::NoteContentKind::Markdown;
    input.layout_key = {320, 96, 96, 1, 4, 8, true};
    input.owner_input.render_active = true;
    input.owner_input.editor_text_core_current = true;
    note::NoteRenderFinalGdiMeasurementProvider provider(surface.dc);
    const auto build = [&](const wchar_t* raw) {
        core.Reset(note::NoteId{13000}, {L"markup.clro", L"markup"}, raw, 1, 1);
        input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> frame;
        const auto built = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &frame);
        if (built != note::NoteRenderFinalCompleteBuildResult::Built) {
            std::cerr << "markup: frame failed result=" << int(built) << " raw=";
            for (const auto* ch = raw; *ch; ++ch) std::cerr << char(*ch);
            std::cerr << '\n';
            frame.reset();
        }
        return frame;
    };
    const auto link = build(L"<l=outer>A<l=inner>MMMM</>C</>");
    if (!link) result.links = false;
    else {
        note::NoteRenderSourceLinePlan source;
        note::NoteRenderLinePlacement placement;
        note::NoteRenderFinalWin32AdapterInput adapterInput;
        adapterInput.text_core = &core;
        adapterInput.structural_publication = link;
        adapterInput.owner_input = input.owner_input;
        adapterInput.measurement_dc = surface.dc;
        std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> adapter;
        result.links = note::NoteRenderFinalWin32AdapterFrame::Build(adapterInput, &adapter) ==
            note::NoteRenderFinalWin32AdapterBuildResult::Built && adapter &&
            link->source_plan()->ResolveLine({0}, &source) && link->placement()->ResolveLine({0}, &placement);
        size_t tested = 0;
        for (size_t i = 0; result.links && i < source.runs.size(); ++i) {
            note::NoteRenderFinalDisplayRun display;
            if (note::BuildNoteRenderFinalDisplayRun(*link->source_plan()->syntax(), source.runs[i], &display) !=
                note::NoteRenderFinalDisplayRunBuildResult::Built) { result.links = false; break; }
            if (display.display_text != L"MMMM" && display.display_text != L"C") continue;
            const auto& placed = placement.runs[i];
            note::NoteRenderFinalHit presented;
            const auto expected = display.display_text == L"MMMM" ? L"inner" : L"outer";
            result.links = adapter->HitTest(placed.x_px + placed.width_px/2, 0, &presented) ==
                note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                presented.link_target == expected;
            if (!result.links) std::wcerr << L"markup: link expected=" << expected
                << L" presented=" << presented.link_target << L'\n';
            ++tested;
        }
        result.links = result.links && tested == 2;
    }
    const auto bare = build(L"$x$");
    const auto styled = build(L"<char=ff0000,s=40,u,l=math>$x$</>");
    const auto height = [](const auto& frame, size_t line) -> uint32_t {
        if (!frame) return 0;
        const auto row = note::NoteRenderLineLayoutMap::LineAt(frame->layout()->line_layouts(), {line});
        return row ? row->layout.height_px : 0;
    };
    result.math = bare && styled && height(styled, 0) > height(bare, 0);
    if (styled) {
        note::NoteRenderSourceLinePlan source;
        note::NoteRenderLinePlacement placed;
        note::NoteRenderFinalWin32AdapterInput adapterInput;
        adapterInput.text_core = &core;
        adapterInput.structural_publication = styled;
        adapterInput.owner_input = input.owner_input;
        adapterInput.measurement_dc = surface.dc;
        std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> adapter;
        result.math = result.math &&
            note::NoteRenderFinalWin32AdapterFrame::Build(adapterInput, &adapter) ==
                note::NoteRenderFinalWin32AdapterBuildResult::Built && adapter &&
            styled->source_plan()->ResolveLine({0}, &source) &&
            styled->placement()->ResolveLine({0}, &placed);
        bool found = false;
        for (size_t i = 0; result.math && i < source.runs.size(); ++i) {
            if (source.runs[i].kind != note::NoteRenderSourceRunKind::InlineMath) continue;
            note::NoteRenderFinalHit hit;
            result.math = source.runs[i].styles.size() == 4 && adapter->HitTest(
                placed.runs[i].x_px + placed.runs[i].width_px/2, 0, &hit) ==
                note::NoteRenderFinalPresentationInteractionResult::Resolved && hit.link_target == L"math";
            found = true;
        }
        surface.clear();
        note::NoteRenderFinalGdiPaintOptions paint;
        paint.theme.background = RGB(255,255,255);
        result.math = result.math && found && note::NoteRenderFinalGdiPainter::Paint(surface.dc, *styled, paint);
        GdiFlush();
        bool red = false;
        for (int y = 0; y < surface.height; ++y) for (int x = 0; x < surface.width; ++x) {
            const auto pixel = GetPixel(surface.dc, x, y);
            red = red || (pixel != CLR_INVALID && GetRValue(pixel) > 100 && GetGValue(pixel) < 80 && GetBValue(pixel) < 80);
        }
        result.math = result.math && red;
    }
    const auto block = build(L"$$x$$");
    const auto blockStyle = build(L"<m=5,d=5>\n$$x$$\n</>");
    result.math = result.math && block && blockStyle && height(blockStyle, 1) > height(block, 0);
    const auto sizedBlock = build(L"<s=40>\n$$x$$\n</>");
    const auto indentedBlock = build(L"<s=40,d=5>\n$$x$$\n</>");
    note::NoteRenderLinePlacement sizedPlacement, indentedPlacement;
    SIZE baseSpace{};
    const bool indentGeometry = sizedBlock && indentedBlock &&
        sizedBlock->placement()->ResolveLine({1}, &sizedPlacement) &&
        indentedBlock->placement()->ResolveLine({1}, &indentedPlacement) &&
        !sizedPlacement.runs.empty() && !indentedPlacement.runs.empty() &&
        GetTextExtentPoint32W(surface.dc, L" ", 1, &baseSpace) &&
        indentedPlacement.runs[0].x_px - sizedPlacement.runs[0].x_px == 5 * baseSpace.cx;
    if (!indentGeometry) std::cerr << "markup: sized math must use the common base-font indent unit\n";
    result.math = result.math && indentGeometry;
    const auto table = build(L"| H |\n| --- |\n| <char=ff0000>$x$</> |");
    result.math = result.math && bool(table);
    const auto tallTable = build(L"| H |\n| --- |\n| <s=40,m=5>$\\frac{x}{y}$</> |");
    result.math = result.math && tallTable && height(tallTable, 2) > height(table, 2);
    const auto wrappedBlock = build(L"<s=40>$$x$$</>");
    result.math = result.math && bool(wrappedBlock);
    const auto baseline = build(L"MMMM");
    for (bool wrap : {false, true}) {
        input.layout_key.word_wrap = wrap;
        for (const auto* raw : {L"<d=200>MMMM</>", L"<d=2147483647>MMMM</>", L"<d=-2147483648>MMMM</>",
                                L"<d=9999999999999999999999>MMMM</>", L"<d=\"right 2147483647\">MMMM</>"}) {
            const auto frame = build(raw);
            result.indent = result.indent && frame && core.MatchesRaw(raw) && height(frame, 0) == height(baseline, 0);
        }
        for (const auto* raw : {L"- <d=-2147483648>MMMM</>", L"> <d=-2147483648>MMMM</>",
                                L"- [ ] <d=-2147483648>MMMM</>"}) {
            const auto frame = build(raw);
            result.indent = result.indent && frame && core.MatchesRaw(raw);
        }
    }
    return result;
}
} // namespace note_markup_test
