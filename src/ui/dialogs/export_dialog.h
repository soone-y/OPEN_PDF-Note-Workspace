#pragma once

#include "core/app_core.h"
#include "file_output/file_output.h"

enum class ExportDialogKind {
    PdfAll,
    PdfPages,
    PdfPng,
    NoteText,
    NoteMarkup
};

struct ExportDialogResult {
    ExportDialogKind kind = ExportDialogKind::PdfAll;
    std::vector<file_output::PdfPageSpec> pages;
    int pageIndex = 0; // 0-based for PNG export
    bool includeAnnots = true;
    bool standardTextAnnots = true;
    bool matchPdfPaneTextLayout = true;
    double pdfScale = 1.0;
    int pngWidthPx = 0;
    int pngHeightPx = 0;
    file_output::PdfPngStyle pngStyle = file_output::PdfPngStyle::PdfLike;
    file_output::TextExportOptions textOptions{};
    file_output::NoteMarkupExportOptions noteMarkupOptions{};
};

bool ShowUnifiedExportDialog(HWND owner, ExportDialogKind preset, std::vector<ExportDialogResult>& out);
void ExecuteUnifiedExport(HWND hWnd, const ExportDialogResult& result);
void ExecuteUnifiedExports(HWND hWnd, const std::vector<ExportDialogResult>& results);
// Called by the main window only after a requested pre-export integration transaction ends.
void CompleteUnifiedExportsAfterSave(HWND hWnd, bool saveSucceeded, bool saveRestarted);


