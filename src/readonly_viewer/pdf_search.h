#pragma once
#include <windows.h>
#include <string>
#include "pdf_view_position.h"
namespace reader_next {
enum class SearchStep { Found, NextPage, Unavailable, Failed };
struct PdfViewState {
    double zoom = 1.0, pan_x = 0.0, scroll_y = 0.0;
    int range_first = 0, range_last = -1;
    bool captured = false;
    readonly_viewer::PdfViewportAnchor anchor;
    int rotation = 0;
};
struct PdfSearchCursor {
    std::wstring path;
    int page = 0;
    int character = 0;
    int pageCount = 0;
    int resultPage = -1;
    int resultCharacter = -1;
    int resultLength = 0;
    bool previous = false;
};
// UI thread owns the cursor and the already loaded PDF. No second file read,
// writes, threads or retained PDFium handles. Each step releases all handles.
[[nodiscard]] PdfSearchCursor StartPdfSearch(bool previous);
[[nodiscard]] SearchStep SearchPdfPage(HWND panel, const std::wstring& query, PdfSearchCursor& cursor);
[[nodiscard]] bool FitPdfReadingWidth(HWND panel);
[[nodiscard]] PdfViewState CapturePdfView(HWND panel, const std::wstring& expected_path);
[[nodiscard]] bool RestorePdfView(HWND panel, const PdfViewState& state);
void ConfigurePdfReadingStyle();
}
