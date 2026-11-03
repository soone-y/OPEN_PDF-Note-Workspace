// Keep PDFium state in one translation unit. This adapter inspects the loaded
// document without exporting handles. Do not also link pdf_preview_panel.cpp.
#include "pdf_preview_panel.cpp"
#include "pdf_search.h"
#include "reader_chrome.h"

namespace reader_next {
PdfViewState CapturePdfView(HWND panel, const std::wstring& expected_path) {
    using namespace readonly_viewer::pdf_panel;
    if (!g_state.pdf || g_state.pdf->path != expected_path) return {};
    auto anchor = CapturePdfViewportAnchor(panel);
    if (!anchor.captured && g_state.resizeDocument == g_state.pdf.get()) anchor = g_state.resizeAnchor;
    return {g_state.zoomFactor, g_state.panX, g_state.scrollY,
            g_state.pageRangeFirst, g_state.pageRangeLast, true, anchor, g_state.viewRotation};
}
bool RestorePdfView(HWND panel, const PdfViewState& state) {
    using namespace readonly_viewer::pdf_panel;
    if (!state.captured || !g_state.pdf || g_state.pdf->pageCount <= 0 || !std::isfinite(state.zoom) ||
        !std::isfinite(state.pan_x) || !std::isfinite(state.scroll_y)) return false;
    g_state.zoomFactor = std::clamp(state.zoom, 0.25, 4.0);
    g_state.viewRotation = page_rotation::Normalize(state.rotation);
    ClearPendingPdfResizeAnchor();
    g_state.panX = state.pan_x;
    g_state.scrollY = std::max(0.0, state.scroll_y);
    g_state.pageRangeFirst = std::clamp(state.range_first, 0, g_state.pdf->pageCount - 1);
    g_state.pageRangeLast = state.range_last < 0 ? -1 :
        std::clamp(state.range_last, g_state.pageRangeFirst, g_state.pdf->pageCount - 1);
    if (RestorePdfViewportAnchor(panel, state.anchor)) return true;
    ClearRenderCaches();
    ClampContinuousScroll(panel);
    SyncCurrentPageFromScroll(panel);
    InvalidateRect(panel, nullptr, FALSE);
    return true;
}
void ConfigurePdfReadingStyle() {
    auto& theme = readonly_viewer::pdf_panel::g_state.theme;
    theme.pdfBg = kCanvas;
    theme.splitterLine = RGB(215, 223, 230);
    theme.panelText = kMuted;
}
bool FitPdfReadingWidth(HWND panel) {
    using namespace readonly_viewer::pdf_panel;
    if (!g_state.pdf || !g_state.pdf->document) return false;
    double width = 0, height = 0;
    if (!PageSizePt(g_state.currentPage, width, height)) return false;
    const RECT available = ViewerAvailableRect(panel);
    const double availableWidth = available.right - available.left;
    const double availableHeight = available.bottom - available.top;
    if (availableWidth <= 0 || availableHeight <= 0) return false;
    const double base = std::min(availableWidth / width, availableHeight / height);
    if (base <= 0) return false;
    g_state.zoomFactor = std::clamp((availableWidth / width) / base, 0.25, 4.0);
    g_state.panX = 0;
    ClearRenderCaches();
    ScrollToPageTop(panel, g_state.currentPage, 0);
    InvalidateRect(panel, nullptr, FALSE);
    return true;
}
PdfSearchCursor StartPdfSearch(bool previous) {
    using namespace readonly_viewer::pdf_panel;
    PdfSearchCursor cursor;
    cursor.previous = previous;
    if (g_state.pdf && g_state.pdf->document) {
        cursor.path = g_state.pdf->path;
        cursor.page = g_state.currentPage;
        cursor.pageCount = g_state.pdf->pageCount;
        cursor.character = previous ? -1 : 0;
    }
    return cursor;
}
SearchStep SearchPdfPage(HWND panel, const std::wstring& query, PdfSearchCursor& cursor) {
    using namespace readonly_viewer::pdf_panel;
    if (!g_state.pdf || !g_state.pdf->document || cursor.path != g_state.pdf->path ||
        cursor.pageCount != g_state.pdf->pageCount || cursor.page < 0 || cursor.page >= cursor.pageCount || query.empty())
        return SearchStep::Unavailable;
    FPDF_PAGE page = FPDF_LoadPage(g_state.pdf->document, cursor.page);
    if (!page) return SearchStep::Failed;
    FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
    if (!text) { FPDF_ClosePage(page); return SearchStep::Failed; }
    FPDF_SCHHANDLE search = FPDFText_FindStart(text, reinterpret_cast<FPDF_WIDESTRING>(query.c_str()), 0, cursor.character);
    if (!search) { FPDFText_ClosePage(text); FPDF_ClosePage(page); return SearchStep::Failed; }
    const bool found = cursor.previous ? FPDFText_FindPrev(search) != 0 : FPDFText_FindNext(search) != 0;
    if (found) {
        const int index = FPDFText_GetSchResultIndex(search);
        const int count = FPDFText_GetSchCount(search);
        cursor.resultPage = cursor.page;
        cursor.resultCharacter = index;
        cursor.resultLength = count;
        double left = 0, right = 0, bottom = 0, top = 0;
        const bool bounds = FPDFText_GetCharBox(text, index, &left, &right, &bottom, &top) != 0;
        readonly_viewer::PdfPreviewPanel_JumpToPdfLocation(panel, cursor.page, top, false);
        if (bounds) ScrollToPdfPoint(panel, cursor.page, left, top);
        if (cursor.previous && index == 0) {
            cursor.page = (cursor.page + cursor.pageCount - 1) % cursor.pageCount;
            cursor.character = -1;
        } else cursor.character = cursor.previous ? index - 1 : index + count;
    } else {
        cursor.page = (cursor.page + (cursor.previous ? cursor.pageCount - 1 : 1)) % cursor.pageCount;
        cursor.character = cursor.previous ? -1 : 0;
    }
    FPDFText_FindClose(search);
    FPDFText_ClosePage(text);
    FPDF_ClosePage(page);
    return found ? SearchStep::Found : SearchStep::NextPage;
}
}
