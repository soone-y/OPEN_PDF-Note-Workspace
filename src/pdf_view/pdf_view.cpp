// file: pdf_view/pdf_view.cpp
#include "pdf_view/pdf_view.h"
#include "pdf_view/render_queue_policy.h"
#include "pdf_view/view_rotation_record.h"
#include "pdf_view/text_input_accessibility.h"
#include "pdf_view/textbox_font_update.h"
#include "pdf_view/text_coverage.h"
#include "pdf_view/worker_render.h"
#include "pdf_view/path_multiply.h"
#include "pdf_view/marker_composite.h"
#include "pdf_view/text_selection_pdfium.h"
#include "ui/scroll_input_win32.h"
#include "ui/empty_view_guide.h"
#include "ui/shortcut_help.h"
#include "core/annotation_rotation.h"
#include "ui/quarter_turn_bitmap.h"
#include "ui/core/main_window_api.h"
#include "bridge/view_bridge.h"
#include "core/ui_notify.h"
#include "core/localization.h"
#include "core/ui_prompts.h"
#include "math/math_render.h"
#include "file_output/file_output.h"
#include "clrop/bridge.h"
#include "clrop/json.h"
#include "clrop/hash.h"
#include "ui/dialogs/annot_math_panel.h"
#include "fpdf_save.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_javascript.h"
#include "core/atomic_write.h"
#include "diagnostics/normal_operations.h"
#include "diagnostics/open_work_counters.h"
#include "core/preview_trace.h"
#include "core/secure_memory.h"
#include "ui/dashed_shape_outline.h"
#include "workspace/workspace_write_lock.h"
#include "workspace/file_ops.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <string_view>
#include <limits>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>
#include <map>
#include <unordered_map>
#include <optional>
#include <commctrl.h>
#include <imm.h>
#include <wincodec.h>

// helper for translucent fill / strokes
static const Annotation& ShapeRotationForPaint(const Annotation& view,Annotation& preview);
static void FillRectAlpha(HDC hdc, const RECT& r, COLORREF color, BYTE alpha);
static void FillTextMarker(HDC hdc, const RECT& rectangle, COLORREF color, BYTE alpha,
                           MarkerDisplayMode mode = MarkerDisplayMode::Multiply);
static void DrawMultiplyShape(HDC dc, ShapeKind kind, ShapeDrawMode drawMode, const RECT& bounds,
    int penWidth, COLORREF color, BYTE alpha, const std::vector<double>& dash, double scale,
    double rotation, int contentRotation = 0, double wholeAngle = 0.0);
static void DrawMultiplyStroke(HDC dc,const std::vector<POINT>& points,int penWidth,COLORREF color,BYTE alpha);
static void TintRectTextPixels(HDC hdc, const RECT& r, COLORREF color,
                               const PageCache& page, int pageLeft, int pageTop);
static bool EnsurePdfPageTextCoverage(int pageIndex);
static double ContrastRatio(COLORREF a, COLORREF b);
static int ColorChannelDistance(COLORREF a, COLORREF b);
static COLORREF ReadableOverlayColorForBackground(COLORREF bg);
static bool SamplePagePixelColor(const PageCache& page, int pageX, int pageY, COLORREF& out);
static RECT PdfRectToPagePixelRect(const PageCache& page, double leftPt, double bottomPt,
                                   double rightPt, double topPt, double scale);
static bool AveragePagePixelBorderColor(const PageCache& page, const RECT& inner, int padPx,
                                        COLORREF& out);
static bool PagePixelRectHasVisibleInk(const PageCache& page, const RECT& inner, COLORREF bg,
                                       int channelThreshold, double contrastThreshold,
                                       int sampleDivisor);

static bool TextBoxReadableBackground(const Annotation& ann, COLORREF* outColor, BYTE* outAlpha);
static void DrawTextBoxReadableBackground(HDC hdc, const RECT& r, const Annotation& ann);
static void DrawPolylineAlphaPx(HDC hdc, const std::vector<POINT>& pts, int penW, COLORREF color, BYTE alpha);
static COLORREF DarkenColor(COLORREF c, double factor = 0.85);
static void CommitTextEditing(HWND hwnd, bool commit);
static std::uint64_t g_pdfTextInputGeneration = 0;
static bool g_pdfTextInputPublished = false;
static text_input_accessibility::Snapshot g_pdfTextInputLast;
static HRESULT ReadPdfTextInput(HWND hwnd, std::uint64_t generation,
                               text_input_accessibility::Snapshot* out) noexcept;
static HRESULT ModifyPdfTextInput(HWND hwnd, std::uint64_t generation,
    text_input_accessibility::Action action, const std::wstring* value) noexcept;
static void BeginPdfTextInputAccessibility(HWND hwnd) noexcept;
static void EndPdfTextInputAccessibility(HWND hwnd) noexcept;
static void NotifyPdfTextInputAccessibility(HWND hwnd) noexcept;
static void StopPdfKeyboardScroll(HWND hwnd);
static bool PtRectToClientRect(int pageIndex, double x1, double y1, double x2, double y2, RECT& out);
static bool GetEditingInnerRectClient(RECT* outInner, int* outWidth);
static bool GetEditingInnerRectLayout(RECT* outInner, int* outWidth);
static bool BuildTextLayout(int innerWidth, const std::wstring& text, TextLayoutResult* out);
static std::vector<TextLineLayout> LayoutTextLines(HDC hdc, const std::wstring& text, int maxWidth);
static int MeasureIdeographicWidth(HDC hdc);
static void AdjustDxForIdeographicSpace(std::wstring_view text, std::vector<int>& dx, int ideographicWidth);
static void BuildAdvancesFromCumulative(const std::vector<int>& widths, std::vector<int>& advances);
static bool BuildAdjustedAdvances(HDC hdc, std::wstring_view text, int ideographicWidth, std::vector<int>& advances);
static void DrawAdjustedLine(HDC hdc, int x, int y, std::wstring_view line, int ideographicWidth,
                             std::vector<int>& advances);
static std::wstring NormalizeTextBoxTextForCommit(std::wstring text);
static std::vector<std::wstring> SplitTextByNewlines(const std::wstring& text);
static std::vector<std::wstring> LayoutResultToLines(const TextLayoutResult& layout, const std::wstring& text);
struct ShapeBounds {
    double left = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double top = 0.0;
};
struct FreehandCorrectionResult {
    bool accepted = false;
    Annotation corrected;
    double confidence = 0.0;
};
static void DrawMultiplyShape(HDC dc, ShapeKind kind, ShapeDrawMode drawMode, const RECT& bounds,
    int penWidth, COLORREF color, BYTE alpha, const std::vector<double>& dash, double scale,
    double rotation, int contentRotation, double wholeAngle) {
    drawMode=ShapeDrawModeForKind(kind,drawMode);
    auto outline=shape_outline::BuildOriented(kind,bounds.left,bounds.top,bounds.right,bounds.bottom,rotation,contentRotation,wholeAngle,drawMode);
    const auto runs=drawMode!=ShapeDrawMode::Fill ? shape_outline::Dash(outline,dash,scale)
        : std::vector<decltype(outline)>{outline};
    std::vector<std::vector<POINT>> paths;
    for(const auto& run:runs){
        std::vector<POINT> points;points.reserve(run.size());
        for(const auto& point:run)points.push_back({static_cast<LONG>(std::lround(point.x)),static_cast<LONG>(std::lround(point.y))});
        paths.push_back(std::move(points));
    }
    RECT area=bounds;
    if (wholeAngle != 0.0) for(const auto& point:outline){
        area.left=std::min(area.left,static_cast<LONG>(std::floor(point.x)));area.right=std::max(area.right,static_cast<LONG>(std::ceil(point.x)));
        area.top=std::min(area.top,static_cast<LONG>(std::floor(point.y)));area.bottom=std::max(area.bottom,static_cast<LONG>(std::ceil(point.y)));
    }
    InflateRect(&area,penWidth/2+2,penWidth/2+2);
    if(!pdf_marker::PaintPaths(dc,area,paths,drawMode==ShapeDrawMode::Fill,penWidth,color,alpha))
        ShowSoftNotice(g_hPdfView,localization::Text(L"annotation.blend.failed"),SoftNoticeKind::Warning);
}

static void DrawMultiplyStroke(HDC dc,const std::vector<POINT>& points,int penWidth,COLORREF color,BYTE alpha) {
    if(points.size()<2)return;
    RECT area{points.front().x,points.front().y,points.front().x,points.front().y};
    for(const auto& point:points){area.left=std::min(area.left,point.x);area.right=std::max(area.right,point.x);
        area.top=std::min(area.top,point.y);area.bottom=std::max(area.bottom,point.y);}
    InflateRect(&area,penWidth/2+2,penWidth/2+2);
    if(!pdf_marker::PaintPaths(dc,area,{points},false,penWidth,color,alpha))
        ShowSoftNotice(g_hPdfView,localization::Text(L"annotation.blend.failed"),SoftNoticeKind::Warning);
}

static ShapeBounds NormalizeShapeBounds(ShapeKind kind, double x1, double y1, double x2, double y2);
static bool TryBuildFreehandCorrection(const Annotation& source, FreehandCorrectionResult* out);
static bool TryBuildFreehandSmoothing(const Annotation& source, Annotation* out);
static void FillPolygonAlphaPx(HDC hdc, const std::vector<POINT>& pts, COLORREF color, BYTE alpha);
static void FillEllipseAlphaPx(HDC hdc, const RECT& r, COLORREF color, BYTE alpha);
static std::vector<POINT> BuildEllipseOutlinePointsPx(const RECT& r);
static double EffectiveRotatedEllipseAngleRad(double angleRad);
static double ShapeRotationFromClientEndpoints(double x1, double y1, double x2, double y2);
static std::vector<POINT> BuildRotatedEllipseOutlinePointsPx(const RECT& r, double angleRad);
static void DrawShapeOutlineAlphaPx(HDC hdc, ShapeKind kind, const RECT& r, const std::vector<POINT>& pts,
                                    int penW, COLORREF color, BYTE alpha);
static bool AnnotationIdExistsIn(const std::vector<Annotation>& annots, std::wstring_view id, int skipIndex = -1);
static std::wstring GenerateUniqueAnnotationId(const std::vector<Annotation>& annots);
static void EnsureAnnotationId(Annotation& ann, const std::vector<Annotation>& annots, int selfIndex = -1);
static void EnsureAnnotationIds(std::vector<Annotation>& annots);
static int FindAnnotIndexById(std::wstring_view id);
static int FindAnnotIndexByIdOrFallback(const std::wstring& id, int fallbackIndex);

static HWND PdfDialogOwner(HWND owner) {
    HWND candidate = owner ? owner : g_hPdfView;
    if (candidate && IsWindow(candidate)) {
        // Password and failure dialogs must disable the actual top-level owner.
        // Passing g_hPdfView directly would otherwise disable only that child
        // control, leaving the main window able to start a second open action.
        HWND rootOwner = GetAncestor(candidate, GA_ROOTOWNER);
        if (rootOwner) return rootOwner;
        HWND parent = GetParent(candidate);
        if (parent) return parent;
    }
    return g_hMainWnd;
}

static std::wstring ToBase36AnnotationId(unsigned long long value) {
    if (value == 0) return L"0";
    std::wstring out;
    while (value > 0) {
        const unsigned digit = static_cast<unsigned>(value % 36ULL);
        out.push_back(static_cast<wchar_t>((digit < 10) ? (L'0' + digit) : (L'a' + (digit - 10))));
        value /= 36ULL;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

static bool AnnotationIdExistsIn(const std::vector<Annotation>& annots, std::wstring_view id, int skipIndex) {
    if (id.empty()) return false;
    for (size_t i = 0; i < annots.size(); ++i) {
        if (static_cast<int>(i) == skipIndex) continue;
        if (annots[i].id == id) return true;
    }
    return false;
}

static std::wstring GenerateUniqueAnnotationId(const std::vector<Annotation>& annots) {
    static unsigned long long s_annotationIdCounter = 0;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    unsigned long long base = (millis > 0) ? static_cast<unsigned long long>(millis)
                                           : static_cast<unsigned long long>(GetTickCount64());
    for (;;) {
        ++s_annotationIdCounter;
        std::wstring candidate = L"a" + ToBase36AnnotationId(base) +
                                 L"_" + ToBase36AnnotationId(static_cast<unsigned long long>(GetCurrentProcessId())) +
                                 L"_" + ToBase36AnnotationId(s_annotationIdCounter);
        if (!AnnotationIdExistsIn(annots, candidate, -1)) {
            return candidate;
        }
        ++base;
    }
}

static void EnsureAnnotationId(Annotation& ann, const std::vector<Annotation>& annots, int selfIndex) {
    if (!ann.id.empty() && !AnnotationIdExistsIn(annots, ann.id, selfIndex)) return;
    ann.id = GenerateUniqueAnnotationId(annots);
}

static void EnsureAnnotationIds(std::vector<Annotation>& annots) {
    for (size_t i = 0; i < annots.size(); ++i) {
        EnsureAnnotationId(annots[i], annots, static_cast<int>(i));
    }
}

static int FindAnnotIndexById(std::wstring_view id) {
    if (id.empty()) return -1;
    for (size_t i = 0; i < g_annots.size(); ++i) {
        if (g_annots[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

static int FindAnnotIndexByIdOrFallback(const std::wstring& id, int fallbackIndex) {
    int resolved = FindAnnotIndexById(id);
    if (resolved >= 0) return resolved;
    if (fallbackIndex >= 0 && fallbackIndex < static_cast<int>(g_annots.size())) {
        return fallbackIndex;
    }
    return -1;
}

static void ShowPdfSoftNotice(HWND owner, const std::wstring& text,
                              SoftNoticeKind kind = SoftNoticeKind::Info) {
    ShowSoftNotice(PdfDialogOwner(owner), text, kind);
}

static void ShowPdfCopySuccessNotice(HWND owner) {
    ShowPdfSoftNotice(owner,
                      localization::Text(L"pdf.view.5b2faff96caf"),
                      SoftNoticeKind::Info);
}
static void ShowPdfMessageDialog(HWND owner, const std::wstring& title,
                                 const std::wstring& message, SoftNoticeKind kind,
                                 const std::vector<SilentDialogPath>& paths = {}) {
    ShowSilentMessageDialog(PdfDialogOwner(owner), title, message, kind, paths);
}

// Returns true only when the user explicitly requests a non-destructive reload.
static bool PromptPdfOpenFailureReload(HWND owner, const std::wstring& title,
                                       const std::wstring& message) {
    const HWND dialogOwner = PdfDialogOwner(owner);
    const bool offerAbnormalExit = CanRequestManagedAbnormalExitFromDialog(dialogOwner,
                                                                             SoftNoticeKind::Error);
    SilentDialogOptions options;
    options.title = title;
    options.message = message;
    options.kind = SoftNoticeKind::Error;
    options.buttons = offerAbnormalExit ? SilentDialogButtons::YesNoCancel
                                        : SilentDialogButtons::YesNo;
    options.yesLabel = localization::Text(L"pdf.view.018a2719c366");
    options.noLabel = localization::Text(L"pdf.view.3672b0b92134");
    options.cancelLabel = localization::Text(L"pdf.view.fea723590aa2");
    options.defaultResult = SilentDialogResult::No;
    options.escapeResult = SilentDialogResult::No;
    const SilentDialogResult result = ShowSilentDialog(dialogOwner, options);
    if (offerAbnormalExit && result == SilentDialogResult::Cancel) {
        RequestManagedAbnormalExitFromDialog(dialogOwner, title, message);
    }
    return result == SilentDialogResult::Yes;
}

static SilentDialogResult ShowPdfDialog(HWND owner, const SilentDialogOptions& options) {
    return ShowSilentDialog(PdfDialogOwner(owner), options);
}

static std::optional<std::string> g_pendingPdfOpenPasswordUtf8;

static void QueuePdfOpenPasswordForNextLoad(const std::string& passwordUtf8) {
    if (passwordUtf8.empty()) {
        SecureResetOptionalString(g_pendingPdfOpenPasswordUtf8);
    } else {
        SecureResetOptionalString(g_pendingPdfOpenPasswordUtf8);
        g_pendingPdfOpenPasswordUtf8 = passwordUtf8;
    }
}

static std::optional<std::string> ConsumePendingPdfOpenPasswordUtf8() {
    std::optional<std::string> password = std::move(g_pendingPdfOpenPasswordUtf8);
    // Ownership of the buffer moved to the return value. Reset only the empty shell.
    g_pendingPdfOpenPasswordUtf8.reset();
    return password;
}

static bool ConfirmPdfYesNo(HWND owner, const std::wstring& title, const std::wstring& message,
                            SoftNoticeKind kind, SilentDialogResult defaultResult,
                            SilentDialogResult escapeResult) {
    SilentDialogOptions options;
    options.title = title;
    options.message = message;
    options.kind = kind;
    options.buttons = SilentDialogButtons::YesNo;
    options.defaultResult = defaultResult;
    options.escapeResult = escapeResult;
    return ShowPdfDialog(owner, options) == SilentDialogResult::Yes;
}

bool PromptPasswordAndReopenCurrentPdf(HWND owner, const std::wstring& title,
                                       const std::wstring& blockedMessage,
                                       const std::wstring& confirmation) {
    const std::wstring pdfPath = CurrentLogicalPdfPath();
    if (pdfPath.empty()) return false;
    const bool hadSelection = g_pdf.hasSelection;
    const int selectionStartPage = g_pdf.selectionStartPage;
    const int selectionEndPage = g_pdf.selectionEndPage;
    const bool selectionByText = g_pdf.selectionByText;
    const int selCharAnchor = g_pdf.selCharAnchor;
    const int selCharFocus = g_pdf.selCharFocus;
    const POINT selStart = g_pdf.selStart;
    const POINT selEnd = g_pdf.selEnd;

    SilentDialogOptions options;
    options.title = title;
    options.message =
        blockedMessage +
        (localization::Text(L"pdf.view.1f2723e36a6b"));
    options.kind = SoftNoticeKind::Warning;
    options.buttons = SilentDialogButtons::YesNo;
    options.yesLabel = localization::Text(L"pdf.view.7eb7a2cc15ed");
    options.noLabel = localization::Text(L"pdf.view.3672b0b92134");
    options.defaultResult = SilentDialogResult::Yes;
    options.escapeResult = SilentDialogResult::No;
    if (ShowPdfDialog(owner, options) != SilentDialogResult::Yes) {
        return false;
    }

    std::wstring password;
    SecureWideStringScope passwordScope(&password);
    const std::wstring promptMessage =
        localization::Text(L"pdf.view.ee6127d8900a");
    if (!PromptPasswordText(PdfDialogOwner(owner), title, promptMessage, password, confirmation)) {
        return false;
    }

    std::string passwordUtf8 = WideToUTF8(password);
    SecureStringScope passwordUtf8Scope(&passwordUtf8);
    QueuePdfOpenPasswordForNextLoad(passwordUtf8);
    if (!OpenPdfWithAnnotations(owner, pdfPath)) {
        return false;
    }

    if (hadSelection && g_pdf.kind == DocKind::Pdf &&
        selectionStartPage >= 0 && selectionEndPage >= 0 &&
        selectionStartPage < g_pdf.pageCount &&
        selectionEndPage < g_pdf.pageCount) {
        g_pdf.hasSelection = true;
        g_pdf.selectionStartPage = selectionStartPage;
        g_pdf.selectionEndPage = selectionEndPage;
        g_pdf.selectionByText = selectionByText;
        g_pdf.selCharAnchor = selCharAnchor;
        g_pdf.selCharFocus = selCharFocus;
        g_pdf.selStart = selStart;
        g_pdf.selEnd = selEnd;
        if (g_hPdfView) {
            InvalidateRect(g_hPdfView, nullptr, FALSE);
        }
    }
    return true;
}

static HWND FindComboListBoxUnderCursor(POINT screenPt) {
    HWND h = WindowFromPoint(screenPt);
    while (h) {
        wchar_t cls[64]{};
        if (GetClassNameW(h, cls, static_cast<int>(std::size(cls))) > 0) {
            if (wcscmp(cls, L"ComboLBox") == 0) return h;
        }
        h = GetParent(h);
    }
    return nullptr;
}

static bool ForwardWheelToDroppedCombo(UINT msg, WPARAM wParam, LPARAM lParam) {
    POINT wheelPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; // screen coords
    HWND underCursor = WindowFromPoint(wheelPt);

    HWND combos[] = {
        g_hComboFont,
        g_hComboFontSize,
        g_hComboFontSizeAlt,
        g_hComboWidth,
        g_hComboMarkerAlpha,
        g_hComboShapeKind,
        g_hComboLineDashStyle,
        g_hComboMagnifierShape,
    };

    for (HWND combo : combos) {
        if (!combo || !IsWindow(combo)) continue;
        if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) == 0) continue;

        COMBOBOXINFO cbi{};
        cbi.cbSize = sizeof(cbi);
        if (GetComboBoxInfo(combo, &cbi) && cbi.hwndList) {
            HWND list = cbi.hwndList;
            HWND dropWnd = GetParent(list) ? GetParent(list) : list; // often "ComboLBox"

            RECT rcDrop{}, rcCombo{};
            bool hasDrop = GetWindowRect(dropWnd, &rcDrop) != 0;
            bool hasCombo = GetWindowRect(combo, &rcCombo) != 0;

            bool overDrop = hasDrop && PtInRect(&rcDrop, wheelPt);
            bool overCombo = hasCombo && PtInRect(&rcCombo, wheelPt);
            bool overKnownWindow = (underCursor && (underCursor == list || underCursor == dropWnd ||
                                                   IsChild(dropWnd, underCursor) || IsChild(list, underCursor)));

            if (!overDrop && !overCombo && !overKnownWindow) continue;

            SendMessageW(dropWnd, msg, wParam, lParam);
            return true;
        }

        // Fallback: if we can't query list window, only intercept when wheel is on the combo itself.
        RECT rcCombo{};
        if (GetWindowRect(combo, &rcCombo) && PtInRect(&rcCombo, wheelPt)) {
            SendMessageW(combo, msg, wParam, lParam);
            return true;
        }
    }
    return false;
}
static std::vector<std::wstring> LayoutTextLinesWithFont(int innerWidth, const std::wstring& text, double fontPt, const std::wstring& fontName);
static int TextBoxInnerWidthPx(const Annotation& ann);
static void EnsureAnnotationTextLines(Annotation& ann);
static void ReflowMovedTextBox(Annotation& ann);
static void FinalizeTextBoxAnnotationLayout(Annotation& ann);
static size_t FindLineForCaret(const std::vector<TextLineLayout>& lines, size_t caret);
static size_t HitTestLineForX(const TextLineLayout& line, int x);
static size_t HitTestCaretIndex(const TextLayoutResult& layout, const RECT& inner, const POINT& pt);
static int ColumnWidthForLine(const TextLineLayout& line, size_t column);
static bool MoveCaretVertical(int direction);
static bool MoveCaretToLineBoundary(bool toEnd);
static bool IsCaretAtLineBoundary(bool toEnd);
static void ResetTextBoxRapidClickState();
static bool RegisterTextBoxRapidClick(int pageIndex, double x1, double y1, double x2, double y2);
static bool CopyActiveTextBoxToClipboard(HWND hwnd);
static void FinishTextBoxTripleClickCopy(HWND hwnd, bool commitActiveEdit);
static bool CopyTextToClipboard(HWND hwnd, const std::wstring& text);
static void ClearEditSelection();
static bool HasEditSelection();
static void SelectAllEditText();
static bool DeleteEditSelection();
static bool CopyEditSelectionToClipboard(HWND hwnd);
bool PasteClipboardIntoActivePdfTextBox(HWND hwnd);
static bool InlineCaretClientPoint(POINT& out, int* outLineHeight = nullptr);
static void RecalcEditingTextboxSize(bool includeComp);
struct TextBoxEditBounds { double left, top, right, bottom; };
struct TextBoxEditGeometry {
    TextBoxEditBounds bounds{};
    TextLayoutResult layout;
    int innerWidth = 0;
};
[[nodiscard]] static bool MeasureTextBoxEditGeometry(const Annotation& box, TextBoxEditGeometry& out);
[[nodiscard]] static bool TextBoxReeditHoverCaret(const Annotation& box, const POINT& hit,
    POINT& start, POINT& end, RECT& bounds, int& lineHeight, size_t* caretIndex = nullptr);
[[nodiscard]] static int FindTextReeditAnnotation(int pageIndex, double px, double py);
[[nodiscard]] static double TextBoxReeditLineHeightPt(const Annotation& ann);
static bool EnlargeEditingTextBoxFont(HWND hwnd);
static bool ShrinkEditingTextBoxFont(HWND hwnd);

static void UpdateImeWindowPosition(HWND hwnd, bool updateCandidate);
static bool IsImeOpen(HWND hwnd);
static void DisablePdfViewIme(HWND hwnd);
static void EnablePdfViewIme(HWND hwnd);
static void ForceImeComplete(HWND hwnd, bool commit);
static std::wstring BuildEditDisplayText(bool includeComp);
static size_t EditIndexToDisplayIndex(size_t index);
static size_t DisplayIndexToEditIndex(size_t index);
static size_t EditCaretDisplayIndex();
static bool EnsureEditLayoutCache(bool includeComp);
static bool HitTestEditCaretFromClientPoint(const POINT& pt, size_t* outEditIndex, bool clampToOuter);
static bool HitTestPagePoint(const POINT& pt, int& pageIndex, double& pdfX, double& pdfY, double& pageTopOut);
static std::wstring NormalizeMathAnnotText(const Annotation& ann);
static void DrawArrowLinePx(HDC hdc, const POINT& p1, const POINT& p2, int penW, COLORREF color, BYTE alpha,
                            ArrowHead arrowHead);
static void DrawWaveLinePx(HDC hdc, const POINT& p1, const POINT& p2, int penW, COLORREF color, BYTE alpha);
static std::vector<POINT> BuildWavePointsPx(const POINT& p1, const POINT& p2, double amplitude, double wavelength);
static void DrawMagnifierLens(HDC hdc, const RECT& client);
static void DrawEraserHitVisualization(HDC hdc, const RECT& client);
void MarkAnnotsDirty(HWND owner);
static void ClearTextBoxRasterCache();
static void DrawTextBoxStable(HDC hdc, HDC scratchDc, const RECT& r, const Annotation& ann);
static bool SaveAnnotHistoryIfNeeded();
static bool LoadAnnotHistoryForCurrentPdf();
static void MarkAnnotHistoryDirty();
static void EnsurePdfViewFocus(HWND hwnd);
static bool LoadImageDocument(HWND pdfWnd, const std::wstring& path);

// Runtime projections are disposable. Persisted annotations/history stay in
// the original page basis; view-only rotation never dirties the document.
static bool EnsurePdfPageCoordinates(int pageIndex);
static page_rotation::Point PdfPointForView(const PageCache& page, double x, double y);
static void SetAnnotationInputBasis(Annotation& annotation);
static bool g_pdfEditDrawLocal = false;

// Begin-edit and hover use identical page-edge normalization without opening an editor.
[[nodiscard]] static TextBoxEditBounds InitialTextBoxEditBounds(const PageCache& page, double anchorX,
    double anchorY, double width, double height, int contentRotation) {
    double left = anchorX, top = anchorY, right = left + width, bottom = top - height;
    if (contentRotation == 0 && right > page.widthPt) {
        const double shift = right - page.widthPt;
        left = std::max(0.0, left - shift);
        right = page.widthPt;
    }
    if (contentRotation == 0 && bottom < 0.0) {
        const double shift = -bottom;
        top = std::min(page.heightPt, top + shift);
        bottom = 0.0;
    }
    if (contentRotation == 0) {
        if (left < 0.0) left = 0.0;
        if (right > page.widthPt) right = page.widthPt;
        if (top > page.heightPt) top = page.heightPt;
    }
    return {left, top, right, bottom};
}

static bool EditingBoxClientRect(RECT& out, bool logical = false) {
    if (!g_pdf.editingText || g_pdf.editPage < 0) return false;
    const double width = std::abs(g_pdf.editX2 - g_pdf.editX1);
    const double height = std::abs(g_pdf.editY2 - g_pdf.editY1);
    const double scale = kDpi / 72.0 * g_pdf.scale;
    if (g_pdfEditDrawLocal) {
        out = {0, 0, static_cast<LONG>(std::lround(width * scale)), static_cast<LONG>(std::lround(height * scale))};
        return out.right > 0 && out.bottom > 0;
    }
    Annotation box;
    box.contentRotation = g_pdf.editContentRotation;
    annotation_rotation::SetContentBounds(box,
        {std::min(g_pdf.editX1, g_pdf.editX2), std::max(g_pdf.editY1, g_pdf.editY2)}, width, height);
    if (!PtRectToClientRect(g_pdf.editPage, box.x1, box.y1, box.x2, box.y2, out)) return false;
    if (logical) {
        out.right = out.left + static_cast<LONG>(std::lround(width * scale));
        out.bottom = out.top + static_cast<LONG>(std::lround(height * scale));
    }
    return true;
}

[[nodiscard]] static POINT TextBoxLogicalToClient(POINT point, const RECT& actual, const RECT& logical,
    int contentRotation, bool inverse = false) {
    const int turn = page_rotation::Normalize(contentRotation);
    const double x = point.x - actual.left, y = point.y - actual.top;
    const double width = logical.right - logical.left, height = logical.bottom - logical.top;
    double u = x, v = y;
    if (!inverse) {
        if (turn == 1) { u = height - y; v = x; }
        else if (turn == 2) { u = width - x; v = height - y; }
        else if (turn == 3) { u = y; v = width - x; }
    } else {
        if (turn == 1) { u = y; v = height - x; }
        else if (turn == 2) { u = width - x; v = height - y; }
        else if (turn == 3) { u = width - y; v = x; }
    }
    return {actual.left + static_cast<LONG>(std::lround(u)), actual.top + static_cast<LONG>(std::lround(v))};
}

static POINT EditingLogicalToClient(POINT point, bool inverse = false) {
    if (g_pdfEditDrawLocal || g_pdf.editContentRotation == 0) return point;
    RECT actual{}, logical{};
    if (!EditingBoxClientRect(actual) || !EditingBoxClientRect(logical, true)) return point;
    return TextBoxLogicalToClient(point, actual, logical, g_pdf.editContentRotation, inverse);
}

static void SetEditingAnnotationBounds(Annotation& annotation) {
    annotation.contentRotation = g_pdf.editContentRotation;
    annotation_rotation::SetContentBounds(annotation,
        {std::min(g_pdf.editX1, g_pdf.editX2), std::max(g_pdf.editY1, g_pdf.editY2)},
        std::abs(g_pdf.editX2 - g_pdf.editX1), std::abs(g_pdf.editY2 - g_pdf.editY1));
    SetAnnotationInputBasis(annotation);
}

static Annotation ProjectAnnotationForView(const Annotation& source) {
    if (source.pageIndex < 0 || source.pageIndex >= static_cast<int>(g_pdf.pages.size())) return source;
    if (g_pdf.kind == DocKind::Pdf && !EnsurePdfPageCoordinates(source.pageIndex)) return source;
    const auto& page = g_pdf.pages[static_cast<size_t>(source.pageIndex)];
    const auto projected = annotation_rotation::InBasis(source, page.intrinsicRotation + page.viewRotation,
        page.pdfWidthPt, page.pdfHeightPt, page.pdfOriginX, page.pdfOriginY);
    return projected ? *projected : source;
}

static const Annotation& AnnotationForView(const Annotation& source, Annotation& storage) {
    if (source.pageIndex < 0 || source.pageIndex >= static_cast<int>(g_pdf.pages.size())) return source;
    if (g_pdf.kind == DocKind::Pdf && !EnsurePdfPageCoordinates(source.pageIndex)) return source;
    const auto& page = g_pdf.pages[static_cast<size_t>(source.pageIndex)];
    if (!source.projectedCoordinates && page_rotation::Normalize(page.intrinsicRotation + page.viewRotation) == 0 &&
        page.pdfOriginX == 0 && page.pdfOriginY == 0) return source;
    storage = ProjectAnnotationForView(source);
    return storage;
}

static void SetAnnotationInputBasis(Annotation& annotation) {
    if (annotation.pageIndex < 0 || annotation.pageIndex >= static_cast<int>(g_pdf.pages.size())) return;
    if (g_pdf.kind == DocKind::Pdf && !EnsurePdfPageCoordinates(annotation.pageIndex)) return;
    const auto& page = g_pdf.pages[static_cast<size_t>(annotation.pageIndex)];
    annotation.coordinateRotation = page_rotation::Normalize(page.intrinsicRotation + page.viewRotation);
    annotation.projectedCoordinates = true;
    annotation.basisWidthPt = page.pdfWidthPt;
    annotation.basisHeightPt = page.pdfHeightPt;
    annotation.basisOriginX = page.pdfOriginX;
    annotation.basisOriginY = page.pdfOriginY;
}

static bool CanonicalizeAnnotation(Annotation& annotation) {
    auto canonical = annotation_rotation::Canonical(annotation);
    if (!canonical) return false;
    annotation = std::move(*canonical);
    return true;
}

#include "pdf_view/navigation.cppinc"

#include "pdf_view/render.cppinc"

#include "pdf_view/input.cppinc"
#include "pdf_view/initial_viewport_work_tests.cppinc"
#include "pdf_view/worker_concurrency_tests.cppinc"

// ---------------------------------------------------------------------
// helper for translucent fill
// ---------------------------------------------------------------------
static void FillRectAlpha(HDC hdc, const RECT& r, COLORREF color, BYTE alpha) {
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    // premultiply to avoid黒縁; AlphaBlend expects premultiplied colors when AC_SRC_ALPHA
    double a = alpha / 255.0;
    BYTE rC = static_cast<BYTE>(std::round(GetRValue(color) * a));
    BYTE gC = static_cast<BYTE>(std::round(GetGValue(color) * a));
    BYTE bC = static_cast<BYTE>(std::round(GetBValue(color) * a));
    DWORD premul = (static_cast<DWORD>(alpha) << 24) | (rC << 16) | (gC << 8) | bC;
    for (int y = 0; y < h; ++y) {
        DWORD* row = buf.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            row[x] = premul;
        }
    }
    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, buf.data(), buf.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA }; // use per-pixel alpha
        AlphaBlend(hdc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static void FillTextMarker(HDC hdc, const RECT& rectangle, COLORREF color, BYTE alpha, MarkerDisplayMode mode) {
    if (mode == MarkerDisplayMode::Legacy) {
        FillRectAlpha(hdc, rectangle, DarkenColor(color, 0.85), alpha);
        return;
    }
    if (!pdf_marker::Paint(hdc, rectangle, color, alpha)) {
        ShowSoftNotice(g_hPdfView, localization::Text(L"pdf.marker.display_failed"), SoftNoticeKind::Warning);
    }
}

static ShapeBounds NormalizeShapeBounds(ShapeKind kind, double x1, double y1, double x2, double y2) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    double signX = (dx >= 0.0) ? 1.0 : -1.0;
    double signY = (dy >= 0.0) ? 1.0 : -1.0;
    double width = std::abs(dx);
    double height = std::abs(dy);

    if (kind == ShapeKind::Square || kind == ShapeKind::Circle) {
        double side = std::min(width, height);
        x2 = x1 + signX * side;
        y2 = y1 + signY * side;
    } else if (kind == ShapeKind::Star) {
        const double ratio=(1+std::cos(shape_rotation::kPi/5))/(2*std::sin(2*shape_rotation::kPi/5));
        const double useWidth=std::min(width,height/ratio);
        x2=x1+signX*useWidth; y2=y1+signY*useWidth*ratio;
    } else if (kind == ShapeKind::EquilateralTriangle) {
        constexpr double kTriHeightRatio = 0.86602540378443864676; // sqrt(3) / 2
        double eqWidthFromHeight = height / kTriHeightRatio;
        double useWidth = std::min(width, eqWidthFromHeight);
        double useHeight = useWidth * kTriHeightRatio;
        x2 = x1 + signX * useWidth;
        y2 = y1 + signY * useHeight;
    }

    ShapeBounds out;
    out.left = std::min(x1, x2);
    out.right = std::max(x1, x2);
    out.bottom = std::min(y1, y2);
    out.top = std::max(y1, y2);
    return out;
}

static double Clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}

static double PtDistance(const Annotation::Pt& a, const Annotation::Pt& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

static bool PtFinite(const Annotation::Pt& p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

static double PathLengthPt(const std::vector<Annotation::Pt>& pts) {
    double len = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        len += PtDistance(pts[i - 1], pts[i]);
    }
    return len;
}

static ShapeBounds BoundsForPath(const std::vector<Annotation::Pt>& pts) {
    ShapeBounds b;
    if (pts.empty()) return b;
    b.left = b.right = pts.front().x;
    b.bottom = b.top = pts.front().y;
    for (const auto& p : pts) {
        b.left = std::min(b.left, p.x);
        b.right = std::max(b.right, p.x);
        b.bottom = std::min(b.bottom, p.y);
        b.top = std::max(b.top, p.y);
    }
    return b;
}

static double BoundsWidth(const ShapeBounds& b) {
    return std::max(0.0, b.right - b.left);
}

static double BoundsHeight(const ShapeBounds& b) {
    return std::max(0.0, b.top - b.bottom);
}

static double BoundsDiagonal(const ShapeBounds& b) {
    return std::hypot(BoundsWidth(b), BoundsHeight(b));
}

static double PointSegmentDistance(const Annotation::Pt& p, const Annotation::Pt& a, const Annotation::Pt& b) {
    double vx = b.x - a.x;
    double vy = b.y - a.y;
    double len2 = vx * vx + vy * vy;
    if (len2 <= 1e-9) return PtDistance(p, a);
    double t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2;
    t = std::clamp(t, 0.0, 1.0);
    Annotation::Pt proj{ a.x + vx * t, a.y + vy * t };
    return PtDistance(p, proj);
}

static double SignedPointLineDistance(const Annotation::Pt& p, const Annotation::Pt& a, const Annotation::Pt& b) {
    double vx = b.x - a.x;
    double vy = b.y - a.y;
    double len = std::hypot(vx, vy);
    if (len <= 1e-9) return 0.0;
    return ((p.x - a.x) * vy - (p.y - a.y) * vx) / len;
}

static Annotation BuildLineLikeCorrection(const Annotation& source,
                                          Annotation::Type type,
                                          ToolMode styleMode,
                                          const Annotation::Pt& a,
                                          const Annotation::Pt& b) {
    Annotation ann = source;
    ann.type = type;
    ann.x1 = a.x;
    ann.y1 = a.y;
    ann.x2 = b.x;
    ann.y2 = b.y;
    ann.path.clear();
    ann.quads.clear();
    if (type == Annotation::Type::Arrow) ann.arrowHead = g_arrowHead;
    if (NormalizeFreehandCorrectionStyle(g_config.freehandCorrectionStyle) == L"shape") {
        ann.color = ToolColorForMode(styleMode);
        ann.width = ToolWidthPtForMode(styleMode);
        if (ann.width <= 0.0) ann.width = source.width;
        ann.alpha = ToolAlphaForMode(styleMode);
    }
    return ann;
}

static void SimplifyRdpRange(const std::vector<Annotation::Pt>& src,
                             size_t first,
                             size_t last,
                             double tolerance,
                             std::vector<bool>& keep) {
    if (last <= first + 1) return;
    double maxDist = 0.0;
    size_t maxIndex = first;
    for (size_t i = first + 1; i < last; ++i) {
        double d = PointSegmentDistance(src[i], src[first], src[last]);
        if (d > maxDist) {
            maxDist = d;
            maxIndex = i;
        }
    }
    if (maxDist <= tolerance) return;
    keep[maxIndex] = true;
    SimplifyRdpRange(src, first, maxIndex, tolerance, keep);
    SimplifyRdpRange(src, maxIndex, last, tolerance, keep);
}

static std::vector<Annotation::Pt> SimplifyPathRdp(const std::vector<Annotation::Pt>& src, double tolerance) {
    if (src.size() <= 2) return src;
    std::vector<bool> keep(src.size(), false);
    keep.front() = true;
    keep.back() = true;
    SimplifyRdpRange(src, 0, src.size() - 1, tolerance, keep);
    std::vector<Annotation::Pt> out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (keep[i]) out.push_back(src[i]);
    }
    return out;
}

static std::vector<Annotation::Pt> NormalizedFreehandPoints(const std::vector<Annotation::Pt>& src,
                                                           double widthPt) {
    std::vector<Annotation::Pt> out;
    out.reserve(src.size());
    const double eps = std::max(0.25, widthPt * 0.15);
    for (const auto& p : src) {
        if (!PtFinite(p)) {
            out.clear();
            return out;
        }
        if (!out.empty() && PtDistance(out.back(), p) < eps) continue;
        out.push_back(p);
    }
    return out;
}

static std::vector<Annotation::Pt> SmoothFreehandPath(const std::vector<Annotation::Pt>& src) {
    if (src.size() <= 4) return src;
    std::vector<Annotation::Pt> out;
    out.reserve(src.size());
    out.push_back(src.front());
    for (size_t i = 1; i + 1 < src.size(); ++i) {
        const auto& a = src[i - 1];
        const auto& b = src[i];
        const auto& c = src[i + 1];
        out.push_back({
            a.x * 0.20 + b.x * 0.60 + c.x * 0.20,
            a.y * 0.20 + b.y * 0.60 + c.y * 0.20
        });
    }
    out.push_back(src.back());
    return out;
}

static bool TryBuildFreehandSmoothing(const Annotation& source, Annotation* out) {
    if (!out) return false;
    if (!FreehandSmoothingEnabled(g_config.freehandCorrection)) return false;
    if (source.type != Annotation::Type::Freehand) return false;
    if (source.path.size() < 5) return false;

    std::vector<Annotation::Pt> pts = NormalizedFreehandPoints(source.path, source.width);
    if (pts.size() < 5) return false;
    double pathLength = PathLengthPt(pts);
    double minPathLength = std::max(8.0, source.width * 4.0);
    if (pathLength < minPathLength) return false;

    double tolerance = std::max({ 0.5, source.width * 0.25, BoundsDiagonal(BoundsForPath(pts)) * 0.006 });
    std::vector<Annotation::Pt> simplified = SimplifyPathRdp(pts, tolerance);
    if (simplified.size() < 3) return false;
    std::vector<Annotation::Pt> smoothed = SmoothFreehandPath(simplified);
    if (smoothed.size() >= source.path.size()) return false;

    Annotation ann = source;
    ann.path = std::move(smoothed);
    if (!ann.path.empty()) {
        ann.x1 = ann.path.front().x;
        ann.y1 = ann.path.front().y;
        ann.x2 = ann.path.back().x;
        ann.y2 = ann.path.back().y;
    }
    *out = std::move(ann);
    return true;
}

static bool TryLineCorrection(const Annotation& source,
                              const std::vector<Annotation::Pt>& pts,
                              const ShapeBounds& bounds,
                              double pathLength,
                              double closedness,
                              Annotation* out,
                              double* confidence) {
    if (pts.size() < 2 || pathLength <= 0.0) return false;
    if (closedness <= 0.18) return false;
    const Annotation::Pt& a = pts.front();
    const Annotation::Pt& b = pts.back();
    double segmentLength = PtDistance(a, b);
    double diagonal = BoundsDiagonal(bounds);
    double minPathLength = std::max(8.0, source.width * 4.0);
    if (segmentLength < minPathLength) return false;
    if (diagonal > 0.0 && segmentLength < diagonal * 0.55) return false;
    double lengthRatio = segmentLength / pathLength;
    if (lengthRatio < 0.74) return false;

    double maxDeviation = 0.0;
    double sumDeviation = 0.0;
    for (const auto& p : pts) {
        double d = PointSegmentDistance(p, a, b);
        maxDeviation = std::max(maxDeviation, d);
        sumDeviation += d;
    }
    double meanDeviation = sumDeviation / static_cast<double>(pts.size());
    double maxLimit = std::max({ 4.5, source.width * 2.3, segmentLength * 0.14 });
    double meanLimit = std::max({ 2.5, source.width * 1.25, segmentLength * 0.065 });
    if (maxDeviation > maxLimit || meanDeviation > meanLimit) return false;

    double score =
        0.45 * Clamp01(lengthRatio) +
        0.35 * (1.0 - Clamp01(meanDeviation / meanLimit)) +
        0.20 * (1.0 - Clamp01(maxDeviation / maxLimit));
    if (score < 0.58) return false;

    *out = BuildLineLikeCorrection(source, Annotation::Type::Line, ToolMode::Line, a, b);
    *confidence = score;
    return true;
}

static bool TryArrowCorrection(const Annotation& source,
                               const std::vector<Annotation::Pt>& pts,
                               double pathLength,
                               double closedness,
                               Annotation* out,
                               double* confidence) {
    if (pts.size() < 6 || pathLength <= 0.0) return false;
    if (closedness <= 0.18) return false;
    const Annotation::Pt& start = pts.front();
    double shaftLen = 0.0;
    size_t tipIndex = 0;
    for (size_t i = 1; i < pts.size(); ++i) {
        double d = PtDistance(start, pts[i]);
        if (d > shaftLen) {
            shaftLen = d;
            tipIndex = i;
        }
    }
    double minShaft = std::max(12.0, source.width * 6.0);
    if (shaftLen < minShaft) return false;
    if (tipIndex < pts.size() / 3 || tipIndex + 2 >= pts.size()) return false;

    const Annotation::Pt& tip = pts[tipIndex];
    double shaftMaxDeviation = 0.0;
    double shaftSumDeviation = 0.0;
    for (size_t i = 0; i <= tipIndex; ++i) {
        double d = PointSegmentDistance(pts[i], start, tip);
        shaftMaxDeviation = std::max(shaftMaxDeviation, d);
        shaftSumDeviation += d;
    }
    double shaftMeanDeviation = shaftSumDeviation / static_cast<double>(tipIndex + 1);
    double maxShaftLimit = std::max({ 4.5, source.width * 2.2, shaftLen * 0.13 });
    double meanShaftLimit = std::max({ 2.5, source.width * 1.2, shaftLen * 0.07 });
    if (shaftMaxDeviation > maxShaftLimit || shaftMeanDeviation > meanShaftLimit) return false;

    std::vector<Annotation::Pt> afterTip;
    afterTip.reserve(pts.size() - tipIndex);
    for (size_t i = tipIndex; i < pts.size(); ++i) afterTip.push_back(pts[i]);
    double headTolerance = std::max({ 1.0, source.width * 0.8, shaftLen * 0.025 });
    auto simplifiedHead = SimplifyPathRdp(afterTip, headTolerance);

    struct WingCandidate {
        Annotation::Pt pt;
        double dist = 0.0;
        double side = 0.0;
        double cosToBack = 0.0;
    };
    std::vector<WingCandidate> wings;
    double backX = start.x - tip.x;
    double backY = start.y - tip.y;
    double backLen = std::hypot(backX, backY);
    if (backLen <= 0.0) return false;
    backX /= backLen;
    backY /= backLen;

    for (const auto& p : simplifiedHead) {
        double d = PtDistance(tip, p);
        if (d < shaftLen * 0.05 || d > shaftLen * 0.50) continue;
        double vx = p.x - tip.x;
        double vy = p.y - tip.y;
        double vLen = std::hypot(vx, vy);
        if (vLen <= 0.0) continue;
        double cosToBack = (vx / vLen) * backX + (vy / vLen) * backY;
        if (cosToBack < 0.15 || cosToBack > 0.985) continue;
        double side = vx * backY - vy * backX;
        if (std::abs(side) < shaftLen * 0.015) continue;
        wings.push_back({ p, d, side, cosToBack });
    }

    bool hasLeft = false;
    bool hasRight = false;
    double bestLeft = 0.0;
    double bestRight = 0.0;
    for (const auto& w : wings) {
        double quality = Clamp01(w.dist / (shaftLen * 0.16)) * Clamp01(std::abs(w.side) / (shaftLen * 0.08));
        if (w.side < 0.0) {
            hasLeft = true;
            bestLeft = std::max(bestLeft, quality);
        } else {
            hasRight = true;
            bestRight = std::max(bestRight, quality);
        }
    }
    if (!hasLeft || !hasRight) return false;

    double score =
        0.40 * (1.0 - Clamp01(shaftMeanDeviation / meanShaftLimit)) +
        0.25 * (1.0 - Clamp01(shaftMaxDeviation / maxShaftLimit)) +
        0.20 * Clamp01(std::min(bestLeft, bestRight)) +
        0.15 * Clamp01(shaftLen / pathLength);
    if (score < 0.58) return false;

    *out = BuildLineLikeCorrection(source, Annotation::Type::Arrow, ToolMode::Arrow, start, tip);
    *confidence = score;
    return true;
}

static bool TryComplexArrowCorrection(const Annotation& source,
                                      const std::vector<Annotation::Pt>& pts,
                                      double pathLength,
                                      double closedness,
                                      Annotation* out,
                                      double* confidence) {
    if (pts.size() < 8 || pathLength <= 0.0) return false;
    if (closedness <= 0.14) return false;

    const Annotation::Pt& start = pts.front();
    double shaftLen = 0.0;
    size_t tipIndex = 0;
    for (size_t i = 2; i + 2 < pts.size(); ++i) {
        double d = PtDistance(start, pts[i]);
        if (d > shaftLen) {
            shaftLen = d;
            tipIndex = i;
        }
    }

    double minShaft = std::max(12.0, source.width * 6.0);
    if (shaftLen < minShaft) return false;
    if (tipIndex < pts.size() / 3 || tipIndex + 2 >= pts.size()) return false;

    const Annotation::Pt& tip = pts[tipIndex];
    double axisX = tip.x - start.x;
    double axisY = tip.y - start.y;
    double axisLenSq = axisX * axisX + axisY * axisY;
    if (axisLenSq <= 1e-9) return false;

    std::vector<double> shaftDeviations;
    shaftDeviations.reserve(tipIndex + 1);
    double shaftSumDeviation = 0.0;
    int nearShaftCount = 0;
    const double nearShaftLimit = std::max({ 6.0, source.width * 3.0, shaftLen * 0.22 });
    for (size_t i = 0; i <= tipIndex; ++i) {
        const auto& p = pts[i];
        double projection = ((p.x - start.x) * axisX + (p.y - start.y) * axisY) / axisLenSq;
        if (projection < -0.08 || projection > 1.08) continue;
        double d = PointSegmentDistance(p, start, tip);
        shaftDeviations.push_back(d);
        shaftSumDeviation += d;
        if (d <= nearShaftLimit) ++nearShaftCount;
    }
    if (shaftDeviations.size() < std::max<size_t>(4, (tipIndex + 1) / 2)) return false;

    std::sort(shaftDeviations.begin(), shaftDeviations.end());
    double shaftMeanDeviation = shaftSumDeviation / static_cast<double>(shaftDeviations.size());
    double shaftP85Deviation = shaftDeviations[static_cast<size_t>(
        std::min<double>(shaftDeviations.size() - 1, std::floor(shaftDeviations.size() * 0.85)))];
    double nearShaftRatio = static_cast<double>(nearShaftCount) / static_cast<double>(shaftDeviations.size());
    double meanLimit = std::max({ 3.2, source.width * 1.6, shaftLen * 0.105 });
    double p85Limit = std::max({ 5.5, source.width * 2.6, shaftLen * 0.18 });
    if (nearShaftRatio < 0.62 || shaftMeanDeviation > meanLimit || shaftP85Deviation > p85Limit) {
        return false;
    }

    std::vector<Annotation::Pt> afterTip;
    afterTip.reserve(pts.size() - tipIndex);
    for (size_t i = tipIndex; i < pts.size(); ++i) afterTip.push_back(pts[i]);
    double headTolerance = std::max({ 1.2, source.width * 1.1, shaftLen * 0.035 });
    auto simplifiedHead = SimplifyPathRdp(afterTip, headTolerance);

    struct WingCandidate {
        double dist = 0.0;
        double side = 0.0;
    };
    std::vector<WingCandidate> wings;
    double backX = start.x - tip.x;
    double backY = start.y - tip.y;
    double backLen = std::hypot(backX, backY);
    if (backLen <= 0.0) return false;
    backX /= backLen;
    backY /= backLen;

    for (const auto& p : simplifiedHead) {
        double d = PtDistance(tip, p);
        if (d < shaftLen * 0.045 || d > shaftLen * 0.55) continue;
        double vx = p.x - tip.x;
        double vy = p.y - tip.y;
        double vLen = std::hypot(vx, vy);
        if (vLen <= 0.0) continue;
        double cosToBack = (vx / vLen) * backX + (vy / vLen) * backY;
        if (cosToBack < 0.10 || cosToBack > 0.99) continue;
        double side = vx * backY - vy * backX;
        if (std::abs(side) < shaftLen * 0.012) continue;
        wings.push_back({ d, side });
    }

    bool hasLeft = false;
    bool hasRight = false;
    double bestLeft = 0.0;
    double bestRight = 0.0;
    for (const auto& w : wings) {
        double quality = Clamp01(w.dist / (shaftLen * 0.16)) * Clamp01(std::abs(w.side) / (shaftLen * 0.07));
        if (w.side < 0.0) {
            hasLeft = true;
            bestLeft = std::max(bestLeft, quality);
        } else {
            hasRight = true;
            bestRight = std::max(bestRight, quality);
        }
    }
    if (!hasLeft || !hasRight) return false;

    double shaftPathRatio = shaftLen / pathLength;
    double score =
        0.30 * Clamp01(nearShaftRatio) +
        0.22 * (1.0 - Clamp01(shaftMeanDeviation / meanLimit)) +
        0.18 * (1.0 - Clamp01(shaftP85Deviation / p85Limit)) +
        0.20 * Clamp01(std::min(bestLeft, bestRight)) +
        0.10 * Clamp01(shaftPathRatio / 0.45);
    if (score < 0.58) return false;

    *out = BuildLineLikeCorrection(source, Annotation::Type::Arrow, ToolMode::Arrow, start, tip);
    *confidence = score;
    return true;
}

static bool TryWaveCorrection(const Annotation& source,
                              const std::vector<Annotation::Pt>& pts,
                              double pathLength,
                              double closedness,
                              Annotation* out,
                              double* confidence) {
    if (pts.size() < 8 || pathLength <= 0.0) return false;
    if (closedness <= 0.18) return false;
    const Annotation::Pt& a = pts.front();
    const Annotation::Pt& b = pts.back();
    double baseline = PtDistance(a, b);
    double minBaseline = std::max(16.0, source.width * 8.0);
    if (baseline < minBaseline) return false;
    double lengthRatio = pathLength / baseline;
    if (lengthRatio < 1.03 || lengthRatio > 2.80) return false;

    double maxAbs = 0.0;
    double sumAbs = 0.0;
    int crossings = 0;
    int prevSign = 0;
    double ignoreBand = std::max(0.35, source.width * 0.20);
    for (const auto& p : pts) {
        double d = SignedPointLineDistance(p, a, b);
        double absD = std::abs(d);
        maxAbs = std::max(maxAbs, absD);
        sumAbs += absD;
        int sign = (d > ignoreBand) ? 1 : (d < -ignoreBand ? -1 : 0);
        if (sign != 0) {
            if (prevSign != 0 && sign != prevSign) ++crossings;
            prevSign = sign;
        }
    }
    double meanAbs = sumAbs / static_cast<double>(pts.size());
    double minAmplitude = std::max(1.5, source.width * 0.75);
    if (maxAbs < minAmplitude * 0.8 || meanAbs < minAmplitude * 0.25) return false;
    if (crossings < 2) return false;
    if (maxAbs > baseline * 0.38) return false;

    double score =
        0.35 * Clamp01((lengthRatio - 1.0) / 0.45) +
        0.30 * Clamp01(static_cast<double>(crossings) / 6.0) +
        0.20 * Clamp01(maxAbs / std::max(minAmplitude, source.width * 2.0)) +
        0.15 * (1.0 - Clamp01(maxAbs / (baseline * 0.38)));
    if (score < 0.58) return false;

    *out = BuildLineLikeCorrection(source, Annotation::Type::Wave, ToolMode::Wave, a, b);
    *confidence = score;
    return true;
}

static bool TryEllipseCorrection(const Annotation& source,
                                 const std::vector<Annotation::Pt>& pts,
                                 const ShapeBounds& bounds,
                                 double pathLength,
                                 double closedness,
                                 Annotation* out,
                                 double* confidence) {
    double w = BoundsWidth(bounds);
    double h = BoundsHeight(bounds);
    double minSize = std::max(3.0, source.width * 1.5);
    if (pts.size() < 8 || w < minSize || h < minSize) return false;
    if (closedness > 0.40) return false;
    double rx = w * 0.5;
    double ry = h * 0.5;
    if (rx <= 0.0 || ry <= 0.0) return false;
    constexpr double kPi = 3.14159265358979323846;
    double circumferenceLower = 2.0 * kPi * std::min(rx, ry);
    if (pathLength < circumferenceLower * 0.50) return false;

    double cx = (bounds.left + bounds.right) * 0.5;
    double cy = (bounds.bottom + bounds.top) * 0.5;
    std::vector<double> radialErrors;
    radialErrors.reserve(pts.size());
    bool sectors[12]{};
    for (const auto& p : pts) {
        double nx = (p.x - cx) / rx;
        double ny = (p.y - cy) / ry;
        double radial = std::hypot(nx, ny);
        radialErrors.push_back(std::abs(radial - 1.0));
        double angle = std::atan2(ny, nx);
        if (angle < 0.0) angle += 2.0 * kPi;
        int sector = std::clamp(static_cast<int>(std::floor(angle / (2.0 * kPi) * 12.0)), 0, 11);
        sectors[sector] = true;
    }
    double sum = 0.0;
    for (double e : radialErrors) sum += e;
    double mean = sum / static_cast<double>(radialErrors.size());
    std::sort(radialErrors.begin(), radialErrors.end());
    size_t p90Index = std::min(radialErrors.size() - 1,
                               static_cast<size_t>(std::floor((radialErrors.size() - 1) * 0.90)));
    double p90 = radialErrors[p90Index];
    int covered = 0;
    for (bool sector : sectors) {
        if (sector) ++covered;
    }
    if (mean > 0.34 || p90 > 0.62 || covered < 7) return false;

    double score =
        0.35 * (1.0 - Clamp01(mean / 0.34)) +
        0.25 * (1.0 - Clamp01(p90 / 0.62)) +
        0.25 * Clamp01(static_cast<double>(covered) / 12.0) +
        0.15 * (1.0 - Clamp01(closedness / 0.40));
    if (score < 0.58) return false;

    double aspect = std::max(w, h) / std::max(0.001, std::min(w, h));
    ShapeKind kind = (aspect <= 1.25) ? ShapeKind::Circle : ShapeKind::Ellipse;
    ShapeBounds finalBounds = (kind == ShapeKind::Circle)
        ? NormalizeShapeBounds(ShapeKind::Circle, bounds.left, bounds.top, bounds.right, bounds.bottom)
        : bounds;

    Annotation ann = source;
    ann.type = Annotation::Type::Shape;
    ann.shapeKind = kind;
    ann.shapeDrawMode = ShapeDrawMode::Outline;
    ann.x1 = finalBounds.left;
    ann.y1 = finalBounds.top;
    ann.x2 = finalBounds.right;
    ann.y2 = finalBounds.bottom;
    ann.path.clear();
    ann.quads.clear();

    const std::wstring style = NormalizeFreehandCorrectionStyle(g_config.freehandCorrectionStyle);
    const std::wstring fill = NormalizeFreehandCorrectionFill(g_config.freehandCorrectionFill);
    if (style == L"shape") {
        ann.color = g_shapeColor;
        ann.alpha = g_shapeAlpha;
        ann.width = (g_lineWidthPt > 0.0) ? g_lineWidthPt : ann.width;
    }
    if (fill == L"use_shape_setting") {
        ann.shapeDrawMode = g_shapeDrawMode;
        ann.alpha = (ann.shapeDrawMode == ShapeDrawMode::Fill) ? g_shapeAlpha : ann.alpha;
    } else if (fill == L"always_fill") {
        ann.shapeDrawMode = ShapeDrawMode::Fill;
        ann.alpha = g_shapeAlpha;
        if (style != L"pen") ann.color = g_shapeColor;
    }
    *out = ann;
    *confidence = score;
    return true;
}

static double NormalizedTargetDistance(const Annotation::Pt& p,
                                       double tx,
                                       double ty,
                                       const ShapeBounds& bounds) {
    double w = std::max(0.001, BoundsWidth(bounds));
    double h = std::max(0.001, BoundsHeight(bounds));
    double nx = (p.x - bounds.left) / w;
    double ny = (p.y - bounds.bottom) / h;
    return std::hypot(nx - tx, ny - ty);
}

static int CountCornersNearTargets(const std::vector<Annotation::Pt>& corners,
                                   const ShapeBounds& bounds,
                                   const std::vector<std::pair<double, double>>& targets,
                                   double limit) {
    int hits = 0;
    std::vector<bool> used(corners.size(), false);
    for (const auto& target : targets) {
        double best = std::numeric_limits<double>::max();
        int bestIndex = -1;
        for (size_t i = 0; i < corners.size(); ++i) {
            if (used[i]) continue;
            double d = NormalizedTargetDistance(corners[i], target.first, target.second, bounds);
            if (d < best) {
                best = d;
                bestIndex = static_cast<int>(i);
            }
        }
        if (bestIndex >= 0 && best <= limit) {
            used[static_cast<size_t>(bestIndex)] = true;
            ++hits;
        }
    }
    return hits;
}

static bool TryPolygonShapeCorrection(const Annotation& source,
                                      const std::vector<Annotation::Pt>& pts,
                                      const ShapeBounds& bounds,
                                      double closedness,
                                      Annotation* out,
                                      double* confidence) {
    double w = BoundsWidth(bounds);
    double h = BoundsHeight(bounds);
    double minSize = std::max(5.0, source.width * 2.0);
    if (pts.size() < 6 || w < minSize || h < minSize) return false;
    if (closedness > 0.38) return false;

    double tolerance = std::max({ 1.2, source.width * 0.95, BoundsDiagonal(bounds) * 0.055 });
    std::vector<Annotation::Pt> closed = pts;
    if (PtDistance(closed.front(), closed.back()) > tolerance) {
        closed.push_back(closed.front());
    }
    std::vector<Annotation::Pt> simplified = SimplifyPathRdp(closed, tolerance);
    if (simplified.size() >= 2 && PtDistance(simplified.front(), simplified.back()) <= tolerance * 1.5) {
        simplified.pop_back();
    }
    if (simplified.size() < 3 || simplified.size() > 7) return false;

    const std::vector<std::pair<double, double>> rectTargets = {
        {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}
    };
    const std::vector<std::pair<double, double>> diamondTargets = {
        {0.5, 1.0}, {1.0, 0.5}, {0.5, 0.0}, {0.0, 0.5}
    };
    const std::vector<std::pair<double, double>> triangleTargets = {
        {0.5, 1.0}, {1.0, 0.0}, {0.0, 0.0}
    };

    ShapeKind kind = ShapeKind::Rectangle;
    double score = 0.0;
    if (simplified.size() == 4 || simplified.size() == 5) {
        int rectHits = CountCornersNearTargets(simplified, bounds, rectTargets, 0.34);
        int diamondHits = CountCornersNearTargets(simplified, bounds, diamondTargets, 0.32);
        if (rectHits >= 4) {
            double aspect = std::max(w, h) / std::max(0.001, std::min(w, h));
            kind = (aspect <= 1.25) ? ShapeKind::Square : ShapeKind::Rectangle;
            score = 0.66;
        } else if (diamondHits >= 4) {
            kind = ShapeKind::Diamond;
            score = 0.64;
        } else {
            return false;
        }
    } else if (simplified.size() == 3) {
        int triHits = CountCornersNearTargets(simplified, bounds, triangleTargets, 0.36);
        if (triHits < 3) return false;
        std::array<double, 3> sideLens{
            PtDistance(simplified[0], simplified[1]),
            PtDistance(simplified[1], simplified[2]),
            PtDistance(simplified[2], simplified[0])
        };
        double minSide = std::max(0.001, *std::min_element(sideLens.begin(), sideLens.end()));
        double maxSide = *std::max_element(sideLens.begin(), sideLens.end());
        kind = (maxSide / minSide <= 1.28) ? ShapeKind::EquilateralTriangle : ShapeKind::Triangle;
        score = 0.64;
    } else {
        return false;
    }

    Annotation ann = source;
    ann.type = Annotation::Type::Shape;
    ann.shapeKind = kind;
    ann.shapeDrawMode = ShapeDrawMode::Outline;
    ShapeBounds finalBounds = (kind == ShapeKind::Square)
        ? NormalizeShapeBounds(ShapeKind::Square, bounds.left, bounds.top, bounds.right, bounds.bottom)
        : (kind == ShapeKind::EquilateralTriangle
               ? NormalizeShapeBounds(ShapeKind::EquilateralTriangle, bounds.left, bounds.top, bounds.right, bounds.bottom)
               : bounds);
    ann.x1 = finalBounds.left;
    ann.y1 = finalBounds.top;
    ann.x2 = finalBounds.right;
    ann.y2 = finalBounds.bottom;
    ann.path.clear();
    ann.quads.clear();

    const std::wstring style = NormalizeFreehandCorrectionStyle(g_config.freehandCorrectionStyle);
    const std::wstring fill = NormalizeFreehandCorrectionFill(g_config.freehandCorrectionFill);
    if (style == L"shape") {
        ann.color = g_shapeColor;
        ann.alpha = g_shapeAlpha;
        ann.width = (g_lineWidthPt > 0.0) ? g_lineWidthPt : ann.width;
    }
    if (fill == L"use_shape_setting") {
        ann.shapeDrawMode = g_shapeDrawMode;
        ann.alpha = (ann.shapeDrawMode == ShapeDrawMode::Fill) ? g_shapeAlpha : ann.alpha;
    } else if (fill == L"always_fill") {
        ann.shapeDrawMode = ShapeDrawMode::Fill;
        ann.alpha = g_shapeAlpha;
        if (style != L"pen") ann.color = g_shapeColor;
    }
    *out = ann;
    *confidence = score;
    return true;
}

static bool TryBuildFreehandCorrection(const Annotation& source, FreehandCorrectionResult* out) {
    if (out) *out = {};
    if (!out) return false;
    if (!FreehandCorrectionEnabled(g_config.freehandCorrection)) return false;
    if (source.type != Annotation::Type::Freehand) return false;
    if (source.path.size() < 2) return false;

    std::vector<Annotation::Pt> pts = NormalizedFreehandPoints(source.path, source.width);
    if (pts.size() < 2) return false;
    ShapeBounds bounds = BoundsForPath(pts);
    double pathLength = PathLengthPt(pts);
    double minPathLength = std::max(8.0, source.width * 4.0);
    if (pathLength < minPathLength) return false;
    double diagonal = BoundsDiagonal(bounds);
    if (diagonal <= 0.0) return false;
    double endpointDistance = PtDistance(pts.front(), pts.back());
    double closedness = endpointDistance / std::max(0.001, std::max(BoundsWidth(bounds), BoundsHeight(bounds)));

    Annotation best;
    double bestConfidence = 0.0;
    Annotation candidate;
    double score = 0.0;
    if (TryArrowCorrection(source, pts, pathLength, closedness, &candidate, &score)) {
        best = candidate;
        bestConfidence = score;
    }
    if (TryComplexArrowCorrection(source, pts, pathLength, closedness, &candidate, &score) &&
        score > bestConfidence) {
        best = candidate;
        bestConfidence = score;
    }
    if (TryWaveCorrection(source, pts, pathLength, closedness, &candidate, &score) &&
        score > bestConfidence) {
        best = candidate;
        bestConfidence = score;
    }
    if (TryLineCorrection(source, pts, bounds, pathLength, closedness, &candidate, &score)) {
        if (score > bestConfidence) {
            best = candidate;
            bestConfidence = score;
        }
    }
    if (TryEllipseCorrection(source, pts, bounds, pathLength, closedness, &candidate, &score) &&
        score > bestConfidence) {
        best = candidate;
        bestConfidence = score;
    }
    if (TryPolygonShapeCorrection(source, pts, bounds, closedness, &candidate, &score) &&
        score > bestConfidence) {
        best = candidate;
        bestConfidence = score;
    }
    if (bestConfidence < 0.58) return false;
    best.pageIndex = source.pageIndex;
    best.id = source.id;
    out->accepted = true;
    out->corrected = std::move(best);
    out->confidence = bestConfidence;
    return true;
}

bool TryCorrectFreehandAnnotation(const Annotation& source, Annotation* corrected) {
    if (!corrected) return false;
    FreehandCorrectionResult result;
    if (!TryBuildFreehandCorrection(source, &result) || !result.accepted) return false;
    *corrected = std::move(result.corrected);
    return true;
}

static void FillPolygonAlphaPx(HDC hdc, const std::vector<POINT>& pts, COLORREF color, BYTE alpha) {
    if (pts.size() < 3 || alpha == 0) return;
    LONG minX = pts[0].x;
    LONG maxX = pts[0].x;
    LONG minY = pts[0].y;
    LONG maxY = pts[0].y;
    for (const auto& p : pts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    int w = static_cast<int>(maxX - minX + 1);
    int h = static_cast<int>(maxY - minY + 1);
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    const double a = alpha / 255.0;
    const DWORD premul = (static_cast<DWORD>(alpha) << 24) |
                         (static_cast<DWORD>(std::lround(GetRValue(color) * a)) << 16) |
                         (static_cast<DWORD>(std::lround(GetGValue(color) * a)) << 8) |
                         static_cast<DWORD>(std::lround(GetBValue(color) * a));

    std::vector<POINT> local;
    local.reserve(pts.size());
    for (const auto& p : pts) {
        local.push_back(POINT{ p.x - minX, p.y - minY });
    }
    auto inside = [&](double x, double y) {
        bool hit = false;
        size_t j = local.size() - 1;
        for (size_t i = 0; i < local.size(); ++i) {
            const double xi = static_cast<double>(local[i].x);
            const double yi = static_cast<double>(local[i].y);
            const double xj = static_cast<double>(local[j].x);
            const double yj = static_cast<double>(local[j].y);
            const bool intersect = ((yi > y) != (yj > y)) &&
                                   (x < (xj - xi) * (y - yi) / ((yj - yi) == 0.0 ? 1.0 : (yj - yi)) + xi);
            if (intersect) hit = !hit;
            j = i;
        }
        return hit;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (inside(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5)) {
                buf[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = premul;
            }
        }
    }

    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, buf.data(), buf.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, minX, minY, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static void FillEllipseAlphaPx(HDC hdc, const RECT& r, COLORREF color, BYTE alpha) {
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || alpha == 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    const double a = alpha / 255.0;
    const DWORD premul = (static_cast<DWORD>(alpha) << 24) |
                         (static_cast<DWORD>(std::lround(GetRValue(color) * a)) << 16) |
                         (static_cast<DWORD>(std::lround(GetGValue(color) * a)) << 8) |
                         static_cast<DWORD>(std::lround(GetBValue(color) * a));
    const double rx = static_cast<double>(w) * 0.5;
    const double ry = static_cast<double>(h) * 0.5;
    if (rx <= 0.0 || ry <= 0.0) return;
    for (int y = 0; y < h; ++y) {
        double ny = ((static_cast<double>(y) + 0.5) - ry) / ry;
        for (int x = 0; x < w; ++x) {
            double nx = ((static_cast<double>(x) + 0.5) - rx) / rx;
            if (nx * nx + ny * ny <= 1.0) {
                buf[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = premul;
            }
        }
    }

    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, buf.data(), buf.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static double SrgbLinearComponent(BYTE v) {
    double c = static_cast<double>(v) / 255.0;
    return (c <= 0.03928) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

static double RelativeLuminance(COLORREF c) {
    return 0.2126 * SrgbLinearComponent(GetRValue(c)) +
           0.7152 * SrgbLinearComponent(GetGValue(c)) +
           0.0722 * SrgbLinearComponent(GetBValue(c));
}

static double ContrastRatio(COLORREF a, COLORREF b) {
    double la = RelativeLuminance(a);
    double lb = RelativeLuminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

static int ColorChannelDistance(COLORREF a, COLORREF b) {
    int dr = std::abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b)));
    int dg = std::abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b)));
    int db = std::abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
    return std::max(dr, std::max(dg, db));
}

static COLORREF ReadableOverlayColorForBackground(COLORREF bg) {
    return ContrastRatio(RGB(0, 0, 0), bg) >= ContrastRatio(RGB(255, 255, 255), bg)
        ? RGB(0, 0, 0)
        : RGB(255, 255, 255);
}

static bool SamplePagePixelColor(const PageCache& page, int pageX, int pageY, COLORREF& out) {
    if (page.pixels.empty() || page.stride <= 0 || page.w <= 0 || page.h <= 0 ||
        page.bmpW <= 0 || page.bmpH <= 0) {
        return false;
    }
    const double scaleX = static_cast<double>(page.bmpW) / static_cast<double>(page.w);
    const double scaleY = static_cast<double>(page.bmpH) / static_cast<double>(page.h);
    const int srcX = std::clamp(static_cast<int>(std::lround(pageX * scaleX)), 0, page.bmpW - 1);
    const int srcY = std::clamp(static_cast<int>(std::lround(pageY * scaleY)), 0, page.bmpH - 1);
    const uint8_t* src = page.pixels.data() + static_cast<size_t>(srcY) * page.stride + static_cast<size_t>(srcX) * 4;
    out = RGB(src[2], src[1], src[0]);
    return true;
}

static RECT PdfRectToPagePixelRect(const PageCache& page, double leftPt, double bottomPt,
                                   double rightPt, double topPt, double scale) {
    const double pxPerPt = kDpi / 72.0 * scale;
    int l = static_cast<int>(std::floor(leftPt * pxPerPt));
    int r = static_cast<int>(std::ceil(rightPt * pxPerPt));
    int t = static_cast<int>(std::floor((page.heightPt - topPt) * pxPerPt));
    int b = static_cast<int>(std::ceil((page.heightPt - bottomPt) * pxPerPt));
    if (l > r) std::swap(l, r);
    if (t > b) std::swap(t, b);
    l = std::clamp(l, 0, std::max(0, page.w - 1));
    r = std::clamp(r, 0, std::max(0, page.w - 1));
    t = std::clamp(t, 0, std::max(0, page.h - 1));
    b = std::clamp(b, 0, std::max(0, page.h - 1));
    return RECT{ l, t, r, b };
}

static bool AveragePagePixelBorderColor(const PageCache& page, const RECT& inner, int padPx,
                                        COLORREF& out) {
    if (inner.left > inner.right || inner.top > inner.bottom) return false;
    const int l = static_cast<int>(inner.left);
    const int r = static_cast<int>(inner.right);
    const int t = static_cast<int>(inner.top);
    const int b = static_cast<int>(inner.bottom);
    const int exL = std::clamp(l - padPx, 0, std::max(0, page.w - 1));
    const int exR = std::clamp(r + padPx, 0, std::max(0, page.w - 1));
    const int exT = std::clamp(t - padPx, 0, std::max(0, page.h - 1));
    const int exB = std::clamp(b + padPx, 0, std::max(0, page.h - 1));

    unsigned long long sumR = 0;
    unsigned long long sumG = 0;
    unsigned long long sumB = 0;
    unsigned int count = 0;
    const int step = std::max(1, std::max(exR - exL, exB - exT) / 24);
    for (int y = exT; y <= exB; y += step) {
        for (int x = exL; x <= exR; x += step) {
            if (x >= l && x <= r && y >= t && y <= b) continue;
            COLORREF c{};
            if (!SamplePagePixelColor(page, x, y, c)) continue;
            sumR += GetRValue(c);
            sumG += GetGValue(c);
            sumB += GetBValue(c);
            ++count;
        }
    }
    if (count == 0) return false;
    out = RGB(static_cast<BYTE>(sumR / count),
              static_cast<BYTE>(sumG / count),
              static_cast<BYTE>(sumB / count));
    return true;
}

static bool PagePixelRectHasVisibleInk(const PageCache& page, const RECT& inner, COLORREF bg,
                                       int channelThreshold, double contrastThreshold,
                                       int sampleDivisor) {
    if (inner.left >= inner.right || inner.top >= inner.bottom) return false;
    const int l = static_cast<int>(inner.left);
    const int r = static_cast<int>(inner.right);
    const int t = static_cast<int>(inner.top);
    const int b = static_cast<int>(inner.bottom);
    const int step = std::max(1, std::max(r - l, b - t) / sampleDivisor);
    for (int y = t; y <= b; y += step) {
        for (int x = l; x <= r; x += step) {
            COLORREF c{};
            if (!SamplePagePixelColor(page, x, y, c)) continue;
            if (ColorChannelDistance(c, bg) >= channelThreshold ||
                ContrastRatio(c, bg) >= contrastThreshold) {
                return true;
            }
        }
    }
    return false;
}

static void TintRectTextPixels(HDC hdc, const RECT& r, COLORREF color,
                               const PageCache& page, int pageLeft, int pageTop) {
    const RECT pageRect{pageLeft, pageTop, pageLeft + page.w, pageTop + page.h};
    RECT clip{}, area{}, visible{};
    if (GetClipBox(hdc, &clip) == ERROR || !IntersectRect(&area, &r, &pageRect) ||
        !IntersectRect(&visible, &area, &clip)) return;
    if (!EnsurePdfPageTextCoverage(page.index)) return;
    if (!pdf_text_coverage::Paint(hdc, r, color, pageRect, page.textCoverage,
                                  page.textCoverageW, page.textCoverageH)) {
        ShowSoftNotice(g_hPdfView, localization::Text(L"pdf.text_color.display_failed"), SoftNoticeKind::Warning);
    }
}

static void DrawPolylineAlphaPx(HDC hdc, const std::vector<POINT>& pts, int penW, COLORREF color, BYTE alpha) {
    if (pts.size() < 2 || penW <= 0 || alpha == 0) return;
    LONG minX = pts[0].x, maxX = pts[0].x;
    LONG minY = pts[0].y, maxY = pts[0].y;
    for (const auto& p : pts) {
        minX = std::min<LONG>(minX, p.x);
        maxX = std::max<LONG>(maxX, p.x);
        minY = std::min<LONG>(minY, p.y);
        maxY = std::max<LONG>(maxY, p.y);
    }
    int margin = penW / 2 + 2;
    int w = (maxX - minX + 1) + margin * 2;
    int h = (maxY - minY + 1) + margin * 2;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        return;
    }
    std::fill_n(static_cast<DWORD*>(bits), static_cast<size_t>(w) * h, 0);

    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ oldBmp = SelectObject(mem, hbmp);
    // IMPORTANT: draw with a non-zero mask color so that black strokes are distinguishable
    // from the transparent background in the DIB (0x00000000).
    HPEN pen = CreatePen(PS_SOLID, penW, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(mem, pen);
    std::vector<POINT> local;
    local.reserve(pts.size());
    for (const auto& p : pts) {
        POINT lp{};
        lp.x = p.x - minX + margin;
        lp.y = p.y - minY + margin;
        local.push_back(lp);
    }
    Polyline(mem, local.data(), static_cast<int>(local.size()));
    SelectObject(mem, oldPen);
    DeleteObject(pen);

    // convert drawn pixels to premultiplied alpha so背景は透明のまま
    DWORD* px = static_cast<DWORD*>(bits);
    double a = alpha / 255.0;
    BYTE rT = static_cast<BYTE>(std::round(GetRValue(color) * a));
    BYTE gT = static_cast<BYTE>(std::round(GetGValue(color) * a));
    BYTE bT = static_cast<BYTE>(std::round(GetBValue(color) * a));
    for (int i = 0; i < w * h; ++i) {
        if (px[i] == 0) {
            px[i] = 0; // keep transparent
            continue;
        }
        px[i] = (static_cast<DWORD>(alpha) << 24) | (static_cast<DWORD>(rT) << 16) | (static_cast<DWORD>(gT) << 8) | bT;
    }

    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(hdc, minX - margin, minY - margin, w, h, mem, 0, 0, w, h, bf);

    SelectObject(mem, oldBmp);
    DeleteObject(hbmp);
    DeleteDC(mem);
}

static std::vector<POINT> BuildEllipseOutlinePointsPx(const RECT& r) {
    constexpr double kTwoPi = 6.28318530717958647692;
    std::vector<POINT> pts;
    const int w = std::max<int>(1, static_cast<int>(r.right - r.left));
    const int h = std::max<int>(1, static_cast<int>(r.bottom - r.top));
    const int segments = std::max(24, std::min(96, std::max(w, h) / 6));
    const double cx = (static_cast<double>(r.left) + static_cast<double>(r.right)) * 0.5;
    const double cy = (static_cast<double>(r.top) + static_cast<double>(r.bottom)) * 0.5;
    const double rx = std::max(0.5, static_cast<double>(w) * 0.5);
    const double ry = std::max(0.5, static_cast<double>(h) * 0.5);
    pts.reserve(static_cast<size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        double t = (static_cast<double>(i) / static_cast<double>(segments)) * kTwoPi;
        POINT pt{};
        pt.x = static_cast<LONG>(std::lround(cx + std::cos(t) * rx));
        pt.y = static_cast<LONG>(std::lround(cy + std::sin(t) * ry));
        pts.push_back(pt);
    }
    return pts;
}

static double EffectiveRotatedEllipseAngleRad(double angleRad) {
    return std::isfinite(angleRad) ? angleRad : kDefaultRotatedEllipseAngleRad;
}

static double ShapeRotationFromClientEndpoints(double x1, double y1, double x2, double y2) {
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) {
        return kDefaultRotatedEllipseAngleRad;
    }
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    if ((dx * dx + dy * dy) < 0.25) {
        return kDefaultRotatedEllipseAngleRad;
    }
    return std::atan2(dy, dx);
}

static std::vector<POINT> BuildRotatedEllipseOutlinePointsPx(const RECT& r, double angleRad) {
    constexpr double kTwoPi = 6.28318530717958647692;
    std::vector<POINT> pts;
    const int w = std::max<int>(1, static_cast<int>(r.right - r.left));
    const int h = std::max<int>(1, static_cast<int>(r.bottom - r.top));
    const int segments = std::max(32, std::min(128, std::max(w, h) / 4));
    const double cx = (static_cast<double>(r.left) + static_cast<double>(r.right)) * 0.5;
    const double cy = (static_cast<double>(r.top) + static_cast<double>(r.bottom)) * 0.5;
    const double rx = std::max(0.5, static_cast<double>(w) * 0.5);
    const double ry = std::max(0.5, static_cast<double>(h) * 0.5);
    const double angle = EffectiveRotatedEllipseAngleRad(angleRad);
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double extentX = std::sqrt((rx * c) * (rx * c) + (ry * s) * (ry * s));
    const double extentY = std::sqrt((rx * s) * (rx * s) + (ry * c) * (ry * c));
    const double fit = std::min(rx / std::max(0.5, extentX), ry / std::max(0.5, extentY));
    pts.reserve(static_cast<size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const double t = (static_cast<double>(i) / static_cast<double>(segments)) * kTwoPi;
        const double x = std::cos(t) * rx * fit;
        const double y = std::sin(t) * ry * fit;
        POINT pt{};
        pt.x = static_cast<LONG>(std::lround(cx + x * c - y * s));
        pt.y = static_cast<LONG>(std::lround(cy + x * s + y * c));
        pts.push_back(pt);
    }
    return pts;
}

static void DrawShapeOutlineAlphaPx(HDC hdc, ShapeKind kind, const RECT& r, const std::vector<POINT>& pts,
                                    int penW, COLORREF color, BYTE alpha) {
    if (penW <= 0 || alpha == 0) return;
    if (kind == ShapeKind::Rectangle || kind == ShapeKind::Square) {
        std::vector<POINT> border = {
            { r.left, r.top },
            { r.right, r.top },
            { r.right, r.bottom },
            { r.left, r.bottom },
            { r.left, r.top }
        };
        DrawPolylineAlphaPx(hdc, border, penW, color, alpha);
        return;
    }
    if (kind == ShapeKind::Ellipse || kind == ShapeKind::Circle) {
        DrawPolylineAlphaPx(hdc, BuildEllipseOutlinePointsPx(r), penW, color, alpha);
        return;
    }
    if (kind == ShapeKind::RotatedEllipse) {
        if (pts.size() >= 3) {
            DrawPolylineAlphaPx(hdc, pts, penW, color, alpha);
        } else {
            DrawPolylineAlphaPx(hdc, BuildRotatedEllipseOutlinePointsPx(r, kDefaultRotatedEllipseAngleRad), penW, color, alpha);
        }
        return;
    }
    if (pts.size() >= 3) {
        std::vector<POINT> border = pts;
        border.push_back(pts.front());
        DrawPolylineAlphaPx(hdc, border, penW, color, alpha);
    }
}

static COLORREF DarkenColor(COLORREF c, double factor) {
    factor = std::clamp(factor, 0.0, 1.0);
    int r = static_cast<int>(std::round(GetRValue(c) * factor));
    int g = static_cast<int>(std::round(GetGValue(c) * factor));
    int b = static_cast<int>(std::round(GetBValue(c) * factor));
    return RGB(r, g, b);
}

static TextBackgroundAssistMode CurrentTextBoxBackgroundAssistMode() {
    // A disabled toolbar leaves older text eligible for the global preference.
    if (!g_textBoxReadableBackground) return TextBackgroundAssistMode::Inherit;
    return g_textBoxReadableBackgroundInverted
        ? TextBackgroundAssistMode::Inverted
        : TextBackgroundAssistMode::Auto;
}

void EndInlineTextEditing(HWND pdfWnd, bool commit) {
    if (g_pdf.editingText) {
        CommitTextEditing(pdfWnd, commit);
    }
}

bool TryBuildCurrentAnnotationSaveSnapshot(std::vector<Annotation>* outAnnotations) {
    if (!outAnnotations) return false;

    const auto* logical = CurrentLogicalPdfAnnotations();
    *outAnnotations = logical ? *logical : g_annots;
    for (auto& annotation : *outAnnotations) if (!CanonicalizeAnnotation(annotation)) return false;

    if (!g_pdf.editingText) return true;
    if (g_pdf.imeComposing) return false;

    enum class PendingCommitAction {
        None,
        Add,
        Update,
        Remove,
        RestoreOriginal,
    };

    PendingCommitAction pendingAction = PendingCommitAction::None;
    Annotation pendingAnnot;
    int pendingIndex = -1;
    const bool editingExistingAnnot = g_editingExistingAnnot;
    const int editingOriginalIndex = g_editingOriginalIndex;
    const Annotation editingOriginal = g_editingOriginal;

    if (g_pdf.editPage >= 0) {
        const std::wstring committedText = NormalizeTextBoxTextForCommit(g_pdf.editText);
        if (!committedText.empty()) {
            Annotation ann;
            ann.type = Annotation::Type::TextBox;
            if (editingExistingAnnot) {
                ann.id = editingOriginal.id;
            }
            ann.pageIndex = g_pdf.editPage;
            ann.color = g_activeColor;
            double left = std::min(g_pdf.editX1, g_pdf.editX2);
            double right = std::max(g_pdf.editX1, g_pdf.editX2);
            double top = std::max(g_pdf.editY1, g_pdf.editY2);
            double bottom = std::min(g_pdf.editY1, g_pdf.editY2);
            ann.x1 = left;
            ann.y1 = top;
            ann.x2 = right;
            ann.y2 = bottom;
            SetEditingAnnotationBounds(ann);
            ann.text = committedText;
            ann.fontName = g_pdf.editFontName;
            ann.fontPt = g_pdf.editFontPt;
            ann.backgroundAssistMode = CurrentTextBoxBackgroundAssistMode();
            FinalizeTextBoxAnnotationLayout(ann);
            if (editingExistingAnnot && CanonicalizeAnnotation(ann)) {
                if ((ann.x1 > ann.x2) != (editingOriginal.x1 > editingOriginal.x2)) std::swap(ann.x1, ann.x2);
                if ((ann.y1 > ann.y2) != (editingOriginal.y1 > editingOriginal.y2)) std::swap(ann.y1, ann.y2);
            }

            if (editingExistingAnnot && AnnotationEquals(editingOriginal, ann)) {
                pendingAction = PendingCommitAction::RestoreOriginal;
                pendingAnnot = editingOriginal;
                pendingIndex = editingOriginalIndex;
            } else {
                pendingAnnot = std::move(ann);
                pendingIndex = static_cast<int>(outAnnotations->size());
                if (editingExistingAnnot) {
                    pendingIndex = std::clamp(editingOriginalIndex, 0, static_cast<int>(outAnnotations->size()));
                    pendingAction = PendingCommitAction::Update;
                } else {
                    pendingAction = PendingCommitAction::Add;
                }
            }
        } else if (editingExistingAnnot) {
            pendingAction = PendingCommitAction::Remove;
        }
    } else if (editingExistingAnnot) {
        pendingAction = PendingCommitAction::RestoreOriginal;
        pendingAnnot = editingOriginal;
        pendingIndex = editingOriginalIndex;
    }

    switch (pendingAction) {
    case PendingCommitAction::Add:
    case PendingCommitAction::Update:
    case PendingCommitAction::RestoreOriginal:
        if (pendingIndex < 0 || pendingIndex > static_cast<int>(outAnnotations->size())) {
            pendingIndex = static_cast<int>(outAnnotations->size());
        }
        outAnnotations->insert(outAnnotations->begin() + pendingIndex, pendingAnnot);
        break;
    case PendingCommitAction::Remove:
    case PendingCommitAction::None:
        break;
    }

    EnsureAnnotationIds(*outAnnotations);

    for (auto& annotation : *outAnnotations) if (!CanonicalizeAnnotation(annotation)) return false;
    return true;
}

static void CommitTextEditing(HWND hwnd, bool commit) {
    if (!g_pdf.editingText) return;
    ForceImeComplete(hwnd, commit);
    const bool editingExistingAnnot = g_editingExistingAnnot;
    const int editingOriginalIndex = g_editingOriginalIndex;
    const Annotation editingOriginal = g_editingOriginal;
    const HWND owner = GetParent(hwnd);

    enum class PendingCommitAction {
        None,
        Add,
        Update,
        Remove,
        RestoreOriginal,
    };

    PendingCommitAction pendingAction = PendingCommitAction::None;
    Annotation pendingAnnot;
    int pendingIndex = -1;
    int pendingBeforeIndex = -1;
    bool refreshAnnotPanelOnly = false;

    if (commit && g_pdf.editPage >= 0) {
        g_pdf.editText = NormalizeTextBoxTextForCommit(g_pdf.editText);
        g_pdf.editCaret = std::min(g_pdf.editCaret, g_pdf.editText.size());
        g_pdf.editSelStart = std::min(g_pdf.editSelStart, g_pdf.editText.size());
        g_pdf.editSelEnd = std::min(g_pdf.editSelEnd, g_pdf.editText.size());
        if (!g_pdf.editText.empty()) {
            // Normalize trailing blank lines before measuring the final TextBox rectangle.
            RecalcEditingTextboxSize(false);

            Annotation ann;
            ann.type = Annotation::Type::TextBox;
            if (editingExistingAnnot) {
                ann.id = editingOriginal.id;
            }
            ann.pageIndex = g_pdf.editPage;
            ann.color = g_activeColor;
            double left = std::min(g_pdf.editX1, g_pdf.editX2);
            double right = std::max(g_pdf.editX1, g_pdf.editX2);
            double top = std::max(g_pdf.editY1, g_pdf.editY2);
            double bottom = std::min(g_pdf.editY1, g_pdf.editY2);
            ann.x1 = left;
            ann.y1 = top;
            ann.x2 = right;
            ann.y2 = bottom;
            SetEditingAnnotationBounds(ann);
            ann.text = g_pdf.editText;
            ann.fontName = g_pdf.editFontName;
            ann.fontPt = g_pdf.editFontPt;
            ann.backgroundAssistMode = CurrentTextBoxBackgroundAssistMode();
            FinalizeTextBoxAnnotationLayout(ann);
            if (editingExistingAnnot && CanonicalizeAnnotation(ann)) {
                if ((ann.x1 > ann.x2) != (editingOriginal.x1 > editingOriginal.x2)) std::swap(ann.x1, ann.x2);
                if ((ann.y1 > ann.y2) != (editingOriginal.y1 > editingOriginal.y2)) std::swap(ann.y1, ann.y2);
            }

            if (editingExistingAnnot && AnnotationEquals(editingOriginal, ann)) {
                pendingAction = PendingCommitAction::RestoreOriginal;
                pendingAnnot = editingOriginal;
                pendingIndex = editingOriginalIndex;
                refreshAnnotPanelOnly = true;
            } else {
                pendingAnnot = std::move(ann);
                pendingIndex = static_cast<int>(g_annots.size());
                if (editingExistingAnnot) {
                    pendingIndex = std::clamp(editingOriginalIndex, 0, static_cast<int>(g_annots.size()));
                    pendingBeforeIndex = editingOriginalIndex;
                    pendingAction = PendingCommitAction::Update;
                } else {
                    pendingAction = PendingCommitAction::Add;
                }
            }
        } else if (editingExistingAnnot) {
            pendingAction = PendingCommitAction::Remove;
            pendingBeforeIndex = editingOriginalIndex;
        }
    } else if (editingExistingAnnot) {
        pendingAction = PendingCommitAction::RestoreOriginal;
        pendingAnnot = editingOriginal;
        pendingIndex = editingOriginalIndex;
        refreshAnnotPanelOnly = true;
    }

    g_pdf.editingText = false;
    EndPdfTextInputAccessibility(hwnd);
    g_pdf.editPage = -1;
    g_pdf.editText.clear();
    g_pdf.editCaret = 0;
    g_pdf.editSelStart = 0;
    g_pdf.editSelEnd = 0;
    g_pdf.imeComp.clear();
    g_pdf.imeComposing = false;
    g_pdf.suppressNextImeResultReturnChar = false;
    g_pdf.imeResultMessageTime = 0;
    g_pdf.editSelectingText = false;
    g_pdf.editSelAnchor = 0;
    g_pdf.editLayoutDirty = true;
    g_pdf.editInnerLayoutW = 0;
    g_pdf.editLayout.lines.clear();
    g_pdf.textBoxHoverValid = false;
    ClearTextEditHistory();
    g_pdf.movingAnnot = false;
    g_pdf.movingIndex = -1;
    g_pdf.movePage = -1;
    ResetEditOriginal();
    // Disable IME since text editing has ended.
    DisablePdfViewIme(hwnd);

    g_pdf.pendingTextBoxIndex = -1;
    if (g_hAnnotList) {
        SendMessageW(g_hAnnotList, LB_SETCURSEL, -1, 0);
    }

    if (editingExistingAnnot && g_preEditToolbarState.valid) {
        g_toolMode = g_preEditToolbarState.toolMode;
        g_textFontName = g_preEditToolbarState.textFontName;
        // Reediting never changes the creation size implicitly. Keep explicit
        // A/B choices made during editing instead of restoring a stale size.
        g_textColor = g_preEditToolbarState.textColor;
        g_activeColor = ToolColorForMode(g_toolMode);
        g_textBoxReadableBackground = g_preEditToolbarState.readableBackground;
        g_textBoxReadableBackgroundInverted = g_preEditToolbarState.readableBackgroundInverted;
        g_preEditToolbarState.valid = false;
        
        if (g_hPdfToolbar) {
            RedrawWindow(g_hPdfToolbar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        if (g_hMainWnd) {
            UpdateToolbarUI(g_hMainWnd);
        }
    }

    switch (pendingAction) {
    case PendingCommitAction::Add:
        InsertAnnotAt(pendingAnnot, pendingIndex);
        RecordAnnotAdd(pendingIndex, g_annots[static_cast<size_t>(pendingIndex)]);
        MarkAnnotsDirty(owner);
        break;
    case PendingCommitAction::Update:
        InsertAnnotAt(pendingAnnot, pendingIndex);
        RecordAnnotUpdate(pendingBeforeIndex, pendingIndex, editingOriginal,
                          g_annots[static_cast<size_t>(pendingIndex)]);
        MarkAnnotsDirty(owner);
        break;
    case PendingCommitAction::Remove:
        RecordAnnotRemove(pendingBeforeIndex, editingOriginal);
        MarkAnnotsDirty(owner);
        break;
    case PendingCommitAction::RestoreOriginal:
        InsertAnnotAt(pendingAnnot, pendingIndex);
        break;
    case PendingCommitAction::None:
        break;
    }

    if (refreshAnnotPanelOnly) {
        RefreshAnnotPanel();
    }

    // Ensure the text box and selection models are completely deselected after commit.
    ClearAnnotationSelection();

    InvalidateRect(hwnd, nullptr, FALSE);
}

static int MeasureIdeographicWidth(HDC hdc) {
    SIZE sz{};
    wchar_t sample = static_cast<wchar_t>(0x4E00);
    if (GetTextExtentPoint32W(hdc, &sample, 1, &sz) && sz.cx > 0) {
        return sz.cx;
    }
    TEXTMETRICW tm{};
    if (GetTextMetricsW(hdc, &tm)) {
        if (tm.tmMaxCharWidth > 0) return tm.tmMaxCharWidth;
        if (tm.tmAveCharWidth > 0) return tm.tmAveCharWidth;
    }
    return 0;
}

static void AdjustDxForIdeographicSpace(std::wstring_view text, std::vector<int>& dx, int ideographicWidth) {
    if (ideographicWidth <= 0 || dx.empty()) return;
    constexpr wchar_t kIdeographicSpace = static_cast<wchar_t>(0x3000);
    int extra = 0;
    int prevOrig = 0;
    for (size_t i = 0; i < dx.size(); ++i) {
        int orig = dx[i];
        int origWidth = orig - prevOrig;
        if (text[i] == kIdeographicSpace && origWidth < ideographicWidth) {
            extra += (ideographicWidth - origWidth);
        }
        dx[i] = orig + extra;
        prevOrig = orig;
    }
}

static void BuildAdvancesFromCumulative(const std::vector<int>& widths, std::vector<int>& advances) {
    advances.resize(widths.size());
    int prev = 0;
    for (size_t i = 0; i < widths.size(); ++i) {
        advances[i] = widths[i] - prev;
        prev = widths[i];
    }
}

static bool BuildAdjustedAdvances(HDC hdc, std::wstring_view text, int ideographicWidth, std::vector<int>& advances) {
    advances.clear();
    if (text.empty()) return false;
    std::vector<int> dx(text.size());
    SIZE sz{};
    if (!GetTextExtentExPointW(hdc, text.data(), static_cast<int>(text.size()),
                               std::numeric_limits<int>::max(), nullptr, dx.data(), &sz)) {
        return false;
    }
    AdjustDxForIdeographicSpace(text, dx, ideographicWidth);
    BuildAdvancesFromCumulative(dx, advances);
    return true;
}

static void DrawAdjustedLine(HDC hdc, int x, int y, std::wstring_view line, int ideographicWidth,
                             std::vector<int>& advances) {
    if (line.empty()) return;
    if (BuildAdjustedAdvances(hdc, line, ideographicWidth, advances)) {
        ExtTextOutW(hdc, x, y, 0, nullptr, line.data(), static_cast<UINT>(line.size()), advances.data());
    } else {
        TextOutW(hdc, x, y, line.data(), static_cast<int>(line.size()));
    }
}

static std::vector<TextLineLayout> LayoutTextLines(HDC hdc, const std::wstring& text, int maxWidth) {
    std::vector<TextLineLayout> lines;
    if (maxWidth <= 0) return lines;
    size_t pos = 0;
    if (text.empty()) {
        lines.push_back({ 0, 0, {} });
        return lines;
    }
    int ideographicWidth = MeasureIdeographicWidth(hdc);
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        size_t paraEnd = (nl == std::wstring::npos) ? text.size() : nl;
        size_t paraLen = paraEnd - pos;
        if (paraLen == 0) {
            lines.push_back({ pos, 0, {} });
        } else {
            std::vector<int> dx(paraLen);
            SIZE sz{};
            GetTextExtentExPointW(hdc, text.c_str() + pos, static_cast<int>(paraLen),
                                  std::numeric_limits<int>::max(), nullptr, dx.data(), &sz);
            AdjustDxForIdeographicSpace(std::wstring_view(text).substr(pos, paraLen), dx, ideographicWidth);
            size_t start = 0;
            while (start < paraLen) {
                size_t bestBreak = start;
                size_t lastSpace = start;
                for (size_t i = start; i < paraLen; ++i) {
                    int base = (start > 0) ? dx[start - 1] : 0;
                    int w = dx[i] - base;
                    if (w <= maxWidth + kTextWrapEpsilonPx) {
                        bestBreak = i + 1;
                        if (text[pos + i] == L' ' || text[pos + i] == L'\t') {
                            lastSpace = i + 1;
                        }
                    } else {
                        if (lastSpace > start) bestBreak = lastSpace;
                        else if (bestBreak == start) bestBreak = i + 1;
                        break;
                    }
                }
                if (bestBreak <= start) bestBreak = std::min(start + 1, paraLen);
                int base = (start > 0) ? dx[start - 1] : 0;
                std::vector<int> widths;
                widths.reserve(bestBreak - start);
                for (size_t i = start; i < bestBreak; ++i) {
                    widths.push_back(dx[i] - base);
                }
                lines.push_back({ pos + start, bestBreak - start, std::move(widths) });
                start = bestBreak;
                while (start < paraLen && (text[pos + start] == L' ' || text[pos + start] == L'\t')) ++start;
            }
        }
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
        if (pos == text.size()) {
            lines.push_back({ pos, 0, {} });
            break;
        }
    }
    if (lines.empty()) lines.push_back({ 0, 0, {} });
    return lines;
}

static std::wstring NormalizeTextBoxTextForCommit(std::wstring text) {
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return text;
}

static std::vector<std::wstring> SplitTextByNewlines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

static std::vector<std::wstring> LayoutResultToLines(const TextLayoutResult& layout, const std::wstring& text) {
    std::vector<std::wstring> lines;
    lines.reserve(layout.lines.size());
    for (const auto& ln : layout.lines) {
        if (ln.len == 0) {
            lines.emplace_back();
        } else {
            lines.emplace_back(text.substr(ln.start, ln.len));
        }
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

static void ReflowMovedTextBox(Annotation& ann) {
    if (ann.type != Annotation::Type::TextBox) return;
    if (ann.pageIndex < 0 || ann.pageIndex >= static_cast<int>(g_pdf.pages.size())) return;
    if (!g_hPdfView) {
        ann.textLines = SplitTextByNewlines(ann.text);
        return;
    }
    const bool canonicalInput = !ann.projectedCoordinates;
    if (canonicalInput) ann = ProjectAnnotationForView(ann);
    const auto origin = annotation_rotation::ContentOrigin(ann);
    const auto& page = g_pdf.pages[static_cast<size_t>(ann.pageIndex)];
    double availableWidth = page.widthPt - origin.x;
    double availableHeight = origin.y;
    switch (page_rotation::Normalize(ann.contentRotation)) {
    case 1: availableWidth = origin.y; availableHeight = origin.x; break;
    case 2: availableWidth = origin.x; availableHeight = page.heightPt - origin.y; break;
    case 3: availableWidth = page.heightPt - origin.y; availableHeight = page.widthPt - origin.x; break;
    }
    double maxWidthPt = std::max(10.0, availableWidth - 4.0);
    double maxHeightPt = std::max(8.0, availableHeight);
    int maxPxW = PtToLayoutPx(maxWidthPt);
    int padPx = LayoutPadPx();
    int innerPxW = std::max(1, maxPxW - padPx * 2);

    HDC hdc = GetDC(g_hPdfView);
    if (!hdc) {
        ann.textLines = SplitTextByNewlines(ann.text);
        if (canonicalInput) CanonicalizeAnnotation(ann);
        return;
    }
    LOGFONTW lf{};
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    std::wstring fontName = ann.fontName.empty() ? g_textFontName : ann.fontName;
    if (fontName.empty()) {
        fontName = GetDefaultFontFaceName();
    }
    lf.lfHeight = -TextBoxFontHeightPx(fontPt);
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    if (!fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, fontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = nullptr;
    if (hFont) oldFont = SelectObject(hdc, hFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    int lineHeight = std::max(1, static_cast<int>(tm.tmHeight));
    TextLayoutResult layout;
    layout.lines = LayoutTextLines(hdc, ann.text, innerPxW);
    if (layout.lines.empty()) layout.lines.push_back({ 0, 0, {} });
    int maxLineW = 0;
    for (const auto& ln : layout.lines) {
        if (!ln.widths.empty()) {
            maxLineW = std::max(maxLineW, ln.widths.back());
        }
    }
    if (maxLineW <= 0) {
        maxLineW = std::max(1, static_cast<int>(tm.tmAveCharWidth));
    }
    int lineCount = std::max(1, static_cast<int>(layout.lines.size()));
    // tmHeight already includes descent; retain only the symmetric inner padding.
    const int neededHpx = lineHeight * lineCount;
    double wCalcPt = LayoutPxToPt(maxLineW + kTextBoxGlyphOverhangPadPx + padPx * 2);
    double hCalcPt = LayoutPxToPt(neededHpx + padPx * 2);
    wCalcPt = std::min(wCalcPt, maxWidthPt);
    hCalcPt = std::min(hCalcPt, maxHeightPt);
    if (oldFont) SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
    ReleaseDC(g_hPdfView, hdc);

    annotation_rotation::SetContentBounds(ann, origin, wCalcPt, hCalcPt);
    if (canonicalInput) CanonicalizeAnnotation(ann);
    ann.textLines = LayoutResultToLines(layout, ann.text);
    if (ann.textLines.empty()) {
        ann.textLines = SplitTextByNewlines(ann.text);
    }
}

static void FinalizeTextBoxAnnotationLayout(Annotation& ann) {
    if (ann.type != Annotation::Type::TextBox) return;
    ann.textLines.clear();
    ReflowMovedTextBox(ann);
}

static std::vector<std::wstring> LayoutTextLinesWithFont(int innerWidth, const std::wstring& text, double fontPt, const std::wstring& fontName) {
    if (innerWidth <= 0 || !g_hPdfView) return {};
    HDC hdc = GetDC(g_hPdfView);
    if (!hdc) return {};
    LOGFONTW lf{};
    std::wstring resolvedName = fontName;
    if (resolvedName.empty()) {
        resolvedName = GetDefaultFontFaceName();
    }
    lf.lfHeight = -TextBoxFontHeightPx(fontPt);
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    if (!resolvedName.empty()) {
        wcsncpy_s(lf.lfFaceName, resolvedName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = nullptr;
    if (hFont) oldFont = SelectObject(hdc, hFont);
    TextLayoutResult layout;
    layout.lines = LayoutTextLines(hdc, text, innerWidth);
    std::vector<std::wstring> lines = LayoutResultToLines(layout, text);
    if (oldFont) SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
    ReleaseDC(g_hPdfView, hdc);
    return lines;
}

static int TextBoxInnerWidthPx(const Annotation& ann) {
    double widthPt = annotation_rotation::ContentExtent(ann).width;
    if (widthPt <= 0.0) return 0;
    int px = PtToLayoutPx(widthPt);
    int inner = px - LayoutPadPx() * 2;
    return std::max(1, inner);
}

static void EnsureAnnotationTextLines(Annotation& ann) {
    if (ann.type != Annotation::Type::TextBox) return;
    if (ann.pageIndex < 0 || ann.pageIndex >= static_cast<int>(g_pdf.pages.size())) return;
    int innerPx = TextBoxInnerWidthPx(ann);
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    std::wstring fontName = ann.fontName.empty() ? g_textFontName : ann.fontName;
    std::vector<std::wstring> lines;
    if (innerPx > 0) {
        lines = LayoutTextLinesWithFont(innerPx, ann.text, fontPt, fontName);
    }
    if (lines.empty()) lines = SplitTextByNewlines(ann.text);
    ann.textLines = std::move(lines);
}

struct TextBoxRasterKey {
    std::wstring renderText; // already wrapped by newlines for stable reflow
    std::wstring fontName;
    int fontPx = 0; // Same high-DPI integer font height used by layout.
    int wPt1000 = 0;
    int hPt1000 = 0;
    COLORREF color = 0;
    int rotation = 0;
    bool operator==(const TextBoxRasterKey& o) const {
        return fontPx == o.fontPx &&
               wPt1000 == o.wPt1000 &&
               hPt1000 == o.hPt1000 &&
               color == o.color && rotation == o.rotation &&
               fontName == o.fontName &&
               renderText == o.renderText;
    }
};

struct TextBoxRasterKeyHash {
    static size_t HashCombine(size_t a, size_t b) {
        // boost-like hash combine
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }
    size_t operator()(const TextBoxRasterKey& k) const {
        size_t h = 0;
        h = HashCombine(h, std::hash<std::wstring>{}(k.renderText));
        h = HashCombine(h, std::hash<std::wstring>{}(k.fontName));
        h = HashCombine(h, std::hash<int>{}(k.fontPx));
        h = HashCombine(h, std::hash<int>{}(k.wPt1000));
        h = HashCombine(h, std::hash<int>{}(k.hPt1000));
        h = HashCombine(h, std::hash<int>{}(k.rotation));
        h = HashCombine(h, std::hash<unsigned long>{}(static_cast<unsigned long>(k.color)));
        return h;
    }
};

struct TextBoxRasterEntry {
    HBITMAP hbmp = nullptr; // 32bpp premultiplied BGRA
    int w = 0;
    int h = 0;
    uint64_t lastUsed = 0;
};

static std::unordered_map<TextBoxRasterKey, TextBoxRasterEntry, TextBoxRasterKeyHash> g_textBoxRasterCache;
static uint64_t g_textBoxRasterTick = 0;
static constexpr size_t kTextBoxRasterCacheLimit = 256;

static void FreeTextBoxRasterEntry(TextBoxRasterEntry& e) {
    if (e.hbmp) {
        DeleteObject(e.hbmp);
        e.hbmp = nullptr;
    }
    e.w = 0;
    e.h = 0;
    e.lastUsed = 0;
}

static std::wstring BuildTextBoxRenderText(const Annotation& ann) {
    if (!ann.textLines.empty()) {
        std::wstring out;
        size_t total = 0;
        for (const auto& ln : ann.textLines) total += ln.size() + 1;
        out.reserve(total);
        for (size_t i = 0; i < ann.textLines.size(); ++i) {
            if (i) out.push_back(L'\n');
            out.append(ann.textLines[i]);
        }
        return out;
    }
    // Fallback: wrap based on current box width at base DPI so zoom doesn't affect line breaks.
    int innerPx = TextBoxInnerWidthPx(ann);
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    std::wstring fontName = ann.fontName.empty() ? g_textFontName : ann.fontName;
    auto lines = LayoutTextLinesWithFont(innerPx, ann.text, fontPt, fontName);
    if (lines.empty()) lines = SplitTextByNewlines(ann.text);
    std::wstring out;
    size_t total = 0;
    for (const auto& ln : lines) total += ln.size() + 1;
    out.reserve(total);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out.push_back(L'\n');
        out.append(lines[i]);
    }
    return out;
}

static TextBoxRasterKey BuildTextBoxRasterKey(const Annotation& ann) {
    TextBoxRasterKey k;
    k.renderText = BuildTextBoxRenderText(ann);
    k.fontName = ann.fontName.empty() ? g_textFontName : ann.fontName;
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    k.fontPx = TextBoxFontHeightPx(fontPt);
    const auto extent = annotation_rotation::ContentExtent(ann);
    double wPt = extent.width;
    double hPt = extent.height;
    k.rotation = ann.contentRotation;
    k.wPt1000 = static_cast<int>(std::lround(wPt * 1000.0));
    k.hPt1000 = static_cast<int>(std::lround(hPt * 1000.0));
    k.color = ann.color;
    return k;
}

static void TrimTextBoxRasterCacheIfNeeded() {
    if (g_textBoxRasterCache.size() <= kTextBoxRasterCacheLimit) return;
    // Simple O(n) eviction (cache is small).
    while (g_textBoxRasterCache.size() > kTextBoxRasterCacheLimit) {
        auto victim = g_textBoxRasterCache.end();
        for (auto it = g_textBoxRasterCache.begin(); it != g_textBoxRasterCache.end(); ++it) {
            if (victim == g_textBoxRasterCache.end() || it->second.lastUsed < victim->second.lastUsed) {
                victim = it;
            }
        }
        if (victim == g_textBoxRasterCache.end()) break;
        FreeTextBoxRasterEntry(victim->second);
        g_textBoxRasterCache.erase(victim);
    }
}

static void ClearTextBoxRasterCache() {
    for (auto& kv : g_textBoxRasterCache) {
        FreeTextBoxRasterEntry(kv.second);
    }
    g_textBoxRasterCache.clear();
    g_textBoxRasterTick = 0;
}

static bool BuildTextBoxRaster(HDC refHdc, const TextBoxRasterKey& key, TextBoxRasterEntry& out) {
    FreeTextBoxRasterEntry(out);
    if (key.wPt1000 <= 0 || key.hPt1000 <= 0) return false;
    const double wPt = key.wPt1000 / 1000.0;
    const double hPt = key.hPt1000 / 1000.0;
    int wPx = std::max(1, static_cast<int>(std::lround(wPt * kTextBoxStableRasterDpi / 72.0)));
    int hPx = std::max(1, static_cast<int>(std::lround(hPt * kTextBoxStableRasterDpi / 72.0)));
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = wPx;
    bmi.bmiHeader.biHeight = -hPx; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(refHdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        return false;
    }
    std::fill_n(static_cast<DWORD*>(bits), static_cast<size_t>(wPx) * hPx, 0);

    HDC mem = CreateCompatibleDC(refHdc);
    if (!mem) {
        DeleteObject(hbmp);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(mem, hbmp);

    LOGFONTW lf{};
    lf.lfHeight = -key.fontPx;
    lf.lfQuality = ANTIALIASED_QUALITY; // grayscale (avoid ClearType color fringes)
    lf.lfCharSet = DEFAULT_CHARSET;
    if (!key.fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, key.fontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = nullptr;
    if (hFont) oldFont = SelectObject(mem, hFont);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255)); // build coverage mask in RGB

    TEXTMETRICW tm{};
    GetTextMetricsW(mem, &tm);
    const int lineHeight = std::max(1, static_cast<int>(tm.tmHeight));

    const int padPx = std::max(1, static_cast<int>(std::lround(kTextPadPx * kTextBoxStableRasterDpi / kDpi)));
    const int x0 = padPx;
    int y = padPx;
    const int bottomLimit = hPx - padPx;

    int ideographicWidth = MeasureIdeographicWidth(mem);
    std::vector<int> advances;

    // Render pre-wrapped lines (each '\n' is a new line).
    size_t start = 0;
    while (start <= key.renderText.size()) {
        size_t nl = key.renderText.find(L'\n', start);
        size_t end = (nl == std::wstring::npos) ? key.renderText.size() : nl;
        std::wstring_view line(key.renderText.data() + start, end - start);
        if (y >= bottomLimit) break;
        if (!line.empty()) {
            DrawAdjustedLine(mem, x0, y, line, ideographicWidth, advances);
        }
        y += lineHeight;
        if (nl == std::wstring::npos) break;
        start = nl + 1;
        if (start == key.renderText.size()) {
            // Trailing newline -> draw a final empty line.
            if (y < bottomLimit) y += lineHeight;
            break;
        }
    }

    if (oldFont) SelectObject(mem, oldFont);
    if (hFont) DeleteObject(hFont);
    SelectObject(mem, oldBmp);
    DeleteDC(mem);

    // Convert the grayscale mask (white-on-black) into premultiplied color with alpha.
    DWORD* px = static_cast<DWORD*>(bits);
    BYTE rC = GetRValue(key.color);
    BYTE gC = GetGValue(key.color);
    BYTE bC = GetBValue(key.color);
    for (int i = 0; i < wPx * hPx; ++i) {
        DWORD v = px[i];
        BYTE b = static_cast<BYTE>(v & 0xFF);
        BYTE g = static_cast<BYTE>((v >> 8) & 0xFF);
        BYTE r = static_cast<BYTE>((v >> 16) & 0xFF);
        BYTE a = std::max(r, std::max(g, b));
        if (a == 0) {
            px[i] = 0;
            continue;
        }
        BYTE rT = static_cast<BYTE>((static_cast<unsigned int>(rC) * a + 127) / 255);
        BYTE gT = static_cast<BYTE>((static_cast<unsigned int>(gC) * a + 127) / 255);
        BYTE bT = static_cast<BYTE>((static_cast<unsigned int>(bC) * a + 127) / 255);
        px[i] = (static_cast<DWORD>(a) << 24) |
                (static_cast<DWORD>(rT) << 16) |
                (static_cast<DWORD>(gT) << 8) |
                (static_cast<DWORD>(bT));
    }

    if (key.rotation != 0) {
        HBITMAP rotated = ui::RotateTopDownDibQuarterTurns(refHdc, hbmp, key.rotation, &wPx, &hPx);
        if (!rotated) { DeleteObject(hbmp); return false; }
        DeleteObject(hbmp);
        hbmp = rotated;
    }
    out.hbmp = hbmp;
    out.w = wPx;
    out.h = hPx;
    return true;
}

static const TextBoxRasterEntry* GetTextBoxRaster(HDC refHdc, const Annotation& ann) {
    TextBoxRasterKey key = BuildTextBoxRasterKey(ann);
    auto it = g_textBoxRasterCache.find(key);
    if (it == g_textBoxRasterCache.end()) {
        TextBoxRasterEntry e;
        if (!BuildTextBoxRaster(refHdc, key, e)) {
            FreeTextBoxRasterEntry(e);
            return nullptr;
        }
        e.lastUsed = ++g_textBoxRasterTick;
        auto [insIt, _] = g_textBoxRasterCache.emplace(std::move(key), e);
        TrimTextBoxRasterCacheIfNeeded();
        return &insIt->second;
    }
    it->second.lastUsed = ++g_textBoxRasterTick;
    return &it->second;
}

static BYTE TextBoxLowZoomBoostAlpha(const Annotation& ann) {
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    double displayFontPx = fontPt * kDpi / 72.0 * g_pdf.scale;
    if (displayFontPx < 5.5) return 190;
    if (displayFontPx < 7.0) return 145;
    if (displayFontPx < 8.5) return 105;
    if (displayFontPx < 11.5) return 75;
    if (displayFontPx < 14.0) return 50;
    if (displayFontPx < 18.0) return 30;
    return 0;
}

static int PerceivedBrightness(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

static bool TextBoxReadableBackground(const Annotation& ann, COLORREF* outColor, BYTE* outAlpha) {
    bool effectiveEnabled = false;
    bool effectiveInverted = false;
    switch (ann.backgroundAssistMode) {
    case TextBackgroundAssistMode::Off:
        return false;
    case TextBackgroundAssistMode::Auto:
        effectiveEnabled = true;
        break;
    case TextBackgroundAssistMode::Inverted:
        effectiveEnabled = true;
        effectiveInverted = true;
        break;
    case TextBackgroundAssistMode::Inherit:
        effectiveEnabled = g_preEditToolbarState.valid
            ? g_preEditToolbarState.readableBackground
            : g_textBoxReadableBackground;
        effectiveInverted = g_preEditToolbarState.valid
            ? g_preEditToolbarState.readableBackgroundInverted
            : g_textBoxReadableBackgroundInverted;
        break;
    }
    if (!effectiveEnabled) return false;
    bool useLightBackground = PerceivedBrightness(ann.color) < 128;
    if (effectiveInverted) useLightBackground = !useLightBackground;
    if (outColor) *outColor = useLightBackground ? RGB(255, 255, 255) : RGB(0, 0, 0);
    if (outAlpha) *outAlpha = useLightBackground ? 178 : 150;
    return true;
}

static void DrawTextBoxReadableBackground(HDC hdc, const RECT& r, const Annotation& ann) {
    COLORREF bg = RGB(255, 255, 255);
    BYTE alpha = 0;
    if (!TextBoxReadableBackground(ann, &bg, &alpha)) return;
    FillRectAlpha(hdc, r, bg, alpha);
}

static void DrawTextBoxStable(HDC hdc, HDC scratchDc, const RECT& r, const Annotation& ann) {
    if (!hdc || !scratchDc) return;
    DrawTextBoxReadableBackground(hdc, r, ann);

    const TextBoxRasterEntry* ras = GetTextBoxRaster(hdc, ann);
    if (!ras || !ras->hbmp || ras->w <= 0 || ras->h <= 0) return;

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, r.left, r.top, r.right, r.bottom);

    int oldMode = SetStretchBltMode(hdc, HALFTONE);
    POINT oldOrg{};
    SetBrushOrgEx(hdc, 0, 0, &oldOrg);

    HGDIOBJ oldBmp = SelectObject(scratchDc, ras->hbmp);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(hdc, r.left, r.top, w, h, scratchDc, 0, 0, ras->w, ras->h, bf);
    BYTE boostAlpha = TextBoxLowZoomBoostAlpha(ann);
    if (boostAlpha > 0) {
        BLENDFUNCTION boost{ AC_SRC_OVER, 0, boostAlpha, AC_SRC_ALPHA };
        AlphaBlend(hdc, r.left, r.top, w, h, scratchDc, 0, 0, ras->w, ras->h, boost);
    }
    SelectObject(scratchDc, oldBmp);

    SetStretchBltMode(hdc, oldMode);
    SetBrushOrgEx(hdc, oldOrg.x, oldOrg.y, nullptr);
    if (saved != 0) RestoreDC(hdc, saved);
}

static bool BuildTextLayout(int innerWidth, const std::wstring& text, TextLayoutResult* out) {
    if (!out || innerWidth <= 0 || !g_pdf.editingText || g_pdf.editPage < 0 || !g_hPdfView) {
        return false;
    }
    HDC hdc = GetDC(g_hPdfView);
    if (!hdc) return false;
    LOGFONTW lf{};
    lf.lfHeight = -TextBoxFontHeightPx(g_pdf.editFontPt);
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    std::wstring fontName = g_pdf.editFontName.empty() ? g_textFontName : g_pdf.editFontName;
    if (fontName.empty()) {
        fontName = GetDefaultFontFaceName();
    }
    if (!fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, fontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = nullptr;
    if (hFont) oldFont = SelectObject(hdc, hFont);
    GetTextMetricsW(hdc, &out->tm);
    out->lines = LayoutTextLines(hdc, text, innerWidth);
    if (oldFont) SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
    ReleaseDC(g_hPdfView, hdc);
    return !out->lines.empty();
}

static bool GetEditingInnerRectClient(RECT* outInner, int* outWidth) {
    if (!outInner || !g_pdf.editingText || g_pdf.editPage < 0 ||
        g_pdf.editPage >= static_cast<int>(g_pdf.pages.size())) {
        return false;
    }
    RECT outer{};
    if (!EditingBoxClientRect(outer)) {
        return false;
    }
    double scale = LayoutToClientScale();
    int padLayoutPx = LayoutPadPx();
    int padClient = std::max(1, static_cast<int>(std::lround(padLayoutPx * scale)));
    outInner->left = outer.left + padClient;
    outInner->right = outer.right - padClient;
    outInner->top = outer.top + padClient;
    outInner->bottom = outer.bottom - padClient;
    if (outInner->right <= outInner->left || outInner->bottom <= outInner->top) return false;
    if (outWidth) *outWidth = outInner->right - outInner->left;
    return true;
}

static bool GetEditingInnerRectLayout(RECT* outInner, int* outWidth) {
    if (!outInner || !g_pdf.editingText || g_pdf.editPage < 0 ||
        g_pdf.editPage >= static_cast<int>(g_pdf.pages.size())) {
        return false;
    }
    int outerW = PtToLayoutPx(std::abs(g_pdf.editX2 - g_pdf.editX1));
    int outerH = PtToLayoutPx(std::abs(g_pdf.editY2 - g_pdf.editY1));
    if (outerW <= 0 || outerH <= 0) return false;
    int padPx = LayoutPadPx();
    outInner->left = padPx;
    outInner->right = std::max(padPx + 1, outerW - padPx);
    outInner->top = padPx;
    outInner->bottom = std::max(padPx + 1, outerH - padPx);
    if (outInner->right <= outInner->left || outInner->bottom <= outInner->top) return false;
    if (outWidth) *outWidth = outInner->right - outInner->left;
    return true;
}

static std::wstring BuildEditDisplayText(bool includeComp) {
    std::wstring text = g_pdf.editText;
    size_t caret = std::min(g_pdf.editCaret, text.size());
    if (includeComp && !g_pdf.imeComp.empty()) {
        text.insert(caret, g_pdf.imeComp);
    }
    return text;
}

static size_t EditIndexToDisplayIndex(size_t index) {
    size_t caret = std::min(g_pdf.editCaret, g_pdf.editText.size());
    size_t compLen = g_pdf.imeComp.size();
    if (compLen == 0) return index;
    if (index <= caret) return index;
    return index + compLen;
}

static size_t DisplayIndexToEditIndex(size_t index) {
    size_t caret = std::min(g_pdf.editCaret, g_pdf.editText.size());
    size_t compLen = g_pdf.imeComp.size();
    if (compLen == 0) return index;
    if (index <= caret) return index;
    if (index <= caret + compLen) return caret;
    return index - compLen;
}

static size_t EditCaretDisplayIndex() {
    size_t caret = std::min(g_pdf.editCaret, g_pdf.editText.size());
    return caret + g_pdf.imeComp.size();
}

static bool EnsureEditLayoutCache(bool includeComp) {
    if (!g_pdf.editingText || g_pdf.editPage < 0) return false;
    RECT inner{};
    int innerWidth = 0;
    if (!GetEditingInnerRectLayout(&inner, &innerWidth) || innerWidth <= 0) return false;
    if (!g_pdf.editLayoutDirty && g_pdf.editInnerLayoutW == innerWidth) return true;
    std::wstring text = BuildEditDisplayText(includeComp);
    TextLayoutResult layout;
    if (!BuildTextLayout(innerWidth, text, &layout)) return false;
    if (layout.lines.empty()) layout.lines.push_back({ 0, 0, {} });
    g_pdf.editLayout = std::move(layout);
    g_pdf.editInnerLayoutW = innerWidth;
    g_pdf.editLayoutDirty = false;
    return true;
}

static bool HitTestEditCaretFromClientPoint(const POINT& pt, size_t* outEditIndex, bool clampToOuter) {
    if (!outEditIndex || !g_pdf.editingText || g_pdf.editPage < 0) return false;
    RECT outer{};
    if (!EditingBoxClientRect(outer, true)) {
        return false;
    }
    RECT actual{};
    if (!EditingBoxClientRect(actual)) return false;
    if (!PtInRect(&actual, pt) && !clampToOuter) return false;
    POINT clamped = TextBoxCaretHitPoint(pt);
    clamped.x = std::clamp(clamped.x, actual.left, std::max(actual.left, actual.right - 1));
    clamped.y = std::clamp(clamped.y, actual.top, std::max(actual.top, actual.bottom - 1));
    clamped = EditingLogicalToClient(clamped, true);
    if (!EnsureEditLayoutCache(true)) return false;
    RECT inner{};
    int innerWidth = 0;
    if (!GetEditingInnerRectLayout(&inner, &innerWidth) || innerWidth <= 0) return false;
    double scale = LayoutToClientScale();
    if (scale <= 0.0) scale = 1.0;
    POINT layoutPt{};
    layoutPt.x = static_cast<LONG>(std::lround((clamped.x - outer.left) / scale));
    layoutPt.y = static_cast<LONG>(std::lround((clamped.y - outer.top) / scale));
    size_t displayIndex = HitTestCaretIndex(g_pdf.editLayout, inner, layoutPt);
    size_t editIndex = DisplayIndexToEditIndex(displayIndex);
    if (editIndex > g_pdf.editText.size()) editIndex = g_pdf.editText.size();
    *outEditIndex = editIndex;
    return true;
}

static size_t FindLineForCaret(const std::vector<TextLineLayout>& lines, size_t caret) {
    if (lines.empty()) return 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& ln = lines[i];
        size_t start = ln.start;
        size_t end = start + ln.len;
        if (caret <= end || i + 1 == lines.size()) {
            return i;
        }
    }
    return lines.size() - 1;
}

static int ColumnWidthForLine(const TextLineLayout& line, size_t column) {
    if (line.len == 0 || line.widths.empty()) return 0;
    if (column == 0) return 0;
    if (column > line.len) column = line.len;
    size_t idx = column - 1;
    if (idx >= line.widths.size()) idx = line.widths.size() - 1;
    return line.widths[idx];
}

static size_t HitTestLineForX(const TextLineLayout& line, int x) {
    if (line.len == 0) return line.start;
    int prevX = 0;
    for (size_t i = 0; i < line.widths.size(); ++i) {
        int currX = line.widths[i];
        int midX = (prevX + currX) / 2;
        if (x <= midX) {
            return line.start + i;
        }
        prevX = currX;
    }
    return line.start + line.widths.size();
}

static size_t HitTestCaretIndex(const TextLayoutResult& layout, const RECT& inner, const POINT& pt) {
    if (layout.lines.empty()) return 0;
    int relX = pt.x - inner.left;
    int relY = pt.y - inner.top;
    int lineHeight = std::max(1, static_cast<int>(layout.tm.tmHeight));
    int lineIndex = relY / lineHeight;
    if (lineIndex < 0) lineIndex = 0;
    if (lineIndex >= static_cast<int>(layout.lines.size())) {
        lineIndex = static_cast<int>(layout.lines.size()) - 1;
    }
    relX = std::max(0, relX);
    return HitTestLineForX(layout.lines[static_cast<size_t>(lineIndex)], relX);
}

static bool MoveCaretVertical(int direction) {
    if (!g_pdf.editingText) return false;
    if (!EnsureEditLayoutCache(true)) return false;
    const auto& layout = g_pdf.editLayout;
    if (layout.lines.empty()) return false;
    size_t displayCaret = EditCaretDisplayIndex();
    size_t lineIndex = FindLineForCaret(layout.lines, displayCaret);
    const auto& line = layout.lines[lineIndex];
    size_t column = (displayCaret >= line.start) ? (displayCaret - line.start) : 0;
    int targetX = ColumnWidthForLine(line, column);
    int newLineIndex = static_cast<int>(lineIndex) + direction;
    if (newLineIndex < 0 || newLineIndex >= static_cast<int>(layout.lines.size())) {
        return false;
    }
    size_t newDisplay = HitTestLineForX(layout.lines[static_cast<size_t>(newLineIndex)], targetX);
    size_t newCaret = DisplayIndexToEditIndex(newDisplay);
    g_pdf.editCaret = std::min(newCaret, g_pdf.editText.size());
    return true;
}

static bool MoveCaretToLineBoundary(bool toEnd) {
    if (!g_pdf.editingText) return false;
    if (!EnsureEditLayoutCache(true)) return false;
    const auto& layout = g_pdf.editLayout;
    if (layout.lines.empty()) return false;
    size_t displayCaret = EditCaretDisplayIndex();
    size_t lineIndex = FindLineForCaret(layout.lines, displayCaret);
    const auto& line = layout.lines[lineIndex];
    size_t offset = toEnd ? line.len : 0;
    size_t newDisplay = line.start + offset;
    size_t newCaret = DisplayIndexToEditIndex(newDisplay);
    g_pdf.editCaret = std::min(newCaret, g_pdf.editText.size());
    return true;
}

static bool IsCaretAtLineBoundary(bool toEnd) {
    if (!g_pdf.editingText) return false;
    if (!EnsureEditLayoutCache(true)) return false;
    const auto& layout = g_pdf.editLayout;
    if (layout.lines.empty()) return false;
    size_t displayCaret = EditCaretDisplayIndex();
    size_t lineIndex = FindLineForCaret(layout.lines, displayCaret);
    const auto& line = layout.lines[lineIndex];
    size_t boundary = line.start + (toEnd ? line.len : 0);
    size_t caret = std::min(displayCaret, BuildEditDisplayText(true).size());
    return caret == boundary;
}

static void ResetTextBoxRapidClickState() {
    g_pdf.textBoxRapidClickTick = 0;
    g_pdf.textBoxRapidClickCount = 0;
    g_pdf.textBoxRapidClickPage = -1;
    g_pdf.textBoxRapidClickX1 = 0.0;
    g_pdf.textBoxRapidClickY1 = 0.0;
    g_pdf.textBoxRapidClickX2 = 0.0;
    g_pdf.textBoxRapidClickY2 = 0.0;
}

static bool RegisterTextBoxRapidClick(int pageIndex, double x1, double y1, double x2, double y2) {
    if (pageIndex < 0) {
        ResetTextBoxRapidClickState();
        return false;
    }

    const double left = std::min(x1, x2);
    const double right = std::max(x1, x2);
    const double bottom = std::min(y1, y2);
    const double top = std::max(y1, y2);
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG limitMs = static_cast<ULONGLONG>(std::max<UINT>(1, GetDoubleClickTime()));
    constexpr double kRectTolerancePt = 8.0;

    const bool sameTarget =
        g_pdf.textBoxRapidClickCount > 0 &&
        g_pdf.textBoxRapidClickPage == pageIndex &&
        std::abs(g_pdf.textBoxRapidClickX1 - left) <= kRectTolerancePt &&
        std::abs(g_pdf.textBoxRapidClickY1 - bottom) <= kRectTolerancePt &&
        std::abs(g_pdf.textBoxRapidClickX2 - right) <= kRectTolerancePt &&
        std::abs(g_pdf.textBoxRapidClickY2 - top) <= kRectTolerancePt;
    const bool withinWindow =
        g_pdf.textBoxRapidClickTick != 0 &&
        (now - g_pdf.textBoxRapidClickTick) <= limitMs;

    if (!sameTarget || !withinWindow) {
        g_pdf.textBoxRapidClickCount = 0;
    }

    g_pdf.textBoxRapidClickTick = now;
    g_pdf.textBoxRapidClickPage = pageIndex;
    g_pdf.textBoxRapidClickX1 = left;
    g_pdf.textBoxRapidClickY1 = bottom;
    g_pdf.textBoxRapidClickX2 = right;
    g_pdf.textBoxRapidClickY2 = top;
    ++g_pdf.textBoxRapidClickCount;

    if (g_pdf.textBoxRapidClickCount >= 3) {
        ResetTextBoxRapidClickState();
        return true;
    }
    return false;
}

// STA adapter: the accessibility object never owns PDF/document state. Reads
// and replacements must still belong to the same live inline edit generation.
static HRESULT ReadPdfTextInput(HWND hwnd, std::uint64_t generation,
                               text_input_accessibility::Snapshot* out) noexcept {
    if (!out) return E_POINTER;
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (hwnd != g_hPdfView || !g_pdfTextInputPublished || !g_pdf.editingText ||
        generation != g_pdfTextInputGeneration) return CO_E_OBJNOTCONNECTED;
    try {
        out->name = localization::Text(L"pdf.accessibility.textbox");
        out->value = g_pdf.editText;
        out->focused = GetFocus() == hwnd;
        out->enabled = IsWindowEnabled(hwnd) && IsWindowEnabled(GetAncestor(hwnd, GA_ROOT));
        out->readOnly = IsPdfPreviewReadOnlyActive() || IsSaveTransactionRunning();
        RECT bounds{}, client{}, visible{};
        if (EditingBoxClientRect(bounds) && GetClientRect(hwnd, &client)) {
            out->offscreen = !IsWindowVisible(hwnd) || !IntersectRect(&visible, &bounds, &client);
            POINT origin{};
            if (!ClientToScreen(hwnd, &origin)) return E_FAIL;
            OffsetRect(&bounds, origin.x, origin.y);
            out->screenBounds = bounds;
        }
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

static void NotifyPdfTextInputAccessibility(HWND hwnd) noexcept {
    if (!g_pdfTextInputPublished) return;
    text_input_accessibility::Snapshot state;
    if (FAILED(ReadPdfTextInput(hwnd, g_pdfTextInputGeneration, &state))) return;
    const bool nameChanged = state.name != g_pdfTextInputLast.name;
    const bool valueChanged = state.value != g_pdfTextInputLast.value;
    const bool positionChanged = !EqualRect(&state.screenBounds, &g_pdfTextInputLast.screenBounds);
    const bool stateChanged = state.readOnly != g_pdfTextInputLast.readOnly ||
        state.enabled != g_pdfTextInputLast.enabled || state.offscreen != g_pdfTextInputLast.offscreen ||
        state.focused != g_pdfTextInputLast.focused;
    const bool focused = state.focused && !g_pdfTextInputLast.focused;
    g_pdfTextInputLast = std::move(state);
    if (nameChanged) NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
    if (valueChanged) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
    if (positionChanged) NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
    if (stateChanged) NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
    if (focused) NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, CHILDID_SELF);
}

static void BeginPdfTextInputAccessibility(HWND hwnd) noexcept {
    ++g_pdfTextInputGeneration;
    g_pdfTextInputPublished = true;
    g_pdfTextInputLast = {};
    NotifyPdfTextInputAccessibility(hwnd);
}

static void EndPdfTextInputAccessibility(HWND hwnd) noexcept {
    if (!g_pdfTextInputPublished) return;
    g_pdfTextInputPublished = false;
    ++g_pdfTextInputGeneration;
    g_pdfTextInputLast = {};
    if (IsWindow(hwnd)) {
        NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
    }
}

static HRESULT ModifyPdfTextInput(HWND hwnd, std::uint64_t generation,
    text_input_accessibility::Action action, const std::wstring* value) noexcept {
    text_input_accessibility::Snapshot state;
    const HRESULT status = ReadPdfTextInput(hwnd, generation, &state);
    if (FAILED(status)) return status;
    if (!state.enabled) return E_ACCESSDENIED;
    if (action == text_input_accessibility::Action::Focus) {
        SetFocus(hwnd);
        NotifyPdfTextInputAccessibility(hwnd);
        return GetFocus() == hwnd ? S_OK : E_FAIL;
    }
    if (state.readOnly) return E_ACCESSDENIED;
    if (g_pdf.imeComposing || !g_pdf.imeComp.empty()) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (!value || value->find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    try {
        std::wstring replacement;
        replacement.reserve(value->size());
        for (size_t i = 0; i < value->size(); ++i) {
            if ((*value)[i] != L'\r') replacement.push_back((*value)[i]);
            else {
                replacement.push_back(L'\n');
                if (i + 1 < value->size() && (*value)[i + 1] == L'\n') ++i;
            }
        }
        if (replacement == g_pdf.editText) return S_OK;
        // Prepare history and every fallible value allocation before changing
        // the live text. Failure here preserves the editor's existing value.
        if (!FinalizePendingTextEditChange()) return E_OUTOFMEMORY;
        TextEditSnapshot before;
        if (!TryCaptureTextEditSnapshot(&before)) return E_OUTOFMEMORY;
        g_pendingTextEditBefore = std::move(before);
        g_pdf.editText.swap(replacement);
        g_pdf.editCaret = g_pdf.editText.size();
        g_pdf.editSelStart = g_pdf.editSelEnd = g_pdf.editSelAnchor = g_pdf.editCaret;
        g_pdf.editSelectingText = false;
        g_pdf.editLayoutDirty = true;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
    // The value is now accepted and undoable. A layout allocation failure must
    // not claim that the value was rejected or discard the accepted edit.
    try {
        RecalcEditingTextboxSize(true);
        EnsureEditingTextBoxVisible(hwnd, L"accessible_text_input");
        UpdateImeWindowPosition(hwnd, true);
    } catch (...) { g_pdf.editLayoutDirty = true; }
    NotifyPdfTextInputAccessibility(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    return S_OK;
}

static bool CopyActiveTextBoxToClipboard(HWND hwnd) {
    if (!g_pdf.editingText) return false;
    std::wstring text = g_pdf.editText;
    if (!g_pdf.imeComp.empty()) {
        text = BuildEditDisplayText(true);
    }
    return CopyTextToClipboard(hwnd, text);
}

static void FinishTextBoxTripleClickCopy(HWND hwnd, bool commitActiveEdit) {
    if (commitActiveEdit && g_pdf.editingText) {
        CommitTextEditing(hwnd, true);
    }
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    g_pdf.pendingTextBoxHit = false;
    g_pdf.pendingTextBoxIndex = -1;
    g_pdf.editSelectingText = false;
    g_pdf.editSelectMoved = false;
    EnsurePdfViewFocus(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void ClearEditSelection() {
    g_pdf.editSelStart = g_pdf.editCaret;
    g_pdf.editSelEnd = g_pdf.editCaret;
}

static bool HasEditSelection() {
    return g_pdf.editSelStart != g_pdf.editSelEnd;
}

static void SelectAllEditText() {
    g_pdf.editSelStart = 0;
    g_pdf.editSelEnd = g_pdf.editText.size();
    g_pdf.editCaret = g_pdf.editSelEnd;
}

static bool DeleteEditSelection() {
    if (!HasEditSelection()) return false;
    size_t start = g_pdf.editSelStart;
    size_t end = g_pdf.editSelEnd;
    if (start > end) std::swap(start, end);
    if (end > g_pdf.editText.size()) end = g_pdf.editText.size();
    if (start > g_pdf.editText.size()) start = g_pdf.editText.size();
    if (end > start) {
        g_pdf.editText.erase(start, end - start);
        g_pdf.editCaret = start;
    }
    ClearEditSelection();
    return true;
}

static bool CopyEditSelectionToClipboard(HWND hwnd) {
    if (!HasEditSelection()) return false;
    size_t start = g_pdf.editSelStart;
    size_t end = g_pdf.editSelEnd;
    if (start > end) std::swap(start, end);
    if (end > g_pdf.editText.size()) end = g_pdf.editText.size();
    if (start >= end) return false;
    std::wstring text = g_pdf.editText.substr(start, end - start);
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) return false;
    void* dst = GlobalLock(hmem);
    if (!dst) {
        GlobalFree(hmem);
        return false;
    }
    std::memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(hmem);
    if (!OpenClipboard(hwnd)) {
        GlobalFree(hmem);
        return false;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(hmem);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    ShowPdfCopySuccessNotice(hwnd);
    return true;
}

bool PasteClipboardIntoActivePdfTextBox(HWND hwnd) {
    if (!g_pdf.editingText) return false;
    if (!OpenClipboard(hwnd)) return false;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return false;
    }
    const wchar_t* src = static_cast<const wchar_t*>(GlobalLock(data));
    if (!src) {
        CloseClipboard();
        return false;
    }
    std::wstring raw(src);
    GlobalUnlock(data);
    CloseClipboard();
    if (raw.empty()) return false;
    std::wstring text;
    text.reserve(raw.size());
    for (wchar_t ch : raw) {
        if (ch != L'\r') text.push_back(ch);
    }
    if (text.empty()) return false;
    RecordTextEditChange();
    DeleteEditSelection();
    g_pdf.editText.insert(g_pdf.editCaret, text);
    g_pdf.editCaret += text.size();
    ClearEditSelection();
    return true;
}

static bool InlineCaretClientPoint(POINT& out, int* outLineHeight) {
    if (!g_pdf.editingText || g_pdf.editPage < 0) return false;
    if (!EnsureEditLayoutCache(true)) return false;
    RECT outer{};
    if (!EditingBoxClientRect(outer, true)) {
        return false;
    }
    const auto& layout = g_pdf.editLayout;
    if (layout.lines.empty()) return false;
    size_t caretLogical = EditCaretDisplayIndex();
    size_t lineIndex = FindLineForCaret(layout.lines, caretLogical);
    const auto& line = layout.lines[lineIndex];
    size_t lineStart = line.start;
    size_t lineEnd = lineStart + line.len;
    size_t inLine = (caretLogical > lineStart) ? (std::min(caretLogical, lineEnd) - lineStart) : 0;
    int caretX = ColumnWidthForLine(line, inLine);
    int lineHeight = std::max(1, static_cast<int>(layout.tm.tmHeight));
    int padPx = LayoutPadPx();
    double scale = LayoutToClientScale();
    out.x = outer.left + static_cast<int>(std::lround((padPx + caretX) * scale));
    out.y = outer.top + static_cast<int>(std::lround((padPx + static_cast<int>(lineIndex) * lineHeight) * scale));
    out = EditingLogicalToClient(out);
    if (outLineHeight) {
        *outLineHeight = std::max(1, static_cast<int>(std::lround(lineHeight * scale)));
    }
    return true;
}

static void UpdateImeWindowPosition(HWND hwnd, bool updateCandidate) {
    if (!hwnd || !g_pdf.editingText) return;
    RECT inner{};
    int innerWidth = 0;
    if (!GetEditingInnerRectClient(&inner, &innerWidth)) return;
    POINT caretPt{};
    int lineHeight = 0;
    if (!InlineCaretClientPoint(caretPt, &lineHeight)) return;
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;

    // Ensure IME uses the same font and composition area as the textbox.
    {
        LOGFONTW lf{};
        const double scale = (g_pdf.scale > 0.0) ? g_pdf.scale : 1.0;
        int fontPx = static_cast<int>(std::round(g_pdf.editFontPt * kDpi / 72.0 * scale));
        fontPx = std::max(6, fontPx);
        lf.lfHeight = -fontPx;
        lf.lfQuality = ANTIALIASED_QUALITY;
        lf.lfCharSet = DEFAULT_CHARSET;
        std::wstring fontName = g_pdf.editFontName.empty() ? g_textFontName : g_pdf.editFontName;
        if (fontName.empty()) {
            fontName = GetDefaultFontFaceName();
        }
        if (!fontName.empty()) {
            wcsncpy_s(lf.lfFaceName, fontName.c_str(), LF_FACESIZE - 1);
        }
        ImmSetCompositionFontW(himc, &lf);
    }

    COMPOSITIONFORM cf{};
    // COMPOSITIONFORM requires one positioning mode, not a bitwise mixture.
    cf.dwStyle = CFS_FORCE_POSITION;
    cf.ptCurrentPos = caretPt;
    cf.rcArea = inner;
    ImmSetCompositionWindow(himc, &cf);
    if (updateCandidate) {
        CANDIDATEFORM cand{};
        cand.dwIndex = 0;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        POINT candPt = caretPt;
        int candidateMargin = std::max(48, lineHeight * 6);
        int spaceBelow = rc.bottom - candPt.y;
        int spaceAbove = candPt.y - rc.top;
        RECT exclude = inner;
        exclude.top = std::max(rc.top, caretPt.y - std::max(1, lineHeight / 4));
        exclude.bottom = std::min(rc.bottom, caretPt.y + std::max(1, lineHeight + lineHeight / 2));
        if (exclude.bottom <= exclude.top) {
            exclude.bottom = std::min(rc.bottom, exclude.top + std::max(1, lineHeight));
        }
        const bool preferAbove = (spaceBelow < candidateMargin && spaceAbove > lineHeight);
        cand.dwStyle = CFS_EXCLUDE;
        if (preferAbove) {
            candPt.y = std::max(rc.top, exclude.top - std::max(8, lineHeight / 2));
        } else {
            candPt.y = std::min(rc.bottom - 1, exclude.bottom);
        }
        candPt.x = std::clamp(candPt.x, rc.left, std::max(rc.left, rc.right - 1));
        cand.ptCurrentPos = candPt;
        cand.rcArea = exclude;
        ImmSetCandidateWindow(himc, &cand);
    }
    ImmReleaseContext(hwnd, himc);
}

static bool IsImeOpen(HWND hwnd) {
    if (!hwnd) return false;
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return false;
    BOOL open = ImmGetOpenStatus(himc);
    ImmReleaseContext(hwnd, himc);
    return open != FALSE;
}

// Disable IME for PDF view when not editing text (prevents composition/candidate display).
static void DisablePdfViewIme(HWND hwnd) {
    if (!hwnd) return;
    // Disassociate IME context from the window to completely prevent IME input.
    ImmAssociateContextEx(hwnd, nullptr, 0);
}

// Enable IME for PDF view when editing text.
static void EnablePdfViewIme(HWND hwnd) {
    if (!hwnd) return;
    // Re-associate default IME context to enable IME input.
    if (ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT)) {
        // The PDF pane may already have focus, so WM_SETFOCUS need not run.
        // Replace the previous box's position before any text or IME toggle.
        UpdateImeWindowPosition(hwnd, true);
    }
}

static void ForceImeComplete(HWND hwnd, bool commit) {
    if (!hwnd || !g_pdf.editingText) return;
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) {
        g_pdf.imeComposing = false;
        g_pdf.imeComp.clear();
        g_pdf.suppressNextImeResultReturnChar = false;
        g_pdf.imeResultMessageTime = 0;
        return;
    }
    ImmNotifyIME(himc, NI_COMPOSITIONSTR, commit ? CPS_COMPLETE : CPS_CANCEL, 0);
    ImmReleaseContext(hwnd, himc);
    g_pdf.imeComposing = false;
    g_pdf.imeComp.clear();
    g_pdf.suppressNextImeResultReturnChar = false;
    g_pdf.imeResultMessageTime = 0;
    g_pdf.editLayoutDirty = true;
}

// UI-thread measurement only: owns local layout/GDI objects and never publishes edit state.
[[nodiscard]] static bool MeasureTextBoxEditGeometry(const Annotation& box, TextBoxEditGeometry& out) {
    if (box.pageIndex < 0 || box.pageIndex >= static_cast<int>(g_pdf.pages.size()) || !g_hPdfView) return false;
    const auto origin = annotation_rotation::ContentOrigin(box);
    const double left = origin.x, top = origin.y;
    const auto& page = g_pdf.pages[static_cast<size_t>(box.pageIndex)];
    double pageAvailWidthPt = std::max(10.0, page.widthPt - 8.0);
    // The default mode preserves the insertion anchor and starts wrapping at
    // the page edge. The legacy mode keeps its whole-page measure/left-shift
    // behavior for users who explicitly select it.
    double maxWidthPt = g_textBoxAutoWrap
        ? std::max(10.0, page.widthPt - left - 4.0)
        : pageAvailWidthPt;
    double maxHeightPt = std::max(8.0, top);
    switch (page_rotation::Normalize(box.contentRotation)) {
    case 1: maxWidthPt = std::max(10.0, top - 4.0); maxHeightPt = std::max(8.0, left); break;
    case 2: maxWidthPt = std::max(10.0, left - 4.0); maxHeightPt = std::max(8.0, page.heightPt - top); break;
    case 3: maxWidthPt = std::max(10.0, page.heightPt - top - 4.0); maxHeightPt = std::max(8.0, page.widthPt - left); break;
    }
    int maxPxW = PtToLayoutPx(maxWidthPt);
    if (maxPxW <= 0) maxPxW = 1;
    int padPx = LayoutPadPx();
    int innerPxW = std::max(1, maxPxW - padPx * 2);

    const std::wstring& text = box.text;

    HDC hdc = GetDC(g_hPdfView);
    if (!hdc) return false;
    LOGFONTW lf{};
    lf.lfHeight = -TextBoxFontHeightPx(box.fontPt);
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    std::wstring fontName = box.fontName.empty() ? g_textFontName : box.fontName;
    if (fontName.empty()) {
        fontName = GetDefaultFontFaceName();
    }
    if (!fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, fontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = nullptr;
    if (hFont) oldFont = SelectObject(hdc, hFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    int lineHeight = std::max(1, static_cast<int>(tm.tmHeight));
    TextLayoutResult layout;
    layout.tm = tm;
    layout.lines = LayoutTextLines(hdc, text, innerPxW);
    if (layout.lines.empty()) layout.lines.push_back({ 0, 0, {} });
    int maxLineW = 0;
    for (const auto& ln : layout.lines) {
        if (!ln.widths.empty()) {
            maxLineW = std::max(maxLineW, ln.widths.back());
        }
    }
    if (maxLineW <= 0) {
        maxLineW = std::max(1, static_cast<int>(tm.tmAveCharWidth));
    }
    int lineCount = std::max(1, static_cast<int>(layout.lines.size()));
    // tmHeight already includes descent; retain only the symmetric inner padding.
    const int neededHpx = lineHeight * lineCount;
    double wCalcPt = LayoutPxToPt(maxLineW + kTextBoxGlyphOverhangPadPx + padPx * 2);
    double hCalcPt = LayoutPxToPt(neededHpx + padPx * 2);
    wCalcPt = std::clamp(wCalcPt, 10.0, maxWidthPt);
    hCalcPt = std::clamp(hCalcPt, std::min(12.0, maxHeightPt), maxHeightPt);
    if (oldFont) SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
    ReleaseDC(g_hPdfView, hdc);

    double newLeft = left;
    double newTop = box.contentRotation == 0 ? std::clamp(top, 0.0, page.heightPt) : top;
    double newRight = newLeft + wCalcPt;
    double newBottom = newTop - hCalcPt;

    if (box.contentRotation == 0 && !g_textBoxAutoWrap && newRight > page.widthPt) {
        double shift = newRight - page.widthPt;
        newLeft = std::max(0.0, newLeft - shift);
        newRight = page.widthPt;
    }
    if (box.contentRotation == 0 && newLeft < 0.0) {
        double shift = -newLeft;
        newLeft = 0.0;
        newRight = std::min(page.widthPt, newRight + shift);
    }
    if (box.contentRotation == 0 && newBottom < 0.0) {
        double delta = -newBottom;
        newTop = std::min(page.heightPt, newTop + delta);
        newBottom = 0.0;
    }

    out.bounds = {newLeft, newTop, newRight, newBottom};
    out.innerWidth = std::max(1, PtToLayoutPx(std::abs(newRight - newLeft)) - padPx * 2);
    out.layout = std::move(layout);
    return true;
}

static void RecalcEditingTextboxSize(bool includeComp) {
    if (!g_pdf.editingText || g_pdf.editPage < 0 ||
        g_pdf.editPage >= static_cast<int>(g_pdf.pages.size())) return;
    Annotation box;
    box.pageIndex = g_pdf.editPage;
    box.contentRotation = g_pdf.editContentRotation;
    annotation_rotation::SetContentBounds(box,
        {std::min(g_pdf.editX1, g_pdf.editX2), std::max(g_pdf.editY1, g_pdf.editY2)},
        std::abs(g_pdf.editX2 - g_pdf.editX1), std::abs(g_pdf.editY2 - g_pdf.editY1));
    box.text = BuildEditDisplayText(includeComp);
    box.fontName = g_pdf.editFontName;
    box.fontPt = g_pdf.editFontPt;
    TextBoxEditGeometry geometry;
    if (!MeasureTextBoxEditGeometry(box, geometry)) return;
    g_pdf.editX1 = geometry.bounds.left;
    g_pdf.editX2 = geometry.bounds.right;
    g_pdf.editY1 = geometry.bounds.top;
    g_pdf.editY2 = geometry.bounds.bottom;
    g_pdf.editLayout = std::move(geometry.layout);
    g_pdf.editInnerLayoutW = geometry.innerWidth;
    g_pdf.editLayoutDirty = false;
}

[[nodiscard]] static bool TextBoxReeditHoverCaret(const Annotation& view, const POINT& hit,
    POINT& start, POINT& end, RECT& bounds, int& lineHeight, size_t* caretIndex) {
    if (view.type != Annotation::Type::TextBox || view.pageIndex < 0 ||
        view.pageIndex >= static_cast<int>(g_pdf.pages.size()) || g_pdf.scale <= 0.0) return false;
    auto box = view;
    const auto origin = annotation_rotation::ContentOrigin(box);
    const auto extent = annotation_rotation::ContentExtent(box);
    const auto initial = InitialTextBoxEditBounds(g_pdf.pages[static_cast<size_t>(box.pageIndex)],
        origin.x, origin.y, extent.width, extent.height, box.contentRotation);
    annotation_rotation::SetContentBounds(box, {initial.left, initial.top},
        initial.right - initial.left, initial.top - initial.bottom);
    if (box.fontPt <= 0.0) box.fontPt = ResolveAdaptiveTextFontPtForPage(box.pageIndex, g_textFontPt);
    TextBoxEditGeometry geometry;
    if (!MeasureTextBoxEditGeometry(box, geometry) || geometry.layout.lines.empty()) return false;
    const auto& measured = geometry.bounds;
    const double width = measured.right - measured.left, height = measured.top - measured.bottom;
    annotation_rotation::SetContentBounds(box, {measured.left, measured.top}, width, height);
    if (!PtRectToClientRect(box.pageIndex, box.x1, box.y1, box.x2, box.y2, bounds)) return false;
    const double pixelsPerPt = kDpi / 72.0 * g_pdf.scale;
    const RECT logical{bounds.left, bounds.top,
        bounds.left + static_cast<LONG>(std::lround(width * pixelsPerPt)),
        bounds.top + static_cast<LONG>(std::lround(height * pixelsPerPt))};
    POINT point = TextBoxCaretHitPoint(hit);
    point.x = std::clamp(point.x, bounds.left, std::max(bounds.left, bounds.right - 1));
    point.y = std::clamp(point.y, bounds.top, std::max(bounds.top, bounds.bottom - 1));
    point = TextBoxLogicalToClient(point, bounds, logical, box.contentRotation, true);
    const double scale = LayoutToClientScale();
    const POINT local{static_cast<LONG>(std::lround((point.x - logical.left) / scale)),
                      static_cast<LONG>(std::lround((point.y - logical.top) / scale))};
    const int pad = LayoutPadPx();
    const RECT inner{pad, pad, std::max(pad + 1, PtToLayoutPx(width) - pad),
                               std::max(pad + 1, PtToLayoutPx(height) - pad)};
    const size_t caret = std::min(HitTestCaretIndex(geometry.layout, inner, local), box.text.size());
    const size_t row = FindLineForCaret(geometry.layout.lines, caret);
    const auto& line = geometry.layout.lines[row];
    const int x = ColumnWidthForLine(line, caret > line.start ? std::min(caret - line.start, line.len) : 0);
    const int rowHeight = std::max(1, static_cast<int>(geometry.layout.tm.tmHeight));
    lineHeight = std::max(1, static_cast<int>(std::lround(rowHeight * scale)));
    POINT caretStart{logical.left + static_cast<LONG>(std::lround((pad + x) * scale)),
        logical.top + static_cast<LONG>(std::lround((pad + static_cast<int>(row) * rowHeight) * scale))};
    start = TextBoxLogicalToClient({caretStart.x, caretStart.y + 2}, bounds, logical, box.contentRotation);
    end = TextBoxLogicalToClient({caretStart.x, caretStart.y + std::max(2, lineHeight - 2)},
        bounds, logical, box.contentRotation);
    if (caretIndex) *caretIndex = caret;
    return true;
}

// ---------------------------------------------------------------------
// mode buttons
// ---------------------------------------------------------------------
void SetModeButtons() {
    const bool previewReadOnly = IsPdfPreviewReadOnlyActive();
    ToolMode displayMode = g_toolMode;
    const auto previewAllowsTool = [](ToolMode mode) {
        return mode == ToolMode::Select || mode == ToolMode::Pan || mode == ToolMode::Magnifier;
    };
    if (previewReadOnly && !previewAllowsTool(displayMode)) {
        displayMode = ToolMode::Pan;
    }
    if (g_hBtnModeSelect)
        SendMessageW(g_hBtnModeSelect, BM_SETCHECK,
                     displayMode == ToolMode::Select ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModePan)
        SendMessageW(g_hBtnModePan, BM_SETCHECK,
                     displayMode == ToolMode::Pan ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeMagnifier)
        SendMessageW(g_hBtnModeMagnifier, BM_SETCHECK,
                     displayMode == ToolMode::Magnifier ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeMarker)
        SendMessageW(g_hBtnModeMarker, BM_SETCHECK,
                     IsMarkerGroupMode(displayMode) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeMarkerFree)
        SendMessageW(g_hBtnModeMarkerFree, BM_SETCHECK,
                     displayMode == ToolMode::MarkerFree ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeMarkerLine)
        SendMessageW(g_hBtnModeMarkerLine, BM_SETCHECK,
                     displayMode == ToolMode::MarkerLine ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeMarkerArrow)
        SendMessageW(g_hBtnModeMarkerArrow, BM_SETCHECK,
                     displayMode == ToolMode::MarkerArrow ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeMarkerWave)
        SendMessageW(g_hBtnModeMarkerWave, BM_SETCHECK,
                     displayMode == ToolMode::MarkerWave ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeText)
        SendMessageW(g_hBtnModeText, BM_SETCHECK,
                     displayMode == ToolMode::TextBox ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeLine)
        SendMessageW(g_hBtnModeLine, BM_SETCHECK,
                     displayMode == ToolMode::Line ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeArrow)
        SendMessageW(g_hBtnModeArrow, BM_SETCHECK,
                     displayMode == ToolMode::Arrow ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeWave)
        SendMessageW(g_hBtnModeWave, BM_SETCHECK,
                     displayMode == ToolMode::Wave ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeFreehand)
        SendMessageW(g_hBtnModeFreehand, BM_SETCHECK,
                     IsPenGroupMode(displayMode) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeShape)
        SendMessageW(g_hBtnModeShape, BM_SETCHECK,
                     IsShapeGroupMode(displayMode) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hBtnModeEraser)
        SendMessageW(g_hBtnModeEraser, BM_SETCHECK,
                     displayMode == ToolMode::Eraser ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hChkTextReadableBackground)
        SendMessageW(g_hChkTextReadableBackground, BM_SETCHECK,
                     g_textBoxReadableBackground ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hRadioTextReadableBackgroundNormal)
        SendMessageW(g_hRadioTextReadableBackgroundNormal, BM_SETCHECK,
                     g_textBoxReadableBackgroundInverted ? BST_UNCHECKED : BST_CHECKED, 0);
    if (g_hRadioTextReadableBackgroundInverted)
        SendMessageW(g_hRadioTextReadableBackgroundInverted, BM_SETCHECK,
                     g_textBoxReadableBackgroundInverted ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hChkTextAutoWrap)
        SendMessageW(g_hChkTextAutoWrap, BM_SETCHECK,
                     g_textBoxAutoWrap ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hChkPanMouseWheelZoom)
        SendMessageW(g_hChkPanMouseWheelZoom, BM_SETCHECK,
                     g_config.panMouseWheelZoom ? BST_CHECKED : BST_UNCHECKED, 0);

    const BOOL previewAllowsAnnot = previewReadOnly ? FALSE : TRUE;
    if (g_hBtnModeMarker) EnableWindow(g_hBtnModeMarker, previewAllowsAnnot);
    if (g_hBtnModeFreehand) EnableWindow(g_hBtnModeFreehand, previewAllowsAnnot);
    if (g_hBtnModeText) EnableWindow(g_hBtnModeText, previewAllowsAnnot);
    if (g_hBtnModeShape) EnableWindow(g_hBtnModeShape, previewAllowsAnnot);
    if (g_hBtnModeEraser) EnableWindow(g_hBtnModeEraser, previewAllowsAnnot);
    if (g_hComboFont) EnableWindow(g_hComboFont, previewAllowsAnnot);
    if (g_hComboFontSize) EnableWindow(g_hComboFontSize, previewAllowsAnnot);
    if (g_hComboFontSizeAlt) EnableWindow(g_hComboFontSizeAlt, previewAllowsAnnot);
    if (g_hRadioFontSizeSlotA) EnableWindow(g_hRadioFontSizeSlotA, previewAllowsAnnot);
    if (g_hRadioFontSizeSlotB) EnableWindow(g_hRadioFontSizeSlotB, previewAllowsAnnot);
    if (g_hChkTextReadableBackground) EnableWindow(g_hChkTextReadableBackground, previewAllowsAnnot);
    if (g_hRadioTextReadableBackgroundNormal) EnableWindow(g_hRadioTextReadableBackgroundNormal, previewAllowsAnnot);
    if (g_hRadioTextReadableBackgroundInverted) EnableWindow(g_hRadioTextReadableBackgroundInverted, previewAllowsAnnot);
    if (g_hChkTextAutoWrap) EnableWindow(g_hChkTextAutoWrap, previewAllowsAnnot);
    if (g_hChkPanMouseWheelZoom) EnableWindow(g_hChkPanMouseWheelZoom, previewAllowsAnnot);
    if (g_hComboWidth) EnableWindow(g_hComboWidth, previewAllowsAnnot);
    if (g_hComboMarkerAlpha) EnableWindow(g_hComboMarkerAlpha, previewAllowsAnnot);
    if (g_hComboAnnotMethod) EnableWindow(g_hComboAnnotMethod, previewAllowsAnnot);
    if (g_hComboMarkerTextStyle) EnableWindow(g_hComboMarkerTextStyle, previewAllowsAnnot);
    if (g_hComboLineDashStyle) EnableWindow(g_hComboLineDashStyle, previewAllowsAnnot);
    if (g_hComboShapeKind) EnableWindow(g_hComboShapeKind, previewAllowsAnnot);
    if (g_hComboShapeDrawMode) EnableWindow(g_hComboShapeDrawMode, previewAllowsAnnot);
    if (g_hAnnotShow) EnableWindow(g_hAnnotShow, previewAllowsAnnot);
    if (g_hAnnotSettings) EnableWindow(g_hAnnotSettings, previewAllowsAnnot);
    if (g_hAnnotClear) EnableWindow(g_hAnnotClear, previewAllowsAnnot);
    if (g_hAnnotList) EnableWindow(g_hAnnotList, previewAllowsAnnot);
    if (g_hAnnotSummary) { EnableWindow(g_hAnnotSummary, previewAllowsAnnot); ShowWindow(g_hAnnotSummary, SW_HIDE); }
}


static bool RotatePdfPageViews(HWND pdfWnd, int quarterTurns, bool reset, bool allPages) {
    if (!pdfWnd || g_pdf.kind != DocKind::Pdf || IsSaveTransactionRunning()) return false;
    CommitActiveTextEditing(true);
    if (g_pdf.editingText) return false;
    const int index = PageAtCurrentView();
    if (index < 0 || index >= static_cast<int>(g_pdf.pages.size()) ||
        g_pdf.pages.size() != static_cast<size_t>(g_pdf.pageCount)) return false;
    const size_t first = allPages ? 0 : static_cast<size_t>(index);
    const size_t last = allPages ? g_pdf.pages.size() : first + 1;
    const int delta = page_rotation::Normalize(quarterTurns);
    const auto nextTurn = [reset, delta](const PageCache& page) {
        return reset ? 0 : page_rotation::Normalize(page.viewRotation + delta);
    };
    bool changed = false;
    // Preflight metadata already available from layout; do not load every PDF
    // page's contents merely to change its view angle.
    for (size_t i = first; i < last; ++i) {
        const auto& target = g_pdf.pages[i];
        if (target.index != static_cast<int>(i) || !std::isfinite(target.canonicalWidthPt) ||
            !std::isfinite(target.canonicalHeightPt) || target.canonicalWidthPt <= 0 || target.canonicalHeightPt <= 0)
            return false;
        changed = changed || nextTurn(target) != target.viewRotation;
    }
    if (!changed) return true;
    if (!EnsurePdfPageCoordinates(index)) return false;
    // Allocate the session vector before changing any page in the batch.
    g_pdf.pageViewRotations.resize(static_cast<size_t>(g_pdf.pageCount), 0);
    const auto& page = g_pdf.pages[static_cast<size_t>(index)];
    const int next = nextTurn(page);
    RECT client{};
    GetClientRect(pdfWnd, &client);
    const int viewW = client.right - client.left, viewH = client.bottom - client.top;
    const double pixelsPerPt = kDpi / 72.0 * g_pdf.scale;
    const auto local = page_rotation::Point{
        std::clamp((viewW * 0.5 - PageClientLeft(index)) / pixelsPerPt, 0.0, page.widthPt),
        std::clamp(page.heightPt - (viewH * 0.5 - PageClientTop(index)) / pixelsPerPt, 0.0, page.heightPt)};
    const auto anchor = page_rotation::Between(local, page.canonicalWidthPt, page.canonicalHeightPt,
                                               page.viewRotation, next);
    if (g_pdf.movingAnnot && g_pdf.movingIndex >= 0 && g_pdf.movingIndex < static_cast<int>(g_annots.size())) {
        const int movingIndex = g_pdf.movingIndex;
        const Annotation after = g_annots[static_cast<size_t>(movingIndex)];
        if (!AnnotationEquals(g_pdf.movingOriginal, after)) {
            RecordAnnotUpdate(movingIndex, movingIndex, g_pdf.movingOriginal, after);
            MarkAnnotsDirty(GetParent(pdfWnd));
        }
    }
    ClearAnnotationSelection();
    ClearPdfTextSelection();
    g_scrollVelocity = 0;
    KillTimer(pdfWnd, kScrollTimerId);
    g_pdf.panning = false;
    g_pdf.drawingAnnot = false;
    g_pdf.drawPath.clear();
    if (GetCapture() == pdfWnd) ReleaseCapture();
    for (size_t i = first; i < last; ++i) {
        auto& target = g_pdf.pages[i];
        const int rotation = nextTurn(target);
        g_pdf.pageViewRotations[i] = rotation;
        if (rotation == target.viewRotation) continue;
        target.viewRotation = rotation;
        const auto size = page_rotation::Extent(target.canonicalWidthPt, target.canonicalHeightPt, rotation);
        target.widthPt = size.width;
        target.heightPt = size.height;
        target.w = static_cast<int>(std::round(size.width * pixelsPerPt));
        target.h = static_cast<int>(std::round(size.height * pixelsPerPt));
        EvictPageBitmap(target);
        EvictPageCharBoxes(target);
    }
    const auto extent = page_rotation::Extent(page.canonicalWidthPt, page.canonicalHeightPt, next);
    // One generation/layout/cache update for the whole operation, including
    // when only pages before the current page changed during a reset.
    InvalidatePdfRenderGeneration();
    UpdatePageLayout(pdfWnd);
    const auto rect = g_pdf.pageRects[static_cast<size_t>(index)];
    g_pdf.scrollX = rect.left + anchor.x * pixelsPerPt + FlowCrossPaddingX(CurrentFlowMode()) - viewW * 0.5;
    g_pdf.scrollY = rect.top + (extent.height - anchor.y) * pixelsPerPt + FlowCrossPaddingY(CurrentFlowMode()) - viewH * 0.5;
    ClampPdfScroll(viewH, viewW);
    UpdatePdfVirtualScrollCaches(pdfWnd);
    InvalidateRect(pdfWnd, nullptr, FALSE);
    SaveLastPdfViewInfo();
    return true;
}

bool RotateCurrentPdfPageView(HWND pdfWnd, int quarterTurns, bool reset) {
    return RotatePdfPageViews(pdfWnd, quarterTurns, reset, false);
}

bool RotateAllPdfPagesView(HWND pdfWnd, int quarterTurns, bool reset) {
    return RotatePdfPageViews(pdfWnd, quarterTurns, reset, true);
}

#include "pdf_view/page_rotation_tests.cppinc"
