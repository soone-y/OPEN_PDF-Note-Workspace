#include "ui/dialogs/export_dialog.h"
#include "ui/dialogs/dialogs.h"
#include "ui/noop_nav_guard.h"
#include "workspace/workspace_config_io.h"

static std::wstring ExperimentalExportDialogTitle(const std::wstring& base) {
    if (base.find(L"試験的") != std::wstring::npos ||
        base.find(L"Experimental") != std::wstring::npos) {
        return base;
    }
    return IsEnglishUi() ? (base + L" (Experimental)") : (base + L"（試験的）");
}

static bool ParsePositiveInt(const std::wstring& text, int& out) {
    std::wstring t = TrimWhitespace(text);
    if (t.empty()) return false;
    int value = 0;
    for (wchar_t c : t) {
        if (c < L'0' || c > L'9') return false;
        value = value * 10 + (c - L'0');
    }
    if (value <= 0) return false;
    out = value;
    return true;
}
enum class ExportSizeMode { Half, One, Two, Custom };
static ExportSizeMode s_lastExportSizeMode = ExportSizeMode::One;
static double s_lastExportCustomPdfWPt = 0.0;
static double s_lastExportCustomPdfHPt = 0.0;
static int s_lastExportCustomPngWPx = 0;
static int s_lastExportCustomPngHPx = 0;
static std::vector<ExportDialogResult> s_pendingExportsAfterSave;
static int s_lastExportCustomAxis = 0; // 0=width, 1=height
static int s_lastExportPaperPresetId = 0;
static bool s_lastNoteStripMarkup = true;
static bool s_lastNoteTitleHeading = false;
static bool s_lastNoteShiftHeadings = true;
static bool s_lastNoteIncludeComments = true;
static int s_lastNoteMarkupFormat = 0; // 0=md, 1=html
static bool s_lastNoteMarkupMathPlaceholder = false;
static std::wstring s_lastNoteMarkupMathPlaceholderText = L"[math]";

struct ExportDialogState {
    HWND hwnd{};
    bool ok = false;
    bool done = false;
    bool hasPdf = false;
    bool hasNote = false;
    bool updatingSize = false;
    ExportDialogKind preset = ExportDialogKind::PdfAll;
    std::optional<ExportDialogResult> reservedPdf;
    std::optional<ExportDialogResult> reservedNote;
    std::vector<ExportDialogResult> committedResults;

    HWND topPdf{};
    HWND topNote{};
    HWND pdfAll{};
    HWND pdfPages{};
    HWND pdfPng{};
    HWND noteText{};
    HWND noteMarkup{};
    HWND labelFileExample{};
    HWND labelPageSpec{};
    HWND editPageSpec{};
    HWND labelPageExample{};
    HWND labelPageNumber{};
    HWND editPageNumber{};
    HWND labelAnnot{};
    HWND radioAnnotYes{};
    HWND radioAnnotNo{};
    HWND labelPngStyle{};
    HWND radioPngStylePdf{};
    HWND radioPngStyleViewer{};
    HWND labelOutSize{};
    HWND radioOutSizeHalf{};
    HWND radioOutSizeOne{};
    HWND radioOutSizeTwo{};
    HWND radioOutSizeCustom{};
    HWND labelOutSizeW{};
    HWND editOutSizeW{};
    HWND labelOutSizeH{};
    HWND editOutSizeH{};
    HWND labelOutSizeMm{};
    HWND labelPaper{};
    HWND comboPaper{};
    HWND checkStandardText{};
    HWND checkMatchPdfPaneTextLayout{};
    HWND labelMath{};
    HWND radioMathKeep{};
    HWND radioMathReplace{};
    HWND labelMathPlaceholder{};
    HWND editMathPlaceholder{};
    HWND checkIncludeComments{};
    HWND checkStripMarkup{};
    HWND labelMarkupFormat{};
    HWND radioMarkupMd{};
    HWND radioMarkupHtml{};
    HWND checkTitleHeading{};
    HWND checkShiftHeadings{};
    HWND btnSet{};
    HWND labelInlineError{};
    HWND labelReservationTitle{};
    HWND labelReservationList{};
    HWND inlineErrorTarget{};
};

constexpr int kExportDlgIdTopPdf = 4001;
constexpr int kExportDlgIdTopNote = 4002;
constexpr int kExportDlgIdPdfAll = 4011;
constexpr int kExportDlgIdPdfPages = 4012;
constexpr int kExportDlgIdPdfPng = 4013;
constexpr int kExportDlgIdNoteText = 4021;
constexpr int kExportDlgIdNoteMarkup = 4022;
constexpr int kExportDlgIdPageSpec = 4031;
constexpr int kExportDlgIdPageNumber = 4032;
constexpr int kExportDlgIdAnnotYes = 4041;
constexpr int kExportDlgIdAnnotNo = 4042;
constexpr int kExportDlgIdMathKeep = 4051;
constexpr int kExportDlgIdMathReplace = 4052;
constexpr int kExportDlgIdMathPlaceholder = 4053;
constexpr int kExportDlgIdStripMarkup = 4054;
constexpr int kExportDlgIdMarkupMd = 4055;
constexpr int kExportDlgIdMarkupHtml = 4056;
constexpr int kExportDlgIdTitleHeading = 4057;
constexpr int kExportDlgIdShiftHeadings = 4058;
constexpr int kExportDlgIdIncludeComments = 4059;
constexpr int kExportDlgIdPngStyleLabel = 4061;
constexpr int kExportDlgIdPngStylePdf = 4062;
constexpr int kExportDlgIdPngStyleViewer = 4063;
constexpr int kExportDlgIdStandardText = 4071;
constexpr int kExportDlgIdMatchPdfPaneTextLayout = 4072;
constexpr int kExportDlgIdOutSizeHalf = 4081;
constexpr int kExportDlgIdOutSizeOne = 4082;
constexpr int kExportDlgIdOutSizeTwo = 4083;
constexpr int kExportDlgIdOutSizeCustom = 4084;
constexpr int kExportDlgIdOutSizeW = 4091;
constexpr int kExportDlgIdOutSizeH = 4092;
constexpr int kExportDlgIdPaperCombo = 4101;
constexpr int kExportDlgIdSet = 4201;

static bool IsExportDlgChecked(HWND hWnd) {
    return hWnd && SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static std::wstring ReadDialogText(HWND hWnd) {
    if (!hWnd) return L"";
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) return L"";
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hWnd, text.data(), len + 1);
    if (copied < 0) copied = 0;
    text.resize(static_cast<size_t>(copied));
    return text;
}

static ExportDialogKind GetExportDialogKind(ExportDialogState* ctx);
static bool BuildExportDialogResult(ExportDialogState* ctx, ExportDialogResult& outResult);
static void UpdateReservationSummaryUi(ExportDialogState* ctx);

static COLORREF BlendExportDialogColor(COLORREF a, COLORREF b, double t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = static_cast<int>(std::lround(ar + (br - ar) * t));
    int g = static_cast<int>(std::lround(ag + (bg - ag) * t));
    int b2 = static_cast<int>(std::lround(ab + (bb - ab) * t));
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b2, 0, 255));
}

static COLORREF ExportDialogErrorTextColor() {
    return BlendExportDialogColor(g_theme.panelText, RGB(198, 58, 58), 0.78);
}

static void ClearExportDialogInlineError(ExportDialogState* ctx) {
    if (!ctx) return;
    ctx->inlineErrorTarget = nullptr;
    if (!ctx->labelInlineError) return;
    SetWindowTextW(ctx->labelInlineError, L"");
    ShowWindow(ctx->labelInlineError, SW_HIDE);
}

static void FocusExportDialogControl(HWND hWnd, bool selectAll) {
    if (!hWnd) return;
    SetFocus(hWnd);
    if (selectAll) {
        SendMessageW(hWnd, EM_SETSEL, 0, -1);
    }
}

static bool RejectExportDialogInput(ExportDialogState* ctx,
                                    const std::wstring& message,
                                    HWND focusCtrl = nullptr,
                                    bool selectAll = false) {
    if (!ctx) return false;
    ctx->inlineErrorTarget = focusCtrl;
    if (ctx->labelInlineError) {
        SetWindowTextW(ctx->labelInlineError, message.c_str());
        ShowWindow(ctx->labelInlineError, SW_SHOW);
        InvalidateRect(ctx->labelInlineError, nullptr, TRUE);
    }
    if (focusCtrl) {
        FocusExportDialogControl(focusCtrl, selectAll);
    } else {
        ShowSoftNotice(ctx->hwnd, message, SoftNoticeKind::Warning);
    }
    return false;
}

static int ExportDialogEnterCommand(const ExportDialogState* ctx) {
    if (!ctx) return IDOK;
    // When a reservation already exists, Enter should update/store the current side
    // rather than closing the dialog and exporting only the stored items.
    if (ctx->reservedPdf.has_value() || ctx->reservedNote.has_value()) {
        return kExportDlgIdSet;
    }
    return IDOK;
}

static LRESULT CALLBACK ExportDialogEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR idSubclass, DWORD_PTR refData) {
    auto* ctx = reinterpret_cast<ExportDialogState*>(refData);
    if (msg == WM_KEYDOWN) {
        MSG edgeNavMsg{};
        edgeNavMsg.hwnd = hWnd;
        edgeNavMsg.message = msg;
        edgeNavMsg.wParam = wParam;
        edgeNavMsg.lParam = lParam;
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(edgeNavMsg)) return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        HWND parent = GetParent(hWnd);
        if (parent && ctx) {
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(ExportDialogEnterCommand(ctx), BN_CLICKED), 0);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        HWND parent = GetParent(hWnd);
        if (parent && ctx) {
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        }
        return 0;
    }
    if (msg == WM_CHAR && (wParam == L'\r' || wParam == 27)) {
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static std::optional<int> ReadPositiveIntFromEdit(HWND hWnd) {
    std::wstring s = TrimWhitespace(ReadDialogText(hWnd));
    if (s.empty()) return std::nullopt;
    int v = 0;
    if (!ParsePositiveInt(s, v)) return std::nullopt;
    return v;
}

static void SetDialogInt(HWND hWnd, int v) {
    if (!hWnd) return;
    SetWindowTextW(hWnd, std::to_wstring(v).c_str());
}

static std::optional<double> ReadPositiveDoubleFromEdit(HWND hWnd) {
    std::wstring s = TrimWhitespace(ReadDialogText(hWnd));
    if (s.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    double val = std::wcstod(s.c_str(), &end);
    if (end == s.c_str()) return std::nullopt;
    while (end && *end && std::iswspace(*end)) ++end;
    if (end && *end) return std::nullopt;
    if (!std::isfinite(val) || val <= 0.0) return std::nullopt;
    return val;
}

static std::wstring FormatDoubleCompact(double v, int decimals) {
    if (!std::isfinite(v)) return L"";
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.*f", decimals, v);
    std::wstring s(buf);
    size_t dot = s.find(L'.');
    if (dot != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    return s;
}

static void SetDialogDouble(HWND hWnd, double v, int decimals = 1) {
    if (!hWnd) return;
    std::wstring s = FormatDoubleCompact(v, decimals);
    if (s.empty()) s = L"0";
    SetWindowTextW(hWnd, s.c_str());
}

static constexpr double kPtPerInch = 72.0;
static constexpr double kMmPerInch = 25.4;

static double PtToMm(double pt) {
    return pt * kMmPerInch / kPtPerInch;
}

static double MmToPt(double mm) {
    return mm * kPtPerInch / kMmPerInch;
}

struct PaperPreset {
    int id = 0;
    const wchar_t* label = L"";
    double wMm = 0.0;
    double hMm = 0.0;
};

static constexpr PaperPreset kPaperPresets[] = {
    { 101, L"A4 縦", 210.0, 297.0 },
    { 102, L"A4 横", 297.0, 210.0 },
    { 111, L"A5 縦", 148.0, 210.0 },
    { 112, L"A5 横", 210.0, 148.0 },
    { 121, L"B5 縦", 176.0, 250.0 },
    { 122, L"B5 横", 250.0, 176.0 },
    { 131, L"Letter 縦", 215.9, 279.4 },
    { 132, L"Letter 横", 279.4, 215.9 },
};

static bool LookupPaperPresetMm(int id, double& outWmm, double& outHmm) {
    for (const auto& p : kPaperPresets) {
        if (p.id != id) continue;
        outWmm = p.wMm;
        outHmm = p.hMm;
        return true;
    }
    return false;
}

static bool RatioClose(double a, double b, double relTol = 0.01) {
    if (!std::isfinite(a) || !std::isfinite(b) || a <= 0.0 || b <= 0.0) return false;
    double diff = std::abs(a - b);
    double base = std::max(a, b);
    return (diff / base) <= relTol;
}

static int GetComboItemData(HWND combo) {
    if (!combo) return 0;
    int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel == CB_ERR) return 0;
    return static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, sel, 0));
}

static void PopulatePaperPresetCombo(ExportDialogState* ctx, double pageRatio) {
    if (!ctx || !ctx->comboPaper) return;

    SendMessageW(ctx->comboPaper, CB_RESETCONTENT, 0, 0);
    int idxNone = static_cast<int>(SendMessageW(ctx->comboPaper, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"なし")));
    SendMessageW(ctx->comboPaper, CB_SETITEMDATA, idxNone, 0);

    for (const auto& p : kPaperPresets) {
        double r = p.wMm / p.hMm;
        if (!RatioClose(pageRatio, r, 0.01)) continue;
        int idx = static_cast<int>(SendMessageW(ctx->comboPaper, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.label)));
        if (idx != CB_ERR) {
            SendMessageW(ctx->comboPaper, CB_SETITEMDATA, idx, p.id);
        }
    }

    int best = 0;
    int count = static_cast<int>(SendMessageW(ctx->comboPaper, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        int id = static_cast<int>(SendMessageW(ctx->comboPaper, CB_GETITEMDATA, i, 0));
        if (id == s_lastExportPaperPresetId) { best = i; break; }
    }
    SendMessageW(ctx->comboPaper, CB_SETCURSEL, best, 0);
    s_lastExportPaperPresetId = GetComboItemData(ctx->comboPaper);
}

static bool TryGetPageSizePt(int pageIndex, double& outWPt, double& outHPt) {
    outWPt = 0.0;
    outHPt = 0.0;
    if (!g_pdf.doc) return false;
    if (pageIndex >= 0 && pageIndex < static_cast<int>(g_pdf.pages.size())) {
        const auto& p = g_pdf.pages[static_cast<size_t>(pageIndex)];
        if (p.widthPt > 0.0 && p.heightPt > 0.0) {
            outWPt = p.widthPt;
            outHPt = p.heightPt;
            return true;
        }
    }
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        if (FPDF_GetPageSizeByIndex(g_pdf.doc, pageIndex, &outWPt, &outHPt)) {
            return outWPt > 0.0 && outHPt > 0.0;
        }
    }
    return false;
}

static int ReferencePageIndexForSizePreview(ExportDialogState* ctx, ExportDialogKind kind) {
    if (!ctx || !g_pdf.doc) return 0;
    int count = 0;
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        count = FPDF_GetPageCount(g_pdf.doc);
    }
    if (count <= 0) return 0;
    auto clampIndex = [&](int i) { return std::clamp(i, 0, std::max(0, count - 1)); };
    if (kind == ExportDialogKind::PdfPng) {
        auto v = ReadPositiveIntFromEdit(ctx->editPageNumber);
        if (v && *v >= 1) return clampIndex(*v - 1);
        return 0;
    }
    if (kind == ExportDialogKind::PdfPages) {
        std::wstring spec = TrimWhitespace(ReadDialogText(ctx->editPageSpec));
        std::wstring err;
        bool defaultAnnot = IsExportDlgChecked(ctx->radioAnnotYes);
        auto pages = file_output::ParsePdfPageSpec(spec, defaultAnnot, &err);
        if (!pages.empty()) return clampIndex(pages.front().pageIndex);
        return 0;
    }
    return 0;
}

static ExportSizeMode GetExportSizeMode(ExportDialogState* ctx) {
    if (!ctx) return ExportSizeMode::One;
    if (IsExportDlgChecked(ctx->radioOutSizeHalf)) return ExportSizeMode::Half;
    if (IsExportDlgChecked(ctx->radioOutSizeTwo)) return ExportSizeMode::Two;
    if (IsExportDlgChecked(ctx->radioOutSizeCustom)) return ExportSizeMode::Custom;
    return ExportSizeMode::One;
}

static void UpdateExportDialogSizeUi(ExportDialogState* ctx, int changedId) {
    if (!ctx) return;
    ExportDialogKind kind = GetExportDialogKind(ctx);
    bool showOutSize = (kind == ExportDialogKind::PdfAll || kind == ExportDialogKind::PdfPages || kind == ExportDialogKind::PdfPng);
    if (!showOutSize) return;

    int refPage = ReferencePageIndexForSizePreview(ctx, kind);
    double wPt = 0.0, hPt = 0.0;
    if (!TryGetPageSizePt(refPage, wPt, hPt)) return;

    const bool isPdfOut = (kind == ExportDialogKind::PdfAll || kind == ExportDialogKind::PdfPages);
    const bool isPngOut = (kind == ExportDialogKind::PdfPng);

    if (ctx->labelOutSizeW) SetWindowTextW(ctx->labelOutSizeW, isPdfOut ? L"横(pt):" : L"横(px):");
    if (ctx->labelOutSizeH) SetWindowTextW(ctx->labelOutSizeH, isPdfOut ? L"縦(pt):" : L"縦(px):");

    if (isPdfOut) {
        double baseWPt = wPt;
        double baseHPt = hPt;
        double ratio = (baseHPt > 0.0) ? (baseWPt / baseHPt) : 0.0;
        if (changedId == 0 || changedId == kExportDlgIdPageSpec || changedId == kExportDlgIdPageNumber) {
            PopulatePaperPresetCombo(ctx, ratio);
        }

        ExportSizeMode mode = GetExportSizeMode(ctx);
        bool custom = (mode == ExportSizeMode::Custom);
        EnableWindow(ctx->editOutSizeW, custom ? TRUE : FALSE);
        EnableWindow(ctx->editOutSizeH, custom ? TRUE : FALSE);

        double outWPt = 0.0, outHPt = 0.0;
        double scale = 1.0;
        if (mode == ExportSizeMode::Half) scale = 0.5;
        else if (mode == ExportSizeMode::Two) scale = 2.0;

        if (!custom) {
            outWPt = baseWPt * scale;
            outHPt = baseHPt * scale;
            ctx->updatingSize = true;
            SetDialogDouble(ctx->editOutSizeW, outWPt, 1);
            SetDialogDouble(ctx->editOutSizeH, outHPt, 1);
            ctx->updatingSize = false;
        } else {
            if (changedId == kExportDlgIdOutSizeW) s_lastExportCustomAxis = 0;
            if (changedId == kExportDlgIdOutSizeH) s_lastExportCustomAxis = 1;

            double inW = ReadPositiveDoubleFromEdit(ctx->editOutSizeW).value_or(0.0);
            double inH = ReadPositiveDoubleFromEdit(ctx->editOutSizeH).value_or(0.0);
            if (inW <= 0.0 && inH <= 0.0) {
                if (s_lastExportCustomPdfWPt > 0.0) inW = s_lastExportCustomPdfWPt;
                if (s_lastExportCustomPdfHPt > 0.0) inH = s_lastExportCustomPdfHPt;
                if (inW <= 0.0 && inH <= 0.0) {
                    inW = baseWPt;
                    inH = baseHPt;
                }
            }

            if (s_lastExportCustomAxis == 0) {
                if (inW <= 0.0) inW = baseWPt * (inH / std::max(0.01, baseHPt));
                scale = inW / std::max(0.01, baseWPt);
            } else {
                if (inH <= 0.0) inH = baseHPt * (inW / std::max(0.01, baseWPt));
                scale = inH / std::max(0.01, baseHPt);
            }
            outWPt = baseWPt * scale;
            outHPt = baseHPt * scale;

            s_lastExportCustomPdfWPt = outWPt;
            s_lastExportCustomPdfHPt = outHPt;

            ctx->updatingSize = true;
            SetDialogDouble(ctx->editOutSizeW, outWPt, 1);
            SetDialogDouble(ctx->editOutSizeH, outHPt, 1);
            ctx->updatingSize = false;
        }

        if (ctx->labelOutSizeMm) {
            double baseWmm = PtToMm(baseWPt);
            double baseHmm = PtToMm(baseHPt);
            double outWmm = PtToMm(outWPt > 0.0 ? outWPt : baseWPt);
            double outHmm = PtToMm(outHPt > 0.0 ? outHPt : baseHPt);
            std::wstring msg = L"推奨(元): ≒ " + FormatDoubleCompact(baseWmm, 1) + L"×" + FormatDoubleCompact(baseHmm, 1) +
                               L" mm / 出力: ≒ " + FormatDoubleCompact(outWmm, 1) + L"×" + FormatDoubleCompact(outHmm, 1) + L" mm";
            SetWindowTextW(ctx->labelOutSizeMm, msg.c_str());
        }
        return;
    }

    if (isPngOut) {
        constexpr double kBaseDpi = 144.0;
        int baseW = std::max(1, static_cast<int>(std::lround(wPt * kBaseDpi / 72.0)));
        int baseH = std::max(1, static_cast<int>(std::lround(hPt * kBaseDpi / 72.0)));

        ExportSizeMode mode = GetExportSizeMode(ctx);
        bool custom = (mode == ExportSizeMode::Custom);
        EnableWindow(ctx->editOutSizeW, custom ? TRUE : FALSE);
        EnableWindow(ctx->editOutSizeH, custom ? TRUE : FALSE);

        int outW = 0, outH = 0;
        double scale = 1.0;
        if (mode == ExportSizeMode::Half) scale = 0.5;
        else if (mode == ExportSizeMode::Two) scale = 2.0;

        if (!custom) {
            outW = std::max(1, static_cast<int>(std::lround(baseW * scale)));
            outH = std::max(1, static_cast<int>(std::lround(baseH * scale)));
            ctx->updatingSize = true;
            SetDialogInt(ctx->editOutSizeW, outW);
            SetDialogInt(ctx->editOutSizeH, outH);
            ctx->updatingSize = false;
        } else {
            if (changedId == kExportDlgIdOutSizeW) s_lastExportCustomAxis = 0;
            if (changedId == kExportDlgIdOutSizeH) s_lastExportCustomAxis = 1;

            int wPx = ReadPositiveIntFromEdit(ctx->editOutSizeW).value_or(0);
            int hPx = ReadPositiveIntFromEdit(ctx->editOutSizeH).value_or(0);
            if (wPx <= 0 && hPx <= 0) {
                if (s_lastExportCustomPngWPx > 0) wPx = s_lastExportCustomPngWPx;
                if (s_lastExportCustomPngHPx > 0) hPx = s_lastExportCustomPngHPx;
                if (wPx <= 0 && hPx <= 0) {
                    wPx = baseW;
                    hPx = baseH;
                }
            }

            if (s_lastExportCustomAxis == 0) {
                if (wPx <= 0) wPx = std::max(1, static_cast<int>(std::lround(baseW * (static_cast<double>(hPx) / baseH))));
                scale = static_cast<double>(wPx) / baseW;
                outW = wPx;
                outH = std::max(1, static_cast<int>(std::lround(baseH * scale)));
            } else {
                if (hPx <= 0) hPx = std::max(1, static_cast<int>(std::lround(baseH * (static_cast<double>(wPx) / baseW))));
                scale = static_cast<double>(hPx) / baseH;
                outH = hPx;
                outW = std::max(1, static_cast<int>(std::lround(baseW * scale)));
            }

            s_lastExportCustomPngWPx = outW;
            s_lastExportCustomPngHPx = outH;

            ctx->updatingSize = true;
            SetDialogInt(ctx->editOutSizeW, outW);
            SetDialogInt(ctx->editOutSizeH, outH);
            ctx->updatingSize = false;
        }

        if (ctx->labelOutSizeMm) {
            SetWindowTextW(ctx->labelOutSizeMm, L"");
        }
    }
}

static ExportDialogKind GetExportDialogKind(ExportDialogState* ctx) {
    if (!ctx) return ExportDialogKind::PdfAll;
    if (IsExportDlgChecked(ctx->topPdf)) {
        if (IsExportDlgChecked(ctx->pdfPages)) return ExportDialogKind::PdfPages;
        if (IsExportDlgChecked(ctx->pdfPng)) return ExportDialogKind::PdfPng;
        return ExportDialogKind::PdfAll;
    }
    if (IsExportDlgChecked(ctx->noteMarkup)) return ExportDialogKind::NoteMarkup;
    return ExportDialogKind::NoteText;
}

static const wchar_t* FileExampleForKind(ExportDialogKind kind) {
    switch (kind) {
    case ExportDialogKind::PdfPages:
        return L"ファイル例: lecture_pages.pdf";
    case ExportDialogKind::PdfPng:
        return L"ファイル例: page_001.png";
    case ExportDialogKind::NoteText:
        return L"ファイル例: note.txt";
    case ExportDialogKind::NoteMarkup:
        return L"ファイル例: note.md / note.html";
    case ExportDialogKind::PdfAll:
    default:
        return L"ファイル例: lecture_annotated.pdf";
    }
}

static bool IsPdfExportKind(ExportDialogKind kind) {
    return kind == ExportDialogKind::PdfAll ||
           kind == ExportDialogKind::PdfPages ||
           kind == ExportDialogKind::PdfPng;
}

static bool IsNoteExportKind(ExportDialogKind kind) {
    return kind == ExportDialogKind::NoteText ||
           kind == ExportDialogKind::NoteMarkup;
}

static std::wstring ReservationLabelForResult(const ExportDialogResult& result) {
    switch (result.kind) {
    case ExportDialogKind::PdfAll:
        return L"注釈PDF";
    case ExportDialogKind::PdfPages:
        return L"ページ指定PDF";
    case ExportDialogKind::PdfPng:
        return L"単ページPNG";
    case ExportDialogKind::NoteText:
        return (result.textOptions.markupMode == file_output::MarkupMode::Simplified) ? L"txt (マークアップ除去)" : L"txt";
    case ExportDialogKind::NoteMarkup:
        return (result.noteMarkupOptions.format == file_output::NoteMarkupExportOptions::Format::Html)
                   ? L"マークアップ (html)"
                   : L"マークアップ (md)";
    default:
        return L"(不明)";
    }
}

static void UpdateReservationSummaryUi(ExportDialogState* ctx) {
    if (!ctx || !ctx->labelReservationList) return;
    std::wstring pdf = L"PDF: 未設定";
    std::wstring note = L"ノート: 未設定";
    if (ctx->reservedPdf.has_value()) {
        pdf = L"PDF: " + ReservationLabelForResult(*ctx->reservedPdf);
    }
    if (ctx->reservedNote.has_value()) {
        note = L"ノート: " + ReservationLabelForResult(*ctx->reservedNote);
    }
    std::wstring text = pdf + L"\r\n" + note;
    SetWindowTextW(ctx->labelReservationList, text.c_str());
}

static void UpdateExportDialogUi(ExportDialogState* ctx) {
    if (!ctx) return;

    if (!ctx->hasPdf) {
        EnableWindow(ctx->topPdf, FALSE);
    }
    if (!ctx->hasNote) {
        EnableWindow(ctx->topNote, FALSE);
    }
    if (!ctx->hasPdf && ctx->hasNote) {
        CheckRadioButton(ctx->hwnd, kExportDlgIdTopPdf, kExportDlgIdTopNote, kExportDlgIdTopNote);
    } else if (ctx->hasPdf && !ctx->hasNote) {
        CheckRadioButton(ctx->hwnd, kExportDlgIdTopPdf, kExportDlgIdTopNote, kExportDlgIdTopPdf);
    }

    bool isPdf = IsExportDlgChecked(ctx->topPdf);
    ShowWindow(ctx->pdfAll, isPdf ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->pdfPages, isPdf ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->pdfPng, isPdf ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->noteText, isPdf ? SW_HIDE : SW_SHOW);
    ShowWindow(ctx->noteMarkup, isPdf ? SW_HIDE : SW_SHOW);

    if (isPdf) {
        if (!IsExportDlgChecked(ctx->pdfAll) &&
            !IsExportDlgChecked(ctx->pdfPages) &&
            !IsExportDlgChecked(ctx->pdfPng)) {
            CheckRadioButton(ctx->hwnd, kExportDlgIdPdfAll, kExportDlgIdPdfPng, kExportDlgIdPdfAll);
        }
    } else {
        if (!IsExportDlgChecked(ctx->noteText) &&
            !IsExportDlgChecked(ctx->noteMarkup)) {
            CheckRadioButton(ctx->hwnd, kExportDlgIdNoteText, kExportDlgIdNoteMarkup, kExportDlgIdNoteText);
        }
    }

    ExportDialogKind kind = GetExportDialogKind(ctx);
    bool showPageSpec = (kind == ExportDialogKind::PdfPages);
    bool showPageNumber = (kind == ExportDialogKind::PdfPng);
    bool showAnnots = (kind == ExportDialogKind::PdfPages || kind == ExportDialogKind::PdfPng);
    bool showMath = (kind == ExportDialogKind::NoteText || kind == ExportDialogKind::NoteMarkup);
    bool showPlaceholder = showMath && IsExportDlgChecked(ctx->radioMathReplace);
    bool showIncludeComments = (kind == ExportDialogKind::NoteText ||
                                kind == ExportDialogKind::NoteMarkup);
    bool showStripMarkup = (kind == ExportDialogKind::NoteText);
    bool showFormat = (kind == ExportDialogKind::NoteMarkup);
    bool showTitle = (kind == ExportDialogKind::NoteMarkup);
    bool showShiftHeadings = (kind == ExportDialogKind::NoteMarkup) && IsExportDlgChecked(ctx->checkTitleHeading);
    bool showStandardText = (kind == ExportDialogKind::PdfAll || kind == ExportDialogKind::PdfPages);
    bool showOutSize = (kind == ExportDialogKind::PdfAll || kind == ExportDialogKind::PdfPages || kind == ExportDialogKind::PdfPng);
    bool showPaper = (kind == ExportDialogKind::PdfAll || kind == ExportDialogKind::PdfPages);

    ShowWindow(ctx->labelPageSpec, showPageSpec ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->editPageSpec, showPageSpec ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelPageNumber, showPageNumber ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->editPageNumber, showPageNumber ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelPageExample, (showPageSpec || showPageNumber) ? SW_SHOW : SW_HIDE);

    ShowWindow(ctx->labelAnnot, showAnnots ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioAnnotYes, showAnnots ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioAnnotNo, showAnnots ? SW_SHOW : SW_HIDE);
    bool showPngStyle = (kind == ExportDialogKind::PdfPng);
    ShowWindow(ctx->labelPngStyle, showPngStyle ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioPngStylePdf, showPngStyle ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioPngStyleViewer, showPngStyle ? SW_SHOW : SW_HIDE);

    ShowWindow(ctx->labelOutSize, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioOutSizeHalf, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioOutSizeOne, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioOutSizeTwo, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioOutSizeCustom, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelOutSizeW, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->editOutSizeW, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelOutSizeH, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->editOutSizeH, showOutSize ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelOutSizeMm, showPaper ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelPaper, showPaper ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->comboPaper, showPaper ? SW_SHOW : SW_HIDE);

    ShowWindow(ctx->checkStandardText, showStandardText ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->checkMatchPdfPaneTextLayout, showStandardText ? SW_SHOW : SW_HIDE);

    ShowWindow(ctx->labelMath, showMath ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioMathKeep, showMath ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioMathReplace, showMath ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelMathPlaceholder, showPlaceholder ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->editMathPlaceholder, showPlaceholder ? SW_SHOW : SW_HIDE);

    ShowWindow(ctx->checkIncludeComments, showIncludeComments ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->checkStripMarkup, showStripMarkup ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->labelMarkupFormat, showFormat ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioMarkupMd, showFormat ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->radioMarkupHtml, showFormat ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->checkTitleHeading, showTitle ? SW_SHOW : SW_HIDE);
    ShowWindow(ctx->checkShiftHeadings, showShiftHeadings ? SW_SHOW : SW_HIDE);

    if (ctx->radioMathKeep) {
        SetWindowTextW(ctx->radioMathKeep, (kind == ExportDialogKind::NoteMarkup) ? L"形式に当てはめる" : L"そのまま");
    }

    SetWindowTextW(ctx->labelFileExample, FileExampleForKind(kind));
    if (showPageSpec) {
        SetWindowTextW(ctx->labelPageExample, L"ページ指定例: 1,2,6 / 4a,1a,1 / 1[0:0:360:540]");
    } else if (showPageNumber) {
        SetWindowTextW(ctx->labelPageExample, L"ページ番号例: 3");
    }

    UpdateExportDialogSizeUi(ctx, /*changedId=*/0);

    InvalidateRect(ctx->hwnd, nullptr, TRUE);
    UpdateWindow(ctx->hwnd);
}

static bool BuildExportDialogResult(ExportDialogState* ctx, ExportDialogResult& outResult) {
    if (!ctx) return false;
    ClearExportDialogInlineError(ctx);
    ExportDialogKind kind = GetExportDialogKind(ctx);
    if (IsPdfExportKind(kind) && !ctx->hasPdf) {
        return RejectExportDialogInput(ctx, L"PDFが開かれていません。");
    }
    if (IsNoteExportKind(kind) && !ctx->hasNote) {
        return RejectExportDialogInput(ctx, L"ノートが開かれていません。");
    }

    ExportDialogResult result{};
    result.kind = kind;
    result.standardTextAnnots = IsExportDlgChecked(ctx->checkStandardText);
    result.matchPdfPaneTextLayout = IsExportDlgChecked(ctx->checkMatchPdfPaneTextLayout);
    result.pdfScale = 1.0;
    result.pngWidthPx = 0;
    result.pngHeightPx = 0;
    if (kind == ExportDialogKind::PdfAll || kind == ExportDialogKind::PdfPages) {
        int refPage = ReferencePageIndexForSizePreview(ctx, kind);
        double baseWPt = 0.0, baseHPt = 0.0;
        if (!TryGetPageSizePt(refPage, baseWPt, baseHPt)) {
            return RejectExportDialogInput(ctx, L"出力サイズの計算に失敗しました。");
        }
        ExportSizeMode mode = GetExportSizeMode(ctx);
        s_lastExportSizeMode = mode;
        double scale = 1.0;
        if (mode == ExportSizeMode::Half) scale = 0.5;
        else if (mode == ExportSizeMode::Two) scale = 2.0;
        else if (mode == ExportSizeMode::Custom) {
            double inWPt = ReadPositiveDoubleFromEdit(ctx->editOutSizeW).value_or(0.0);
            double inHPt = ReadPositiveDoubleFromEdit(ctx->editOutSizeH).value_or(0.0);
            if (inWPt <= 0.0 && inHPt <= 0.0) {
                return RejectExportDialogInput(ctx, L"出力サイズ(pt)を入力してください。", ctx->editOutSizeW, true);
            }
            if (s_lastExportCustomAxis == 0) {
                if (inWPt <= 0.0) inWPt = baseWPt * (inHPt / std::max(0.01, baseHPt));
                scale = inWPt / std::max(0.01, baseWPt);
            } else {
                if (inHPt <= 0.0) inHPt = baseHPt * (inWPt / std::max(0.01, baseWPt));
                scale = inHPt / std::max(0.01, baseHPt);
            }
        }
        if (!std::isfinite(scale) || scale < 0.125 || scale > 8.0) {
            return RejectExportDialogInput(ctx, L"出力サイズが範囲外です。", ctx->editOutSizeW, true);
        }
        result.pdfScale = scale;
    } else if (kind == ExportDialogKind::PdfPng) {
        constexpr double kBaseDpi = 144.0;
        int refPage = ReferencePageIndexForSizePreview(ctx, kind);
        double wPt = 0.0, hPt = 0.0;
        if (!TryGetPageSizePt(refPage, wPt, hPt)) {
            return RejectExportDialogInput(ctx, L"出力サイズの計算に失敗しました。");
        }
        int baseW = std::max(1, static_cast<int>(std::lround(wPt * kBaseDpi / 72.0)));
        int baseH = std::max(1, static_cast<int>(std::lround(hPt * kBaseDpi / 72.0)));

        ExportSizeMode mode = GetExportSizeMode(ctx);
        s_lastExportSizeMode = mode;
        double scale = 1.0;
        if (mode == ExportSizeMode::Half) scale = 0.5;
        else if (mode == ExportSizeMode::Two) scale = 2.0;

        int outW = baseW;
        int outH = baseH;
        if (mode != ExportSizeMode::Custom) {
            outW = std::max(1, static_cast<int>(std::lround(baseW * scale)));
            outH = std::max(1, static_cast<int>(std::lround(baseH * scale)));
        } else {
            int wPx = ReadPositiveIntFromEdit(ctx->editOutSizeW).value_or(0);
            int hPx = ReadPositiveIntFromEdit(ctx->editOutSizeH).value_or(0);
            if (wPx <= 0 && hPx <= 0) {
                return RejectExportDialogInput(ctx, L"出力サイズ(px)を入力してください。", ctx->editOutSizeW, true);
            }
            if (s_lastExportCustomAxis == 0) {
                if (wPx <= 0) wPx = std::max(1, static_cast<int>(std::lround(baseW * (static_cast<double>(hPx) / baseH))));
                scale = static_cast<double>(wPx) / baseW;
                outW = wPx;
                outH = std::max(1, static_cast<int>(std::lround(baseH * scale)));
            } else {
                if (hPx <= 0) hPx = std::max(1, static_cast<int>(std::lround(baseH * (static_cast<double>(wPx) / baseW))));
                scale = static_cast<double>(hPx) / baseH;
                outH = hPx;
                outW = std::max(1, static_cast<int>(std::lround(baseW * scale)));
            }
            s_lastExportCustomPngWPx = outW;
            s_lastExportCustomPngHPx = outH;
        }

        if (!std::isfinite(scale) || scale < 0.125 || scale > 8.0) {
            return RejectExportDialogInput(ctx, L"出力サイズが範囲外です。", ctx->editOutSizeW, true);
        }

        constexpr int64_t kMaxPixels = 8'000'000;
        int64_t total = static_cast<int64_t>(outW) * static_cast<int64_t>(outH);
        if (outW <= 0 || outH <= 0 || total <= 0 || total > kMaxPixels) {
            return RejectExportDialogInput(ctx, L"指定サイズが大きすぎます（PNG出力）。", ctx->editOutSizeW, true);
        }
        result.pngWidthPx = outW;
        result.pngHeightPx = outH;
    }
    if (!g_workspaceRoot.empty()) {
        g_config.exportStandardTextAnnots = result.standardTextAnnots;
        SaveWorkspaceConfig(g_workspaceRoot, g_config);
    }
    if (kind == ExportDialogKind::PdfPages) {
        std::wstring spec = TrimWhitespace(ReadDialogText(ctx->editPageSpec));
        if (spec.empty()) {
            return RejectExportDialogInput(ctx, L"ページ指定が空です。", ctx->editPageSpec, true);
        }
        bool defaultAnnot = IsExportDlgChecked(ctx->radioAnnotYes);
        std::wstring err;
        auto pages = file_output::ParsePdfPageSpec(spec, defaultAnnot, &err);
        if (pages.empty()) {
            std::wstring msg = err.empty() ? L"ページ指定が不正です。" : err;
            return RejectExportDialogInput(ctx, msg, ctx->editPageSpec, true);
        }
        result.pages = std::move(pages);
    } else if (kind == ExportDialogKind::PdfPng) {
        std::wstring input = ReadDialogText(ctx->editPageNumber);
        int pageNo = 0;
        if (!ParsePositiveInt(input, pageNo)) {
            return RejectExportDialogInput(ctx, L"ページ番号が不正です。", ctx->editPageNumber, true);
        }
        if (g_pdf.doc) {
            int count = 0;
            {
                std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
                count = FPDF_GetPageCount(g_pdf.doc);
            }
            if (pageNo > count) {
                return RejectExportDialogInput(ctx, L"ページ番号が範囲外です。", ctx->editPageNumber, true);
            }
        }
        result.pageIndex = pageNo - 1;
        result.includeAnnots = IsExportDlgChecked(ctx->radioAnnotYes);
        result.pngStyle = IsExportDlgChecked(ctx->radioPngStyleViewer)
                               ? file_output::PdfPngStyle::ViewerLike
                               : file_output::PdfPngStyle::PdfLike;
    } else if (kind == ExportDialogKind::NoteText) {
        file_output::TextExportOptions options{};
        if (IsExportDlgChecked(ctx->radioMathReplace)) {
            std::wstring placeholder = TrimWhitespace(ReadDialogText(ctx->editMathPlaceholder));
            if (placeholder.empty()) placeholder = L"[math]";
            options.mathMode = file_output::MathMode::Placeholder;
            options.mathPlaceholder = WideToUTF8(placeholder);
        } else {
            options.mathMode = file_output::MathMode::Raw;
        }
        options.includeCommentLines = IsExportDlgChecked(ctx->checkIncludeComments);
        bool stripMarkup = IsExportDlgChecked(ctx->checkStripMarkup);
        options.markupMode = stripMarkup ? file_output::MarkupMode::Simplified : file_output::MarkupMode::Raw;
        s_lastNoteStripMarkup = stripMarkup;
        s_lastNoteIncludeComments = options.includeCommentLines;
        result.textOptions = options;
    } else if (kind == ExportDialogKind::NoteMarkup) {
        file_output::NoteMarkupExportOptions options{};
        options.format = IsExportDlgChecked(ctx->radioMarkupHtml)
                            ? file_output::NoteMarkupExportOptions::Format::Html
                            : file_output::NoteMarkupExportOptions::Format::Markdown;
        s_lastNoteMarkupFormat = (options.format == file_output::NoteMarkupExportOptions::Format::Html) ? 1 : 0;

        if (IsExportDlgChecked(ctx->radioMathReplace)) {
            std::wstring placeholder = TrimWhitespace(ReadDialogText(ctx->editMathPlaceholder));
            if (placeholder.empty()) placeholder = L"[math]";
            options.mathMode = file_output::NoteMarkupExportOptions::MathMode::Placeholder;
            options.mathPlaceholder = WideToUTF8(placeholder);
            s_lastNoteMarkupMathPlaceholder = true;
            s_lastNoteMarkupMathPlaceholderText = placeholder;
        } else {
            options.mathMode = file_output::NoteMarkupExportOptions::MathMode::Format;
            s_lastNoteMarkupMathPlaceholder = false;
        }
        options.includeCommentLines = IsExportDlgChecked(ctx->checkIncludeComments);
        options.includeTitleHeading = IsExportDlgChecked(ctx->checkTitleHeading);
        options.shiftHeadingLevels = IsExportDlgChecked(ctx->checkShiftHeadings);
        s_lastNoteIncludeComments = options.includeCommentLines;
        s_lastNoteTitleHeading = options.includeTitleHeading;
        s_lastNoteShiftHeadings = options.shiftHeadingLevels;
        result.noteMarkupOptions = options;
    } else if (kind == ExportDialogKind::PdfAll) {
    }
    outResult = std::move(result);
    return true;
}

static void StoreReservation(ExportDialogState* ctx, const ExportDialogResult& result) {
    if (!ctx) return;
    if (IsPdfExportKind(result.kind)) {
        ctx->reservedPdf = result;
    } else if (IsNoteExportKind(result.kind)) {
        ctx->reservedNote = result;
    }
    UpdateReservationSummaryUi(ctx);
}

static void SwitchCategoryAfterSet(ExportDialogState* ctx, ExportDialogKind justStoredKind) {
    if (!ctx) return;
    if (IsPdfExportKind(justStoredKind) && ctx->hasNote && !ctx->reservedNote.has_value()) {
        CheckRadioButton(ctx->hwnd, kExportDlgIdTopPdf, kExportDlgIdTopNote, kExportDlgIdTopNote);
        UpdateExportDialogUi(ctx);
        return;
    }
    if (IsNoteExportKind(justStoredKind) && ctx->hasPdf && !ctx->reservedPdf.has_value()) {
        CheckRadioButton(ctx->hwnd, kExportDlgIdTopPdf, kExportDlgIdTopNote, kExportDlgIdTopPdf);
        UpdateExportDialogUi(ctx);
    }
}

static LRESULT CALLBACK ExportDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ExportDialogState* ctx = reinterpret_cast<ExportDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<ExportDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;

        const int margin = 12;
        const int row1Y = 10;
        const int row2Y = 42;
        const int rowH = 26;
        const int fileY = 76;
        const int optY = 104;
        const int outSizeY = optY + 84;
        const int outSizeMmY = outSizeY + 52;
        const int paperY = outSizeY + 74;
        const int pngStyleY = outSizeY + 102;
        const int stdTextY = outSizeY + 130;
        const int buttonsY = outSizeY + 192;
        const int reservationTitleY = buttonsY + 36;
        const int reservationListY = reservationTitleY + 20;

        ctx->topPdf = CreateWindowExW(0, L"BUTTON", L"PDF",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                      margin, row1Y, 80, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdTopPdf),
                                      cs->hInstance, nullptr);
        ctx->topNote = CreateWindowExW(0, L"BUTTON", L"ノート",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                       margin + 90, row1Y, 80, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdTopNote),
                                       cs->hInstance, nullptr);

        ctx->pdfAll = CreateWindowExW(0, L"BUTTON", L"注釈PDF",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                      margin, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPdfAll),
                                      cs->hInstance, nullptr);
        ctx->pdfPages = CreateWindowExW(0, L"BUTTON", L"ページ指定",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                        margin + 160, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPdfPages),
                                        cs->hInstance, nullptr);
        ctx->pdfPng = CreateWindowExW(0, L"BUTTON", L"単ページPNG",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                      margin + 320, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPdfPng),
                                      cs->hInstance, nullptr);

        ctx->noteText = CreateWindowExW(0, L"BUTTON", L"txt",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                        margin, row2Y, 100, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdNoteText),
                                        cs->hInstance, nullptr);
        ctx->noteMarkup = CreateWindowExW(0, L"BUTTON", L"マークアップ",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                          margin + 110, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdNoteMarkup),
                                          cs->hInstance, nullptr);

        ctx->labelFileExample = CreateWindowExW(0, L"STATIC", L"",
                                                WS_CHILD | WS_VISIBLE,
                                                margin, fileY, 460, 20, hWnd, nullptr,
                                                cs->hInstance, nullptr);

        ctx->labelPageSpec = CreateWindowExW(0, L"STATIC", L"ページ指定:",
                                             WS_CHILD | WS_VISIBLE,
                                             margin, optY, 80, 20, hWnd, nullptr,
                                             cs->hInstance, nullptr);
        ctx->editPageSpec = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                            margin + 90, optY - 2, 220, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPageSpec),
                                            cs->hInstance, nullptr);

        ctx->labelPageNumber = CreateWindowExW(0, L"STATIC", L"ページ番号:",
                                               WS_CHILD | WS_VISIBLE,
                                               margin, optY, 80, 20, hWnd, nullptr,
                                               cs->hInstance, nullptr);
        ctx->editPageNumber = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                              margin + 90, optY - 2, 80, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPageNumber),
                                              cs->hInstance, nullptr);

        ctx->labelPageExample = CreateWindowExW(0, L"STATIC", L"",
                                                WS_CHILD | WS_VISIBLE,
                                                margin + 90, optY + 24, 360, 20, hWnd, nullptr,
                                                cs->hInstance, nullptr);

        ctx->labelAnnot = CreateWindowExW(0, L"STATIC", L"注釈:",
                                          WS_CHILD | WS_VISIBLE,
                                          margin, optY + 52, 80, 20, hWnd, nullptr,
                                          cs->hInstance, nullptr);
        ctx->radioAnnotYes = CreateWindowExW(0, L"BUTTON", L"含める",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                             margin + 90, optY + 50, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdAnnotYes),
                                             cs->hInstance, nullptr);
        ctx->radioAnnotNo = CreateWindowExW(0, L"BUTTON", L"含めない",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                            margin + 200, optY + 50, 110, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdAnnotNo),
                                            cs->hInstance, nullptr);

        int styleY = pngStyleY;
        ctx->labelPngStyle = CreateWindowExW(0, L"STATIC", L"画像スタイル:",
                                             WS_CHILD | WS_VISIBLE,
                                             margin, styleY, 100, 20, hWnd, nullptr,
                                             cs->hInstance, nullptr);
        ctx->radioPngStylePdf = CreateWindowExW(0, L"BUTTON", L"標準PDF風",
                                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                                 margin + 90, styleY, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPngStylePdf),
                                                 cs->hInstance, nullptr);
        ctx->radioPngStyleViewer = CreateWindowExW(0, L"BUTTON", L"エディタの表示風",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                                    margin + 200, styleY, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPngStyleViewer),
                                                    cs->hInstance, nullptr);

        ctx->labelOutSize = CreateWindowExW(0, L"STATIC", L"出力サイズ:",
                                            WS_CHILD | WS_VISIBLE,
                                            margin, outSizeY, 100, 20, hWnd, nullptr,
                                            cs->hInstance, nullptr);
        ctx->radioOutSizeHalf = CreateWindowExW(0, L"BUTTON", L"半分",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                                margin + 90, outSizeY, 70, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeHalf),
                                                cs->hInstance, nullptr);
        ctx->radioOutSizeOne = CreateWindowExW(0, L"BUTTON", L"そのまま（推奨）",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                               margin + 170, outSizeY, 130, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeOne),
                                               cs->hInstance, nullptr);
        ctx->radioOutSizeTwo = CreateWindowExW(0, L"BUTTON", L"倍",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                               margin + 310, outSizeY, 50, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeTwo),
                                               cs->hInstance, nullptr);
        ctx->radioOutSizeCustom = CreateWindowExW(0, L"BUTTON", L"指定",
                                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                                  margin + 370, outSizeY, 60, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeCustom),
                                                  cs->hInstance, nullptr);

        ctx->labelOutSizeW = CreateWindowExW(0, L"STATIC", L"横:",
                                             WS_CHILD | WS_VISIBLE,
                                             margin, outSizeY + 28, 60, 20, hWnd, nullptr,
                                             cs->hInstance, nullptr);
        ctx->editOutSizeW = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                            margin + 65, outSizeY + 26, 90, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeW),
                                            cs->hInstance, nullptr);
        ctx->labelOutSizeH = CreateWindowExW(0, L"STATIC", L"縦:",
                                             WS_CHILD | WS_VISIBLE,
                                             margin + 170, outSizeY + 28, 60, 20, hWnd, nullptr,
                                             cs->hInstance, nullptr);
        ctx->editOutSizeH = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                            margin + 235, outSizeY + 26, 90, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeH),
                                            cs->hInstance, nullptr);

        ctx->labelOutSizeMm = CreateWindowExW(0, L"STATIC", L"",
                                              WS_CHILD | WS_VISIBLE,
                                              margin, outSizeMmY, 460, 18, hWnd, nullptr,
                                              cs->hInstance, nullptr);

        ctx->labelPaper = CreateWindowExW(0, L"STATIC", L"用紙:",
                                          WS_CHILD | WS_VISIBLE,
                                          margin, paperY, 50, 20, hWnd, nullptr,
                                          cs->hInstance, nullptr);
        ctx->comboPaper = CreateWindowExW(0, L"COMBOBOX", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          margin + 55, paperY - 2, 150, 200, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPaperCombo),
                                          cs->hInstance, nullptr);

        ctx->checkStandardText = CreateWindowExW(0, L"BUTTON", L"テキスト注釈を標準で保存（見えない場合は画像にフォールバック）",
                                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                 margin, stdTextY, 460, 22, hWnd,
                                                 reinterpret_cast<HMENU>(kExportDlgIdStandardText),
                                                 cs->hInstance, nullptr);
        ctx->checkMatchPdfPaneTextLayout = CreateWindowExW(0, L"BUTTON", L"TextBoxの改行をPDF欄の表示に合わせる（推奨）",
                                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                            margin, stdTextY + 24, 460, 22, hWnd,
                                                            reinterpret_cast<HMENU>(kExportDlgIdMatchPdfPaneTextLayout),
                                                            cs->hInstance, nullptr);

        ctx->labelMath = CreateWindowExW(0, L"STATIC", L"数式:",
                                         WS_CHILD | WS_VISIBLE,
                                         margin, optY, 80, 20, hWnd, nullptr,
                                         cs->hInstance, nullptr);
        ctx->radioMathKeep = CreateWindowExW(0, L"BUTTON", L"そのまま",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                             margin + 90, optY - 2, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdMathKeep),
                                             cs->hInstance, nullptr);
        ctx->radioMathReplace = CreateWindowExW(0, L"BUTTON", L"置き換える",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                                margin + 200, optY - 2, 120, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdMathReplace),
                                                cs->hInstance, nullptr);
        ctx->labelMathPlaceholder = CreateWindowExW(0, L"STATIC", L"置き換え文字列:",
                                                    WS_CHILD | WS_VISIBLE,
                                                    margin, optY + 28, 100, 20, hWnd, nullptr,
                                                    cs->hInstance, nullptr);
        ctx->editMathPlaceholder = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"[math]",
                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                   margin + 110, optY + 26, 200, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdMathPlaceholder),
                                                   cs->hInstance, nullptr);

        ctx->checkIncludeComments = CreateWindowExW(0, L"BUTTON", L"コメント行も出力",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                    margin, optY + 52, 220, 22, hWnd,
                                                    reinterpret_cast<HMENU>(kExportDlgIdIncludeComments),
                                                    cs->hInstance, nullptr);

        ctx->checkStripMarkup = CreateWindowExW(0, L"BUTTON", L"マークアップ除去 (txt)",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                margin, optY + 78, 260, 22, hWnd,
                                                reinterpret_cast<HMENU>(kExportDlgIdStripMarkup),
                                                cs->hInstance, nullptr);

        ctx->labelMarkupFormat = CreateWindowExW(0, L"STATIC", L"形式:",
                                                 WS_CHILD | WS_VISIBLE,
                                                 margin, optY + 78, 80, 20, hWnd, nullptr,
                                                 cs->hInstance, nullptr);
        ctx->radioMarkupMd = CreateWindowExW(0, L"BUTTON", L"md",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                             margin + 90, optY + 76, 70, 22, hWnd,
                                             reinterpret_cast<HMENU>(kExportDlgIdMarkupMd),
                                             cs->hInstance, nullptr);
        ctx->radioMarkupHtml = CreateWindowExW(0, L"BUTTON", L"html",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                               margin + 170, optY + 76, 70, 22, hWnd,
                                               reinterpret_cast<HMENU>(kExportDlgIdMarkupHtml),
                                               cs->hInstance, nullptr);

        ctx->checkTitleHeading = CreateWindowExW(0, L"BUTTON", L"ノート名を先頭見出しにする",
                                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                 margin, optY + 104, 320, 22, hWnd,
                                                 reinterpret_cast<HMENU>(kExportDlgIdTitleHeading),
                                                 cs->hInstance, nullptr);
        ctx->checkShiftHeadings = CreateWindowExW(0, L"BUTTON", L"既存見出しを1段下げる",
                                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                  margin, optY + 132, 260, 22, hWnd,
                                                  reinterpret_cast<HMENU>(kExportDlgIdShiftHeadings),
                                                  cs->hInstance, nullptr);

        ctx->btnSet = CreateWindowExW(0, L"BUTTON", L"Set", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      210, buttonsY, 80, 26, hWnd, reinterpret_cast<HMENU>(kExportDlgIdSet),
                                      cs->hInstance, nullptr);
        ctx->labelInlineError = CreateWindowExW(0, L"STATIC", L"",
                                                WS_CHILD,
                                                margin, buttonsY - 6, 188, 40, hWnd, nullptr,
                                                cs->hInstance, nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     300, buttonsY, 80, 26, hWnd, reinterpret_cast<HMENU>(IDOK),
                                     cs->hInstance, nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         390, buttonsY, 80, 26, hWnd, reinterpret_cast<HMENU>(IDCANCEL),
                                         cs->hInstance, nullptr);
        ctx->labelReservationTitle = CreateWindowExW(0, L"STATIC", L"予約一覧（このウィンドウを閉じると破棄）",
                                                     WS_CHILD | WS_VISIBLE,
                                                     margin, reservationTitleY, 460, 20, hWnd, nullptr,
                                                     cs->hInstance, nullptr);
        ctx->labelReservationList = CreateWindowExW(0, L"STATIC", L"",
                                                    WS_CHILD | WS_VISIBLE,
                                                    margin, reservationListY, 460, 52, hWnd, nullptr,
                                                    cs->hInstance, nullptr);

        auto applyFont = [&](HWND h) {
            if (h && g_hUIFont) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
        };
        applyFont(ctx->topPdf);
        applyFont(ctx->topNote);
        applyFont(ctx->pdfAll);
        applyFont(ctx->pdfPages);
        applyFont(ctx->pdfPng);
        applyFont(ctx->noteText);
        applyFont(ctx->noteMarkup);
        applyFont(ctx->labelFileExample);
        applyFont(ctx->labelPageSpec);
        applyFont(ctx->editPageSpec);
        applyFont(ctx->labelPageNumber);
        applyFont(ctx->editPageNumber);
        applyFont(ctx->labelPageExample);
        applyFont(ctx->labelAnnot);
        applyFont(ctx->radioAnnotYes);
        applyFont(ctx->radioAnnotNo);
        applyFont(ctx->labelPngStyle);
        applyFont(ctx->radioPngStylePdf);
        applyFont(ctx->radioPngStyleViewer);
        applyFont(ctx->labelOutSize);
        applyFont(ctx->radioOutSizeHalf);
        applyFont(ctx->radioOutSizeOne);
        applyFont(ctx->radioOutSizeTwo);
        applyFont(ctx->radioOutSizeCustom);
        applyFont(ctx->labelOutSizeW);
        applyFont(ctx->editOutSizeW);
        applyFont(ctx->labelOutSizeH);
        applyFont(ctx->editOutSizeH);
        applyFont(ctx->labelOutSizeMm);
        applyFont(ctx->labelPaper);
        applyFont(ctx->comboPaper);
        applyFont(ctx->checkStandardText);
        applyFont(ctx->checkMatchPdfPaneTextLayout);
        applyFont(ctx->labelMath);
        applyFont(ctx->radioMathKeep);
        applyFont(ctx->radioMathReplace);
        applyFont(ctx->labelMathPlaceholder);
        applyFont(ctx->editMathPlaceholder);
        applyFont(ctx->checkIncludeComments);
        applyFont(ctx->checkStripMarkup);
        applyFont(ctx->labelMarkupFormat);
        applyFont(ctx->radioMarkupMd);
        applyFont(ctx->radioMarkupHtml);
        applyFont(ctx->checkTitleHeading);
        applyFont(ctx->checkShiftHeadings);
        applyFont(ctx->btnSet);
        applyFont(ctx->labelInlineError);
        applyFont(okBtn);
        applyFont(cancelBtn);
        applyFont(ctx->labelReservationTitle);
        applyFont(ctx->labelReservationList);

        auto attachSilentEdit = [&](HWND h) {
            if (h) SetWindowSubclass(h, ExportDialogEditProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        };
        attachSilentEdit(ctx->editPageSpec);
        attachSilentEdit(ctx->editPageNumber);
        attachSilentEdit(ctx->editOutSizeW);
        attachSilentEdit(ctx->editOutSizeH);
        attachSilentEdit(ctx->editMathPlaceholder);

        if (ctx->comboPaper) {
            SendMessageW(ctx->comboPaper, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 18);
            SendMessageW(ctx->comboPaper, CB_SETITEMHEIGHT, 0, 18);
        }

        bool preferPdf = (ctx->preset == ExportDialogKind::PdfAll ||
                          ctx->preset == ExportDialogKind::PdfPages ||
                          ctx->preset == ExportDialogKind::PdfPng);
        if (!ctx->hasPdf && ctx->hasNote) preferPdf = false;
        if (ctx->hasPdf && !ctx->hasNote) preferPdf = true;

        CheckRadioButton(hWnd, kExportDlgIdTopPdf, kExportDlgIdTopNote,
                         preferPdf ? kExportDlgIdTopPdf : kExportDlgIdTopNote);
        if (preferPdf) {
            int detail = kExportDlgIdPdfAll;
            if (ctx->preset == ExportDialogKind::PdfPages) detail = kExportDlgIdPdfPages;
            else if (ctx->preset == ExportDialogKind::PdfPng) detail = kExportDlgIdPdfPng;
            CheckRadioButton(hWnd, kExportDlgIdPdfAll, kExportDlgIdPdfPng, detail);
        } else {
            int detail = kExportDlgIdNoteText;
            if (ctx->preset == ExportDialogKind::NoteMarkup) detail = kExportDlgIdNoteMarkup;
            CheckRadioButton(hWnd, kExportDlgIdNoteText, kExportDlgIdNoteMarkup, detail);
        }

        CheckRadioButton(hWnd, kExportDlgIdAnnotYes, kExportDlgIdAnnotNo, kExportDlgIdAnnotYes);
        CheckRadioButton(hWnd, kExportDlgIdMathKeep, kExportDlgIdMathReplace, kExportDlgIdMathKeep);
        CheckRadioButton(hWnd, kExportDlgIdPngStylePdf, kExportDlgIdPngStyleViewer, kExportDlgIdPngStylePdf);
        CheckRadioButton(hWnd, kExportDlgIdMarkupMd, kExportDlgIdMarkupHtml,
                         (s_lastNoteMarkupFormat == 1) ? kExportDlgIdMarkupHtml : kExportDlgIdMarkupMd);
        if (ctx->checkStripMarkup) {
            SendMessageW(ctx->checkStripMarkup, BM_SETCHECK, s_lastNoteStripMarkup ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (ctx->checkIncludeComments) {
            SendMessageW(ctx->checkIncludeComments, BM_SETCHECK,
                         s_lastNoteIncludeComments ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (ctx->checkTitleHeading) {
            SendMessageW(ctx->checkTitleHeading, BM_SETCHECK, s_lastNoteTitleHeading ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (ctx->checkShiftHeadings) {
            SendMessageW(ctx->checkShiftHeadings, BM_SETCHECK, s_lastNoteShiftHeadings ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (s_lastNoteMarkupMathPlaceholder) {
            CheckRadioButton(hWnd, kExportDlgIdMathKeep, kExportDlgIdMathReplace, kExportDlgIdMathReplace);
            if (ctx->editMathPlaceholder) {
                SetWindowTextW(ctx->editMathPlaceholder, s_lastNoteMarkupMathPlaceholderText.c_str());
            }
        }
        {
            int id = kExportDlgIdOutSizeOne;
            if (s_lastExportSizeMode == ExportSizeMode::Half) id = kExportDlgIdOutSizeHalf;
            else if (s_lastExportSizeMode == ExportSizeMode::Two) id = kExportDlgIdOutSizeTwo;
            else if (s_lastExportSizeMode == ExportSizeMode::Custom) id = kExportDlgIdOutSizeCustom;
            CheckRadioButton(hWnd, kExportDlgIdOutSizeHalf, kExportDlgIdOutSizeCustom, id);
        }
        if (ctx->checkStandardText) {
            SendMessageW(ctx->checkStandardText, BM_SETCHECK,
                         g_config.exportStandardTextAnnots ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (ctx->checkMatchPdfPaneTextLayout) {
            SendMessageW(ctx->checkMatchPdfPaneTextLayout, BM_SETCHECK, BST_CHECKED, 0);
        }

        UpdateReservationSummaryUi(ctx);
        UpdateExportDialogUi(ctx);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        if (ctx && ctl == ctx->labelInlineError) {
            SetTextColor(hdc, ExportDialogErrorTextColor());
            SetBkColor(hdc, g_theme.panelBg);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(g_hThemePanelBrush ? g_hThemePanelBrush : GetSysColorBrush(COLOR_WINDOW));
        }
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK) {
            if (ctx && (ctx->reservedPdf.has_value() || ctx->reservedNote.has_value())) {
                ctx->committedResults.clear();
                if (ctx->reservedNote.has_value()) ctx->committedResults.push_back(*ctx->reservedNote);
                if (ctx->reservedPdf.has_value()) ctx->committedResults.push_back(*ctx->reservedPdf);
                ctx->ok = !ctx->committedResults.empty();
                ctx->done = true;
                DestroyWindow(hWnd);
            } else {
                ExportDialogResult current{};
                if (!BuildExportDialogResult(ctx, current)) return 0;
                ctx->committedResults.clear();
                ctx->committedResults.push_back(std::move(current));
                ctx->ok = true;
                ctx->done = true;
                DestroyWindow(hWnd);
            }
            return 0;
        }
        if (id == kExportDlgIdSet) {
            ExportDialogResult current{};
            if (!BuildExportDialogResult(ctx, current)) return 0;
            StoreReservation(ctx, current);
            SwitchCategoryAfterSet(ctx, current.kind);
            return 0;
        }
        if (id == IDCANCEL) {
            ctx->ok = false;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE) {
            ClearExportDialogInlineError(ctx);
            if (id == kExportDlgIdPaperCombo) {
                s_lastExportPaperPresetId = GetComboItemData(ctx->comboPaper);
                if (s_lastExportPaperPresetId != 0) {
                    double wmm = 0.0, hmm = 0.0;
                    if (LookupPaperPresetMm(s_lastExportPaperPresetId, wmm, hmm)) {
                        CheckRadioButton(ctx->hwnd, kExportDlgIdOutSizeHalf, kExportDlgIdOutSizeCustom, kExportDlgIdOutSizeCustom);
                        ctx->updatingSize = true;
                        SetDialogDouble(ctx->editOutSizeW, MmToPt(wmm), 1);
                        SetDialogDouble(ctx->editOutSizeH, MmToPt(hmm), 1);
                        ctx->updatingSize = false;
                        UpdateExportDialogSizeUi(ctx, kExportDlgIdPaperCombo);
                    }
                } else {
                    UpdateExportDialogSizeUi(ctx, kExportDlgIdPaperCombo);
                }
                return 0;
            }
        }
        if (HIWORD(wParam) == BN_CLICKED) {
            ClearExportDialogInlineError(ctx);
            UpdateExportDialogUi(ctx);
            UpdateExportDialogSizeUi(ctx, id);
        } else if (HIWORD(wParam) == EN_CHANGE) {
            ClearExportDialogInlineError(ctx);
            if (ctx && ctx->updatingSize) return 0;
            if (id == kExportDlgIdOutSizeW || id == kExportDlgIdOutSizeH ||
                id == kExportDlgIdPageNumber || id == kExportDlgIdPageSpec) {
                if ((id == kExportDlgIdOutSizeW || id == kExportDlgIdOutSizeH) && ctx && ctx->comboPaper) {
                    ExportDialogKind kind = GetExportDialogKind(ctx);
                    if (kind == ExportDialogKind::PdfAll || kind == ExportDialogKind::PdfPages) {
                        if (GetComboItemData(ctx->comboPaper) != 0) {
                            s_lastExportPaperPresetId = 0;
                            SendMessageW(ctx->comboPaper, CB_SETCURSEL, 0, 0);
                        }
                    }
                }
                UpdateExportDialogSizeUi(ctx, id);
            }
        }
        break;
    }
    case WM_CLOSE:
        ctx->ok = false;
        ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool ShowUnifiedExportDialog(HWND owner, ExportDialogKind preset, std::vector<ExportDialogResult>& out) {
    ExportDialogState ctx;
    ctx.preset = preset;
    ctx.hasPdf = (g_pdf.doc != nullptr);
    ctx.hasNote = !g_currentNotePath.empty();

    WNDCLASSW wc{};
    wc.lpfnWndProc = ExportDialogProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"UnifiedExportDialog";
    RegisterClassW(&wc);
    const std::wstring dialogTitle = ExperimentalExportDialogTitle(GetUiText().menuExport);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, dialogTitle.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 520, 548,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return false;
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    MSG msg;
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ShouldSkipImeMessageInLoop(msg)) continue;
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(msg, g_hNoteEdit)) continue;
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (ctx.ok) {
        out = std::move(ctx.committedResults);
        return !out.empty();
    }
    return false;
}

void ExecuteUnifiedExport(HWND hWnd, const ExportDialogResult& result) {
    switch (result.kind) {
    case ExportDialogKind::PdfAll:
        file_output::ExportPdfWithAnnotations(hWnd, true, result.standardTextAnnots, result.pdfScale,
                                               result.matchPdfPaneTextLayout);
        break;
    case ExportDialogKind::PdfPages:
        file_output::ExportPdfPages(hWnd, result.pages, result.standardTextAnnots, result.pdfScale,
                                    result.matchPdfPaneTextLayout);
        break;
    case ExportDialogKind::PdfPng:
        file_output::ExportPdfPagePng(hWnd, result.pageIndex, result.pngStyle, result.includeAnnots,
                                result.pngWidthPx, result.pngHeightPx);
        break;
    case ExportDialogKind::NoteText:
        file_output::ExportNotePlainText(hWnd, result.textOptions);
        break;
    case ExportDialogKind::NoteMarkup:
        file_output::ExportNoteMarkup(hWnd, result.noteMarkupOptions);
        break;
    }
}

void ExecuteUnifiedExports(HWND hWnd, const std::vector<ExportDialogResult>& results) {
    if (results.empty()) return;
    if (!s_pendingExportsAfterSave.empty()) return;

    const file_output::SaveTransactionStartResult start =
        file_output::StartBackgroundSaveAndIntegrateTransaction(hWnd);
    if (start == file_output::SaveTransactionStartResult::Failed) return;
    if (start == file_output::SaveTransactionStartResult::Started) {
        s_pendingExportsAfterSave = results;
        ShowSoftNotice(hWnd,
                       IsEnglishUi() ? L"Export will start after saving is complete."
                                     : L"統合保存が完了したら出力を開始します。",
                       SoftNoticeKind::Info);
        return;
    }

    FinalizeManualSaveUi(hWnd, /*updateWindowTitleAfterSave=*/true);
    for (const auto& result : results) {
        ExecuteUnifiedExport(hWnd, result);
    }
}

void CompleteUnifiedExportsAfterSave(HWND hWnd, bool saveSucceeded, bool saveRestarted) {
    if (s_pendingExportsAfterSave.empty()) return;
    if (!saveSucceeded) {
        s_pendingExportsAfterSave.clear();
        return;
    }
    // A newer edit arrived while saving; wait for the restarted transaction so the
    // output always reflects the final integrated state.
    if (saveRestarted) return;

    std::vector<ExportDialogResult> results = std::move(s_pendingExportsAfterSave);
    s_pendingExportsAfterSave.clear();
    for (const auto& result : results) {
        ExecuteUnifiedExport(hWnd, result);
    }
}



