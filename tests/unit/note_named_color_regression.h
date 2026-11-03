#pragma once

#include "note_font_size_regression.h"
#include "note/note_export.h"
#include "note/note_markup_color.h"
#include "note/note_parser.h"

namespace note_named_color_test {
struct Result { bool vocabulary = true, exports = true, pixels = true; size_t aliases = 0, paints = 0; };

[[nodiscard]] inline Result Run() {
    Result result;
    struct Case { const wchar_t* names; uint32_t rgb; const char* hex; };
    const Case cases[] = {
        {L"black|黒|黒色", 0x000000, "000000"}, {L"white|白|白色", 0xffffff, "ffffff"},
        {L"gray|grey|灰|灰色", 0x808080, "808080"}, {L"lightgray|lightgrey|薄灰|薄灰色", 0xd3d3d3, "d3d3d3"},
        {L"red|赤|赤色", 0xff0000, "ff0000"}, {L"orange|オレンジ|オレンジ色|橙|橙色", 0xffa500, "ffa500"},
        {L"yellow|黄|黄色", 0xffff00, "ffff00"}, {L"green|緑|緑色", 0x008000, "008000"},
        {L"lime|ライム|明るい緑|明るい緑色", 0x00ff00, "00ff00"}, {L"blue|青|青色", 0x0000ff, "0000ff"},
        {L"navy|紺|紺色", 0x000080, "000080"}, {L"cyan|シアン|水色", 0x00ffff, "00ffff"},
        {L"purple|紫|紫色", 0x800080, "800080"}, {L"magenta|マゼンタ|赤紫|赤紫色", 0xff00ff, "ff00ff"},
        {L"pink|ピンク|ピンク色|桃色", 0xffc0cb, "ffc0cb"}, {L"brown|茶|茶色", 0xa52a2a, "a52a2a"},
        {L"lightred|薄赤|薄赤色", 0xffe4e4, "ffe4e4"},
        {L"lightorange|薄橙|薄橙色|薄オレンジ", 0xfff0db, "fff0db"},
        {L"lightyellow|薄黄|薄黄色", 0xfff9c4, "fff9c4"},
        {L"lightgreen|薄緑|薄緑色", 0xe5f3e5, "e5f3e5"},
        {L"lightblue|薄青|薄青色", 0xe3f0ff, "e3f0ff"},
        {L"lightpurple|薄紫|薄紫色", 0xf0e6fa, "f0e6fa"},
        {L"lightpink|薄桃|薄桃色|薄ピンク", 0xfce4ec, "fce4ec"},
    };
    for (const auto& test : cases) {
        std::wstring_view names(test.names);
        while (!names.empty()) {
            const size_t separator = names.find(L'|');
            std::wstring name(names.substr(0, separator));
            for (int variant = 0; variant < 2; ++variant) {
                if (variant) for (auto& c : name) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
                const auto color = note::ResolveNoteMarkupColor(L" \t" + name + L"\r\n");
                const bool correct = color.kind == note::NoteMarkupColorKind::Explicit && color.rgb == test.rgb;
                result.vocabulary = correct && result.vocabulary;
                note::NoteTextModel model;
                const auto raw = L"<char=" + name + L",back=" + name + L">TOKEN</>";
                model.raw = raw;
                model.line_starts = note::BuildLineStarts(raw);
                const auto doc = note::ParseNoteDocument(model);
                const auto html = note::ExportHtml(model, doc, {});
                const auto markdown = note::ExportMarkdown(model, doc, {});
                note::TextExportConfig rawConfig;
                rawConfig.markupMode = note::ExportTextMarkupMode::Raw;
                const bool correctExport = html.find(std::string("color:#") + test.hex + ";") != std::string::npos &&
                    html.find(std::string("background-color:#") + test.hex + ";") != std::string::npos &&
                    note::ExportPlainText(model, doc, {}) == "TOKEN\n" && model.raw == raw && markdown &&
                    *markdown == note::ExportPlainText(model, doc, rawConfig);
                if (!correct || !correctExport) std::wcerr << L"named colour failed: " << name << L'\n';
                result.exports = correctExport && result.exports;
            }
            ++result.aliases;
            if (separator == std::wstring_view::npos) break;
            names.remove_prefix(separator + 1);
        }
    }
    for (auto invalid : {L"", L"redder", L"RED色", L"薄ブルー", L"#red", L"0xblue", L"#ffff", L"#1234567",
                         L"#gg0000", L"red;position:fixed", L"transparent", L"inherit", L"light blue"})
        result.vocabulary = note::ResolveNoteMarkupColor(invalid).kind == note::NoteMarkupColorKind::Inherit && result.vocabulary;
    for (auto special : {L"hicont", L"HI-CONT", L"hi-contrast", L"highcontrast", L"HIGH-CONTRAST", L"高コントラスト"}) {
        // HI-CONT has never been an accepted spelling.
        const auto expected = std::wstring_view(special) == L"HI-CONT" ? note::NoteMarkupColorKind::Inherit : note::NoteMarkupColorKind::HighContrast;
        result.vocabulary = note::ResolveNoteMarkupColor(special).kind == expected && result.vocabulary;
    }
    for (auto numeric : {L"1a73E8", L"#1A73e8", L"0x1a73e8", L" 0X1A73E8 "}) {
        const auto color = note::ResolveNoteMarkupColor(numeric);
        result.vocabulary = color.kind == note::NoteMarkupColorKind::Explicit && color.rgb == 0x1a73e8 && result.vocabulary;
    }

    note_font_test::Surface surface;
    LOGFONTW description{};
    description.lfHeight = -24;
    description.lfQuality = NONANTIALIASED_QUALITY;
    std::copy_n(L"Segoe UI", 9, description.lfFaceName);
    note_font_test::Font font(surface.dc, description);
    if (!surface.valid() || !font.valid()) { result.pixels = false; return result; }
    note::NoteTextCore core;
    note::NoteRenderFinalCompleteBuildInput input;
    input.content_kind = note::NoteContentKind::Markdown;
    input.layout_key = {384, 96, 96, 1, 4, 8, true};
    input.owner_input.render_active = true;
    input.owner_input.editor_text_core_current = true;
    note::NoteRenderFinalGdiMeasurementProvider provider(surface.dc);
    const auto paint = [&](const std::wstring& raw, std::vector<uint32_t>* pixels) {
        core.Reset(note::NoteId{14000}, {L"colors.clro", L"colors"}, raw, 1, 1);
        input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> frame;
        if (note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &frame) !=
                note::NoteRenderFinalCompleteBuildResult::Built || !frame) return false;
        surface.clear();
        note::NoteRenderFinalGdiPaintOptions options;
        options.theme.background = RGB(255, 255, 255);
        if (!note::NoteRenderFinalGdiPainter::Paint(surface.dc, *frame, options) || !core.MatchesRaw(raw)) return false;
        GdiFlush();
        const auto* begin = static_cast<const uint32_t*>(surface.pixels);
        pixels->assign(begin, begin + surface.width * surface.height);
        return true;
    };
    for (const auto& test : cases) {
        const std::wstring_view aliases(test.names);
        const auto englishEnd = aliases.find(L'|');
        const std::wstring english(aliases.substr(0, englishEnd));
        const auto japaneseStart = aliases.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz|", englishEnd);
        const auto japaneseEnd = aliases.find(L'|', japaneseStart);
        const std::wstring japanese(aliases.substr(japaneseStart, japaneseEnd - japaneseStart));
        std::wstring hex;
        for (const char* p = test.hex; *p; ++p) hex.push_back(*p);
        for (const auto& alias : {english, japanese}) for (const auto* attribute : {L"char", L"back"}) {
            const auto markup = [&](const auto& value) { return L"<" + std::wstring(attribute) + L"=" + value + L">MMMM</>"; };
            std::vector<uint32_t> actual, expected;
            const bool matches = paint(markup(alias), &actual) && paint(markup(hex), &expected) && actual == expected;
            if (!matches) std::wcerr << L"named colour pixels failed: " << alias << L" " << attribute << L'\n';
            result.pixels = matches && result.pixels;
            ++result.paints;
        }
    }
    for (const auto& pair : {std::pair{L"<char=赤>$x$</>", L"<char=ff0000>$x$</>"},
                            std::pair{L"| H |\n| --- |\n| <back=薄青>MMMM</> |", L"| H |\n| --- |\n| <back=e3f0ff>MMMM</> |"},
                            std::pair{L"<char=青>A<char=赤>B</>C</>", L"<char=0000ff>A<char=ff0000>B</>C</>"},
                            std::pair{L"<char=高コントラスト,back=薄黄>MMMM</>", L"<char=hicont,back=fff9c4>MMMM</>"}}) {
        std::vector<uint32_t> actual, expected;
        result.pixels = (paint(pair.first, &actual) && paint(pair.second, &expected) && actual == expected) && result.pixels;
        ++result.paints;
    }
    return result;
}
} // namespace note_named_color_test
