// file: pdf_view/pdf_view.h
#pragma once
#include "core/annot_commands.h"
#include "core/app_core.h"

struct PreEditToolbarState {
    bool valid = false;
    ToolMode toolMode = ToolMode::TextBox;
    std::wstring textFontName;
    double textFontPt = 12.0;
    COLORREF textColor = RGB(0, 0, 0);
    bool readableBackground = false;
    bool readableBackgroundInverted = false;
};

#include <string>
#include <string_view>
#include <vector>

// PDF 表示・編集関連 API

void ClearPdfState();
void BuildPageCaches(HWND pdfWnd);
bool LoadPdfDocument(HWND pdfWnd, const std::wstring& path);
// UI-thread only, after a successful open with stable client geometry.
// False means deferred/not pending; the existing posted fit remains available.
[[nodiscard]] bool StartPendingPdfInitialRender(HWND pdfWnd);
// Test-only entry, called with the isolated multi-page automation fixture.
[[nodiscard]] bool RunPdfInitialViewportWorkProbe(HWND pdfWnd, std::wstring* outError);
[[nodiscard]] bool RunPdfWorkerConcurrencyProbe(HWND pdfWnd, const std::wstring& alternatePdf,
                                               std::wstring* outError);
// Test-only, disposable UI fixture; no source PDF or staged data is saved.
[[nodiscard]] bool RunPdfTextboxReeditFontProbe(HWND owner, std::wstring* trace, std::wstring* outError);
[[nodiscard]] bool RunPdfLinePlacementProbe(HWND owner, std::wstring* trace, std::wstring* outError);
[[nodiscard]] bool RunPdfPageRotationProbe(HWND owner, const std::wstring& outputDirectory,
                                         std::wstring* trace, std::wstring* outError);
void SetPdfScale(HWND pdfWnd, double newScale, POINT focusInClient);
// UI-thread view-only action. Keeps the current page/rotation; refuses active
// editing/dragging and invalid viewport geometry without changing the view.
[[nodiscard]] bool FitCurrentPdfPageToView(HWND pdfWnd);
int  PageAtCurrentView();
void SetPdfFlowMode(HWND pdfWnd, const std::wstring& mode);
void ApplyPdfDisplayMode(HWND pdfWnd);
void TickScroll(HWND hwnd);
void JumpToPage(HWND pdfWnd, int index);
void JumpToPdfPoint(HWND pdfWnd, int pageIndex, double xPt, double yPt);
void SetPdfSearchTextMarker(int pageIndex, size_t charStart, size_t charEnd);
void SetPdfSearchPointMarker(int pageIndex, double xPt, double yPt);
void ClearPdfSearchResultMarker();
[[nodiscard]] bool SaveAnnotationsIfDirty(HWND owner);
void MarkAnnotsDirty(HWND owner);
// Annotation undo/redo is intentionally limited to the current session and
// document. Clear it at a document boundary or an isolated local test.
void ClearPdfAnnotationHistory();
void InvalidateAnnotHistoryForPath(const std::wstring& path);
bool LoadAnnotationsForCurrentPdf(HWND owner);
bool OpenPdfWithAnnotations(HWND owner, const std::wstring& path);
bool PromptPasswordAndReopenCurrentPdf(HWND owner, const std::wstring& title,
                                       const std::wstring& blockedMessage,
                                       const std::wstring& confirmation = L"");
void CommitActiveTextEditing(bool commit);
void ClearPdfTextSelection();
void ClearPdfAnnotationSelection();
void ClearAnnotationSelection();
int GetActiveSelectedAnnotationIndex();
void InsertAnnotAt(const Annotation& ann, int index);
bool ApplyActiveColorToSelectedAnnotation(HWND hwnd);
bool DeleteAnnotationAtIndex(HWND owner, int index);
bool DuplicateAnnotationAtIndex(HWND owner, int index);
bool ReorderAnnotationAtIndex(HWND owner, int index, bool bringToFront);
bool UpdateAnnotationAtIndex(HWND owner, int index, const Annotation& after);
// Converts a recognized freehand stroke to a shape/line using the active
// correction policy. Returns false when the stroke should remain freehand.
bool TryCorrectFreehandAnnotation(const Annotation& source, Annotation* corrected);
void ClearAllAnnotationsWithUndo(HWND owner);
bool AddMathAnnotationFromTextAtPoint(HWND pdfWnd, const std::wstring& rawText, MathKind kind, const POINT& screenPt);
bool AddMathAnnotationFromText(HWND pdfWnd, const std::wstring& rawText, MathKind kind);
bool FinalizePendingPdfLinkMarkers(HWND owner, const std::wstring& linkId, const std::wstring& notePath);
bool DiscardPendingPdfLinkMarkers(const std::wstring& linkId);
void SaveLastPdfViewInfo();
bool MovePdfViewInfoPath(const std::wstring& oldPath, const std::wstring& newPath);
void ClearLastPdfViewInfo();

// Undo/redo is document-local and may only change the focused PDF view.
bool CanExecutePdfUndoRedoFromFocus(bool undo);
bool ExecutePdfUndoRedoFromFocus(HWND owner, bool undo);

// 注釈・モード
void SetModeButtons();

// PdfView ウィンドウプロシージャ
LRESULT CALLBACK PdfViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// インラインテキスト編集を終了（commit=trueなら確定、falseなら破棄）
void EndInlineTextEditing(HWND pdfWnd, bool commit);

// アクティブな PDF TextBox へ clipboard の Unicode text を貼り付ける。
// 成功時は本文・selectionを一つのローカル編集履歴として記録する。
bool PasteClipboardIntoActivePdfTextBox(HWND pdfWnd);

// Rotates only the current page's temporary view. No original/save mutation.
[[nodiscard]] bool RotateCurrentPdfPageView(HWND pdfWnd, int quarterTurns, bool reset = false);

// Adds the same temporary quarter turn to every page, retaining per-page differences.
[[nodiscard]] bool RotateAllPdfPagesView(HWND pdfWnd, int quarterTurns, bool reset = false);
