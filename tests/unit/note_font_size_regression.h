#pragma once

#include "note/note_render_final_gdi_measurement_provider.h"
#include "note/note_render_final_gdi_painter.h"
#include "note/note_render_final_interaction.h"
#include "note/note_render_final_transaction.h"
#include "note/note_render_final_win32_adapter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <iostream>

namespace note_font_test {
// Private memory DCs only: no user windows, files, fonts or settings mutate.
struct Surface {
    static constexpr int width = 384, height = 96;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    void* pixels = nullptr;
    Surface() {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (dc) bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (bitmap) old = SelectObject(dc, bitmap);
        if (dc && old && old != HGDI_ERROR) (void)IntersectClipRect(dc, 0, 0, width, height);
    }
    ~Surface() {
        if (dc && old && old != HGDI_ERROR) (void)SelectObject(dc, old);
        if (bitmap) (void)DeleteObject(bitmap);
        if (dc) (void)DeleteDC(dc);
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    [[nodiscard]] bool valid() const { return dc && bitmap && pixels && old && old != HGDI_ERROR; }
    void clear() {
        RECT rect{0, 0, width, height};
        (void)FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }
};
struct Font {
    HDC dc;
    HFONT font;
    HGDIOBJ old = nullptr;
    Font(HDC target, const LOGFONTW& description) : dc(target), font(CreateFontIndirectW(&description)) {
        if (font) old = SelectObject(dc, font);
    }
    ~Font() {
        if (old && old != HGDI_ERROR) (void)SelectObject(dc, old);
        if (font) (void)DeleteObject(font);
    }
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    [[nodiscard]] bool valid() const { return font && old && old != HGDI_ERROR; }
};
struct Result { bool metrics = true, pixels = true, source = true; size_t cases = 0; };

[[nodiscard]] inline Result Run() {
    Surface actual, expected;
    if (!actual.valid() || !expected.valid()) {
        std::cerr << "font oracle: memory surface unavailable\n";
        return {false, false, false, 0};
    }
    Result result;
    struct Case { const wchar_t* text; int absolute, delta; bool bold, italic; };
    const std::array<Case, 12> cases{{
        {L"MMMM", 0, 0, false, false}, {L"**MMMM**", 0, 0, true, false},
        {L"<b>MMMM</>", 0, 0, true, false}, {L"*MMMM*", 0, 0, false, true},
        {L"<s=-9>MMMM</>", 0, -9, false, false}, {L"<s=-999>MMMM</>", 6, 0, false, false},
        {L"<s=9>MMMM</>", 9, 0, false, false}, {L"<s=+9>MMMM</>", 0, 9, false, false},
        {L"<s=30><s=-9>MMMM</></>", 21, 0, false, false},
        {L"<s=+3><s=+3>MMMM</></>", 0, 6, false, false},
        {L"<s=-9>**MMMM**</>", 0, -9, true, false},
        {L"<s=30>MMMM<s=-9>MMMM</>MMMM</>", 0, 0, false, false}
    }};
    for (const wchar_t* face : {L"Segoe UI", L"Meiryo"}) {
      for (LONG requestedHeight : {-24L, 32L}) {
        LOGFONTW base{};
        base.lfHeight = requestedHeight;
        base.lfWeight = FW_NORMAL;
        base.lfCharSet = DEFAULT_CHARSET;
        base.lfQuality = NONANTIALIASED_QUALITY;
        std::copy(face, face + std::wcslen(face) + 1, base.lfFaceName);
        Font baseFont(actual.dc, base);
        TEXTMETRICW tm{};
        if (!baseFont.valid() || !GetTextMetricsW(actual.dc, &tm) || tm.tmInternalLeading <= 0) {
            std::wcerr << L"font oracle: base font unavailable: " << face << L" height=" << requestedHeight
                       << L" leading=" << tm.tmInternalLeading << L" cases=" << result.cases << L"\n";
            return {false, false, false, result.cases};
        }
        const int em = tm.tmHeight - tm.tmInternalLeading; // Independent Win32 oracle.
        for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const auto& test = cases[caseIndex];
            note::NoteTextCore core;
            core.Reset(note::NoteId{1100}, {L"font.md", L"font"}, test.text, 1, 1);
            note::NoteRenderFinalCompleteBuildInput input;
            input.content_kind = note::NoteContentKind::Markdown;
            input.layout_key = {Surface::width, 96, 96, 1, 4, 8, false};
            input.owner_input.render_active = true;
            input.owner_input.editor_text_core_current = true;
            input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
            note::NoteRenderFinalGdiMeasurementProvider provider(actual.dc);
            std::shared_ptr<const note::NoteRenderFinalPublication> frame;
            const auto built = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &frame);
            if (built != note::NoteRenderFinalCompleteBuildResult::Built || !frame) {
                std::cerr << "font oracle: frame build=" << static_cast<int>(built) << " cases=" << result.cases << "\n";
                return {false, false, false, result.cases};
            }
            note::NoteRenderSourceLinePlan source;
            note::NoteRenderLinePlacement placement;
            if (!frame->source_plan()->ResolveLine({0}, &source) || !frame->placement()->ResolveLine({0}, &placement))
                return {false, false, false, result.cases};
            note::NoteRenderFinalWin32AdapterInput adapterInput;
            adapterInput.text_core = &core;
            adapterInput.structural_publication = frame;
            adapterInput.owner_input = input.owner_input;
            adapterInput.measurement_dc = actual.dc;
            std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> adapter;
            if (note::NoteRenderFinalWin32AdapterFrame::Build(adapterInput, &adapter) !=
                    note::NoteRenderFinalWin32AdapterBuildResult::Built || !adapter)
                return {false, false, false, result.cases};
            actual.clear(); expected.clear();
            note::NoteRenderFinalGdiPaintOptions paint;
            paint.theme.background = RGB(255, 255, 255);
            paint.theme.text = RGB(0, 0, 0);
            if (!note::NoteRenderFinalGdiPainter::Paint(actual.dc, *frame, paint)) result.pixels = false;
            size_t visible = 0;
            for (size_t i = 0; i < source.runs.size(); ++i) {
                const auto& run = source.runs[i];
                if (run.kind == note::NoteRenderSourceRunKind::HiddenSyntax) continue;
                LOGFONTW oracle = base;
                if (test.absolute || test.delta)
                    oracle.lfHeight = -std::clamp(test.absolute ? test.absolute : em + test.delta, 6, 256);
                if (caseIndex == cases.size() - 1) oracle.lfHeight = -(visible == 1 ? 21 : 30);
                oracle.lfWeight = test.bold ? FW_BOLD : FW_NORMAL;
                oracle.lfItalic = test.italic;
                Font font(expected.dc, oracle);
                SIZE measured{};
                const bool measuredOk = font.valid() && GetTextExtentPoint32W(expected.dc, L"MMMM", 4, &measured);
                if (!measuredOk || i >= placement.runs.size() || placement.runs[i].width_px != measured.cx) {
                    std::cerr << "font oracle: width mismatch case=" << result.cases << " run=" << i << "\n";
                    result.metrics = false;
                } else {
                    (void)SetTextColor(expected.dc, RGB(0, 0, 0));
                    (void)SetBkMode(expected.dc, TRANSPARENT);
                    if (!ExtTextOutW(expected.dc, placement.runs[i].x_px, 0, 0, nullptr, L"MMMM", 4, nullptr))
                        result.pixels = false;
                    note::NoteRenderFinalHit hit;
                    SIZE firstGlyph{};
                    const bool glyphOk = GetTextExtentPoint32W(expected.dc, L"M", 1, &firstGlyph);
                    // Interior character boundary: a shared edge beside hidden
                    // tags has multiple legal raw offsets, not a unique oracle.
                    result.source = result.source && adapter->HitTest(
                        placement.runs[i].x_px + firstGlyph.cx, 0, &hit) ==
                            note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                        glyphOk && hit.source_offset.value == run.source_span.start.value + 1;
                }
                ++visible;
            }
            (void)GdiFlush();
            result.metrics = result.metrics && visible == (caseIndex == cases.size() - 1 ? 3u : 1u);
            result.pixels = result.pixels && std::memcmp(actual.pixels, expected.pixels,
                Surface::width * Surface::height * 4) == 0;
            result.source = result.source && core.MatchesRaw(test.text) && GetCurrentObject(actual.dc, OBJ_FONT) == baseFont.font;
            ++result.cases;
        }
      }
    }
    return result;
}
} // namespace note_font_test
