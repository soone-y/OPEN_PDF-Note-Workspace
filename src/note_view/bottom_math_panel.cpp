// Bottom MathBox input pane UI.

#include "bottom_math_panel.h"
#include "note_view_internal.h"
#include "ui/noop_nav_guard.h"

#include "note/note_math.h"
#include "pdf_view/pdf_view.h"

#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <richedit.h>

static HWND g_hBottomMathInput = nullptr;
static HWND g_hBottomMathPreview = nullptr;
static HWND g_hBottomMathAddLatex = nullptr;
static HWND g_hBottomMathAddMarkup = nullptr;
static HWND g_hBottomMathHint = nullptr;
static bool g_bottomMathPreviewTracking = false;
static bool g_bottomMathPreviewDragging = false;
static POINT g_bottomMathPreviewDragStart{};
static wchar_t g_bottomMathPendingCtrlInsertNavChar = 0;

constexpr int kBottomMathInputId = 5101;
constexpr int kBottomMathAddLatexId = 5102;
constexpr int kBottomMathAddMarkupId = 5103;
constexpr int kBottomMathPreviewId = 5104;

static void LayoutBottomMathControls(HWND hWnd) {
    if (!hWnd) return;
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const int margin = ScaleY(hWnd, 10);
    const int gap = ScaleY(hWnd, 8);
    const int buttonH = ScaleY(hWnd, 28);
    const int hintH = ScaleY(hWnd, 34);
    const int innerW = std::max<int>(0, static_cast<int>(rc.right - rc.left - margin * 2));
    const int buttonW = std::max(0, (innerW - gap) / 2);
    const int contentH = std::max<int>(0, static_cast<int>(rc.bottom - rc.top - margin * 2 - buttonH - hintH - gap * 3));
    const int splitGap = gap;
    const int editH = std::max<int>(48, (contentH - splitGap) / 2);
    const int previewH = std::max<int>(48, contentH - editH - splitGap);
    int y = margin;
    if (g_hBottomMathInput) {
        MoveWindow(g_hBottomMathInput, margin, y, innerW, editH, TRUE);
    }
    y += editH + splitGap;
    if (g_hBottomMathPreview) {
        MoveWindow(g_hBottomMathPreview, margin, y, innerW, previewH, TRUE);
    }
    y += previewH + gap;
    if (g_hBottomMathAddLatex) {
        MoveWindow(g_hBottomMathAddLatex, margin, y, buttonW, buttonH, TRUE);
    }
    if (g_hBottomMathAddMarkup) {
        MoveWindow(g_hBottomMathAddMarkup, margin + buttonW + gap, y, buttonW, buttonH, TRUE);
    }
    y += buttonH + gap;
    if (g_hBottomMathHint) {
        MoveWindow(g_hBottomMathHint, margin, y, innerW, hintH, TRUE);
    }
}

static std::wstring ReadBottomMathInputText() {
    if (!g_hBottomMathInput) return L"";
    const int len = GetWindowTextLengthW(g_hBottomMathInput);
    std::wstring text(static_cast<size_t>(std::max(0, len)) + 1, L'\0');
    if (len > 0) {
        GetWindowTextW(g_hBottomMathInput, text.data(), static_cast<int>(text.size()));
        text.resize(static_cast<size_t>(len));
    } else {
        text.clear();
    }
    return text;
}

static void SetBottomMathHint(const std::wstring& text) {
    if (!g_hBottomMathHint) return;
    SetWindowTextW(g_hBottomMathHint, text.c_str());
}

static std::wstring DefaultBottomMathHint() {
    return IsEnglishUi()
        ? L"Type a MathBox body here. Ctrl+Enter adds it as LaTeX to the current PDF view center."
        : L"ここに MathBox の本文を入力します。Ctrl+Enter で LaTeX として現在表示中 PDF の中央へ追加します。";
}

static bool BottomMathAnalysisHasError(const note::MathInputAnalysis& analysis) {
    for (const auto& diag : analysis.diagnostics) {
        if (diag.severity == note::DiagnosticSeverity::Error) {
            return true;
        }
    }
    return false;
}

static std::wstring BottomMathValidationMessage(const note::MathInputAnalysis& analysis) {
    const note::Diagnostic* firstError = nullptr;
    for (const auto& diag : analysis.diagnostics) {
        if (diag.severity == note::DiagnosticSeverity::Error) {
            firstError = &diag;
            break;
        }
    }
    if (!firstError) {
        return DefaultBottomMathHint();
    }

    const bool english = IsEnglishUi();
    if (firstError->code == L"NOTE-E-MATHBOX-UNCLOSED-LEGACY-MATH") {
        return english
            ? L"Close the <math> ... </> wrapper before adding the MathBox."
            : L"<math> ... </> を閉じてから MathBox を追加してください。";
    }
    if (firstError->code == L"NOTE-E-MATHBOX-UNCLOSED-DOUBLE-DOLLAR") {
        return english
            ? L"Close $$...$$ before adding the MathBox."
            : L"$$...$$ を閉じてから MathBox を追加してください。";
    }
    if (firstError->code == L"NOTE-E-MATHBOX-UNCLOSED-BRACKET") {
        return english
            ? L"Close \\[...\\] before adding the MathBox."
            : L"\\[...\\] を閉じてから MathBox を追加してください。";
    }
    if (firstError->code == L"NOTE-E-MATHBOX-UNCLOSED-PAREN") {
        return english
            ? L"Close \\(...\\) before adding the MathBox."
            : L"\\(...\\) を閉じてから MathBox を追加してください。";
    }
    if (firstError->code == L"NOTE-E-MATHBOX-UNCLOSED-DOLLAR") {
        return english
            ? L"Close $...$ before adding the MathBox."
            : L"$...$ を閉じてから MathBox を追加してください。";
    }
    return firstError->message;
}

static bool ValidateBottomMathInput(const std::wstring& rawText, std::wstring* outMessage = nullptr) {
    note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(rawText);
    if (BottomMathAnalysisHasError(analysis)) {
        if (outMessage) {
            *outMessage = BottomMathValidationMessage(analysis);
        }
        return false;
    }
    if (outMessage) {
        *outMessage = DefaultBottomMathHint();
    }
    return true;
}

static void RefreshBottomMathHintFromInput() {
    std::wstring message;
    if (!ValidateBottomMathInput(ReadBottomMathInputText(), &message)) {
        SetBottomMathHint(message);
        return;
    }
    SetBottomMathHint(DefaultBottomMathHint());
}

static void ResetBottomMathPreviewDragState() {
    g_bottomMathPreviewTracking = false;
    g_bottomMathPreviewDragging = false;
}

static LRESULT CALLBACK BottomMathPreviewProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR idSubclass, DWORD_PTR refData) {
    (void)idSubclass;
    (void)refData;
    switch (msg) {
    case WM_NCDESTROY:
        if (GetCapture() == hWnd) {
            ReleaseCapture();
        }
        ResetBottomMathPreviewDragState();
        RemoveWindowSubclass(hWnd, BottomMathPreviewProc, 1);
        break;
    case WM_LBUTTONDOWN:
        if (g_hBottomMathInput) SetFocus(g_hBottomMathInput);
        if (BuildBottomMathPreviewDisplay(ReadBottomMathInputText(), nullptr, nullptr)) {
            g_bottomMathPreviewTracking = true;
            g_bottomMathPreviewDragging = false;
            g_bottomMathPreviewDragStart.x = GET_X_LPARAM(lParam);
            g_bottomMathPreviewDragStart.y = GET_Y_LPARAM(lParam);
            SetCapture(hWnd);
            SetBottomMathHint(IsEnglishUi() ? L"Drag the preview onto the PDF to place a MathBox."
                                            : L"プレビューを PDF へドラッグして MathBox を配置します。");
            return 0;
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_bottomMathPreviewTracking && GetCapture() == hWnd) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (!g_bottomMathPreviewDragging) {
                int dx = std::abs(pt.x - g_bottomMathPreviewDragStart.x);
                int dy = std::abs(pt.y - g_bottomMathPreviewDragStart.y);
                int dragX = std::max<int>(1, GetSystemMetrics(SM_CXDRAG));
                int dragY = std::max<int>(1, GetSystemMetrics(SM_CYDRAG));
                if (dx > dragX || dy > dragY) {
                    g_bottomMathPreviewDragging = true;
                }
            }
            if (g_bottomMathPreviewDragging) {
                POINT screenPt = pt;
                ClientToScreen(hWnd, &screenPt);
                bool overPdf = false;
                RECT pdfRc{};
                if (g_hPdfView && GetWindowRect(g_hPdfView, &pdfRc)) {
                    overPdf = PtInRect(&pdfRc, screenPt) != FALSE;
                }
                SetCursor(LoadCursorW(nullptr, overPdf ? IDC_CROSS : IDC_NO));
                SetBottomMathHint(overPdf
                                      ? (IsEnglishUi() ? L"Release to place the MathBox on the PDF."
                                                       : L"離すと PDF 上に MathBox を配置します。")
                                      : (IsEnglishUi() ? L"Drag onto a PDF page, then release."
                                                       : L"PDF ページ上までドラッグして離してください。"));
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_bottomMathPreviewTracking && GetCapture() == hWnd) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            POINT screenPt = pt;
            ClientToScreen(hWnd, &screenPt);
            const bool dragging = g_bottomMathPreviewDragging;
            ReleaseCapture();
            ResetBottomMathPreviewDragState();
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            if (!dragging) {
                SetBottomMathHint(DefaultBottomMathHint());
                return 0;
            }

            std::wstring rawText = TrimWhitespace(ReadBottomMathInputText());
            MathDisplay display;
            MathKind kind = MathKind::Latex;
            if (!BuildBottomMathPreviewDisplay(rawText, &display, &kind)) {
                SetBottomMathHint(IsEnglishUi() ? L"MathBox text is empty." : L"MathBox の文字列が空です。");
                return 0;
            }
            std::wstring validationMessage;
            if (!ValidateBottomMathInput(rawText, &validationMessage)) {
                SetBottomMathHint(validationMessage);
                return 0;
            }
            if (!g_hPdfView || !CurrentLogicalPdfDocument()) {
                SetBottomMathHint(IsEnglishUi() ? L"Open a PDF before dragging a MathBox."
                                                : L"MathBox をドラッグ配置する前に PDF を開いてください。");
                return 0;
            }
            if (!AddMathAnnotationFromTextAtPoint(g_hPdfView, rawText, kind, screenPt)) {
                SetBottomMathHint(IsEnglishUi() ? L"Drop on a visible PDF page to place the MathBox."
                                                : L"表示中の PDF ページ上で離すと MathBox を配置できます。");
                return 0;
            }
            SetBottomMathHint((kind == MathKind::Markup)
                                  ? (IsEnglishUi() ? L"Placed a Markup MathBox on the PDF."
                                                   : L"Markup の MathBox を PDF に配置しました。")
                                  : (IsEnglishUi() ? L"Placed a LaTeX MathBox on the PDF."
                                                   : L"LaTeX の MathBox を PDF に配置しました。"));
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        ResetBottomMathPreviewDragState();
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        COLORREF bg = BlendColor(g_theme.panelBg, g_theme.accent, 0.10);
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = nullptr;
        if (font) oldFont = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);

        const int margin = ScaleY(hWnd, 8);
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);

        RECT titleRc{
            static_cast<LONG>(margin),
            static_cast<LONG>(margin),
            static_cast<LONG>(std::max<LONG>(static_cast<LONG>(margin), rc.right - static_cast<LONG>(margin))),
            static_cast<LONG>(margin + tm.tmHeight)
        };
        SetTextColor(hdc, AdjustColorBrightness(g_theme.panelText, -35));
        DrawTextW(hdc, IsEnglishUi() ? L"Preview" : L"プレビュー", -1, &titleRc,
                  DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);

        MathDisplay display;
        MathKind kind = MathKind::Latex;
        std::wstring rawText = ReadBottomMathInputText();
        RECT bodyRc{
            static_cast<LONG>(margin),
            static_cast<LONG>(margin + tm.tmHeight + ScaleY(hWnd, 6)),
            static_cast<LONG>(std::max<LONG>(static_cast<LONG>(margin), rc.right - static_cast<LONG>(margin))),
            static_cast<LONG>(std::max<LONG>(static_cast<LONG>(margin), rc.bottom - static_cast<LONG>(margin)))
        };
        if (!BuildBottomMathPreviewDisplay(rawText, &display, &kind)) {
            SetTextColor(hdc, AdjustColorBrightness(g_theme.panelText, -55));
            DrawTextW(hdc,
                      IsEnglishUi() ? L"Type a formula to preview it here."
                                    : L"数式を入力するとここにプレビューします。",
                      -1, &bodyRc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        } else {
            std::wstring badge = (kind == MathKind::Markup)
                                     ? (IsEnglishUi() ? L"Markup" : L"Markup")
                                     : (IsEnglishUi() ? L"LaTeX" : L"LaTeX");
            RECT badgeRc = titleRc;
            SetTextColor(hdc, AdjustColorBrightness(g_theme.panelText, -15));
            DrawTextW(hdc, badge.c_str(), -1, &badgeRc, DT_RIGHT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);

            SetTextColor(hdc, g_theme.panelText);
            DrawMathDisplayNoWrap(display, hdc, bodyRc, std::max<int>(static_cast<int>(tm.tmHeight) + 2, ScaleY(hWnd, 18)),
                                  mathrender::RenderStyle::Display, 0, true);
        }

        if (oldFont) SelectObject(hdc, oldFont);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static bool SubmitBottomMathInput(HWND hWnd, MathKind kind) {
    const std::wstring rawText = TrimWhitespace(ReadBottomMathInputText());
    if (rawText.empty()) {
        SetBottomMathHint(IsEnglishUi() ? L"MathBox text is empty." : L"MathBox の文字列が空です。");
        if (g_hBottomMathInput) SetFocus(g_hBottomMathInput);
        return false;
    }
    std::wstring validationMessage;
    if (!ValidateBottomMathInput(rawText, &validationMessage)) {
        SetBottomMathHint(validationMessage);
        if (g_hBottomMathInput) SetFocus(g_hBottomMathInput);
        return false;
    }
    if (!g_hPdfView || !CurrentLogicalPdfDocument()) {
        SetBottomMathHint(IsEnglishUi() ? L"Open a PDF before adding a MathBox." : L"MathBox を追加する前に PDF を開いてください。");
        return false;
    }
    if (!AddMathAnnotationFromText(g_hPdfView, rawText, kind)) {
        SetBottomMathHint(IsEnglishUi() ? L"Failed to add MathBox to the current PDF view." : L"現在の PDF 表示へ MathBox を追加できませんでした。");
        return false;
    }
    SetBottomMathHint(
        (kind == MathKind::Markup)
            ? (IsEnglishUi() ? L"Added a Markup MathBox at the current view center."
                             : L"Markup の MathBox を現在表示の中央へ追加しました。")
            : (IsEnglishUi() ? L"Added a LaTeX MathBox at the current view center."
                             : L"LaTeX の MathBox を現在表示の中央へ追加しました。"));
    if (g_hBottomMathInput) {
        SetFocus(g_hBottomMathInput);
        SendMessageW(g_hBottomMathInput, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    }
    return true;
}

static LRESULT CALLBACK BottomMathInputEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR idSubclass, DWORD_PTR refData) {
    (void)idSubclass;
    (void)refData;
    if (msg == WM_KEYDOWN) {
        MSG edgeNavMsg{};
        edgeNavMsg.hwnd = hWnd;
        edgeNavMsg.message = msg;
        edgeNavMsg.wParam = wParam;
        edgeNavMsg.lParam = lParam;
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(edgeNavMsg)) return 0;

        g_bottomMathPendingCtrlInsertNavChar = 0;
        WPARAM navKey = 0;
        wchar_t suppressedChar = 0;
        if (ResolveCtrlInsertNavKey(hWnd, wParam, &navKey, &suppressedChar)) {
            g_bottomMathPendingCtrlInsertNavChar = suppressedChar;
            if (IsImeComposingOnEditWindow(hWnd)) {
                ForwardImeNavigationKey(hWnd, navKey);
                return 0;
            }
            MoveEditCaretForInsertNav(hWnd, navKey);
            return 0;
        }
    }
    if (msg == WM_CHAR && wParam == 27) {
        return 0;
    }
    if (msg == WM_CHAR && g_bottomMathPendingCtrlInsertNavChar != 0) {
        const wchar_t ch = static_cast<wchar_t>(wParam);
        const wchar_t suppressed = g_bottomMathPendingCtrlInsertNavChar;
        g_bottomMathPendingCtrlInsertNavChar = 0;
        if (ch == suppressed) {
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && wParam == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        HWND parent = GetParent(hWnd);
        if (parent) {
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(kBottomMathAddLatexId, BN_CLICKED), reinterpret_cast<LPARAM>(g_hBottomMathAddLatex));
        }
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK BottomMathProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hBottomMathInput = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 100, 100, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBottomMathInputId)), g_hInst, nullptr);
        g_hBottomMathPreview = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, 100, 100, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBottomMathPreviewId)), g_hInst, nullptr);
        g_hBottomMathAddLatex = CreateWindowExW(
            0, L"BUTTON", IsEnglishUi() ? L"Add LaTeX MathBox" : L"LaTeX で追加",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 100, 28, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBottomMathAddLatexId)), g_hInst, nullptr);
        g_hBottomMathAddMarkup = CreateWindowExW(
            0, L"BUTTON", IsEnglishUi() ? L"Add Markup MathBox" : L"Markup で追加",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 100, 28, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBottomMathAddMarkupId)), g_hInst, nullptr);
        g_hBottomMathHint = CreateWindowExW(
            0, L"STATIC", DefaultBottomMathHint().c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 100, 32, hWnd, nullptr, g_hInst, nullptr);
        SetUIFont(g_hBottomMathInput);
        SetUIFont(g_hBottomMathPreview);
        SetUIFont(g_hBottomMathAddLatex);
        SetUIFont(g_hBottomMathAddMarkup);
        SetUIFont(g_hBottomMathHint);
        if (g_hBottomMathInput) {
            SetWindowSubclass(g_hBottomMathInput, BottomMathInputEditProc, 1, 0);
        }
        if (g_hBottomMathPreview) {
            SetWindowSubclass(g_hBottomMathPreview, BottomMathPreviewProc, 1, 0);
        }
        LayoutBottomMathControls(hWnd);
        return 0;
    case WM_DESTROY:
        g_hBottomMathInput = nullptr;
        g_hBottomMathPreview = nullptr;
        g_hBottomMathAddLatex = nullptr;
        g_hBottomMathAddMarkup = nullptr;
        g_hBottomMathHint = nullptr;
        return 0;
    case WM_SIZE:
        LayoutBottomMathControls(hWnd);
        return 0;
    case WM_SETFOCUS:
        if (g_hBottomMathInput) {
            SetFocus(g_hBottomMathInput);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (g_hBottomMathInput) {
            SetFocus(g_hBottomMathInput);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == kBottomMathAddLatexId && HIWORD(wParam) == BN_CLICKED) {
            return SubmitBottomMathInput(hWnd, MathKind::Latex) ? 0 : 0;
        }
        if (LOWORD(wParam) == kBottomMathAddMarkupId && HIWORD(wParam) == BN_CLICKED) {
            return SubmitBottomMathInput(hWnd, MathKind::Markup) ? 0 : 0;
        }
        if (LOWORD(wParam) == kBottomMathInputId && HIWORD(wParam) == EN_CHANGE) {
            RefreshBottomMathHintFromInput();
            if (g_hBottomMathPreview) InvalidateRect(g_hBottomMathPreview, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        if (g_hThemePanelBrush) {
            FillRect(hdc, &rc, g_hThemePanelBrush);
        } else {
            HBRUSH br = CreateSolidBrush(g_theme.panelBg);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
