#include "ui/dialogs/export_dialog.h"
#include "ui/dialogs/dialogs.h"
#include "ui/noop_nav_guard.h"
#include "core/localization.h"
#include "workspace/workspace_config_io.h"
#include "workspace/workspace_actions.h"

static std::wstring ExperimentalExportDialogTitle(const std::wstring& base) {
    if (base.find(L"試験的") != std::wstring::npos ||
        base.find(L"Experimental") != std::wstring::npos) {
        return base;
    }
    return base + localization::Text(L"dialog.export.experimental_suffix");
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

class ScopedExportDialogOwner {
public:
    explicit ScopedExportDialogOwner(HWND owner) : owner_(owner) {
        wasEnabled_ = owner_ && IsWindow(owner_) && IsWindowEnabled(owner_);
        if (wasEnabled_) EnableWindow(owner_, FALSE);
    }

    ~ScopedExportDialogOwner() {
        if (wasEnabled_ && owner_ && IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
            SetActiveWindow(owner_);
        }
    }

    ScopedExportDialogOwner(const ScopedExportDialogOwner&) = delete;
    ScopedExportDialogOwner& operator=(const ScopedExportDialogOwner&) = delete;

private:
    HWND owner_{};
    bool wasEnabled_ = false;
};

namespace {

constexpr int kExportResultsListId = 4901;
constexpr int kExportResultsOpenId = 4902;
constexpr int kExportResultsFolderId = 4903;
constexpr int kExportResultsCloseId = 4904;
constexpr ULONGLONG kExportResultsOutsideDismissDelayMs = 1800;
constexpr int kQuickExportSettingsCloseId = 4911;

struct ExportResultsDialogState {
    HWND hwnd{};
    HWND list{};
    const std::vector<std::wstring>* paths{};
    bool done = false;
    ULONGLONG outsideDismissAfterTick = 0;
};

static bool IsMouseDownMessage(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
    case WM_NCXBUTTONDOWN:
        return true;
    default:
        return false;
    }
}

static bool IsOwnedWindowMessage(const MSG& message, HWND owner) {
    if (!owner || !message.hwnd) return false;
    return message.hwnd == owner || IsChild(owner, message.hwnd);
}

static bool IsPdfOutputPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    return extension == L".pdf";
}

static bool IsMarkdownOutputPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    return extension == L".md" || extension == L".markdown";
}

static bool IsReadOnlyViewerOutputPath(const std::wstring& path) {
    return IsPdfOutputPath(path) || IsMarkdownOutputPath(path);
}

static void ShowExportResultLaunchFailure(HWND owner, const std::wstring& path) {
    ShowSoftNotice(owner,
                   localization::Format(L"dialog.export.launch_failed", {{L"PATH", path}}),
                   SoftNoticeKind::Warning);
}

static void OpenExportResultFile(HWND owner, const std::wstring& path) {
    if (path.empty()) return;
    if (IsPdfOutputPath(path)) {
        (void)LaunchReadOnlyViewerForPdfAt(owner, path, -1, 0.0, false);
        return;
    }
    if (IsMarkdownOutputPath(path)) {
        (void)LaunchReadOnlyViewerForFile(owner, path);
        return;
    }
    const HINSTANCE result = ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) ShowExportResultLaunchFailure(owner, path);
}

static void OpenExportResultFolder(HWND owner, const std::wstring& path) {
    if (path.empty()) return;
    const std::wstring params = L"/select,\"" + path + L"\"";
    const HINSTANCE result = ShellExecuteW(owner, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) ShowExportResultLaunchFailure(owner, path);
}

static int SelectedExportResultIndex(const ExportResultsDialogState* ctx) {
    if (!ctx || !ctx->list) return LB_ERR;
    return static_cast<int>(SendMessageW(ctx->list, LB_GETCURSEL, 0, 0));
}

static void UpdateExportResultButtons(ExportResultsDialogState* ctx) {
    if (!ctx || !ctx->hwnd || !ctx->paths) return;
    const int selected = SelectedExportResultIndex(ctx);
    const bool valid = selected != LB_ERR && selected >= 0 && selected < static_cast<int>(ctx->paths->size());
    HWND open = GetDlgItem(ctx->hwnd, kExportResultsOpenId);
    HWND folder = GetDlgItem(ctx->hwnd, kExportResultsFolderId);
    EnableWindow(open, valid);
    EnableWindow(folder, valid);
    if (open) {
        const bool viewer = valid && IsReadOnlyViewerOutputPath((*ctx->paths)[static_cast<size_t>(selected)]);
        SetWindowTextW(open, viewer
            ? (localization::Text(L"dialog.export.154cf8fead5d").c_str())
            : (localization::Text(L"dialog.export.7494b10f0fa9").c_str()));
    }
}

static LRESULT CALLBACK ExportResultsDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<ExportResultsDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<ExportResultsDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;
        const int margin = 12;
        CreateWindowExW(0, L"STATIC", localization::Text(L"dialog.export.57a695564262").c_str(),
                        WS_CHILD | WS_VISIBLE, margin, margin, 580, 22, hWnd, nullptr, cs->hInstance, nullptr);
        ctx->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT,
                                    margin, 38, 580, 210, hWnd,
                                    reinterpret_cast<HMENU>(kExportResultsListId), cs->hInstance, nullptr);
        if (ctx->paths) {
            for (const auto& path : *ctx->paths) SendMessageW(ctx->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
        }
        if (ctx->list) SendMessageW(ctx->list, LB_SETHORIZONTALEXTENT, 4096, 0);
        if (ctx->list) SendMessageW(ctx->list, LB_SETCURSEL, 0, 0);
        CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        margin, 262, 190, 28, hWnd, reinterpret_cast<HMENU>(kExportResultsOpenId), cs->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", localization::Text(L"dialog.export.50541c9a3e31").c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        212, 262, 140, 28, hWnd, reinterpret_cast<HMENU>(kExportResultsFolderId), cs->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", localization::Text(L"dialog.export.603bc62f3f34").c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        448, 262, 144, 28, hWnd, reinterpret_cast<HMENU>(kExportResultsCloseId), cs->hInstance, nullptr);
        for (int id : { kExportResultsListId, kExportResultsOpenId, kExportResultsFolderId, kExportResultsCloseId }) {
            HWND child = GetDlgItem(hWnd, id);
            if (child && g_hUIFont) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
        }
        ApplyThemeToDialog(hWnd);
        UpdateExportResultButtons(ctx);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return ThemeCtlColorPanel(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam));
    case WM_DRAWITEM:
        if (DrawThemeButton(reinterpret_cast<LPDRAWITEMSTRUCT>(lParam))) return TRUE;
        break;
    case WM_COMMAND: {
        if (!ctx) break;
        const int id = LOWORD(wParam);
        if (id == kExportResultsListId && HIWORD(wParam) == LBN_SELCHANGE) {
            UpdateExportResultButtons(ctx);
            return 0;
        }
        const int selected = SelectedExportResultIndex(ctx);
        if ((id == kExportResultsOpenId || id == kExportResultsFolderId) && ctx->paths && selected != LB_ERR &&
            selected >= 0 && selected < static_cast<int>(ctx->paths->size())) {
            const std::wstring& path = (*ctx->paths)[static_cast<size_t>(selected)];
            if (id == kExportResultsOpenId) OpenExportResultFile(hWnd, path);
            else OpenExportResultFolder(hWnd, path);
            return 0;
        }
        if (id == kExportResultsCloseId || id == IDCANCEL) {
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ctx) {
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowExportResultsDialog(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    ExportResultsDialogState ctx{};
    ctx.paths = &paths;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ExportResultsDialogProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hThemePanelBrush ? g_hThemePanelBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ExportResultsDialog";
    RegisterClassW(&wc);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName,
                                  localization::Text(L"dialog.export.dbb10e979b7d").c_str(),
                                  WS_CAPTION | WS_POPUPWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 620, 340, owner, nullptr, g_hInst, &ctx);
    if (!dialog) return;
    PlaceOwnedPopupAtAppTopLeft(dialog, owner);
    // The result window only reports a completed export; it has no decision
    // that must block the main window.  Keep the owner interactive so, after
    // the grace period, an outside click can dismiss this transient window.
    ctx.outsideDismissAfterTick = GetTickCount64() + kExportResultsOutsideDismissDelayMs;
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG msg{};
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ShouldSkipImeMessageInLoop(msg)) continue;
        if (GetTickCount64() >= ctx.outsideDismissAfterTick &&
            IsMouseDownMessage(msg.message) && IsOwnedWindowMessage(msg, owner)) {
            // Consume the dismissing click.  It must not both close this
            // result window and invoke an unrelated command behind it.
            ctx.done = true;
            DestroyWindow(dialog);
            continue;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner && IsWindow(owner)) {
        SetActiveWindow(owner);
    }
}

struct QuickExportSettingsDialogState {
    HWND hwnd{};
    std::wstring summary;
    bool done = false;
};

static std::wstring QuickExportEnabledLabel(bool value) {
    return localization::Text(value ? L"export.quick_settings.enabled" : L"export.quick_settings.disabled");
}

static std::wstring BuildQuickExportSettingsSummary() {
    const int percent = std::clamp(g_config.quickPdfScalePercent, 13, 800);
    std::wstring summary = localization::Text(L"export.quick_settings_pdf_heading");
    summary += L"\r\n" + localization::Format(L"export.quick_settings_pdf_scale", {{L"PERCENT", std::to_wstring(percent)}});
    summary += L"\r\n" + localization::Format(L"export.quick_settings_pdf_standard_text",
                                                   {{L"VALUE", QuickExportEnabledLabel(g_config.quickPdfStandardTextAnnots)}});
    summary += L"\r\n" + localization::Format(L"export.quick_settings_pdf_match_layout",
                                                   {{L"VALUE", QuickExportEnabledLabel(g_config.quickPdfMatchPdfPaneTextLayout)}});
    summary += L"\r\n\r\n" + localization::Text(L"export.quick_settings_note_heading");
    summary += L"\r\n" + localization::Text(L"export.quick_settings_note_format");
    summary += L"\r\n" + localization::Format(L"export.quick_settings_note_strip_markup",
                                                   {{L"VALUE", QuickExportEnabledLabel(g_config.quickNoteStripMarkup)}});
    summary += L"\r\n" + localization::Format(L"export.quick_settings_note_comments",
                                                   {{L"VALUE", QuickExportEnabledLabel(g_config.quickNoteIncludeComments)}});
    if (g_config.quickNoteMathPlaceholder) {
        std::wstring placeholder = TrimWhitespace(g_config.quickNoteMathPlaceholderText);
        if (placeholder.empty()) placeholder = L"[math]";
        summary += L"\r\n" + localization::Format(L"export.quick_settings_note_math_placeholder",
                                                       {{L"VALUE", placeholder}});
    } else {
        summary += L"\r\n" + localization::Text(L"export.quick_settings_note_math_keep");
    }
    return summary;
}

static LRESULT CALLBACK QuickExportSettingsDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<QuickExportSettingsDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<QuickExportSettingsDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;
        HWND summary = CreateWindowExW(0, L"STATIC", ctx->summary.c_str(), WS_CHILD | WS_VISIBLE,
                                       12, 12, 430, 198, hWnd, nullptr, cs->hInstance, nullptr);
        HWND close = CreateWindowExW(0, L"BUTTON", localization::Text(L"common.close").c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                     350, 220, 92, 26, hWnd,
                                     reinterpret_cast<HMENU>(kQuickExportSettingsCloseId), cs->hInstance, nullptr);
        if (g_hUIFont) {
            SendMessageW(summary, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
            SendMessageW(close, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
        }
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return ThemeCtlColorPanel(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam));
    case WM_DRAWITEM:
        if (DrawThemeButton(reinterpret_cast<LPDRAWITEMSTRUCT>(lParam))) return TRUE;
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == kQuickExportSettingsCloseId || LOWORD(wParam) == IDCANCEL) {
            if (ctx) ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (ctx) ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (ctx) ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowQuickExportSettingsDialog(HWND owner) {
    QuickExportSettingsDialogState ctx{};
    ctx.summary = BuildQuickExportSettingsSummary();
    WNDCLASSW wc{};
    wc.lpfnWndProc = QuickExportSettingsDialogProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hThemePanelBrush ? g_hThemePanelBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"QuickExportSettingsDialog";
    RegisterClassW(&wc);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName,
                                  localization::Text(L"export.quick_settings_title").c_str(),
                                  WS_CAPTION | WS_POPUPWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 465, 290, owner, nullptr, g_hInst, &ctx);
    if (!dialog) return;
    PlaceOwnedPopupAtAppTopLeft(dialog, owner);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG msg{};
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ShouldSkipImeMessageInLoop(msg)) continue;
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner && IsWindow(owner)) SetActiveWindow(owner);
}

static std::optional<std::wstring> ExecuteUnifiedExportAndGetPath(HWND hWnd, const ExportDialogResult& result) {
    const bool hasChosenOutputPath = !result.outputPath.empty();

    std::error_code existsError;
    if (hasChosenOutputPath && std::filesystem::exists(std::filesystem::path(result.outputPath), existsError) && !existsError) {
        SilentDialogOptions overwrite;
        overwrite.title = localization::Text(L"export.overwrite_title");
        overwrite.message = localization::Text(L"export.overwrite_message");
        overwrite.kind = SoftNoticeKind::Warning;
        overwrite.buttons = SilentDialogButtons::YesNo;
        overwrite.yesLabel = localization::Text(L"export.overwrite_yes");
        overwrite.noLabel = localization::Text(L"export.overwrite_no");
        overwrite.defaultResult = SilentDialogResult::No;
        overwrite.escapeResult = SilentDialogResult::No;
        overwrite.paths = {{localization::Text(L"export.overwrite_path_label"), result.outputPath}};
        if (ShowSilentDialog(hWnd, overwrite) != SilentDialogResult::Yes) return std::nullopt;
    }

    std::wstring path;
    bool ok = false;
    switch (result.kind) {
    case ExportDialogKind::PdfAll:
        ok = hasChosenOutputPath
                 ? file_output::ExportPdfWithAnnotations(hWnd, true, result.outputPath,
                                                         result.standardTextAnnots, result.pdfScale,
                                                         result.matchPdfPaneTextLayout)
                 : file_output::ExportPdfWithAnnotations(hWnd, true, result.standardTextAnnots, result.pdfScale,
                                                         result.matchPdfPaneTextLayout, &path);
        break;
    case ExportDialogKind::PdfPages:
        ok = hasChosenOutputPath
                 ? file_output::ExportPdfPages(hWnd, result.pages, result.outputPath,
                                               result.standardTextAnnots, result.pdfScale,
                                               result.matchPdfPaneTextLayout)
                 : file_output::ExportPdfPages(hWnd, result.pages, result.standardTextAnnots, result.pdfScale,
                                               result.matchPdfPaneTextLayout, &path);
        break;
    case ExportDialogKind::PdfPng:
        ok = hasChosenOutputPath
                 ? file_output::ExportPdfPagePng(hWnd, result.pageIndex, result.outputPath,
                                                 result.pngStyle, result.includeAnnots,
                                                 result.pngWidthPx, result.pngHeightPx)
                 : file_output::ExportPdfPagePng(hWnd, result.pageIndex, result.pngStyle, result.includeAnnots,
                                                 result.pngWidthPx, result.pngHeightPx, &path);
        break;
    case ExportDialogKind::NoteText:
        ok = hasChosenOutputPath
                 ? file_output::ExportNotePlainText(g_currentNotePath, result.outputPath, result.textOptions)
                 : file_output::ExportNotePlainText(hWnd, result.textOptions, &path);
        break;
    case ExportDialogKind::NoteMarkup:
        ok = hasChosenOutputPath
                 ? file_output::ExportNoteMarkup(g_currentNotePath, result.outputPath, result.noteMarkupOptions)
                 : file_output::ExportNoteMarkup(hWnd, result.noteMarkupOptions, &path);
        break;
    }
    if (!ok) return std::nullopt;
    return hasChosenOutputPath ? std::optional<std::wstring>(result.outputPath)
                               : (path.empty() ? std::nullopt : std::optional<std::wstring>(std::move(path)));
}

} // namespace

struct ExportDialogState {
    HWND hwnd{};
    bool ok = false;
    bool done = false;
    bool hasPdf = false;
    bool hasNote = false;
    bool updatingSize = false;
    bool outputTargetInitialized = false;
    ExportDialogKind preset = ExportDialogKind::PdfAll;
    ExportDialogKind outputTargetKind = ExportDialogKind::PdfAll;
    std::wstring suggestedOutputName;
    std::vector<ExportDialogResult> reservedResults;
    std::vector<ExportDialogResult> committedResults;

    HWND topPdf{};
    HWND topNote{};
    HWND pdfAll{};
    HWND pdfPages{};
    HWND pdfPng{};
    HWND noteText{};
    HWND noteMarkup{};
    HWND labelFileExample{};
    HWND labelOutputFolder{};
    HWND editOutputFolder{};
    HWND btnBrowseOutputFolder{};
    HWND labelOutputName{};
    HWND editOutputName{};
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
    HWND btnUpdateReservation{};
    HWND btnSaveQuickPdf{};
    HWND btnSaveQuickNote{};
    HWND btnShowQuickSettings{};
    HWND btnClearReservations{};
    HWND btnExecuteReservation{};
    HWND btnMoveReservationUp{};
    HWND btnMoveReservationDown{};
    HWND btnRemoveReservation{};
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
constexpr int kExportDlgIdOutputFolder = 4111;
constexpr int kExportDlgIdBrowseOutputFolder = 4112;
constexpr int kExportDlgIdOutputName = 4113;
constexpr int kExportDlgIdSet = 4201;
constexpr int kExportDlgIdSaveQuickPdf = 4202;
constexpr int kExportDlgIdSaveQuickNote = 4203;
constexpr int kExportDlgIdClearReservations = 4204;
constexpr int kExportDlgIdMoveReservationUp = 4205;
constexpr int kExportDlgIdMoveReservationDown = 4206;
constexpr int kExportDlgIdRemoveReservation = 4207;
constexpr int kExportDlgIdUpdateReservation = 4208;
constexpr int kExportDlgIdExecuteReservation = 4209;
constexpr int kExportDlgIdReservationList = 4210;
constexpr int kExportDlgIdShowQuickSettings = 4211;

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
static void UpdateReservationButtonsUi(ExportDialogState* ctx);

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
    // Enter is the same as the explicit final action: start output.  Adding a
    // reservation is intentionally available only through its labelled button.
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

    if (ctx->labelOutSize) SetWindowTextW(ctx->labelOutSize, isPngOut ? L"画像品質・サイズ:" : L"出力サイズ:");
    if (ctx->radioOutSizeHalf) SetWindowTextW(ctx->radioOutSizeHalf, isPngOut ? L"72 DPI" : L"半分");
    if (ctx->radioOutSizeOne) SetWindowTextW(ctx->radioOutSizeOne, isPngOut ? L"144 DPI (標準)" : L"そのまま（推奨）");
    if (ctx->radioOutSizeTwo) SetWindowTextW(ctx->radioOutSizeTwo, isPngOut ? L"288 DPI" : L"倍");
    if (ctx->radioOutSizeCustom) SetWindowTextW(ctx->radioOutSizeCustom, L"指定");
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
        constexpr double kBaseDpi = static_cast<double>(file_output::kPdfPngDefaultDpi);
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
            const double dpi = (wPt > 0.0) ? (static_cast<double>(outW) * 72.0 / wPt) : kBaseDpi;
            const double megapixels = static_cast<double>(outW) * static_cast<double>(outH) / 1'000'000.0;
            std::wstring msg = L"可逆PNG / 約 " + FormatDoubleCompact(dpi, 0) + L" DPI / 約 " +
                               FormatDoubleCompact(megapixels, 1) + L" MP（上限 " +
                               FormatDoubleCompact(static_cast<double>(file_output::kPdfPngMaxPixels) / 1'000'000.0, 0) +
                               L" MP）";
            SetWindowTextW(ctx->labelOutSizeMm, msg.c_str());
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
    std::wstring label;
    switch (result.kind) {
    case ExportDialogKind::PdfAll:
        label = localization::Text(L"export.reservation_kind.pdf_all");
        break;
    case ExportDialogKind::PdfPages:
        label = localization::Text(L"export.reservation_kind.pdf_pages");
        break;
    case ExportDialogKind::PdfPng:
        label = localization::Text(L"export.reservation_kind.png");
        break;
    case ExportDialogKind::NoteText:
        label = localization::Text(result.textOptions.markupMode == file_output::MarkupMode::Simplified
                                       ? L"export.reservation_kind.note_text_simplified"
                                       : L"export.reservation_kind.note_text");
        break;
    case ExportDialogKind::NoteMarkup:
        label = localization::Text(result.noteMarkupOptions.format == file_output::NoteMarkupExportOptions::Format::Html
                                       ? L"export.reservation_kind.markup_html"
                                       : L"export.reservation_kind.markup_md");
        break;
    default:
        label = localization::Text(L"export.reservation_kind.unknown");
        break;
    }
    if (!result.outputPath.empty()) {
        label += L" \u2192 " + std::filesystem::path(result.outputPath).filename().wstring();
    }
    return label;
}

static void UpdateReservationSummaryUi(ExportDialogState* ctx) {
    if (!ctx || !ctx->labelReservationList) return;
    int selected = static_cast<int>(SendMessageW(ctx->labelReservationList, LB_GETCURSEL, 0, 0));
    SendMessageW(ctx->labelReservationList, LB_RESETCONTENT, 0, 0);
    if (ctx->reservedResults.empty()) {
        SendMessageW(ctx->labelReservationList, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(localization::Text(L"export.reservation_empty").c_str()));
        EnableWindow(ctx->labelReservationList, FALSE);
        UpdateReservationButtonsUi(ctx);
        return;
    }
    EnableWindow(ctx->labelReservationList, TRUE);
    for (size_t i = 0; i < ctx->reservedResults.size(); ++i) {
        const std::wstring text = std::to_wstring(i + 1) + L". " + ReservationLabelForResult(ctx->reservedResults[i]);
        SendMessageW(ctx->labelReservationList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (selected < 0) selected = 0;
    selected = std::min(selected, static_cast<int>(ctx->reservedResults.size()) - 1);
    SendMessageW(ctx->labelReservationList, LB_SETCURSEL, selected, 0);
    UpdateReservationButtonsUi(ctx);
}

static std::wstring OutputExtensionForKind(const ExportDialogState* ctx, ExportDialogKind kind) {
    switch (kind) {
    case ExportDialogKind::PdfAll:
    case ExportDialogKind::PdfPages:
        return L".pdf";
    case ExportDialogKind::PdfPng:
        return L".png";
    case ExportDialogKind::NoteText:
        return L".txt";
    case ExportDialogKind::NoteMarkup:
        return IsExportDlgChecked(ctx ? ctx->radioMarkupHtml : nullptr) ? L".html" : L".md";
    }
    return L"";
}

static std::wstring OutputExtensionForResult(const ExportDialogState* ctx, const ExportDialogResult& result) {
    if (result.kind == ExportDialogKind::NoteMarkup) {
        return result.noteMarkupOptions.format == file_output::NoteMarkupExportOptions::Format::Html
                   ? L".html"
                   : L".md";
    }
    return OutputExtensionForKind(ctx, result.kind);
}

static std::filesystem::path OutputSourcePathForKind(ExportDialogKind kind) {
    return IsPdfExportKind(kind) ? std::filesystem::path(CurrentLogicalPdfPath())
                                 : std::filesystem::path(g_currentNotePath);
}

static std::wstring DefaultOutputNameForKind(const ExportDialogState* ctx, ExportDialogKind kind) {
    const std::filesystem::path source = OutputSourcePathForKind(kind);
    std::wstring stem = source.stem().wstring();
    if (stem.empty()) stem = IsPdfExportKind(kind) ? L"document" : L"note";
    switch (kind) {
    case ExportDialogKind::PdfAll: return stem + L"_annotated.pdf";
    case ExportDialogKind::PdfPages: return stem + L"_pages.pdf";
    case ExportDialogKind::PdfPng: return stem + L"_page_1.png";
    case ExportDialogKind::NoteText: return stem + L".txt";
    case ExportDialogKind::NoteMarkup: return stem + OutputExtensionForKind(ctx, kind);
    }
    return stem;
}

static std::wstring DefaultOutputFolderForKind(ExportDialogKind kind) {
    const std::filesystem::path source = OutputSourcePathForKind(kind);
    if (!source.parent_path().empty()) return source.parent_path().wstring();
    return g_workspaceRoot;
}

static void ResetSuggestedOutputTarget(ExportDialogState* ctx, ExportDialogKind kind) {
    if (!ctx || !ctx->editOutputFolder || !ctx->editOutputName) return;
    const std::wstring folder = DefaultOutputFolderForKind(kind);
    const std::wstring name = DefaultOutputNameForKind(ctx, kind);
    SetWindowTextW(ctx->editOutputFolder, folder.c_str());
    SetWindowTextW(ctx->editOutputName, name.c_str());
    ctx->outputTargetInitialized = true;
    ctx->outputTargetKind = kind;
    ctx->suggestedOutputName = name;
}

static bool BuildExportOutputTarget(ExportDialogState* ctx, ExportDialogResult& result) {
    if (!ctx || !ctx->editOutputFolder || !ctx->editOutputName) return false;
    const std::wstring folderText = TrimWhitespace(ReadDialogText(ctx->editOutputFolder));
    std::wstring name = TrimWhitespace(ReadDialogText(ctx->editOutputName));
    if (folderText.empty()) {
        return RejectExportDialogInput(ctx, localization::Text(L"export.output_folder_required"),
                                       ctx->editOutputFolder, true);
    }
    if (name.empty()) {
        return RejectExportDialogInput(ctx, localization::Text(L"export.output_name_required"),
                                       ctx->editOutputName, true);
    }
    const std::filesystem::path folder(folderText);
    std::error_code ec;
    if (!folder.is_absolute() || !std::filesystem::is_directory(folder, ec) || ec) {
        return RejectExportDialogInput(ctx, localization::Text(L"export.output_folder_invalid"),
                                       ctx->editOutputFolder, true);
    }
    const std::filesystem::path fileName(name);
    if (fileName.has_parent_path() || fileName.has_root_name() || fileName.has_root_directory() ||
        fileName.filename() != fileName || name == L"." || name == L"..") {
        return RejectExportDialogInput(ctx, localization::Text(L"export.output_name_invalid"),
                                       ctx->editOutputName, true);
    }
    const std::wstring expectedExtension = OutputExtensionForResult(ctx, result);
    std::wstring extension = fileName.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension.empty()) {
        name += expectedExtension;
        SetWindowTextW(ctx->editOutputName, name.c_str());
    } else if (extension != expectedExtension) {
        return RejectExportDialogInput(ctx,
                                       localization::Format(L"export.output_extension_invalid",
                                                            {{L"EXT", expectedExtension}}),
                                       ctx->editOutputName, true);
    }
    result.outputPath = (folder / name).lexically_normal().wstring();
    const std::filesystem::path source = OutputSourcePathForKind(result.kind);
    if (!source.empty()) {
        const auto normalizedSource = source.lexically_normal().wstring();
        if (CompareStringOrdinal(result.outputPath.c_str(), -1, normalizedSource.c_str(), -1, TRUE) == CSTR_EQUAL) {
            return RejectExportDialogInput(ctx, localization::Text(L"export.output_original_forbidden"),
                                           ctx->editOutputName, true);
        }
    }
    return !result.outputPath.empty();
}

static void UpdateReservationButtonsUi(ExportDialogState* ctx) {
    if (!ctx) return;
    const int selected = ctx->labelReservationList
                             ? static_cast<int>(SendMessageW(ctx->labelReservationList, LB_GETCURSEL, 0, 0))
                             : LB_ERR;
    const int count = static_cast<int>(ctx->reservedResults.size());
    EnableWindow(ctx->btnMoveReservationUp, selected > 0 && selected < count);
    EnableWindow(ctx->btnMoveReservationDown, selected >= 0 && selected + 1 < count);
    EnableWindow(ctx->btnRemoveReservation, selected >= 0 && selected < count);
    EnableWindow(ctx->btnUpdateReservation, selected >= 0 && selected < count);
    EnableWindow(ctx->btnExecuteReservation, selected >= 0 && selected < count);
    EnableWindow(ctx->btnClearReservations, count > 0);
}

static void LoadSelectedReservationOutputTarget(ExportDialogState* ctx) {
    if (!ctx || !ctx->labelReservationList || !ctx->editOutputFolder || !ctx->editOutputName) return;
    const int selected = static_cast<int>(SendMessageW(ctx->labelReservationList, LB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(ctx->reservedResults.size())) return;
    const std::filesystem::path output(ctx->reservedResults[static_cast<size_t>(selected)].outputPath);
    SetWindowTextW(ctx->editOutputFolder, output.parent_path().wstring().c_str());
    SetWindowTextW(ctx->editOutputName, output.filename().wstring().c_str());
    ctx->outputTargetInitialized = true;
    ctx->outputTargetKind = ctx->reservedResults[static_cast<size_t>(selected)].kind;
    ctx->suggestedOutputName.clear();
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
    const std::wstring suggestedName = DefaultOutputNameForKind(ctx, kind);
    if (!ctx->outputTargetInitialized || ctx->outputTargetKind != kind) {
        ResetSuggestedOutputTarget(ctx, kind);
    } else if (ReadDialogText(ctx->editOutputName) == ctx->suggestedOutputName &&
               suggestedName != ctx->suggestedOutputName) {
        SetWindowTextW(ctx->editOutputName, suggestedName.c_str());
        ctx->suggestedOutputName = suggestedName;
    }
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
    ShowWindow(ctx->labelOutSizeMm, showOutSize ? SW_SHOW : SW_HIDE);
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
        return RejectExportDialogInput(ctx, localization::Text(L"export.pdf_not_open"));
    }
    if (IsNoteExportKind(kind) && !ctx->hasNote) {
        return RejectExportDialogInput(ctx, localization::Text(L"export.note_not_open"));
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
            return RejectExportDialogInput(ctx, localization::Text(L"export.output_size_calculation_failed"));
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
                return RejectExportDialogInput(ctx, localization::Text(L"export.output_size_pt_required"), ctx->editOutSizeW, true);
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
            return RejectExportDialogInput(ctx, localization::Text(L"export.output_size_out_of_range"), ctx->editOutSizeW, true);
        }
        result.pdfScale = scale;
    } else if (kind == ExportDialogKind::PdfPng) {
        constexpr double kBaseDpi = static_cast<double>(file_output::kPdfPngDefaultDpi);
        int refPage = ReferencePageIndexForSizePreview(ctx, kind);
        double wPt = 0.0, hPt = 0.0;
        if (!TryGetPageSizePt(refPage, wPt, hPt)) {
            return RejectExportDialogInput(ctx, localization::Text(L"export.output_size_calculation_failed"));
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
                return RejectExportDialogInput(ctx, localization::Text(L"export.output_size_px_required"), ctx->editOutSizeW, true);
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
            return RejectExportDialogInput(ctx, localization::Text(L"export.output_size_out_of_range"), ctx->editOutSizeW, true);
        }

        uint64_t total = static_cast<uint64_t>(outW) * static_cast<uint64_t>(outH);
        if (outW <= 0 || outH <= 0 ||
            outW > file_output::kPdfPngMaxDimensionPx ||
            outH > file_output::kPdfPngMaxDimensionPx ||
            total == 0 || total > file_output::kPdfPngMaxPixels) {
            return RejectExportDialogInput(ctx, localization::Text(L"export.png_size_too_large"), ctx->editOutSizeW, true);
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
            return RejectExportDialogInput(ctx, localization::Text(L"export.page_spec_empty"), ctx->editPageSpec, true);
        }
        bool defaultAnnot = IsExportDlgChecked(ctx->radioAnnotYes);
        std::wstring err;
        auto pages = file_output::ParsePdfPageSpec(spec, defaultAnnot, &err);
        if (pages.empty()) {
            std::wstring msg = err.empty() ? localization::Text(L"export.page_spec_invalid") : err;
            return RejectExportDialogInput(ctx, msg, ctx->editPageSpec, true);
        }
        result.pages = std::move(pages);
    } else if (kind == ExportDialogKind::PdfPng) {
        std::wstring input = ReadDialogText(ctx->editPageNumber);
        int pageNo = 0;
        if (!ParsePositiveInt(input, pageNo)) {
            return RejectExportDialogInput(ctx, localization::Text(L"export.page_number_invalid"), ctx->editPageNumber, true);
        }
        if (g_pdf.doc) {
            int count = 0;
            {
                std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
                count = FPDF_GetPageCount(g_pdf.doc);
            }
            if (pageNo > count) {
                return RejectExportDialogInput(ctx, localization::Text(L"export.page_number_out_of_range"), ctx->editPageNumber, true);
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

static bool SaveQuickPdfSettings(ExportDialogState* ctx) {
    ExportDialogResult result{};
    if (!BuildExportDialogResult(ctx, result)) return false;
    if (result.kind != ExportDialogKind::PdfAll) {
        return RejectExportDialogInput(ctx, localization::Text(L"export.quick_pdf_requires"));
    }
    g_config.quickPdfScalePercent = std::clamp(static_cast<int>(std::lround(result.pdfScale * 100.0)), 13, 800);
    g_config.quickPdfStandardTextAnnots = result.standardTextAnnots;
    g_config.quickPdfMatchPdfPaneTextLayout = result.matchPdfPaneTextLayout;
    if (!g_workspaceRoot.empty()) SaveWorkspaceConfig(g_workspaceRoot, g_config);
    ShowSoftNotice(ctx->hwnd, localization::Text(L"export.quick_settings_saved"), SoftNoticeKind::Info);
    return true;
}

static bool SaveQuickNoteSettings(ExportDialogState* ctx) {
    ExportDialogResult result{};
    if (!BuildExportDialogResult(ctx, result)) return false;
    if (result.kind != ExportDialogKind::NoteText) {
        return RejectExportDialogInput(ctx, localization::Text(L"export.quick_note_requires"));
    }
    g_config.quickNoteStripMarkup = result.textOptions.markupMode == file_output::MarkupMode::Simplified;
    g_config.quickNoteIncludeComments = result.textOptions.includeCommentLines;
    g_config.quickNoteMathPlaceholder = result.textOptions.mathMode == file_output::MathMode::Placeholder;
    if (g_config.quickNoteMathPlaceholder) {
        std::wstring placeholder = TrimWhitespace(UTF8ToWide(result.textOptions.mathPlaceholder));
        if (placeholder.empty()) placeholder = L"[math]";
        if (placeholder.size() > 512) placeholder.resize(512);
        g_config.quickNoteMathPlaceholderText = std::move(placeholder);
    }
    if (!g_workspaceRoot.empty()) SaveWorkspaceConfig(g_workspaceRoot, g_config);
    ShowSoftNotice(ctx->hwnd, localization::Text(L"export.quick_settings_saved"), SoftNoticeKind::Info);
    return true;
}

static bool SameOutputPath(const std::wstring& lhs, const std::wstring& rhs) {
    if (lhs.empty() || rhs.empty()) return false;
    const std::wstring normalizedLhs = std::filesystem::path(lhs).lexically_normal().wstring();
    const std::wstring normalizedRhs = std::filesystem::path(rhs).lexically_normal().wstring();
    return CompareStringOrdinal(normalizedLhs.c_str(), -1, normalizedRhs.c_str(), -1, TRUE) == CSTR_EQUAL;
}

static bool ReservationTargetConflicts(const ExportDialogState* ctx,
                                       const ExportDialogResult& result,
                                       int ignoredIndex = -1) {
    if (!ctx) return false;
    for (int i = 0; i < static_cast<int>(ctx->reservedResults.size()); ++i) {
        if (i == ignoredIndex) continue;
        if (SameOutputPath(ctx->reservedResults[static_cast<size_t>(i)].outputPath, result.outputPath)) return true;
    }
    return false;
}

static bool StoreReservation(ExportDialogState* ctx, const ExportDialogResult& result) {
    if (!ctx) return false;
    if (ReservationTargetConflicts(ctx, result)) {
        return RejectExportDialogInput(ctx, localization::Text(L"export.reservation_duplicate_destination"),
                                       ctx->editOutputName, true);
    }
    ctx->reservedResults.push_back(result);
    UpdateReservationSummaryUi(ctx);
    return true;
}

static void SwitchCategoryAfterSet(ExportDialogState* ctx, ExportDialogKind justStoredKind) {
    if (!ctx) return;
    if (IsPdfExportKind(justStoredKind) && ctx->hasNote) {
        CheckRadioButton(ctx->hwnd, kExportDlgIdTopPdf, kExportDlgIdTopNote, kExportDlgIdTopNote);
        UpdateExportDialogUi(ctx);
    } else if (IsNoteExportKind(justStoredKind) && ctx->hasPdf) {
        CheckRadioButton(ctx->hwnd, kExportDlgIdTopPdf, kExportDlgIdTopNote, kExportDlgIdTopPdf);
        UpdateExportDialogUi(ctx);
    }
    // The destination controls now describe the next output, not the last
    // reservation. Require an explicit list selection before allowing update.
    if (ctx->labelReservationList) SendMessageW(ctx->labelReservationList, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    UpdateReservationButtonsUi(ctx);
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
        const int outputFolderY = 100;
        const int outputNameY = 128;
        const int optY = 164;
        const int outSizeY = optY + 84;
        const int outSizeMmY = outSizeY + 52;
        const int paperY = outSizeY + 74;
        const int pngStyleY = outSizeY + 102;
        const int stdTextY = outSizeY + 130;
        const int buttonsY = outSizeY + 192;
        const int quickButtonsY = buttonsY + 36;
        const int reservationTitleY = quickButtonsY + 34;
        const int reservationListY = reservationTitleY + 20;

        ctx->topPdf = CreateWindowExW(0, L"BUTTON", L"PDF",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                      margin, row1Y, 80, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdTopPdf),
                                      cs->hInstance, nullptr);
        ctx->topNote = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.note").c_str(),
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                       margin + 90, row1Y, 80, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdTopNote),
                                       cs->hInstance, nullptr);

        ctx->pdfAll = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.annotated_pdf").c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                      margin, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPdfAll),
                                      cs->hInstance, nullptr);
        ctx->pdfPages = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.page_selection").c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                        margin + 160, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPdfPages),
                                        cs->hInstance, nullptr);
        ctx->pdfPng = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.single_page_png").c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                      margin + 320, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPdfPng),
                                      cs->hInstance, nullptr);

        ctx->noteText = CreateWindowExW(0, L"BUTTON", L"txt",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                        margin, row2Y, 100, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdNoteText),
                                        cs->hInstance, nullptr);
        ctx->noteMarkup = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.markup").c_str(),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                          margin + 110, row2Y, 150, rowH, hWnd, reinterpret_cast<HMENU>(kExportDlgIdNoteMarkup),
                                          cs->hInstance, nullptr);

        ctx->labelFileExample = CreateWindowExW(0, L"STATIC", L"",
                                                WS_CHILD | WS_VISIBLE,
                                                margin, fileY, 460, 20, hWnd, nullptr,
                                                cs->hInstance, nullptr);
        ctx->labelOutputFolder = CreateWindowExW(0, L"STATIC", localization::Text(L"export.output_folder_label").c_str(),
                                                 WS_CHILD | WS_VISIBLE,
                                                 margin, outputFolderY, 90, 20, hWnd, nullptr,
                                                 cs->hInstance, nullptr);
        ctx->editOutputFolder = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                margin + 95, outputFolderY - 2, 290, 22, hWnd,
                                                reinterpret_cast<HMENU>(kExportDlgIdOutputFolder),
                                                cs->hInstance, nullptr);
        ctx->btnBrowseOutputFolder = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.browse").c_str(),
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                     margin + 392, outputFolderY - 2, 76, 22, hWnd,
                                                     reinterpret_cast<HMENU>(kExportDlgIdBrowseOutputFolder),
                                                     cs->hInstance, nullptr);
        ctx->labelOutputName = CreateWindowExW(0, L"STATIC", localization::Text(L"export.output_name_label").c_str(),
                                               WS_CHILD | WS_VISIBLE,
                                               margin, outputNameY, 90, 20, hWnd, nullptr,
                                               cs->hInstance, nullptr);
        ctx->editOutputName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                              margin + 95, outputNameY - 2, 373, 22, hWnd,
                                              reinterpret_cast<HMENU>(kExportDlgIdOutputName),
                                              cs->hInstance, nullptr);

        ctx->labelPageSpec = CreateWindowExW(0, L"STATIC", localization::Text(L"export.page_spec_label").c_str(),
                                             WS_CHILD | WS_VISIBLE,
                                             margin, optY, 80, 20, hWnd, nullptr,
                                             cs->hInstance, nullptr);
        ctx->editPageSpec = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                            margin + 90, optY - 2, 220, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPageSpec),
                                            cs->hInstance, nullptr);

        ctx->labelPageNumber = CreateWindowExW(0, L"STATIC", localization::Text(L"export.page_number_label").c_str(),
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

        ctx->labelAnnot = CreateWindowExW(0, L"STATIC", localization::Text(L"export.annotation_label").c_str(),
                                          WS_CHILD | WS_VISIBLE,
                                          margin, optY + 52, 80, 20, hWnd, nullptr,
                                          cs->hInstance, nullptr);
        ctx->radioAnnotYes = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.include").c_str(),
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                             margin + 90, optY + 50, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdAnnotYes),
                                             cs->hInstance, nullptr);
        ctx->radioAnnotNo = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.exclude").c_str(),
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                            margin + 200, optY + 50, 110, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdAnnotNo),
                                            cs->hInstance, nullptr);

        int styleY = pngStyleY;
        ctx->labelPngStyle = CreateWindowExW(0, L"STATIC", localization::Text(L"export.image_style_label").c_str(),
                                             WS_CHILD | WS_VISIBLE,
                                             margin, styleY, 100, 20, hWnd, nullptr,
                                             cs->hInstance, nullptr);
        ctx->radioPngStylePdf = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.image_style_pdf").c_str(),
                                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                                 margin + 90, styleY, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPngStylePdf),
                                                 cs->hInstance, nullptr);
        ctx->radioPngStyleViewer = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.image_style_viewer").c_str(),
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                                    margin + 200, styleY, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPngStyleViewer),
                                                    cs->hInstance, nullptr);

        ctx->labelOutSize = CreateWindowExW(0, L"STATIC", localization::Text(L"export.output_size_label").c_str(),
                                            WS_CHILD | WS_VISIBLE,
                                            margin, outSizeY, 100, 20, hWnd, nullptr,
                                            cs->hInstance, nullptr);
        ctx->radioOutSizeHalf = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.half").c_str(),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                                margin + 90, outSizeY, 70, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeHalf),
                                                cs->hInstance, nullptr);
        ctx->radioOutSizeOne = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.original_recommended").c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                               margin + 170, outSizeY, 130, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeOne),
                                               cs->hInstance, nullptr);
        ctx->radioOutSizeTwo = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.double").c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                               margin + 310, outSizeY, 50, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeTwo),
                                               cs->hInstance, nullptr);
        ctx->radioOutSizeCustom = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.custom").c_str(),
                                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                                  margin + 370, outSizeY, 60, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeCustom),
                                                  cs->hInstance, nullptr);

        ctx->labelOutSizeW = CreateWindowExW(0, L"STATIC", localization::Text(L"export.width_label").c_str(),
                                             WS_CHILD | WS_VISIBLE,
                                             margin, outSizeY + 28, 60, 20, hWnd, nullptr,
                                             cs->hInstance, nullptr);
        ctx->editOutSizeW = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                            margin + 65, outSizeY + 26, 90, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdOutSizeW),
                                            cs->hInstance, nullptr);
        ctx->labelOutSizeH = CreateWindowExW(0, L"STATIC", localization::Text(L"export.height_label").c_str(),
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

        ctx->labelPaper = CreateWindowExW(0, L"STATIC", localization::Text(L"export.paper_label").c_str(),
                                          WS_CHILD | WS_VISIBLE,
                                          margin, paperY, 50, 20, hWnd, nullptr,
                                          cs->hInstance, nullptr);
        ctx->comboPaper = CreateWindowExW(0, L"COMBOBOX", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          margin + 55, paperY - 2, 150, 200, hWnd, reinterpret_cast<HMENU>(kExportDlgIdPaperCombo),
                                          cs->hInstance, nullptr);

        ctx->checkStandardText = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.standard_text_annotation").c_str(),
                                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                 margin, stdTextY, 460, 22, hWnd,
                                                 reinterpret_cast<HMENU>(kExportDlgIdStandardText),
                                                 cs->hInstance, nullptr);
        ctx->checkMatchPdfPaneTextLayout = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.match_pdf_text_layout").c_str(),
                                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                            margin, stdTextY + 24, 460, 22, hWnd,
                                                            reinterpret_cast<HMENU>(kExportDlgIdMatchPdfPaneTextLayout),
                                                            cs->hInstance, nullptr);

        ctx->labelMath = CreateWindowExW(0, L"STATIC", localization::Text(L"export.math_label").c_str(),
                                         WS_CHILD | WS_VISIBLE,
                                         margin, optY, 80, 20, hWnd, nullptr,
                                         cs->hInstance, nullptr);
        ctx->radioMathKeep = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.keep").c_str(),
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                             margin + 90, optY - 2, 100, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdMathKeep),
                                             cs->hInstance, nullptr);
        ctx->radioMathReplace = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.replace").c_str(),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                                margin + 200, optY - 2, 120, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdMathReplace),
                                                cs->hInstance, nullptr);
        ctx->labelMathPlaceholder = CreateWindowExW(0, L"STATIC", localization::Text(L"export.replacement_label").c_str(),
                                                    WS_CHILD | WS_VISIBLE,
                                                    margin, optY + 28, 100, 20, hWnd, nullptr,
                                                    cs->hInstance, nullptr);
        ctx->editMathPlaceholder = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"[math]",
                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                   margin + 110, optY + 26, 200, 22, hWnd, reinterpret_cast<HMENU>(kExportDlgIdMathPlaceholder),
                                                   cs->hInstance, nullptr);

        ctx->checkIncludeComments = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.include_comments").c_str(),
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                    margin, optY + 52, 220, 22, hWnd,
                                                    reinterpret_cast<HMENU>(kExportDlgIdIncludeComments),
                                                    cs->hInstance, nullptr);

        ctx->checkStripMarkup = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.strip_markup").c_str(),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                margin, optY + 78, 260, 22, hWnd,
                                                reinterpret_cast<HMENU>(kExportDlgIdStripMarkup),
                                                cs->hInstance, nullptr);

        ctx->labelMarkupFormat = CreateWindowExW(0, L"STATIC", localization::Text(L"export.format_label").c_str(),
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

        ctx->checkTitleHeading = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.note_title_heading").c_str(),
                                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                 margin, optY + 104, 320, 22, hWnd,
                                                 reinterpret_cast<HMENU>(kExportDlgIdTitleHeading),
                                                 cs->hInstance, nullptr);
        ctx->checkShiftHeadings = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.shift_headings").c_str(),
                                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                  margin, optY + 132, 260, 22, hWnd,
                                                  reinterpret_cast<HMENU>(kExportDlgIdShiftHeadings),
                                                  cs->hInstance, nullptr);

        ctx->btnSet = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.add_to_list").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      170, buttonsY, 100, 26, hWnd, reinterpret_cast<HMENU>(kExportDlgIdSet),
                                      cs->hInstance, nullptr);
        ctx->btnUpdateReservation = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.update_reservation").c_str(),
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    280, buttonsY, 100, 26, hWnd,
                                                    reinterpret_cast<HMENU>(kExportDlgIdUpdateReservation),
                                                    cs->hInstance, nullptr);
        ctx->labelInlineError = CreateWindowExW(0, L"STATIC", L"",
                                                WS_CHILD,
                                                margin, buttonsY - 6, 148, 40, hWnd, nullptr,
                                                cs->hInstance, nullptr);
        HWND okBtn = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.execute").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     390, buttonsY, 80, 26, hWnd, reinterpret_cast<HMENU>(IDOK),
                                     cs->hInstance, nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", localization::Text(L"common.cancel").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         480, buttonsY, 80, 26, hWnd, reinterpret_cast<HMENU>(IDCANCEL),
                                         cs->hInstance, nullptr);
        ctx->btnSaveQuickPdf = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.save_quick_pdf").c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               margin, quickButtonsY, 145, 26, hWnd,
                                               reinterpret_cast<HMENU>(kExportDlgIdSaveQuickPdf), cs->hInstance, nullptr);
        ctx->btnSaveQuickNote = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.save_quick_note").c_str(),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                margin + 155, quickButtonsY, 145, 26, hWnd,
                                                reinterpret_cast<HMENU>(kExportDlgIdSaveQuickNote), cs->hInstance, nullptr);
        ctx->btnShowQuickSettings = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.show_quick_settings").c_str(),
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                     margin + 310, quickButtonsY, 155, 26, hWnd,
                                                     reinterpret_cast<HMENU>(kExportDlgIdShowQuickSettings), cs->hInstance, nullptr);
        ctx->labelReservationTitle = CreateWindowExW(0, L"STATIC", localization::Text(L"export.reservation_list_title").c_str(),
                                                     WS_CHILD | WS_VISIBLE,
                                                     margin, reservationTitleY, 540, 20, hWnd, nullptr,
                                                     cs->hInstance, nullptr);
        ctx->labelReservationList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                                                    margin, reservationListY, 540, 66, hWnd,
                                                    reinterpret_cast<HMENU>(kExportDlgIdReservationList),
                                                    cs->hInstance, nullptr);
        const int reservationActionsY = reservationListY + 70;
        ctx->btnExecuteReservation = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.execute_selected").c_str(),
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                     margin, reservationActionsY, 110, 24, hWnd,
                                                     reinterpret_cast<HMENU>(kExportDlgIdExecuteReservation), cs->hInstance, nullptr);
        ctx->btnMoveReservationUp = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.move_up").c_str(),
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    margin + 120, reservationActionsY, 70, 24, hWnd,
                                                    reinterpret_cast<HMENU>(kExportDlgIdMoveReservationUp), cs->hInstance, nullptr);
        ctx->btnMoveReservationDown = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.move_down").c_str(),
                                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                      margin + 200, reservationActionsY, 70, 24, hWnd,
                                                      reinterpret_cast<HMENU>(kExportDlgIdMoveReservationDown), cs->hInstance, nullptr);
        ctx->btnRemoveReservation = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.remove_selected").c_str(),
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                     margin + 280, reservationActionsY, 110, 24, hWnd,
                                                     reinterpret_cast<HMENU>(kExportDlgIdRemoveReservation), cs->hInstance, nullptr);
        ctx->btnClearReservations = CreateWindowExW(0, L"BUTTON", localization::Text(L"export.clear_list").c_str(),
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                     margin + 400, reservationActionsY, 120, 24, hWnd,
                                                     reinterpret_cast<HMENU>(kExportDlgIdClearReservations), cs->hInstance, nullptr);

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
        applyFont(ctx->labelOutputFolder);
        applyFont(ctx->editOutputFolder);
        applyFont(ctx->btnBrowseOutputFolder);
        applyFont(ctx->labelOutputName);
        applyFont(ctx->editOutputName);
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
        applyFont(ctx->btnUpdateReservation);
        applyFont(ctx->btnSaveQuickPdf);
        applyFont(ctx->btnSaveQuickNote);
        applyFont(ctx->btnShowQuickSettings);
        applyFont(ctx->btnClearReservations);
        applyFont(ctx->btnExecuteReservation);
        applyFont(ctx->btnMoveReservationUp);
        applyFont(ctx->btnMoveReservationDown);
        applyFont(ctx->btnRemoveReservation);
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
        attachSilentEdit(ctx->editOutputFolder);
        attachSilentEdit(ctx->editOutputName);
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
            if (ctx && !ctx->reservedResults.empty()) {
                ctx->committedResults = ctx->reservedResults;
                ctx->ok = !ctx->committedResults.empty();
                ctx->done = true;
                DestroyWindow(hWnd);
            } else {
                ExportDialogResult current{};
                if (!BuildExportDialogResult(ctx, current)) return 0;
                if (!BuildExportOutputTarget(ctx, current)) return 0;
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
            if (!BuildExportOutputTarget(ctx, current)) return 0;
            if (!StoreReservation(ctx, current)) return 0;
            SwitchCategoryAfterSet(ctx, current.kind);
            return 0;
        }
        if (id == kExportDlgIdExecuteReservation) {
            if (!ctx || !ctx->labelReservationList) return 0;
            const int selected = static_cast<int>(SendMessageW(ctx->labelReservationList, LB_GETCURSEL, 0, 0));
            if (selected < 0 || selected >= static_cast<int>(ctx->reservedResults.size())) return 0;
            ctx->committedResults.clear();
            ctx->committedResults.push_back(ctx->reservedResults[static_cast<size_t>(selected)]);
            ctx->ok = true;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == kExportDlgIdUpdateReservation) {
            if (!ctx || !ctx->labelReservationList) return 0;
            const int selected = static_cast<int>(SendMessageW(ctx->labelReservationList, LB_GETCURSEL, 0, 0));
            if (selected < 0 || selected >= static_cast<int>(ctx->reservedResults.size())) return 0;
            ExportDialogResult updated = ctx->reservedResults[static_cast<size_t>(selected)];
            if (!BuildExportOutputTarget(ctx, updated)) return 0;
            if (ReservationTargetConflicts(ctx, updated, selected)) {
                RejectExportDialogInput(ctx, localization::Text(L"export.reservation_duplicate_destination"),
                                        ctx->editOutputName, true);
                return 0;
            }
            ctx->reservedResults[static_cast<size_t>(selected)].outputPath = std::move(updated.outputPath);
            UpdateReservationSummaryUi(ctx);
            SendMessageW(ctx->labelReservationList, LB_SETCURSEL, selected, 0);
            UpdateReservationButtonsUi(ctx);
            return 0;
        }
        if (id == kExportDlgIdBrowseOutputFolder) {
            if (!ctx) return 0;
            const auto kind = GetExportDialogKind(ctx);
            std::filesystem::path initial(TrimWhitespace(ReadDialogText(ctx->editOutputFolder)));
            if (initial.empty()) initial = std::filesystem::path(DefaultOutputFolderForKind(kind));
            std::wstring name = TrimWhitespace(ReadDialogText(ctx->editOutputName));
            if (name.empty()) name = DefaultOutputNameForKind(ctx, kind);
            auto picked = file_output::PickExportDestination(hWnd, localization::Text(L"export.select_output_path"),
                                                             initial, name, OutputExtensionForKind(ctx, kind).substr(1));
            if (picked) {
                const std::filesystem::path output(*picked);
                SetWindowTextW(ctx->editOutputFolder, output.parent_path().c_str());
                SetWindowTextW(ctx->editOutputName, output.filename().c_str());
            }
            return 0;
        }
        if (id == kExportDlgIdSaveQuickPdf) {
            (void)SaveQuickPdfSettings(ctx);
            return 0;
        }
        if (id == kExportDlgIdSaveQuickNote) {
            (void)SaveQuickNoteSettings(ctx);
            return 0;
        }
        if (id == kExportDlgIdShowQuickSettings) {
            ShowQuickExportSettingsDialog(hWnd);
            return 0;
        }
        if (id == kExportDlgIdClearReservations) {
            if (!ctx) return 0;
            ctx->reservedResults.clear();
            UpdateReservationSummaryUi(ctx);
            return 0;
        }
        if (id == kExportDlgIdMoveReservationUp || id == kExportDlgIdMoveReservationDown ||
            id == kExportDlgIdRemoveReservation) {
            if (!ctx) return 0;
            const int selected = static_cast<int>(SendMessageW(ctx->labelReservationList, LB_GETCURSEL, 0, 0));
            const int count = static_cast<int>(ctx->reservedResults.size());
            if (selected < 0 || selected >= count) return 0;
            if (id == kExportDlgIdMoveReservationUp && selected > 0) {
                std::swap(ctx->reservedResults[static_cast<size_t>(selected)],
                          ctx->reservedResults[static_cast<size_t>(selected - 1)]);
                UpdateReservationSummaryUi(ctx);
                SendMessageW(ctx->labelReservationList, LB_SETCURSEL, selected - 1, 0);
            } else if (id == kExportDlgIdMoveReservationDown && selected + 1 < count) {
                std::swap(ctx->reservedResults[static_cast<size_t>(selected)],
                          ctx->reservedResults[static_cast<size_t>(selected + 1)]);
                UpdateReservationSummaryUi(ctx);
                SendMessageW(ctx->labelReservationList, LB_SETCURSEL, selected + 1, 0);
            } else if (id == kExportDlgIdRemoveReservation) {
                ctx->reservedResults.erase(ctx->reservedResults.begin() + selected);
                UpdateReservationSummaryUi(ctx);
            }
            UpdateReservationButtonsUi(ctx);
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
        if (id == kExportDlgIdReservationList && HIWORD(wParam) == LBN_SELCHANGE) {
            LoadSelectedReservationOutputTarget(ctx);
            UpdateReservationButtonsUi(ctx);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED) {
            ClearExportDialogInlineError(ctx);
            const bool changesOutputKind = id == kExportDlgIdTopPdf || id == kExportDlgIdTopNote ||
                                           id == kExportDlgIdPdfAll || id == kExportDlgIdPdfPages ||
                                           id == kExportDlgIdPdfPng || id == kExportDlgIdNoteText ||
                                           id == kExportDlgIdNoteMarkup;
            if (changesOutputKind && ctx && ctx->labelReservationList) {
                // The fields become a new output's defaults after a format
                // change, so they must not still be presented as an edit of
                // the previously selected reservation.
                SendMessageW(ctx->labelReservationList, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                UpdateReservationButtonsUi(ctx);
            }
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
    case WM_DESTROY:
        UnregisterAppExitDialog(hWnd);
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
    ScopedExportDialogOwner modalOwner(owner);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, dialogTitle.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 580, 670,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return false;
    RegisterAppExitBlockingDialog(w);
    PlaceOwnedPopupAtAppTopLeft(w, owner);
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
    (void)ExecuteUnifiedExportAndGetPath(hWnd, result);
}

void ExecuteUnifiedExports(HWND hWnd, const std::vector<ExportDialogResult>& results) {
    if (results.empty()) return;
    if (!s_pendingExportsAfterSave.empty()) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"dialog.export.641e268761e7").c_str(),
                       SoftNoticeKind::Info);
        return;
    }

    const file_output::SaveTransactionStartResult start =
        file_output::StartBackgroundSaveAndIntegrateTransaction(hWnd);
    if (start == file_output::SaveTransactionStartResult::Failed) return;
    if (start == file_output::SaveTransactionStartResult::Started) {
        s_pendingExportsAfterSave = results;
        ShowSoftNotice(hWnd,
                       localization::Text(L"dialog.export.c74d6b71c5de").c_str(),
                       SoftNoticeKind::Info);
        return;
    }

    FinalizeManualSaveUi(hWnd, /*updateWindowTitleAfterSave=*/true);
    std::vector<std::wstring> savedPaths;
    for (const auto& result : results) {
        if (auto path = ExecuteUnifiedExportAndGetPath(hWnd, result)) savedPaths.push_back(std::move(*path));
    }
    ShowExportResultsDialog(hWnd, savedPaths);
}

void CompleteUnifiedExportsAfterSave(HWND hWnd, bool saveSucceeded, bool saveRestarted) {
    if (s_pendingExportsAfterSave.empty()) return;
    if (!saveSucceeded) {
        s_pendingExportsAfterSave.clear();
        ShowSoftNotice(hWnd,
                       localization::Text(L"dialog.export.e434d92fc856").c_str(),
                       SoftNoticeKind::Warning);
        return;
    }
    // A newer edit arrived while saving; wait for the restarted transaction so the
    // output always reflects the final integrated state.
    if (saveRestarted) return;

    std::vector<ExportDialogResult> results = std::move(s_pendingExportsAfterSave);
    s_pendingExportsAfterSave.clear();
    std::vector<std::wstring> savedPaths;
    for (const auto& result : results) {
        if (auto path = ExecuteUnifiedExportAndGetPath(hWnd, result)) savedPaths.push_back(std::move(*path));
    }
    ShowExportResultsDialog(hWnd, savedPaths);
}



