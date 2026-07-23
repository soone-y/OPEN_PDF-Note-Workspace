// file: pdf_view/pdf_view.h
#pragma once
#include "core/annot_commands.h"
#include "core/app_core.h"

enum class TextBoxInteractionState {
    Idle,           // 通常の非編集状態
    PendingClick,   // MouseDown中のタップ/移動判定待ち
    Moving,         // 枠ドラッグ移動中
    Editing         // インラインテキスト編集モード
};

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
void SetPdfScale(HWND pdfWnd, double newScale, POINT focusInClient);
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
void InvalidateAnnotHistoryForPath(const std::wstring& path);
bool LoadAnnotationsForCurrentPdf(HWND owner);
bool MergeStagedAnnotationsIntoCurrentPdf(HWND owner, const std::filesystem::path& stagePath);
bool OpenPdfWithAnnotations(HWND owner, const std::wstring& path);
bool PromptPasswordAndReopenCurrentPdf(HWND owner, const std::wstring& title,
                                       const std::wstring& blockedMessage);
void CommitActiveTextEditing(bool commit);
void ClearPdfTextSelection();
void ClearPdfAnnotationSelection();
bool ApplyActiveColorToSelectedAnnotation(HWND hwnd);
bool DeleteAnnotationAtIndex(HWND owner, int index);
bool DuplicateAnnotationAtIndex(HWND owner, int index);
bool ReorderAnnotationAtIndex(HWND owner, int index, bool bringToFront);
bool UpdateAnnotationAtIndex(HWND owner, int index, const Annotation& after);
bool ApplyCurrentToolbarStyleToAnnotation(HWND owner, int index);
bool ExportPdfWithAnnotations(HWND owner);
void ClearAllAnnotationsWithUndo(HWND owner);
bool AddMathAnnotationFromList(HWND pdfWnd, int mathIndex, const POINT& screenPt);
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

