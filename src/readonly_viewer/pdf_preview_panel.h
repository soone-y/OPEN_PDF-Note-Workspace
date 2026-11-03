#pragma once

#include "core/theme_types.h"

#include <windows.h>
#include <string>

namespace readonly_viewer {

// Register the Window Class for the PDF preview panel
bool RegisterPdfPreviewPanelClass(HINSTANCE hInstance);

// Create the PDF preview panel as a child of the given parent window
HWND CreatePdfPreviewPanel(HWND hParent, HINSTANCE hInstance, int id);

// The standalone viewer owns its theme state in the preview panel. Main-window
// owner-draw controls use this accessor so they follow the active theme.
ThemeColors PdfPreviewPanel_GetTheme();

// Grayscale affects rendered PDF pixels for this process only.
bool PdfPreviewPanel_IsGrayscaleEnabled(HWND hwnd);
[[nodiscard]] bool PdfPreviewPanel_AreAnnotationsVisible(HWND hwnd);

// Clockwise quarter turns, owned by the current tab's transient view state.
[[nodiscard]] int PdfPreviewPanel_GetRotation(HWND hwnd);
void PdfPreviewPanel_SetRotation(HWND hwnd, int turns);

// Open a PDF (and optionally a CLROP) in the given PDF preview panel
void PdfPreviewPanel_OpenPdf(HWND hwnd, const std::wstring& pdfPath, const std::wstring& clropPath = L"");

// Move an already loaded PDF to a search result location. pageIndex is zero-based;
// when hasYPt is true, yPt is a PDF-space coordinate measured from the bottom.
void PdfPreviewPanel_JumpToPdfLocation(HWND hwnd, int pageIndex, double yPt, bool hasYPt);

// Let the reader choose the contiguous PDF page range that is kept in the
// continuous view.  This reduces layout and render-cache work for large PDFs.
void PdfPreviewPanel_ChoosePageRange(HWND hwnd);
void PdfPreviewPanel_PromptJumpPage(HWND hwnd);

// Get the currently loaded PDF path (if any)
std::wstring PdfPreviewPanel_GetPdfPath(HWND hwnd);

// Close the currently loaded PDF and release resources
void PdfPreviewPanel_ClosePdf(HWND hwnd);

// Command IDs that the PdfPreviewPanel can process via WM_COMMAND
constexpr UINT kCmdPdfExportAnnotatedPdf = 1007;
constexpr UINT kCmdPdfToggleAnnots = 1101;
constexpr UINT kCmdPdfToggleMagnifier = 1102;
constexpr UINT kCmdPdfToggleGrayscale = 1106;
constexpr UINT kCmdPdfZoomIn = 1111;
constexpr UINT kCmdPdfZoomOut = 1112;
constexpr UINT kCmdPdfZoomReset = 1113;
constexpr UINT kCmdPdfRotateLeft = 1114;
constexpr UINT kCmdPdfRotateRight = 1115;
constexpr UINT kCmdPdfRotateReset = 1116;
constexpr UINT kCmdPdfJumpPage = 1117;
constexpr UINT kCmdPdfFitPage = 1118;

} // namespace readonly_viewer
