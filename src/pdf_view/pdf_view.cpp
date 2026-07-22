// file: pdf_view/pdf_view.cpp
#include "pdf_view/pdf_view.h"
#include "ui/core/main_window_api.h"
#include "note_view/note_view.h"
#include "bridge/view_bridge.h"
#include "core/ui_notify.h"
#include "core/ui_prompts.h"
#include "math/math_render.h"
#include "file_output/file_output.h"
#include "clrop/bridge.h"
#include "clrop/hash.h"
#include "fpdf_save.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_javascript.h"
#include "core/atomic_write.h"
#include "core/preview_trace.h"
#include "core/secure_memory.h"
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
static void FillRectAlpha(HDC hdc, const RECT& r, COLORREF color, BYTE alpha);
static void FillRectAlphaMaskedByText(HDC hdc, const RECT& r, COLORREF color, BYTE alpha,
                                      const PageCache& page, int pageLeft, int pageTop,
                                      const std::vector<RECT>& textRects, double textAlphaScale);
static void TintRectTextPixels(HDC hdc, const RECT& r, COLORREF color,
                               const PageCache& page, int pageLeft, int pageTop,
                               const std::vector<RECT>& textRects);
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
static ShapeBounds NormalizeShapeBounds(ShapeKind kind, double x1, double y1, double x2, double y2);
static bool TryBuildFreehandCorrection(const Annotation& source, FreehandCorrectionResult* out);
static bool TryBuildFreehandSmoothing(const Annotation& source, Annotation* out);
static std::vector<Annotation::Pt> BuildShapePolygonPointsPdf(ShapeKind kind, const ShapeBounds& bounds);
static std::vector<POINT> BuildShapePolygonPointsClient(int pageIndex, ShapeKind kind, const ShapeBounds& bounds);
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
    if (owner) return owner;
    if (g_hPdfView) {
        HWND parent = GetParent(g_hPdfView);
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
                      IsEnglishUi() ? L"Copied text." : L"テキストをコピーしました。",
                      SoftNoticeKind::Info);
}
static void ShowPdfMessageDialog(HWND owner, const std::wstring& title,
                                 const std::wstring& message, SoftNoticeKind kind) {
    ShowSilentMessageDialog(PdfDialogOwner(owner), title, message, kind);
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
                                       const std::wstring& blockedMessage) {
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
        (IsEnglishUi()
             ? L"\n\nEnter a password and reopen this PDF?"
             : L"\n\nパスワードを入力してこのPDFを再オープンしますか。");
    options.kind = SoftNoticeKind::Warning;
    options.buttons = SilentDialogButtons::YesNo;
    options.yesLabel = IsEnglishUi() ? L"Enter Password" : L"パスワード入力";
    options.noLabel = IsEnglishUi() ? L"Cancel" : L"キャンセル";
    options.defaultResult = SilentDialogResult::Yes;
    options.escapeResult = SilentDialogResult::No;
    if (ShowPdfDialog(owner, options) != SilentDialogResult::Yes) {
        return false;
    }

    std::wstring password;
    SecureWideStringScope passwordScope(&password);
    const std::wstring promptMessage =
        IsEnglishUi()
            ? L"Enter the PDF password.\nIt is kept only in memory for the current session and is not written to disk."
            : L"PDFのパスワードを入力してください。\nパスワードは現在のセッション中だけメモリに保持し、ディスクには保存しません。";
    if (!PromptPasswordText(PdfDialogOwner(owner), title, promptMessage, password)) {
        return false;
    }

    QueuePdfOpenPasswordForNextLoad(WideToUTF8(password));
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
static bool PasteClipboardToEdit(HWND hwnd);
static bool InlineCaretClientPoint(POINT& out, int* outLineHeight = nullptr);
static void RecalcEditingTextboxSize(bool includeComp);
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
#include "pdf_view/navigation.cppinc"

#include "pdf_view/render.cppinc"

#include "pdf_view/input.cppinc"

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

static std::vector<Annotation::Pt> BuildShapePolygonPointsPdf(ShapeKind kind, const ShapeBounds& bounds) {
    std::vector<Annotation::Pt> pts;
    const double midX = (bounds.left + bounds.right) * 0.5;
    const double midY = (bounds.bottom + bounds.top) * 0.5;
    switch (kind) {
    case ShapeKind::Diamond:
        pts.push_back({ midX, bounds.top });
        pts.push_back({ bounds.right, midY });
        pts.push_back({ midX, bounds.bottom });
        pts.push_back({ bounds.left, midY });
        break;
    case ShapeKind::Triangle:
    case ShapeKind::EquilateralTriangle:
        pts.push_back({ midX, bounds.top });
        pts.push_back({ bounds.right, bounds.bottom });
        pts.push_back({ bounds.left, bounds.bottom });
        break;
    default:
        break;
    }
    return pts;
}

static std::vector<POINT> BuildShapePolygonPointsClient(int pageIndex, ShapeKind kind, const ShapeBounds& bounds) {
    std::vector<POINT> out;
    auto pdfPts = BuildShapePolygonPointsPdf(kind, bounds);
    out.reserve(pdfPts.size());
    for (const auto& pdfPt : pdfPts) {
        POINT pt{};
        if (PdfPtToClientPoint(pageIndex, pdfPt.x, pdfPt.y, pt)) {
            out.push_back(pt);
        }
    }
    return out;
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

static bool IsTextPixel(BYTE r, BYTE g, BYTE b) {
    int maxc = std::max(r, std::max(g, b));
    int minc = std::min(r, std::min(g, b));
    if (maxc <= kMarkerTextDarkMax) return true;
    if (maxc >= kMarkerTextBrightMin) return minc <= kMarkerTextBrightMinChannel;
    return (maxc - minc) >= kMarkerTextSaturationMin;
}

static bool IsBlackTextPixel(BYTE r, BYTE g, BYTE b) {
    int maxc = std::max(r, std::max(g, b));
    int minc = std::min(r, std::min(g, b));
    if (maxc > kMarkerTextBlackMax) return false;
    return (maxc - minc) <= kMarkerTextBlackSatMax;
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

static void FillRectAlphaMaskedByText(HDC hdc, const RECT& r, COLORREF color, BYTE alpha,
                                      const PageCache& page, int pageLeft, int pageTop,
                                      const std::vector<RECT>& textRects, double textAlphaScale) {
    if (textRects.empty() || alpha == 0 || textAlphaScale >= 1.0) {
        FillRectAlpha(hdc, r, color, alpha);
        return;
    }
    if (page.pixels.empty() || page.stride <= 0 || page.w <= 0 || page.h <= 0 ||
        page.bmpW <= 0 || page.bmpH <= 0) {
        FillRectAlpha(hdc, r, color, alpha);
        return;
    }
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    double scale = std::clamp(textAlphaScale, 0.0, 1.0);
    BYTE textAlpha = static_cast<BYTE>(std::round(alpha * scale));
    if (textAlpha == alpha) {
        FillRectAlpha(hdc, r, color, alpha);
        return;
    }

    RECT pageRect{ r.left - pageLeft, r.top - pageTop, r.right - pageLeft, r.bottom - pageTop };
    RECT pageBounds{ 0, 0, page.w, page.h };
    RECT pageClipped{};
    if (!IntersectRect(&pageClipped, &pageRect, &pageBounds)) {
        FillRectAlpha(hdc, r, color, alpha);
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    double baseA = alpha / 255.0;
    BYTE rBase = static_cast<BYTE>(std::round(GetRValue(color) * baseA));
    BYTE gBase = static_cast<BYTE>(std::round(GetGValue(color) * baseA));
    BYTE bBase = static_cast<BYTE>(std::round(GetBValue(color) * baseA));
    DWORD basePremul = (static_cast<DWORD>(alpha) << 24) | (rBase << 16) | (gBase << 8) | bBase;

    double textA = textAlpha / 255.0;
    BYTE rText = static_cast<BYTE>(std::round(GetRValue(color) * textA));
    BYTE gText = static_cast<BYTE>(std::round(GetGValue(color) * textA));
    BYTE bText = static_cast<BYTE>(std::round(GetBValue(color) * textA));
    DWORD textPremul = (static_cast<DWORD>(textAlpha) << 24) | (rText << 16) | (gText << 8) | bText;

    double scaleX = static_cast<double>(page.bmpW) / static_cast<double>(page.w);
    double scaleY = static_cast<double>(page.bmpH) / static_cast<double>(page.h);
    std::fill(buf.begin(), buf.end(), basePremul);
    for (const auto& tr : textRects) {
        RECT inter{};
        if (!IntersectRect(&inter, &tr, &pageClipped)) continue;
        for (int py = inter.top; py < inter.bottom; ++py) {
            int srcY = static_cast<int>(py * scaleY);
            if (srcY < 0) srcY = 0;
            if (srcY >= page.bmpH) srcY = page.bmpH - 1;
            const uint8_t* src = page.pixels.data() + static_cast<size_t>(srcY) * page.stride;
            DWORD* dst = buf.data()
                + static_cast<size_t>(py - pageRect.top) * w
                + static_cast<size_t>(inter.left - pageRect.left);
            for (int px = inter.left; px < inter.right; ++px) {
                int srcX = static_cast<int>(px * scaleX);
                if (srcX < 0) srcX = 0;
                if (srcX >= page.bmpW) srcX = page.bmpW - 1;
                size_t idx = static_cast<size_t>(srcX) * 4;
                BYTE b = src[idx + 0];
                BYTE g = src[idx + 1];
                BYTE rC = src[idx + 2];
                if (IsTextPixel(rC, g, b)) {
                    dst[px - inter.left] = IsBlackTextPixel(rC, g, b) ? 0 : textPremul;
                }
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

static void TintRectTextPixels(HDC hdc, const RECT& r, COLORREF color,
                               const PageCache& page, int pageLeft, int pageTop,
                               const std::vector<RECT>& textRects) {
    if (textRects.empty() || page.pixels.empty() || page.stride <= 0 ||
        page.w <= 0 || page.h <= 0 || page.bmpW <= 0 || page.bmpH <= 0) {
        return;
    }
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    RECT pageRect{ r.left - pageLeft, r.top - pageTop, r.right - pageLeft, r.bottom - pageTop };
    RECT pageBounds{ 0, 0, page.w, page.h };
    RECT pageClipped{};
    if (!IntersectRect(&pageClipped, &pageRect, &pageBounds)) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    for (const auto& tr : textRects) {
        RECT inter{};
        if (!IntersectRect(&inter, &tr, &pageClipped)) continue;
        for (int py = inter.top; py < inter.bottom; ++py) {
            DWORD* dst = buf.data()
                + static_cast<size_t>(py - pageRect.top) * w
                + static_cast<size_t>(inter.left - pageRect.left);
            for (int px = inter.left; px < inter.right; ++px) {
                COLORREF sample{};
                if (!SamplePagePixelColor(page, px, py, sample)) continue;
                const BYTE red = GetRValue(sample);
                const BYTE g = GetGValue(sample);
                const BYTE b = GetBValue(sample);
                if (!IsTextPixel(red, g, b)) continue;
                const int maxc = std::max<int>(red, std::max<int>(g, b));
                const int minc = std::min<int>(red, std::min<int>(g, b));
                const BYTE alpha = static_cast<BYTE>(
                    std::clamp(IsBlackTextPixel(red, g, b) ? 255 - maxc : 255 - minc, 1, 255));
                const double a = alpha / 255.0;
                const BYTE outR = static_cast<BYTE>(std::lround(GetRValue(color) * a));
                const BYTE outG = static_cast<BYTE>(std::lround(GetGValue(color) * a));
                const BYTE outB = static_cast<BYTE>(std::lround(GetBValue(color) * a));
                dst[px - inter.left] = (static_cast<DWORD>(alpha) << 24) |
                    (static_cast<DWORD>(outR) << 16) |
                    (static_cast<DWORD>(outG) << 8) |
                    static_cast<DWORD>(outB);
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

void EndInlineTextEditing(HWND pdfWnd, bool commit) {
    if (g_pdf.editingText) {
        CommitTextEditing(pdfWnd, commit);
    }
}

bool TryBuildCurrentAnnotationSaveSnapshot(std::vector<Annotation>* outAnnotations) {
    if (!outAnnotations) return false;

    const auto* logical = CurrentLogicalPdfAnnotations();
    *outAnnotations = logical ? *logical : g_annots;

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
            ann.text = committedText;
            ann.fontName = g_pdf.editFontName;
            ann.fontPt = g_pdf.editFontPt;
            FinalizeTextBoxAnnotationLayout(ann);

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
            ann.text = g_pdf.editText;
            ann.fontName = g_pdf.editFontName;
            ann.fontPt = g_pdf.editFontPt;
            // A disabled toolbar leaves the annotation inheriting the global
            // assist preference, so enabling it later can assist this text.
            ann.backgroundAssistMode = !g_textBoxReadableBackground
                ? TextBackgroundAssistMode::Inherit
                : (g_textBoxReadableBackgroundInverted
                    ? TextBackgroundAssistMode::Inverted
                    : TextBackgroundAssistMode::Auto);
            FinalizeTextBoxAnnotationLayout(ann);

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
        g_textFontPt = g_preEditToolbarState.textFontPt;
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

    // Ensure the text box is deselected after panel refresh,
    // so that changing tool colors later does not unintentionally modify this text box.
    if (g_hAnnotList) {
        SendMessageW(g_hAnnotList, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }

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
    double left = std::min(ann.x1, ann.x2);
    double top = std::max(ann.y1, ann.y2);
    const auto& page = g_pdf.pages[static_cast<size_t>(ann.pageIndex)];
    double maxWidthPt = std::max(10.0, page.widthPt - left - 4.0);
    double maxHeightPt = std::max(8.0, top);
    int maxPxW = PtToLayoutPx(maxWidthPt);
    int padPx = LayoutPadPx();
    int innerPxW = std::max(1, maxPxW - padPx * 2);

    HDC hdc = GetDC(g_hPdfView);
    if (!hdc) {
        ann.textLines = SplitTextByNewlines(ann.text);
        return;
    }
    LOGFONTW lf{};
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    std::wstring fontName = ann.fontName.empty() ? g_textFontName : ann.fontName;
    if (fontName.empty()) {
        fontName = GetDefaultFontFaceName();
    }
    lf.lfHeight = -static_cast<int>(std::lround(fontPt * kTextBoxLayoutDpi / 72.0));
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
    int neededHpx = lineHeight * lineCount;
    neededHpx += tm.tmDescent + tm.tmExternalLeading + 2;
    double wCalcPt = LayoutPxToPt(maxLineW + kTextBoxGlyphOverhangPadPx + padPx * 2);
    double hCalcPt = LayoutPxToPt(neededHpx + padPx * 2);
    wCalcPt = std::min(wCalcPt, maxWidthPt);
    hCalcPt = std::min(hCalcPt, maxHeightPt);
    if (oldFont) SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
    ReleaseDC(g_hPdfView, hdc);

    ann.x1 = left;
    ann.y1 = top;
    ann.x2 = left + wCalcPt;
    ann.y2 = top - hCalcPt;
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
    lf.lfHeight = -static_cast<int>(std::lround(fontPt * kTextBoxLayoutDpi / 72.0));
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
    double widthPt = std::abs(ann.x2 - ann.x1);
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
    int fontPt100 = 0;
    int wPt1000 = 0;
    int hPt1000 = 0;
    COLORREF color = 0;
    bool operator==(const TextBoxRasterKey& o) const {
        return fontPt100 == o.fontPt100 &&
               wPt1000 == o.wPt1000 &&
               hPt1000 == o.hPt1000 &&
               color == o.color &&
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
        h = HashCombine(h, std::hash<int>{}(k.fontPt100));
        h = HashCombine(h, std::hash<int>{}(k.wPt1000));
        h = HashCombine(h, std::hash<int>{}(k.hPt1000));
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
    k.fontPt100 = static_cast<int>(std::lround(fontPt * 100.0));
    double wPt = std::abs(ann.x2 - ann.x1);
    double hPt = std::abs(ann.y2 - ann.y1);
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

    const double fontPt = key.fontPt100 / 100.0;
    int fontPx = static_cast<int>(std::lround(fontPt * kTextBoxStableRasterDpi / 72.0));
    fontPx = std::max(6, fontPx);
    LOGFONTW lf{};
    lf.lfHeight = -fontPx;
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

static bool DrawTextBoxLowZoomDirect(HDC hdc, const RECT& r, const Annotation& ann) {
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    int fontPx = static_cast<int>(std::lround(fontPt * kDpi / 72.0 * g_pdf.scale));
    if (fontPx <= 0 || fontPx > 13) return false;

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;

    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, r.left, r.top, r.right, r.bottom);

    LOGFONTW lf{};
    lf.lfHeight = -std::max(5, fontPx);
    lf.lfQuality = NONANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    std::wstring fontName = ann.fontName.empty() ? g_textFontName : ann.fontName;
    if (!fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, fontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    int oldBk = SetBkMode(hdc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(hdc, ann.color);

    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    int lineHeight = std::max(1, static_cast<int>(tm.tmHeight));
    int pad = std::max(0, static_cast<int>(std::lround(kTextPadPx * g_pdf.scale)));
    int x = r.left + pad;
    int y = r.top + pad;
    int bottomLimit = r.bottom - pad;

    std::wstring text = BuildTextBoxRenderText(ann);
    int ideographicWidth = MeasureIdeographicWidth(hdc);
    std::vector<int> advances;
    size_t start = 0;
    while (start <= text.size() && y < bottomLimit) {
        size_t nl = text.find(L'\n', start);
        size_t end = (nl == std::wstring::npos) ? text.size() : nl;
        std::wstring_view line(text.data() + start, end - start);
        if (!line.empty()) {
            DrawAdjustedLine(hdc, x, y, line, ideographicWidth, advances);
        }
        y += lineHeight;
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }

    SetTextColor(hdc, oldColor);
    SetBkMode(hdc, oldBk);
    if (oldFont) SelectObject(hdc, oldFont);
    if (font) DeleteObject(font);
    if (saved != 0) RestoreDC(hdc, saved);
    return true;
}

static void DrawTextBoxStable(HDC hdc, HDC scratchDc, const RECT& r, const Annotation& ann) {
    if (!hdc || !scratchDc) return;
    DrawTextBoxReadableBackground(hdc, r, ann);
    if (DrawTextBoxLowZoomDirect(hdc, r, ann)) return;

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
    int fontPx = static_cast<int>(std::lround(g_pdf.editFontPt * kTextBoxLayoutDpi / 72.0));
    fontPx = std::max(1, fontPx);
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
    if (!PtRectToClientRect(g_pdf.editPage, g_pdf.editX1, g_pdf.editY1, g_pdf.editX2, g_pdf.editY2, outer)) {
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
    if (!PtRectToClientRect(g_pdf.editPage, g_pdf.editX1, g_pdf.editY1, g_pdf.editX2, g_pdf.editY2, outer)) {
        return false;
    }
    POINT clamped = pt;
    if (!PtInRect(&outer, pt)) {
        if (!clampToOuter) return false;
        clamped.x = std::clamp(pt.x, outer.left, std::max(outer.left, outer.right - 1));
        clamped.y = std::clamp(pt.y, outer.top, std::max(outer.top, outer.bottom - 1));
    }
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
    size_t idx = 0;
    while (idx < line.widths.size() && x > line.widths[idx]) {
        ++idx;
    }
    if (idx > line.len) idx = line.len;
    return line.start + idx;
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
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) {
        CloseClipboard();
        return false;
    }
    void* dst = GlobalLock(hmem);
    if (!dst) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    std::memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(hmem);
    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    ShowPdfCopySuccessNotice(hwnd);
    return true;
}

static bool PasteClipboardToEdit(HWND hwnd) {
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
    if (!PtRectToClientRect(g_pdf.editPage, g_pdf.editX1, g_pdf.editY1, g_pdf.editX2, g_pdf.editY2, outer)) {
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
    cf.dwStyle = CFS_POINT | CFS_RECT | CFS_FORCE_POSITION;
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
    ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
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

static void RecalcEditingTextboxSize(bool includeComp) {
    if (!g_pdf.editingText || g_pdf.editPage < 0 ||
        g_pdf.editPage >= static_cast<int>(g_pdf.pages.size())) return;
    double left = std::min(g_pdf.editX1, g_pdf.editX2);
    double top = std::max(g_pdf.editY1, g_pdf.editY2);
    const auto& page = g_pdf.pages[static_cast<size_t>(g_pdf.editPage)];
    double maxWidthPt = std::max(10.0, page.widthPt - left - 4.0);
    double maxHeightPt = std::max(8.0, top);
    int maxPxW = PtToLayoutPx(maxWidthPt);
    if (maxPxW <= 0) maxPxW = 1;
    int padPx = LayoutPadPx();
    int innerPxW = std::max(1, maxPxW - padPx * 2);

    std::wstring text = BuildEditDisplayText(includeComp);

    HDC hdc = GetDC(g_hPdfView);
    LOGFONTW lf{};
    int fontPx = static_cast<int>(std::lround(g_pdf.editFontPt * kTextBoxLayoutDpi / 72.0));
    fontPx = std::max(1, fontPx);
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
    int neededHpx = lineHeight * lineCount;
    neededHpx += tm.tmDescent + tm.tmExternalLeading + 2;
    double wCalcPt = LayoutPxToPt(maxLineW + kTextBoxGlyphOverhangPadPx + padPx * 2);
    double hCalcPt = LayoutPxToPt(neededHpx + padPx * 2);
    wCalcPt = std::clamp(wCalcPt, 10.0, maxWidthPt);
    hCalcPt = std::clamp(hCalcPt, 12.0, maxHeightPt);
    if (oldFont) SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
    ReleaseDC(g_hPdfView, hdc);

    double newLeft = left;
    double newTop = std::clamp(top, 0.0, page.heightPt);
    double newRight = newLeft + wCalcPt;
    double newBottom = newTop - hCalcPt;

    if (newRight > page.widthPt) {
        double shift = newRight - page.widthPt;
        newLeft = std::max(0.0, newLeft - shift);
        newRight = page.widthPt;
    }
    if (newLeft < 0.0) {
        double shift = -newLeft;
        newLeft = 0.0;
        newRight = std::min(page.widthPt, newRight + shift);
    }
    if (newBottom < 0.0) {
        double delta = -newBottom;
        newTop = std::min(page.heightPt, newTop + delta);
        newBottom = 0.0;
    }

    g_pdf.editX1 = newLeft;
    g_pdf.editX2 = newRight;
    g_pdf.editY1 = newTop;
    g_pdf.editY2 = newBottom;

    int innerW = std::max(1, PtToLayoutPx(std::abs(newRight - newLeft)) - padPx * 2);
    g_pdf.editLayout = std::move(layout);
    g_pdf.editInnerLayoutW = innerW;
    g_pdf.editLayoutDirty = false;
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
