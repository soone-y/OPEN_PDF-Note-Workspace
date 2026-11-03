// Actual PDF engine and Win32 selection path, with a clipboard API sink.
// Production allocation/encoding/ownership code runs; the user's OS clipboard
// is never opened. This is not an OS clipboard interoperability test.
#include <windows.h>
#include <string>
#include <cstring>
HGLOBAL clipboard_payload = nullptr;
bool clipboard_open = false;
bool clipboard_busy = false;
BOOL TestOpenClipboard(HWND) { if (clipboard_busy) return FALSE; clipboard_open = true; return TRUE; }
BOOL TestEmptyClipboard() {
    if (!clipboard_open) return FALSE;
    if (clipboard_payload) GlobalFree(clipboard_payload);
    clipboard_payload = nullptr;
    return TRUE;
}
HANDLE TestSetClipboardData(UINT format, HANDLE data) {
    if (!clipboard_open || format != CF_UNICODETEXT || !data) return nullptr;
    clipboard_payload = static_cast<HGLOBAL>(data);
    return data;
}
BOOL TestCloseClipboard() { clipboard_open = false; return TRUE; }
#define OpenClipboard TestOpenClipboard
#define EmptyClipboard TestEmptyClipboard
#define SetClipboardData TestSetClipboardData
#define CloseClipboard TestCloseClipboard
#include "readonly_viewer/pdf_search.cpp"
#undef OpenClipboard
#undef EmptyClipboard
#undef SetClipboardData
#undef CloseClipboard
#include <iostream>
#include <stdexcept>

namespace panel = readonly_viewer::pdf_panel;
using readonly_viewer::PdfRegionSelection;
using readonly_viewer::PdfRegionGlyph;

void Require(bool value, const char* label) { if (!value) throw std::runtime_error(label); }

std::wstring ClipboardText(HWND owner) {
    (void)owner;
    HANDLE data = clipboard_payload;
    const wchar_t* text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
    std::wstring value = text ? text : L"";
    if (text) GlobalUnlock(data);
    return value;
}

void PureSelectionTests() {
    PdfRegionSelection selection;
    const std::vector<PdfRegionGlyph> glyphs = {
        {'X', {0, 10, 9, 0}}, // Outside; may never enter the copy result.
        {'A', {10, 10, 19, 0}}, {'B', {20, 10, 29, 0}}, {'C', {30, 10, 39, 0}},
        {'\r', {}, false}, {'\n', {}, false},
        {0x65e5, {10, -2, 19, -12}}, {0x1f600, {20, -2, 29, -12}},
        {'Y', {40, 10, 49, 0}}};
    Require(selection.SetRegion({10, 10, 39, -12}, glyphs), "valid region prepared");
    Require(selection.Begin(10, 5), "start on A"); selection.Extend(39, 5); selection.Finish();
    Require(selection.SelectedText() == L"ABC", "forward exact substring excludes outside glyphs");
    Require(selection.Begin(38, 5), "reverse start"); selection.Extend(10, 5); selection.Finish();
    Require(selection.SelectedText() == L"ABC", "reverse drag preserves reading order");
    Require(selection.Begin(10, 5), "multiline start"); selection.Extend(29, -7); selection.Finish();
    const std::wstring expected = L"ABC\r\n日" + std::wstring{wchar_t(0xd83d), wchar_t(0xde00)};
    Require(selection.SelectedText() == expected, "multiline Unicode and surrogate pair copy");
    Require(selection.Begin(10, 5), "bounded drag start"); selection.Extend(1000, -1000); selection.Finish();
    Require(selection.SelectedText() == expected, "drag outside remains bounded to the region");
    Require(!selection.Begin(-100, 5), "outside start rejected");
    Require(!selection.SetRegion({0, 0, 0, 0}, glyphs), "invalid replacement rejected");
    Require(selection.SelectedText() == expected, "failed replacement preserves selection");
    selection.ClearText(); Require(!selection.HasText(), "second stage clears independently");
    Require(selection.Contains(15, 5), "frame remains after clearing text");
    Require(selection.SetRegion({0, 10, 40, 0}, {{0x05d0,{30,10,39,0}}, {0x05d1,{20,10,29,0}}, {0x05d2,{10,10,19,0}}}),
            "RTL region prepared");
    Require(selection.Begin(39, 5), "RTL reading start"); selection.Extend(10, 5); selection.Finish();
    Require(selection.SelectedText() == L"אבג", "RTL drag follows the source reading order");
    Require(selection.SetRegion({0, 40, 10, 0}, {{0x65e5,{0,39,10,30}}, {0x672c,{0,29,10,20}}, {0x8a9e,{0,19,10,10}}}),
            "vertical region prepared");
    Require(selection.Begin(5, 39), "vertical reading start"); selection.Extend(5, 10); selection.Finish();
    Require(selection.SelectedText() == L"日本語", "vertical drag follows the source reading order");
}

std::uint64_t DrawFingerprint(const panel::PageLayout& page) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 1000; info.bmiHeader.biHeight = -700;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Require(dc && bitmap && pixels, "selection drawing buffer allocated");
    HGDIOBJ original = SelectObject(dc, bitmap);
    PatBlt(dc, 0, 0, 1000, 700, WHITENESS);
    panel::DrawTextSelection(dc, page);
    GdiFlush();
    std::uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const unsigned char*>(pixels);
    for (std::size_t i = 0; i < 1000 * 700 * 4; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    SelectObject(dc, original); DeleteObject(bitmap); DeleteDC(dc);
    return hash;
}

int wmain(int argc, wchar_t** argv) {
    HWND parent = nullptr, viewer = nullptr;
    bool pdfium = false;
    try {
        Require(argc == 2, "explicit owned PDF fixture required");
        PureSelectionTests();
        FPDF_InitLibrary(); pdfium = true;
        parent = CreateWindowW(L"STATIC", L"Owned selection test", WS_OVERLAPPEDWINDOW,
                               0, 0, 1100, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(parent, "owned test parent created");
        viewer = readonly_viewer::CreatePdfPreviewPanel(parent, GetModuleHandleW(nullptr), 1002);
        Require(viewer, "actual PDF panel created");
        Require(MoveWindow(viewer, 0, 0, 1000, 700, FALSE), "PDF viewport established");
        readonly_viewer::PdfPreviewPanel_OpenPdf(viewer, argv[1]);
        Require(reader_next::FitPdfReadingWidth(viewer), "fixture opened and fitted");
        panel::PageLayout page{};
        Require(panel::PageLayoutForIndex(viewer, 0, page), "page layout available");
        auto point = [&](double x, double y) {
            return panel::PdfPointToClient(page.rect, page.pageWidthPt, page.pageHeightPt, x, y);
        };
        auto mouse = [&](UINT message, POINT p) { SendMessageW(viewer, message, MK_LBUTTON, MAKELPARAM(p.x, p.y)); };
        auto drag = [&](POINT start, POINT end) {
            mouse(WM_LBUTTONDOWN, start); mouse(WM_MOUSEMOVE, end); mouse(WM_LBUTTONUP, end);
        };
        drag(point(95, 755), point(290, 670));
        Require(panel::g_state.hasRectSelection, "first-stage frame committed");
        Require(panel::g_state.regionText.Glyphs().size() > 10, "real PDF text prepared in frame");
        Require(panel::g_state.selectedText.find(L"OUTSIDE") == std::wstring::npos, "frame excludes outside text");
        const auto frameDrawing = DrawFingerprint(page);
        auto findGlyph = [&](std::uint32_t unicode, std::size_t from) {
            const auto& glyphs = panel::g_state.regionText.Glyphs();
            for (std::size_t i = from; i < glyphs.size(); ++i) if (glyphs[i].unicode == unicode) return i;
            throw std::runtime_error("expected fixture character missing");
        };
        const auto begin = findGlyph('B', 0), end = findGlyph('A', begin + 1);
        const auto& first = panel::g_state.regionText.Glyphs()[begin].bounds;
        const auto& last = panel::g_state.regionText.Glyphs()[end].bounds;
        const POINT a = point(first.left + (first.right-first.left)*0.15, (first.top+first.bottom)/2);
        const POINT b = point(last.right - (last.right-last.left)*0.15, (last.top+last.bottom)/2);
        drag(a, b);
        Require(panel::g_state.hasRectSelection && panel::g_state.regionText.HasText(), "two selection stages coexist");
        Require(panel::g_state.regionText.SelectedText() == L"BETA", "native mouse path selects exact substring");
        Require(DrawFingerprint(page) != frameDrawing, "character selection visibly differs from the frame");
        SendMessageW(viewer, WM_COPY, 0, 0);
        Require(ClipboardText(viewer) == L"BETA", "clipboard payload contains only the selected substring");
        Require(panel::CopyUnicodeTextToClipboard(viewer, L"previous-copy"), "clipboard sentinel prepared");
        clipboard_busy = true; SendMessageW(viewer, WM_COPY, 0, 0); clipboard_busy = false;
        Require(ClipboardText(viewer) == L"previous-copy" && panel::g_state.regionText.SelectedText() == L"BETA",
                "busy clipboard preserves previous data and current selection");
        BYTE originalKeys[256]{}, copyKeys[256]{};
        Require(GetKeyboardState(originalKeys), "thread keyboard state captured");
        memcpy(copyKeys, originalKeys, sizeof(copyKeys)); copyKeys[VK_CONTROL] = 0x80;
        Require(SetKeyboardState(copyKeys), "Ctrl+C state set only on the test thread");
        SendMessageW(viewer, WM_KEYDOWN, 'C', 0);
        Require(SetKeyboardState(originalKeys), "test thread keyboard state restored");
        Require(ClipboardText(viewer) == L"BETA", "Ctrl+C dispatches the selected text copy");
        drag(b, a);
        Require(panel::g_state.regionText.SelectedText() == L"BETA", "native reverse selection");
        drag(a, point(400, 732));
        Require(panel::g_state.regionText.SelectedText().find(L"OUTSIDE") == std::wstring::npos,
                "native drag outside frame never copies outside text");
        drag(a, b);
        mouse(WM_MBUTTONDOWN, point(200, 730));
        mouse(WM_MOUSEMOVE, point(210, 730));
        mouse(WM_MBUTTONUP, point(210, 730));
        Require(!panel::g_state.panning && panel::g_state.regionText.SelectedText() == L"BETA" && panel::g_state.hasRectSelection,
                "panning preserves both selection stages");
        SendMessageW(viewer, WM_COMMAND, 1108, 0);
        mouse(WM_LBUTTONDOWN, point(200, 730)); mouse(WM_MOUSEMOVE, point(205, 730)); mouse(WM_LBUTTONUP, point(205, 730));
        Require(!panel::g_state.panning && panel::g_state.hasRectSelection, "left-button pan ends without clearing the frame");
        SendMessageW(viewer, WM_COMMAND, 1107, 0);
        Require(MoveWindow(viewer, 0, 0, 850, 650, FALSE), "selection viewport resized");
        Require(panel::g_state.regionText.SelectedText() == L"BETA", "layout preserves document-space text selection");
        SendMessageW(viewer, WM_KEYDOWN, VK_ESCAPE, 0);
        Require(panel::g_state.hasRectSelection && !panel::g_state.regionText.HasText(), "first Escape clears text only");
        SendMessageW(viewer, WM_COPY, 0, 0);
        Require(ClipboardText(viewer).find(L"ALPHA BETA GAMMA") != std::wstring::npos, "first-stage whole-frame copy preserved");
        SendMessageW(viewer, WM_KEYDOWN, VK_ESCAPE, 0);
        Require(!panel::g_state.hasRectSelection, "second Escape clears frame");
        SendMessageW(viewer, WM_COPY, 0, 0);
        Require(ClipboardText(viewer).find(L"ALPHA BETA GAMMA") != std::wstring::npos, "empty copy preserves clipboard");
        Require(panel::PageLayoutForIndex(viewer, 0, page), "resized page layout refreshed");
        drag(point(95, 670), point(290, 620));
        const auto jpBegin = findGlyph(0x65e5, 0), jpEnd = findGlyph(0x8a9e, jpBegin + 1);
        const auto& jpFirst = panel::g_state.regionText.Glyphs()[jpBegin].bounds;
        const auto& jpLast = panel::g_state.regionText.Glyphs()[jpEnd].bounds;
        drag(point(jpFirst.left + (jpFirst.right-jpFirst.left)*0.15, (jpFirst.top+jpFirst.bottom)/2),
             point(jpLast.right - (jpLast.right-jpLast.left)*0.15, (jpLast.top+jpLast.bottom)/2));
        SendMessageW(viewer, WM_COPY, 0, 0);
        Require(ClipboardText(viewer) == L"日本語", "actual PDF Japanese substring copies as UTF-16");
        DestroyWindow(parent); parent = viewer = nullptr;
        FPDF_DestroyLibrary(); pdfium = false;
        if (clipboard_payload) GlobalFree(clipboard_payload);
        clipboard_payload = nullptr;
        std::cout << "PASS: pure glyph boundaries/Unicode and actual PDF mouse/copy/Escape/layout; clipboard API sink (OS clipboard unverified)\n";
        return 0;
    } catch (const std::exception& error) {
        if (parent) DestroyWindow(parent);
        if (pdfium) FPDF_DestroyLibrary();
        if (clipboard_payload) GlobalFree(clipboard_payload);
        std::cerr << "FAIL: " << error.what() << " (Win32=" << GetLastError() << ")\n";
        return 1;
    }
}
