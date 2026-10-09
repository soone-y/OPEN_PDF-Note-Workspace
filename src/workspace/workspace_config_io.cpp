#include "workspace/workspace_config_io.h"
#include "app/main_escape_backup.h"
#include "core/localization.h"
#include "core/app_core.h"
#include "core/json_string.h"
#include "ui/core/main_window_api.h"
#include "ui/dialogs/dialogs.h"
#include "clrop/bridge.h"
#include "file_output/file_output.h"
#include "pdf_view/pdf_view.h"
#include "core/preview_trace.h"
#include "core/atomic_write.h"
#include "diagnostics/normal_operations.h"
#include "settings/settings.h"
#include "bridge/view_bridge.h"
#include "core/font_list.h"
#include "math/math_render.h"
#include "core/fault_injection.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <shlobj.h>
#include <shobjidl.h>
#include <sstream>
#include <unordered_set>
#include <vector>

// file: main/workspace_config.cppinc
// NOTE: Included by workspace_controller.cppinc. Keep configuration/state helpers
// here so workspace loading and user actions can share the same translation unit.
static bool ApplyWindowSizeFromConfig(HWND hWnd) {
    if (!hWnd) return false;
    if (g_config.windowWidth <= 0 || g_config.windowHeight <= 0) return false;
    if (IsIconic(hWnd) || IsZoomed(hWnd)) return false;
    RECT rc{};
    if (!GetWindowRect(hWnd, &rc)) return false;
    int w = std::max(0, static_cast<int>(rc.right - rc.left));
    int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (w == g_config.windowWidth && h == g_config.windowHeight) return false;
    SetWindowPos(hWnd, nullptr, 0, 0, g_config.windowWidth, g_config.windowHeight,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

bool UpdateWindowSizeConfig(HWND hWnd) {
    if (!hWnd) return false;
    if (IsIconic(hWnd) || IsZoomed(hWnd)) return false;
    RECT rc{};
    if (!GetWindowRect(hWnd, &rc)) return false;
    int w = std::max(0, static_cast<int>(rc.right - rc.left));
    int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
    if (w <= 0 || h <= 0) return false;
    if (g_config.windowWidth == w && g_config.windowHeight == h) return false;
    g_config.windowWidth = w;
    g_config.windowHeight = h;
    return true;
}

void ApplyConfigToUI(HWND hWnd) {
    g_showAnnots = g_config.showAnnots;
    g_leftWidth  = g_config.leftWidth;
    g_rightWidth = g_config.rightWidth;
    g_topHeight  = g_config.topHeight;
    ApplyWindowSizeFromConfig(hWnd);
    g_leftSplit1 = g_config.leftSplit1;
    g_leftSplit2 = g_config.leftSplit2;
    g_leftPaneCollapsed = g_config.leftPaneCollapsed;
    g_bottomPanePin = ParseBottomPanePin(g_config.bottomPanePin);
    g_bottomNoteMode = ParseBottomNoteMode(g_config.bottomNoteMode);
    g_notePlacement = ParseNotePlacement(g_config.notePlacement);
    g_config.notePlacement = NotePlacementToString(g_notePlacement);
    g_showMathList = g_config.showMathList;
    if (!g_config.textFontName.empty()) g_textFontName = g_config.textFontName;
    if (g_textFontName.empty()) g_textFontName = GetDefaultFontFaceName();
    g_textFontName = ResolveFontFaceName(g_textFontName);
    g_textFontActiveSizeSlot = std::clamp(g_config.textFontActiveSizeSlot, 0, 1);
    g_textFontPtSlotA = std::clamp(g_config.textFontPtSlotA, 6.0, 96.0);
    g_textFontPtSlotB = std::clamp(g_config.textFontPtSlotB, 6.0, 96.0);
    g_textFontUseA4ScaleSlotA = g_config.textFontUseA4ScaleSlotA;
    g_textFontUseA4ScaleSlotB = g_config.textFontUseA4ScaleSlotB;
    if (g_config.textFontPt > 0.0) g_textFontPt = g_config.textFontPt;
    g_textFontUseA4Scale = g_config.textFontUseA4Scale;
    if (g_textFontActiveSizeSlot == 1) {
        g_textFontPt = g_textFontPtSlotB;
        g_textFontUseA4Scale = g_textFontUseA4ScaleSlotB;
    } else {
        g_textFontPt = g_textFontPtSlotA;
        g_textFontUseA4Scale = g_textFontUseA4ScaleSlotA;
    }
    g_textBoxReadableBackground = g_config.textBoxReadableBackground;
    g_textBoxReadableBackgroundInverted = g_config.textBoxReadableBackgroundInverted;
    g_textBoxAutoWrap = g_config.textBoxAutoWrap;
    if (!g_config.noteFontName.empty()) g_noteFontName = g_config.noteFontName;
    if (g_noteFontName.empty()) g_noteFontName = GetDefaultFontFaceName();
    g_noteFontName = ResolveFontFaceName(g_noteFontName);
    if (g_config.noteFontPt > 0.0) g_noteFontPt = g_config.noteFontPt;
    if (!g_config.noteRenderFontName.empty()) g_noteRenderFontName = g_config.noteRenderFontName;
    if (g_noteRenderFontName.empty()) g_noteRenderFontName = g_noteFontName;
    g_noteRenderFontName = ResolveFontFaceName(g_noteRenderFontName);
    if (!g_config.noteRenderJpFontName.empty()) g_noteRenderJpFontName = g_config.noteRenderJpFontName;
    if (g_noteRenderJpFontName.empty()) g_noteRenderJpFontName = g_noteRenderFontName;
    g_noteRenderJpFontName = ResolveFontFaceName(g_noteRenderJpFontName);
    g_noteRenderFontPt = g_noteFontPt;
    g_noteSystem = NoteSystem::Legacy;
    g_noteRenderEnabled = g_config.noteRenderEnabled;
    g_noteRawOnly = g_config.noteRawOnly;
    g_noteRenderMath = (g_noteRenderEnabled && !g_noteRawOnly && g_config.noteRenderMath);
    g_config.noteRenderMath = g_noteRenderMath;
    g_noteVimModeEnabled = g_config.noteVimModeEnabled;
    g_noteVimCaretLineRawTextVisible = g_config.noteVimCaretLineRawTextVisible;
    g_noteVimClickEntersInsertMode = g_config.noteVimClickEntersInsertMode;
    mathrender::SetSupSubGapSupPercent(g_config.noteMathSupSubGapSupPercent);
    if (!g_noteVimModeEnabled) {
        g_noteNormalMode = false;
        ExitCurrentNoteNormalMode();
    }
    g_noteGridEnabled = false;
    g_noteGridPitch = g_config.noteGridPitch;
    g_noteBgColor = g_config.noteBgColor;
    g_noteFgColor = g_config.noteFgColor;
    g_noteShortcutBackColor = g_config.noteShortcutBackColor;
    g_noteShortcutTextColor = g_config.noteShortcutTextColor;
    g_config.noteCustomTagKey = L"c";
    g_lineToolsShareStyle = g_config.lineToolsShareStyle;
    if (g_config.lineWidthPt > 0.0) g_lineWidthPt = g_config.lineWidthPt;
    if (g_config.arrowWidthPt > 0.0) g_arrowWidthPt = g_config.arrowWidthPt;
    g_arrowHead = ParseArrowHead(g_config.arrowHead);
    if (g_config.waveWidthPt > 0.0) g_waveWidthPt = g_config.waveWidthPt;
    g_lineDashStyle = (g_config.lineDashStyle == L"dash") ? L"dash" : L"solid";
    if (g_config.freehandWidthPt > 0.0) g_freehandWidthPt = g_config.freehandWidthPt;
    if (g_config.markerFreeWidthPt > 0.0) g_markerFreeWidthPt = g_config.markerFreeWidthPt;
    if (g_config.markerTextWidthPt > 0.0) g_markerTextWidthPt = g_config.markerTextWidthPt;
    g_markerTextUnderline = g_config.markerTextUnderline;
    if (IsMarkerGroupMode(g_markerGroupMode) && MarkerTextModeStoresUnderlineOption(g_markerGroupMode)) {
        g_markerGroupMode = g_markerTextUnderline ? ToolMode::MarkerTextUnderline : ToolMode::MarkerText;
    }
    if (g_config.eraserWidthPt > 0.0) g_eraserWidthPt = g_config.eraserWidthPt;
    if (g_config.markerAlpha > 0.0) g_markerAlpha = g_config.markerAlpha;
    if (g_config.lineAlpha > 0.0) g_lineAlpha = g_config.lineAlpha;
    if (g_config.arrowAlpha > 0.0) g_arrowAlpha = g_config.arrowAlpha;
    if (g_config.waveAlpha > 0.0) g_waveAlpha = g_config.waveAlpha;
    if (g_config.freehandAlpha > 0.0) g_freehandAlpha = g_config.freehandAlpha;
    if (g_config.shapeAlpha >= 0.0) g_shapeAlpha = std::clamp(g_config.shapeAlpha, 0.0, 1.0);
    g_textColor = g_config.textColor;
    g_lineColor = g_config.lineColor;
    g_arrowColor = g_config.arrowColor;
    g_waveColor = g_config.waveColor;
    g_freehandColor = g_config.freehandColor;
    g_markerFreeColor = g_config.markerFreeColor;
    g_markerTextColor = g_config.markerTextColor;
    g_shapeColor = g_config.shapeColor;
    SetPaletteCustomColor(g_config.paletteCustomColor);
    SyncUserPaletteToRuntime();
    SyncUserToolShortcutsToRuntime();
    g_magnifierShape = ParseMagnifierShape(g_config.magnifierShape);
    g_magnifierZoom = std::clamp(g_config.magnifierZoom, 1.25, 4.0);
    g_magnifierSizeDip = std::clamp(g_config.magnifierSizeDip, 80, 240);
    g_magnifierPosition = ParseMagnifierPosition(g_config.magnifierPosition);
    g_shapeKind = ParseShapeKind(g_config.shapeKind);
    g_shapeDrawMode = ParseShapeDrawMode(g_config.shapeDrawMode);
    const auto firstAvailableMode = [](ToolMode fallback, auto predicate) {
        for (ToolMode candidate : AnnotToolModeUiOrder()) {
            if (!predicate(candidate)) continue;
            if (AnnotToolModeUiStateFor(candidate) != AnnotToolUiState::Enabled) continue;
            return candidate;
        }
        return fallback;
    };
    const auto validModeOr = [&](const std::wstring& value, ToolMode fallback, auto predicate) {
        ToolMode mode = fallback;
        if (AnnotToolModeFromKey(WideToUTF8(value), mode) &&
            predicate(mode) &&
            AnnotToolModeUiStateFor(mode) == AnnotToolUiState::Enabled) {
            return mode;
        }
        return firstAvailableMode(fallback, predicate);
    };
    g_markerGroupMode = validModeOr(g_config.annotLastMarkerDetail, ToolMode::MarkerText,
                                    [](ToolMode mode) { return IsMarkerGroupMode(mode); });
    g_penGroupMode = validModeOr(g_config.annotLastPenDetail, ToolMode::Freehand,
                                 [](ToolMode mode) { return IsPenGroupMode(mode); });
    ShapeDetail restoredShapeDetail{};
    if (ShapeDetailFromKey(WideToUTF8(g_config.shapeDetail), restoredShapeDetail)) {
        g_shapeDetail = restoredShapeDetail;
    } else {
        ToolMode legacyShapeMode = validModeOr(g_config.annotLastShapeDetail, ToolMode::Line,
                                               [](ToolMode mode) { return IsShapeGroupMode(mode); });
        AnnotToolPresentation shapePresentation{};
        AnnotToolGeometry shapeGeometry{};
        if (ShapeToolPresentationFromKey(WideToUTF8(g_config.annotLastShapePresentation), shapePresentation) &&
            ShapeToolGeometryFromKey(WideToUTF8(g_config.annotLastShapeGeometry), shapeGeometry)) {
            if (auto structuredShapeMode = ToolModeForShapeToolSelection({ shapePresentation, shapeGeometry })) {
                legacyShapeMode = *structuredShapeMode;
                if (shapeGeometry == AnnotToolGeometry::Shape) {
                    g_shapeDrawMode = shapePresentation == AnnotToolPresentation::Emphasis
                        ? ShapeDrawMode::Fill : ShapeDrawMode::Outline;
                }
            }
        }
        g_shapeDetail = ShapeDetailForLegacyState(legacyShapeMode, g_shapeKind);
    }
    SyncLegacyShapeStateFromDetail();
    g_config.shapeDetail = UTF8ToWide(ShapeDetailKey(g_shapeDetail));
    if (g_config.markColor >= 0) {
        g_highlightMarkColor = RGB((g_config.markColor >> 16) & 0xFF, (g_config.markColor >> 8) & 0xFF, g_config.markColor & 0xFF);
    }
    if (g_config.headingColor >= 0) {
        g_highlightHeadingColor = RGB((g_config.headingColor >> 16) & 0xFF, (g_config.headingColor >> 8) & 0xFF, g_config.headingColor & 0xFF);
    }
    g_markFontPx = g_config.markFontPx;
    g_headingFontPx = g_config.headingFontPx;

    ApplyBottomPaneEdgeStyle();
    ApplyNoteFont();
    ApplyNoteSystem(hWnd);
    if (!g_themeCatalog.empty()) {
        ApplyThemeByName(g_themeName, hWnd, false);
    }
    auto selectComboByData = [](HWND combo, DWORD_PTR want) {
        if (!combo) return;
        int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
        int bestIdx = -1;
        DWORD_PTR bestDelta = 0;
        for (int i = 0; i < count; ++i) {
            DWORD_PTR data = static_cast<DWORD_PTR>(SendMessageW(combo, CB_GETITEMDATA, i, 0));
            if (data == want) {
                SendMessageW(combo, CB_SETCURSEL, i, 0);
                return;
            }
            DWORD_PTR delta = (data > want) ? (data - want) : (want - data);
            if (bestIdx < 0 || delta < bestDelta) {
                bestIdx = i;
                bestDelta = delta;
            }
        }
        if (bestIdx >= 0) SendMessageW(combo, CB_SETCURSEL, bestIdx, 0);
    };
    auto ensureComboSelectFont = [](HWND combo, const std::wstring& faceName) {
        if (!combo || faceName.empty()) return;
        std::wstring displayName = ResolveFontDisplayName(faceName);
        int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
        wchar_t buf[256]{};
        for (int i = 0; i < count; ++i) {
            int len = static_cast<int>(SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf)));
            if (len >= 0) {
                buf[std::min<int>(len, 255)] = L'\0';
                if (displayName == buf) {
                    SendMessageW(combo, CB_SETCURSEL, i, 0);
                    return;
                }
            }
        }
        int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(displayName.c_str())));
        if (idx >= 0) SendMessageW(combo, CB_SETCURSEL, idx, 0);
    };

    ensureComboSelectFont(g_hComboFont, g_textFontName);
    SyncToolbarFontSizeCombo();
    if (g_hChkTextReadableBackground) {
        SendMessageW(g_hChkTextReadableBackground, BM_SETCHECK,
                     g_textBoxReadableBackground ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hRadioTextReadableBackgroundNormal) {
        SendMessageW(g_hRadioTextReadableBackgroundNormal, BM_SETCHECK,
                     g_textBoxReadableBackgroundInverted ? BST_UNCHECKED : BST_CHECKED, 0);
    }
    if (g_hRadioTextReadableBackgroundInverted) {
        SendMessageW(g_hRadioTextReadableBackgroundInverted, BM_SETCHECK,
                     g_textBoxReadableBackgroundInverted ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hChkTextAutoWrap) {
        SendMessageW(g_hChkTextAutoWrap, BM_SETCHECK,
                     g_textBoxAutoWrap ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hChkPanMouseWheelZoom) {
        SendMessageW(g_hChkPanMouseWheelZoom, BM_SETCHECK,
                     g_config.panMouseWheelZoom ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    selectComboByData(g_hComboMarkerAlpha, static_cast<DWORD_PTR>(std::llround(ToolAlphaForMode(g_toolMode) * 1000.0)));
    selectComboByData(g_hComboMarkerTextStyle, static_cast<DWORD_PTR>(g_markerTextUnderline ? 1 : 0));
    selectComboByData(g_hComboLineDashStyle, static_cast<DWORD_PTR>(g_lineDashStyle == L"dash" ? 1 : 0));
    selectComboByData(g_hComboShapeKind, static_cast<DWORD_PTR>(g_shapeKind));
    selectComboByData(g_hComboShapeDrawMode, static_cast<DWORD_PTR>(g_shapeDrawMode));
    selectComboByData(g_hComboMagnifierShape, static_cast<DWORD_PTR>(g_magnifierShape));
    // Width combo is tool-dependent; it will be synced on tool switch / UpdateToolbarUI.

    UpdateBottomPaneMenuChecks();
    if (g_hNoteFont) DeleteObject(g_hNoteFont);
    g_hNoteFont = CreateFontFromFaceName(g_noteFontName, g_noteFontPt);
    if (g_hNoteRenderFont) DeleteObject(g_hNoteRenderFont);
    g_hNoteRenderFont = CreateFontFromFaceName(g_noteRenderFontName, g_noteRenderFontPt);
    if (g_hNoteEdit) {
        HFONT activeNoteFont = g_noteRenderEnabled ? g_hNoteRenderFont : g_hNoteFont;
        SendMessageW(g_hNoteEdit, WM_SETFONT, reinterpret_cast<WPARAM>(activeNoteFont), TRUE);
    }
    LayoutChildren(hWnd);
    UpdateMathListVisibility();
    RefreshCurrentNoteBottomPane();
    ApplyActiveColorForMode(hWnd, g_toolMode);
    UpdateToolbarUI(hWnd);
    UpdateAutoSaveTimer(hWnd);
    UpdateAutoIntegrateTimer(hWnd);
}

void ResetSessionAndFiles() {
    g_sessions.clear();
    g_pdfFiles.clear();
    g_noteFiles.clear();
    s_searchTempPdfKeys.clear();
    s_searchTempNoteKeys.clear();
    g_currentSessionPath.clear();
    if (g_hSessionList) {
        SendMessageW(g_hSessionList, LB_RESETCONTENT, 0, 0);
        InvalidateRect(g_hSessionList, nullptr, TRUE);
    }
    if (g_hPdfList) {
        SendMessageW(g_hPdfList, LB_RESETCONTENT, 0, 0);
        InvalidateRect(g_hPdfList, nullptr, TRUE);
    }
    if (g_hNoteList) {
        SendMessageW(g_hNoteList, LB_RESETCONTENT, 0, 0);
        InvalidateRect(g_hNoteList, nullptr, TRUE);
    }
}

void ClearPdfAndNoteSelection() {
    // Persist current PDF view position before tearing down state.
    SaveLastPdfViewInfo();
    ClearPdfState();
    if (g_hPdfView) InvalidateRect(g_hPdfView, nullptr, FALSE);

    ClearNoteEditorSilently(GetParent(g_hNoteEdit));
    g_previewNote.clear();
    RefreshCurrentNoteBottomPane();
}

bool SaveNoteIfDirty(HWND hWnd) {
    return file_output::SaveNoteIfDirty(hWnd);
}

void FinalizeManualSaveUi(HWND hWnd, bool updateWindowTitleAfterSave) {
    const ULONGLONG startTick = preview_trace::TickNow();
    preview_trace::Append(
        L"FinalizeManualSaveUi",
        L"begin notePath=" + g_currentNotePath +
        L" pdfPath=" + g_pdf.path);
    RefreshMainMenuBar(hWnd);
    preview_trace::Append(
        L"FinalizeManualSaveUi",
        L"after_refresh_menu elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    if (updateWindowTitleAfterSave) {
        UpdateWindowTitle(hWnd);
        preview_trace::Append(
            L"FinalizeManualSaveUi",
            L"after_update_title elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    }
    RefreshCurrentNoteFileSnapshot();
    preview_trace::Append(
        L"FinalizeManualSaveUi",
        L"after_refresh_note_snapshot elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    RefreshStatusDisplay(hWnd);
    preview_trace::Append(
        L"FinalizeManualSaveUi",
        L"end notePath=" + g_currentNotePath +
        L" pdfPath=" + g_pdf.path +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

static std::wstring ToExtendedWin32PathIfAbsolute(const std::filesystem::path& p) {
    std::wstring s = p.wstring();
    if (s.empty()) return s;
    if (s.rfind(L"\\\\?\\", 0) == 0) return s;
    if (s.size() >= 2 && s[1] == L':') {
        return L"\\\\?\\" + s;
    }
    if (s.rfind(L"\\\\", 0) == 0) {
        // UNC: \\server\share -> \\?\UNC\server\share
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    return s;
}

bool TryOpenDirForList(const std::filesystem::path& dir, std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (dir.empty()) {
        if (outErr) *outErr = L"invalid directory path";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec || !std::filesystem::is_directory(dir, ec) || ec) {
        if (outErr) *outErr = L"directory not found";
        return false;
    }
    std::wstring openPath = ToExtendedWin32PathIfAbsolute(dir);
    HANDLE h = CreateFileW(openPath.c_str(),
                           FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outErr) *outErr = atomic_write::Win32ErrorMessage(GetLastError());
        return false;
    }
    CloseHandle(h);
    return true;
}

static std::wstring WritableProbeCacheKey(const std::filesystem::path& dir) {
    if (dir.empty()) return L"";
    std::filesystem::path canon = CanonicalOrSelf(dir).lexically_normal();
    std::wstring key = canon.wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return key;
}

static std::unordered_set<std::wstring> s_writableProbeCache;
static std::unordered_set<std::wstring> s_tempExternalAccessWarned;

static bool IsWritableProbeCached(const std::filesystem::path& dir) {
    std::wstring key = WritableProbeCacheKey(dir);
    return !key.empty() && s_writableProbeCache.find(key) != s_writableProbeCache.end();
}

void RememberWritableProbe(const std::filesystem::path& dir) {
    std::wstring key = WritableProbeCacheKey(dir);
    if (!key.empty()) s_writableProbeCache.insert(std::move(key));
}

void ForgetWritableProbe(const std::filesystem::path& dir) {
    std::wstring key = WritableProbeCacheKey(dir);
    if (!key.empty()) s_writableProbeCache.erase(key);
}

void ShowTempExternalLectureAccessWarning(HWND owner,
                                                 const std::filesystem::path& dir,
                                                 const std::wstring& detail) {
    std::wstring key = WritableProbeCacheKey(dir);
    if (!key.empty() && s_tempExternalAccessWarned.find(key) != s_tempExternalAccessWarned.end()) return;
    if (!key.empty()) s_tempExternalAccessWarned.insert(key);

    std::wstring msg = localization::Format(g_config.studentMode
        ? L"workspace.config.temp_external.access_failed.student"
        : L"workspace.config.temp_external.access_failed.parent", {{L"PATH", dir.wstring()}});
    msg += localization::Text(g_config.studentMode
        ? L"workspace.config.temp_external.access_guidance.student"
        : L"workspace.config.temp_external.access_guidance.parent");
    if (!detail.empty()) msg += L"\n\n" + detail;
    ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
}

static bool TryWriteTempDeleteOnCloseFile(const std::filesystem::path& dir, std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (dir.empty()) {
        if (outErr) *outErr = L"invalid directory path";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec || !std::filesystem::is_directory(dir, ec) || ec) {
        if (outErr) *outErr = L"directory not found";
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    for (int i = 0; i < 16; ++i) {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        uint64_t ts = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
        std::wstring name = L".__perm_test__" + std::to_wstring(pid) + L"_" + std::to_wstring(ts) +
                            L"_" + std::to_wstring(i) + L".tmp";
        std::filesystem::path testPath = dir / name;
        std::wstring openPath = ToExtendedWin32PathIfAbsolute(testPath);
        HANDLE h = CreateFileW(openPath.c_str(),
                               GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) continue;
        if (outErr) *outErr = atomic_write::Win32ErrorMessage(e);
        return false;
    }
    if (outErr) *outErr = L"failed to create temp file (name collision)";
    return false;
}

bool EnsureWorkspaceResourceDirsWithErr(const std::wstring& root,
                                               std::filesystem::path* settingsDir,
                                               std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (root.empty()) {
        if (outErr) *outErr = L"workspace root is empty";
        return false;
    }
    std::filesystem::path resource = std::filesystem::path(root) / L"__pdf_note_workspace__";
    std::filesystem::path settings = resource / L"__settings__";
    std::filesystem::path cache = resource / L"__tmp__";
    std::filesystem::path escape = resource / L"__escape__";
    std::filesystem::path themes = resource / L"__theme__";
    std::filesystem::path logs = resource / L"__log__";
    std::error_code ec;
    std::filesystem::create_directories(settings, ec);
    if (ec) {
        if (outErr) *outErr = L"failed to create: " + settings.wstring() + L"\n" + UTF8ToWide(ec.message());
        return false;
    }
    std::filesystem::create_directories(cache, ec);
    if (ec) {
        if (outErr) *outErr = L"failed to create: " + cache.wstring() + L"\n" + UTF8ToWide(ec.message());
        return false;
    }
    std::filesystem::create_directories(escape, ec);
    if (ec) {
        if (outErr) *outErr = L"failed to create: " + escape.wstring() + L"\n" + UTF8ToWide(ec.message());
        return false;
    }
    std::filesystem::create_directories(themes, ec);
    if (ec) {
        if (outErr) *outErr = L"failed to create: " + themes.wstring() + L"\n" + UTF8ToWide(ec.message());
        return false;
    }
    std::filesystem::create_directories(logs, ec);
    if (ec) {
        if (outErr) *outErr = L"failed to create: " + logs.wstring() + L"\n" + UTF8ToWide(ec.message());
        return false;
    }

    // Move stale atomic temp files from __tmp__ to __escape__ (best-effort).
    {
        std::error_code itEc;
        auto now = std::filesystem::file_time_type::clock::now();
        for (auto it = std::filesystem::directory_iterator(cache, itEc);
             !itEc && it != std::filesystem::directory_iterator(); ++it) {
            bool isReparse = false;
            if (TryIsReparsePointNoFollow(it->path(), isReparse) && isReparse) continue;
            std::error_code stEc;
            if (!it->is_regular_file(stEc) || stEc) continue;
            auto p = it->path();
            auto ext = p.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext != L".tmp") continue;
            std::wstring name = p.filename().wstring();
            if (name.find(L".__atomic__.") == std::wstring::npos) continue;
            std::error_code timeEc;
            auto ts = std::filesystem::last_write_time(p, timeEc);
            if (!timeEc) {
                auto age = now - ts;
                if (age < std::chrono::seconds(30)) continue;
            }
            atomic_write::QuarantineFileBestEffort(p, escape, nullptr);
        }
    }

    std::wstring writeErr;
    if (!IsWritableProbeCached(cache) && !TryWriteTempDeleteOnCloseFile(cache, &writeErr)) {
        ForgetWritableProbe(cache);
        if (outErr) {
            *outErr = L"cannot write to: " + cache.wstring();
            if (!writeErr.empty()) *outErr += L"\n" + writeErr;
        }
        return false;
    }
    RememberWritableProbe(cache);

    if (settingsDir) *settingsDir = settings;
    return true;
}

bool EnsureWorkspaceResourceDirs(std::filesystem::path* settingsDir) {
    std::wstring err;
    return EnsureWorkspaceResourceDirsWithErr(g_workspaceRoot, settingsDir, &err);
}

bool VerifyWorkspaceWritableForEditing(HWND owner) {
    std::wstring err;
    if (EnsureWorkspaceResourceDirsWithErr(g_workspaceRoot, nullptr, &err)) return true;
    std::wstring msg = localization::Format(L"workspace.config.not_writable", {{L"ROOT", g_workspaceRoot}});
    msg += localization::Text(L"workspace.config_io.f9e597c9cfc2").c_str();
    if (!err.empty()) msg += L"\n\n" + err;
    ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
    return false;
}

bool VerifyDirReadableWritableForEditing(HWND owner, const std::filesystem::path& dir, const wchar_t* labelId) {
    const std::wstring label = localization::Text(labelId ? labelId : L"workspace.directory.folder");
    std::wstring readErr;
    if (!TryOpenDirForList(dir, &readErr)) {
        ForgetWritableProbe(dir);
        std::wstring msg = localization::Format(L"workspace.config.directory.not_readable",
                                                {{L"LABEL", label}, {L"PATH", dir.wstring()}});
        msg += localization::Text(L"workspace.config_io.f9e597c9cfc2").c_str();
        if (!readErr.empty()) msg += L"\n\n" + readErr;
        if (IsTempExternalLecturePath(dir.wstring())) {
            msg += localization::Text(g_config.studentMode
                ? L"workspace.config.temp_external.registration_kept.student"
                : L"workspace.config.temp_external.registration_kept.parent");
        }
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    std::wstring writeErr;
    if (!IsWritableProbeCached(dir) && !TryWriteTempDeleteOnCloseFile(dir, &writeErr)) {
        ForgetWritableProbe(dir);
        std::wstring msg = localization::Format(L"workspace.config.directory.not_writable",
                                                {{L"LABEL", label}, {L"PATH", dir.wstring()}});
        msg += localization::Text(L"workspace.config_io.f9e597c9cfc2").c_str();
        if (!writeErr.empty()) msg += L"\n\n" + writeErr;
        if (IsTempExternalLecturePath(dir.wstring())) {
            msg += localization::Text(g_config.studentMode
                ? L"workspace.config.temp_external.registration_kept.student"
                : L"workspace.config.temp_external.registration_kept.parent");
        }
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    RememberWritableProbe(dir);
    return true;
}

namespace {
static bool PickSettingsPresetSavePath(HWND owner, std::filesystem::path* outPath);
static bool PickSettingsPresetOpenPath(HWND owner, std::filesystem::path* outPath);

enum class WorkspaceToolsPage { Presets, Restore };

class ScopedWorkspaceToolsDialogOwner {
public:
    explicit ScopedWorkspaceToolsDialogOwner(HWND owner) : owner_(owner) {
        wasEnabled_ = owner_ && IsWindow(owner_) && IsWindowEnabled(owner_);
        if (wasEnabled_) EnableWindow(owner_, FALSE);
    }

    ~ScopedWorkspaceToolsDialogOwner() {
        if (wasEnabled_ && owner_ && IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
            SetActiveWindow(owner_);
        }
    }

    ScopedWorkspaceToolsDialogOwner(const ScopedWorkspaceToolsDialogOwner&) = delete;
    ScopedWorkspaceToolsDialogOwner& operator=(const ScopedWorkspaceToolsDialogOwner&) = delete;

private:
    HWND owner_{};
    bool wasEnabled_ = false;
};

struct SettingsPresetDialogCtx {
    HWND dialog = nullptr;
    bool done = false;
    WorkspaceToolsPage page = WorkspaceToolsPage::Presets;
    UINT commandId = 0;
    std::vector<HWND> presetControls;
    std::vector<HWND> restoreControls;
    HWND subtitle = nullptr;
    UINT dpi = 96;
    int scrollY = 0;
    int contentHeight = 0;
    bool layingOut = false;
};

constexpr wchar_t kSettingsPresetDialogClass[] = L"PdfNoteSettingsPresetDialog";
// Keep navigation separate from Win32's IDOK / IDCANCEL commands.
constexpr int kWorkspaceToolsPresetTab = 7101;
constexpr int kWorkspaceToolsRestoreTab = 7102;
constexpr int kSettingsPresetSaveButton = 10;
constexpr int kSettingsPresetLoadButton = 11;
constexpr int kWorkspaceToolsRestorePdfPosition = 20;
constexpr int kWorkspaceToolsRestoreLectureLastOpen = 21;
constexpr int kWorkspaceToolsRestoreSessionLastOpen = 22;
constexpr int kWorkspaceToolsRestoreSavedFiles = 23;
constexpr int kWorkspaceToolsResetPdfPosition = 24;
constexpr int kWorkspaceToolsResetLectureLastOpen = 25;
constexpr int kWorkspaceToolsResetSessionLastOpen = 26;
constexpr int kWorkspaceToolsDeleteSavedFiles = 27;
constexpr int kSettingsPresetCloseButton = 30;

static void SetPresetDialogFont(HWND control) {
    if (control) SetUIFont(control);
}

static HWND AddWorkspaceToolsText(HWND parent, const std::wstring& text, int x, int y, int width, int height,
                                  std::vector<HWND>* group) {
    HWND control = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   x, y, width, height, parent, nullptr, g_hInst, nullptr);
    SetPresetDialogFont(control);
    if (group) group->push_back(control);
    return control;
}

static HWND AddWorkspaceToolsButton(HWND parent, const std::wstring& text, int id, int x, int y, int width,
                                    bool primary, std::vector<HWND>* group) {
    HWND control = CreateWindowExW(0, L"BUTTON", text.c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                   x, y, width, 26, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    SetPresetDialogFont(control);
    if (group) group->push_back(control);
    return control;
}

static HWND AddWorkspaceToolsTab(HWND parent, const std::wstring& text, int id, int x) {
    HWND control = CreateWindowExW(0, L"BUTTON", text.c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE |
                                       (id == kWorkspaceToolsPresetTab ? WS_GROUP : 0),
                                   x, 60, 160, 26, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    SetPresetDialogFont(control);
    return control;
}

static void AddWorkspaceToolsSeparator(HWND parent, int y, std::vector<HWND>* group) {
    RECT client{};
    GetClientRect(parent, &client);
    HWND control = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                                   12, y, std::max(0L, client.right - 24), 2, parent, nullptr, g_hInst, nullptr);
    if (group) group->push_back(control);
}

static UINT WorkspaceToolsDpi(HWND window) {
    using GetWindowDpi = UINT (WINAPI*)(HWND);
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    GetWindowDpi query = nullptr;
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    if (query && window) { const UINT dpi = query(window); if (dpi) return dpi; }
    HDC dc = GetDC(window);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(window, dc);
    return static_cast<UINT>(std::max(96, dpi));
}

// Text is measured using the same UI font as the output dialog. Scaling only
// fixed pixels is insufficient for translations, larger fonts and narrow screens.
static void LayoutWorkspaceTools(SettingsPresetDialogCtx* ctx) {
    if (!ctx || !ctx->dialog || ctx->layingOut || ctx->presetControls.size() != 13 ||
        ctx->restoreControls.size() != 21) return;
    ctx->layingOut = true;
    HWND window = ctx->dialog;
    const auto scale = [ctx](int value) { return MulDiv(value, ctx->dpi, 96); };
    RECT client{}; GetClientRect(window, &client);
    const int pad = scale(12), gap = scale(8);
    const int width = std::max(1, static_cast<int>(client.right) - pad * 2);
    const auto textSize = [window](HWND control, int width, bool singleLine) {
        const int length = GetWindowTextLengthW(control);
        std::wstring text(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
        GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
        HDC dc = GetDC(window);
        if (!dc) return SIZE{width, 24};
        HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
        RECT measured{0, 0, width, 0};
        DrawTextW(dc, text.c_str(), -1, &measured,
                  DT_CALCRECT | DT_NOPREFIX | (singleLine ? DT_SINGLELINE : DT_WORDBREAK));
        SelectObject(dc, old); ReleaseDC(window, dc);
        return SIZE{measured.right, std::max(1L, measured.bottom)};
    };
    const int rowH = std::max(scale(28), static_cast<int>(textSize(GetDlgItem(window, 30), width, true).cy) + gap);
    const auto buttonW = [&](HWND button, int minimum) {
        return std::min(width, std::max(scale(minimum), static_cast<int>(textSize(button, width, true).cx) + pad * 2));
    };
    const auto place = [&](HWND control, int x, int y, int w, int h) {
        SetWindowPos(control, nullptr, x, y - ctx->scrollY, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const auto text = [&](HWND control, int& y, int w) {
        const int h = static_cast<int>(textSize(control, w, false).cy) + scale(2);
        place(control, pad, y, w, h); y += h + gap;
    };
    const auto separator = [&](HWND control, int& y) {
        place(control, pad, y, width, scale(2)); y += scale(2) + gap;
    };
    int y = pad;
    text(ctx->subtitle, y, width);
    HWND presetTab = GetDlgItem(window, kWorkspaceToolsPresetTab);
    HWND restoreTab = GetDlgItem(window, kWorkspaceToolsRestoreTab);
    const int tabW = std::max(buttonW(presetTab, 160), buttonW(restoreTab, 160));
    place(presetTab, pad, y, std::min(tabW, (width - gap) / 2), rowH);
    place(restoreTab, pad + (width + gap) / 2, y, std::min(tabW, (width - gap) / 2), rowH);
    y += rowH + pad;
    const int pageTop = y;
    auto& presets = ctx->presetControls;
    for (size_t i = 0; i < 11; ++i) {
        if (i == 1 || i == 4 || i == 7 || i == 10) separator(presets[i], y);
        else text(presets[i], y, width);
    }
    const int saveW = buttonW(presets[11], 154), loadW = buttonW(presets[12], 154);
    place(presets[11], pad, y, saveW, rowH);
    if (saveW + gap + loadW > width) {
        y += rowH + gap; place(presets[12], pad, y, loadW, rowH);
    } else place(presets[12], pad + saveW + gap, y, loadW, rowH);
    const int presetEnd = y + rowH + pad;
    y = pageTop;
    auto& restore = ctx->restoreControls;
    text(restore[0], y, width);
    for (size_t row = 0; row < 4; ++row) {
        const size_t first = 1 + row * 5;
        HWND action = restore[first + 2], remove = restore[first + 3];
        const int actionW = std::max(buttonW(action, 130), buttonW(remove, 130));
        const bool stacked = width < scale(500);
        const int descriptionW = stacked ? width : std::max(1, width - actionW - pad);
        const int top = y;
        text(restore[first], y, descriptionW);
        text(restore[first + 1], y, descriptionW);
        const int actionTop = stacked ? y : top;
        place(action, pad + width - actionW, actionTop, actionW, rowH);
        place(remove, pad + width - actionW, actionTop + rowH + gap, actionW, rowH);
        y = std::max(y, actionTop + rowH * 2 + gap) + gap;
        if (row < 3) separator(restore[first + 4], y);
    }
    text(restore[20], y, width);
    y = std::max(presetEnd, y);
    HWND close = GetDlgItem(window, kSettingsPresetCloseButton);
    const int closeW = buttonW(close, 100);
    place(close, pad + width - closeW, y, closeW, rowH);
    ctx->contentHeight = y + rowH + pad;
    const int boundedScroll = std::clamp(ctx->scrollY, 0, std::max(0, ctx->contentHeight - static_cast<int>(client.bottom)));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS, 0, ctx->contentHeight - 1,
                      static_cast<UINT>(std::max(0L, client.bottom)), boundedScroll, 0};
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);
    ctx->layingOut = false;
    if (boundedScroll != ctx->scrollY) { ctx->scrollY = boundedScroll; LayoutWorkspaceTools(ctx); }
}

static void ShowWorkspaceToolsPage(SettingsPresetDialogCtx* ctx, WorkspaceToolsPage page) {
    if (!ctx) return;
    ctx->page = page;
    if (HWND presetsTab = GetDlgItem(ctx->dialog, kWorkspaceToolsPresetTab)) {
        SendMessageW(presetsTab, BM_SETCHECK, page == WorkspaceToolsPage::Presets ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (HWND restoreTab = GetDlgItem(ctx->dialog, kWorkspaceToolsRestoreTab)) {
        SendMessageW(restoreTab, BM_SETCHECK, page == WorkspaceToolsPage::Restore ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    for (HWND control : ctx->presetControls) ShowWindow(control, page == WorkspaceToolsPage::Presets ? SW_SHOW : SW_HIDE);
    for (HWND control : ctx->restoreControls) ShowWindow(control, page == WorkspaceToolsPage::Restore ? SW_SHOW : SW_HIDE);
    LayoutWorkspaceTools(ctx);
}

static LRESULT CALLBACK SettingsPresetDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SettingsPresetDialogCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ctx = create ? static_cast<SettingsPresetDialogCtx*>(create->lpCreateParams) : nullptr;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        // Native initialization stores the caption passed to CreateWindowExW.
        // Returning TRUE directly bypassed it and left the title bar blank.
        return ctx ? DefWindowProcW(hWnd, message, wParam, lParam) : FALSE;
    }
    case WM_CREATE: {
        if (!ctx) return -1;
        ctx->dialog = hWnd;
        // Match the output dialog's common font, 12px margin and 26px actions.
        // The window caption owns the title; the body starts with its purpose.
        RECT client{};
        GetClientRect(hWnd, &client);
        constexpr int pad = 12;
        const int width = static_cast<int>(client.right);
        const int contentW = width - pad * 2;
        ctx->dpi = WorkspaceToolsDpi(hWnd);
        ctx->subtitle = AddWorkspaceToolsText(hWnd, localization::Text(L"settings.workspace_tools.subtitle"), pad, 12, contentW, 38,
                              nullptr);
        AddWorkspaceToolsTab(hWnd, localization::Text(L"settings.workspace_tools.tab_presets"),
                             kWorkspaceToolsPresetTab, pad);
        AddWorkspaceToolsTab(hWnd, localization::Text(L"settings.workspace_tools.tab_restore"),
                             kWorkspaceToolsRestoreTab, pad + 170);

        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.preset.dialog_intro"), pad, 108, contentW, 28,
                              &ctx->presetControls);
        AddWorkspaceToolsSeparator(hWnd, 140, &ctx->presetControls);
        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.preset.how_to"), pad, 152, contentW, 22,
                              &ctx->presetControls);
        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.preset.how_to_body"), pad, 176, contentW, 42,
                              &ctx->presetControls);
        AddWorkspaceToolsSeparator(hWnd, 224, &ctx->presetControls);
        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.preset.when_to_use"), pad, 232, contentW, 22,
                              &ctx->presetControls);
        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.preset.when_to_use_body"), pad, 256, contentW, 42,
                              &ctx->presetControls);
        AddWorkspaceToolsSeparator(hWnd, 304, &ctx->presetControls);
        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.preset.contents"), pad, 312, contentW, 22,
                              &ctx->presetControls);
        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.preset.contents_body"), pad, 336, contentW, 48,
                              &ctx->presetControls);
        AddWorkspaceToolsSeparator(hWnd, 398, &ctx->presetControls);
        AddWorkspaceToolsButton(hWnd, localization::Text(L"settings.preset.save"), kSettingsPresetSaveButton,
                                pad, 414, 154, true, &ctx->presetControls);
        AddWorkspaceToolsButton(hWnd, localization::Text(L"settings.preset.load"), kSettingsPresetLoadButton,
                                pad + 164, 414, 154, false, &ctx->presetControls);

        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.restore.dialog_intro"), pad, 108, contentW, 34,
                              &ctx->restoreControls);
        const EscapeBackupPresence backups = ScanEscapeBackupPresence();
        const auto addRestoreRow = [&](int top, const wchar_t* titleId, const wchar_t* bodyId, int restoreButtonId,
                                       int deleteButtonId, bool available) {
            AddWorkspaceToolsText(hWnd, localization::Text(titleId), pad, top, contentW - 150, 20,
                                  &ctx->restoreControls);
            AddWorkspaceToolsText(hWnd, localization::Text(bodyId), pad, top + 22, contentW - 150, 30,
                                  &ctx->restoreControls);
            HWND button = AddWorkspaceToolsButton(hWnd, localization::Text(L"settings.restore.action"), restoreButtonId,
                                                  width - pad - 130, top + 5, 130, false, &ctx->restoreControls);
            EnableWindow(button, available ? TRUE : FALSE);
            AddWorkspaceToolsButton(hWnd, localization::Text(L"settings.restore.delete_action"), deleteButtonId,
                                    width - pad - 130, top + 38, 130, false, &ctx->restoreControls);
        };
        addRestoreRow(160, L"settings.restore.pdf_position", L"settings.restore.pdf_position_body",
                      kWorkspaceToolsRestorePdfPosition, kWorkspaceToolsResetPdfPosition,
                      backups.hasPdfPositionBackup);
        AddWorkspaceToolsSeparator(hWnd, 228, &ctx->restoreControls);
        addRestoreRow(234, L"settings.restore.lecture_last_open", L"settings.restore.lecture_last_open_body",
                      kWorkspaceToolsRestoreLectureLastOpen, kWorkspaceToolsResetLectureLastOpen,
                      backups.hasLectureLastOpenBackup);
        AddWorkspaceToolsSeparator(hWnd, 302, &ctx->restoreControls);
        addRestoreRow(308, L"settings.restore.session_last_open", L"settings.restore.session_last_open_body",
                      kWorkspaceToolsRestoreSessionLastOpen, kWorkspaceToolsResetSessionLastOpen,
                      backups.hasSessionLastOpenBackup);
        AddWorkspaceToolsSeparator(hWnd, 376, &ctx->restoreControls);
        addRestoreRow(382, L"settings.restore.saved_files", L"settings.restore.saved_files_body",
                      kWorkspaceToolsRestoreSavedFiles, kWorkspaceToolsDeleteSavedFiles,
                      backups.hasSavedFileBackup);
        AddWorkspaceToolsText(hWnd, localization::Text(L"settings.restore.availability_hint"), pad, 460, contentW, 24,
                              &ctx->restoreControls);
        AddWorkspaceToolsButton(hWnd, localization::Text(L"settings.preset.close"), kSettingsPresetCloseButton,
                                width - pad - 100, 502, 100, false, nullptr);
        ApplyThemeToDialog(hWnd);
        ShowWorkspaceToolsPage(ctx, ctx->page);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        LayoutWorkspaceTools(ctx);
        return 0;
    case WM_SIZE:
        LayoutWorkspaceTools(ctx);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = ctx ? ctx->dpi : 96;
        limits->ptMinTrackSize = POINT{MulDiv(360, dpi, 96), MulDiv(240, dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        if (!ctx) break;
        ctx->dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) SetWindowPos(hWnd, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutWorkspaceTools(ctx);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_VSCROLL: {
        if (!ctx) break;
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
        GetScrollInfo(hWnd, SB_VERT, &info);
        int position = ctx->scrollY;
        const int step = MulDiv(32, ctx->dpi, 96);
        if (message == WM_MOUSEWHEEL) position -= GET_WHEEL_DELTA_WPARAM(wParam) * step * 3 / WHEEL_DELTA;
        else switch (LOWORD(wParam)) {
            case SB_LINEUP: position -= step; break;
            case SB_LINEDOWN: position += step; break;
            case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
            case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
            case SB_THUMBTRACK: position = info.nTrackPos; break;
            case SB_TOP: position = 0; break;
            case SB_BOTTOM: position = info.nMax; break;
            default: break;
        }
        ctx->scrollY = std::clamp(position, 0, std::max(0, info.nMax + 1 - static_cast<int>(info.nPage)));
        LayoutWorkspaceTools(ctx); InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT client{};
        GetClientRect(hWnd, &client);
        HBRUSH background = g_hThemeWindowBrush ? g_hThemeWindowBrush : GetSysColorBrush(COLOR_WINDOW);
        FillRect(hdc, &client, background);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return ThemeCtlColorPanel(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam));
    case WM_DRAWITEM:
        if (DrawThemeButton(reinterpret_cast<LPDRAWITEMSTRUCT>(lParam))) return TRUE;
        break;
    case WM_COMMAND:
        if (!ctx || HIWORD(wParam) != BN_CLICKED) break;
        if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(hWnd); return 0; }
        switch (LOWORD(wParam)) {
        case kWorkspaceToolsPresetTab: ShowWorkspaceToolsPage(ctx, WorkspaceToolsPage::Presets); return 0;
        case kWorkspaceToolsRestoreTab: ShowWorkspaceToolsPage(ctx, WorkspaceToolsPage::Restore); return 0;
        case kSettingsPresetSaveButton: ctx->commandId = ID_SETTINGS_PRESET_SAVE; DestroyWindow(hWnd); return 0;
        case kSettingsPresetLoadButton: ctx->commandId = ID_SETTINGS_PRESET_LOAD; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsRestorePdfPosition: ctx->commandId = ID_TEMP_RESTORE_PDF_POSITION; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsRestoreLectureLastOpen: ctx->commandId = ID_TEMP_RESTORE_LECTURE_LAST_OPEN; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsRestoreSessionLastOpen: ctx->commandId = ID_TEMP_RESTORE_SESSION_LAST_OPEN; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsRestoreSavedFiles: ctx->commandId = ID_FILE_RESTORE_BACKUP; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsResetPdfPosition: ctx->commandId = ID_TEMP_RESET_PDF_POSITION; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsResetLectureLastOpen: ctx->commandId = ID_TEMP_RESET_LECTURE_LAST_OPEN; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsResetSessionLastOpen: ctx->commandId = ID_TEMP_RESET_SESSION_LAST_OPEN; DestroyWindow(hWnd); return 0;
        case kWorkspaceToolsDeleteSavedFiles: ctx->commandId = ID_FILE_DELETE_BACKUP; DestroyWindow(hWnd); return 0;
        case kSettingsPresetCloseButton: DestroyWindow(hWnd); return 0;
        default: break;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(hWnd); return 0; }
        break;
    case WM_CLOSE:
        if (ctx) ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        UnregisterAppExitDialog(hWnd);
        if (ctx) ctx->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}
}

static void ShowWorkspaceToolsDialog(HWND owner, WorkspaceToolsPage initialPage) {
    SettingsPresetDialogCtx ctx{};
    ctx.page = initialPage;
    WNDCLASSW cls{};
    cls.lpfnWndProc = SettingsPresetDialogProc;
    cls.hInstance = g_hInst;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = kSettingsPresetDialogClass;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    ctx.dpi = WorkspaceToolsDpi(owner);
    const int width = MulDiv(756, ctx.dpi, 96);
    const int height = MulDiv(576, ctx.dpi, 96);
    ScopedWorkspaceToolsDialogOwner modalOwner(owner);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kSettingsPresetDialogClass, localization::Text(L"settings.workspace_tools.title").c_str(),
        WS_CAPTION | WS_POPUPWINDOW | WS_THICKFRAME | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        owner, nullptr, g_hInst, &ctx);
    if (!dialog) return;
    RECT bounds{}, client{}; GetWindowRect(dialog, &bounds); GetClientRect(dialog, &client);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    const int frameHeight = static_cast<int>(bounds.bottom - bounds.top - client.bottom);
    SetWindowPos(dialog, nullptr, 0, 0, std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left)),
        std::min(ctx.contentHeight + frameHeight, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top)),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RegisterAppExitBlockingDialog(dialog);
    PlaceOwnedPopupAtAppTopLeft(dialog, owner);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg{};
    HWND previousFocus = GetFocus();
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ShouldSkipImeMessageInLoop(msg)) continue;
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // Tab navigation must reveal an off-screen action on small displays.
        HWND focus = GetFocus();
        if (focus != previousFocus && focus && IsChild(dialog, focus) && IsWindow(dialog)) {
            RECT focused{}, viewport{}; GetWindowRect(focus, &focused); GetClientRect(dialog, &viewport);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&focused), 2);
            if (focused.top < 0 || focused.bottom > viewport.bottom) {
                ctx.scrollY += focused.top < 0 ? focused.top : focused.bottom - viewport.bottom;
                LayoutWorkspaceTools(&ctx);
            }
        }
        previousFocus = focus;
    }

    if (ctx.commandId && owner && IsWindow(owner)) {
        SendMessageW(owner, WM_COMMAND, MAKEWPARAM(ctx.commandId, 0), 0);
    }
}

void ShowSettingsPresetDialog(HWND owner) {
    ShowWorkspaceToolsDialog(owner, WorkspaceToolsPage::Presets);
}

void ShowWorkspaceRestoreDialog(HWND owner) {
    ShowWorkspaceToolsDialog(owner, WorkspaceToolsPage::Restore);
}

void SaveSettingsPreset(HWND hWnd) {
    try {
    const auto& ui = GetUiText();
    if (g_workspaceRoot.empty()) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.config_io.55c168692754").c_str(),
                       SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path presetPath;
    if (!PickSettingsPresetSavePath(hWnd, &presetPath)) return;
    std::wstring err;
    if (!ExportSettingsPresetToFile(presetPath, &err)) {
        ShowSilentMessageDialog(hWnd, ui.menuSettings,
                                localization::Text(L"workspace.config_io.59f0b7d1f221"),
                                SoftNoticeKind::Warning,
                                {{localization::Text(L"dialog.path.destination"), presetPath.wstring()}});
        return;
    }
    ShowSilentMessageDialog(hWnd, ui.menuSettings,
                            localization::Text(L"workspace.config_io.d573cf2c8ffb"),
                            SoftNoticeKind::Info,
                            {{localization::Text(L"dialog.path.destination"), presetPath.wstring()}});
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("SaveSettingsPreset", ex.what());
        ReportMainOperationException(hWnd, L"設定プリセット保存");
    } catch (...) {
        AppendMainOperationExceptionLog("SaveSettingsPreset", nullptr);
        ReportMainOperationException(hWnd, L"設定プリセット保存");
    }
}

void LoadSettingsPreset(HWND hWnd) {
    try {
    const auto& ui = GetUiText();
    if (g_workspaceRoot.empty()) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.config_io.55c168692754").c_str(),
                       SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path pickedPath;
    if (!PickSettingsPresetOpenPath(hWnd, &pickedPath)) return;
    if (!ConfirmMainYesNo(hWnd, ui.menuSettings,
                          localization::Text(L"workspace.config_io.0d32b1c7e154").c_str(),
                          SoftNoticeKind::Warning, SilentDialogResult::No, SilentDialogResult::No)) {
        return;
    }
    if (ToLowerAscii(pickedPath.extension().wstring()) == L".pnssettings") {
        std::filesystem::path recoveryBackup;
        if (!ImportSettingsPresetFromFile(pickedPath, nullptr, &recoveryBackup)) {
            std::vector<SilentDialogPath> paths = {
                {localization::Text(L"dialog.path.source"), pickedPath.wstring()},
            };
            if (!recoveryBackup.empty()) {
                paths.push_back({localization::Text(L"workspace.config_io.8d9f0b9e9d60"),
                                 recoveryBackup.wstring()});
            }
            ShowSilentMessageDialog(hWnd, ui.menuSettings,
                                    localization::Text(L"workspace.config_io.fc3104c42d86"),
                                    SoftNoticeKind::Warning, paths);
            return;
        }
        ShowSoftNotice(hWnd, localization::Text(L"workspace.config_io.bb59d199ea60").c_str());
        return;
    }
    // Legacy single-file presets remain readable. Newly saved presets are
    // complete .pnssettings files and can be shared between installations.
    std::wstring err;
    auto loaded = LoadWorkspaceConfigFromFile(pickedPath, &err);
    if (!loaded) {
        ShowSilentMessageDialog(hWnd, ui.menuSettings,
                                localization::Text(L"workspace.config_io.fc3104c42d86"),
                                SoftNoticeKind::Warning,
                                {{localization::Text(L"dialog.path.source"), pickedPath.wstring()}});
        return;
    }
    WorkspaceConfig preset = *loaded;
    preset.classesDir = g_config.classesDir;
    preset.cacheDir = g_config.cacheDir;
    preset.colorTone = g_config.colorTone;
    preset.toneVariant = g_config.toneVariant;
    preset.ownerDrawUi = g_config.ownerDrawUi;
    g_config = preset;
    ApplyConfigToUI(hWnd);
    PersistConfig();
    ShowSoftNotice(hWnd, localization::Text(L"workspace.config_io.bb59d199ea60").c_str());
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("LoadSettingsPreset", ex.what());
        ReportMainOperationException(hWnd, L"設定プリセット読込");
    } catch (...) {
        AppendMainOperationExceptionLog("LoadSettingsPreset", nullptr);
        ReportMainOperationException(hWnd, L"設定プリセット読込");
    }
}

namespace {
struct SettingsBundleEntry { std::wstring name; std::filesystem::path path; };
struct SettingsFileSnapshot {
    std::wstring name;
    std::filesystem::path path;
    std::optional<std::string> bytes;
};

static std::vector<SettingsBundleEntry> CurrentSettingsBundleEntries() {
    const std::filesystem::path root(g_workspaceRoot);
    const std::filesystem::path settings = root / L"__pdf_note_workspace__" / L"__settings__";
    const std::filesystem::path themes = root / L"__pdf_note_workspace__" / L"__theme__";
    std::vector<SettingsBundleEntry> entries = {
        {L"workspace.json", root / L"workspace.json"},
        {L"user_palette.json", settings / L"user_palette.json"},
        {L"tool_shortcuts.json", settings / L"tool_shortcuts.json"},
        {L"schedule.json", settings / L"schedule.json"},
        {L"theme/theme.json", themes / L"theme.json"},
    };
    std::error_code ec;
    for (std::filesystem::directory_iterator it(themes, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_symlink(ec) || !it->is_regular_file(ec)) { ec.clear(); continue; }
        const std::wstring name = it->path().filename().wstring();
        const bool valid = name.size() > 11 && name.rfind(L"theme_", 0) == 0 &&
                           name.compare(name.size() - 5, 5, L".json") == 0 &&
                           name.find_first_of(L"\\/:") == std::wstring::npos;
        if (valid) entries.push_back({L"theme/" + name, it->path()});
    }
    return entries;
}

static bool IsThemeBundleEntryName(const std::wstring& name) {
    if (name == L"theme/theme.json") return true;
    if (name.rfind(L"theme/theme_", 0) != 0 || name.size() < 5 ||
        name.compare(name.size() - 5, 5, L".json") != 0) return false;
    const std::wstring file = name.substr(6);
    return file.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos &&
           file.find(L"..") == std::wstring::npos;
}

static std::filesystem::path SettingsBundleEntryPath(const std::wstring& name) {
    const std::filesystem::path root(g_workspaceRoot);
    const std::filesystem::path settings = root / L"__pdf_note_workspace__" / L"__settings__";
    if (name == L"workspace.json") return root / L"workspace.json";
    if (name == L"user_palette.json") return settings / L"user_palette.json";
    if (name == L"tool_shortcuts.json") return settings / L"tool_shortcuts.json";
    if (name == L"schedule.json") return settings / L"schedule.json";
    if (IsThemeBundleEntryName(name)) return root / L"__pdf_note_workspace__" / L"__theme__" / name.substr(6);
    return {};
}

static bool ReadBundleBytes(const std::filesystem::path& path, std::string* out) {
    if (!out) return false;
    out->clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return static_cast<bool>(in) || in.eof();
}

static std::string TrimAsciiForSettingsBundle(std::string s) {
    auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

static void SkipSettingsJsonWhitespace(const std::string& json, size_t* pos) {
    while (pos && *pos < json.size() && std::isspace(static_cast<unsigned char>(json[*pos]))) ++(*pos);
}

static bool ParseSettingsJsonStringToken(const std::string& json, size_t* pos) {
    std::string decoded;
    return json_string::DecodeToken(json, pos, &decoded);
}

static bool ParseSettingsJsonValue(const std::string& json, size_t* pos, int depth);

static bool ParseSettingsJsonLiteral(const std::string& json, size_t* pos, const char* literal) {
    if (!pos || !literal) return false;
    const size_t len = std::char_traits<char>::length(literal);
    if (json.size() - *pos < len || json.compare(*pos, len, literal) != 0) return false;
    *pos += len;
    return true;
}

static bool ParseSettingsJsonNumber(const std::string& json, size_t* pos) {
    if (!pos || *pos >= json.size()) return false;
    size_t i = *pos;
    if (json[i] == '-') ++i;
    if (i >= json.size()) return false;
    if (json[i] == '0') {
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(json[i]))) {
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    } else {
        return false;
    }
    if (i < json.size() && json[i] == '.') {
        ++i;
        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) return false;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    }
    if (i < json.size() && (json[i] == 'e' || json[i] == 'E')) {
        ++i;
        if (i < json.size() && (json[i] == '+' || json[i] == '-')) ++i;
        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) return false;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    }
    *pos = i;
    return true;
}

static bool ParseSettingsJsonArray(const std::string& json, size_t* pos, int depth) {
    if (!pos || *pos >= json.size() || json[*pos] != '[') return false;
    ++(*pos);
    SkipSettingsJsonWhitespace(json, pos);
    if (*pos < json.size() && json[*pos] == ']') {
        ++(*pos);
        return true;
    }
    while (*pos < json.size()) {
        if (!ParseSettingsJsonValue(json, pos, depth + 1)) return false;
        SkipSettingsJsonWhitespace(json, pos);
        if (*pos >= json.size()) return false;
        if (json[*pos] == ',') {
            ++(*pos);
            SkipSettingsJsonWhitespace(json, pos);
            continue;
        }
        if (json[*pos] == ']') {
            ++(*pos);
            return true;
        }
        return false;
    }
    return false;
}

static bool ParseSettingsJsonObject(const std::string& json, size_t* pos, int depth) {
    if (!pos || *pos >= json.size() || json[*pos] != '{') return false;
    ++(*pos);
    SkipSettingsJsonWhitespace(json, pos);
    if (*pos < json.size() && json[*pos] == '}') {
        ++(*pos);
        return true;
    }
    while (*pos < json.size()) {
        if (!ParseSettingsJsonStringToken(json, pos)) return false;
        SkipSettingsJsonWhitespace(json, pos);
        if (*pos >= json.size() || json[*pos] != ':') return false;
        ++(*pos);
        if (!ParseSettingsJsonValue(json, pos, depth + 1)) return false;
        SkipSettingsJsonWhitespace(json, pos);
        if (*pos >= json.size()) return false;
        if (json[*pos] == ',') {
            ++(*pos);
            SkipSettingsJsonWhitespace(json, pos);
            continue;
        }
        if (json[*pos] == '}') {
            ++(*pos);
            return true;
        }
        return false;
    }
    return false;
}

static bool ParseSettingsJsonValue(const std::string& json, size_t* pos, int depth) {
    if (!pos || depth > 64) return false;
    SkipSettingsJsonWhitespace(json, pos);
    if (*pos >= json.size()) return false;
    const char ch = json[*pos];
    if (ch == '{') return ParseSettingsJsonObject(json, pos, depth + 1);
    if (ch == '[') return ParseSettingsJsonArray(json, pos, depth + 1);
    if (ch == '"') return ParseSettingsJsonStringToken(json, pos);
    if (ch == 't') return ParseSettingsJsonLiteral(json, pos, "true");
    if (ch == 'f') return ParseSettingsJsonLiteral(json, pos, "false");
    if (ch == 'n') return ParseSettingsJsonLiteral(json, pos, "null");
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return ParseSettingsJsonNumber(json, pos);
    return false;
}

static bool IsSettingsJsonObjectSyntaxValid(const std::string& rawJson) {
    const std::string json = TrimAsciiForSettingsBundle(rawJson);
    if (json.size() < 2 || json.front() != '{' || json.back() != '}') return false;
    size_t pos = 0;
    if (!ParseSettingsJsonValue(json, &pos, 0)) return false;
    SkipSettingsJsonWhitespace(json, &pos);
    return pos == json.size();
}

static std::optional<std::wstring> ExtractSettingsJsonStringField(const std::string& rawJson,
                                                                  const char* key) {
    if (!key || !*key) return std::nullopt;
    const std::string json = TrimAsciiForSettingsBundle(rawJson);
    size_t pos = 0;
    SkipSettingsJsonWhitespace(json, &pos);
    if (pos >= json.size() || json[pos++] != '{') return std::nullopt;
    for (;;) {
        SkipSettingsJsonWhitespace(json, &pos);
        std::string field;
        if (!json_string::DecodeToken(json, &pos, &field)) return std::nullopt;
        SkipSettingsJsonWhitespace(json, &pos);
        if (pos >= json.size() || json[pos++] != ':') return std::nullopt;
        SkipSettingsJsonWhitespace(json, &pos);
        const size_t start = pos;
        if (!ParseSettingsJsonValue(json, &pos, 0)) return std::nullopt;
        if (field == key) {
            size_t cursor = start;
            std::string decoded;
            if (!json_string::DecodeToken(json, &cursor, &decoded)) return std::nullopt;
            return UTF8ToWide(decoded);
        }
        SkipSettingsJsonWhitespace(json, &pos);
        if (pos >= json.size() || json[pos++] != ',') return std::nullopt;
    }
}

static bool EquivalentSettingsPathText(std::wstring a, std::wstring b) {
    std::replace(a.begin(), a.end(), L'\\', L'/');
    std::replace(b.begin(), b.end(), L'\\', L'/');
    while (a.size() > 1 && a.back() == L'/') a.pop_back();
    while (b.size() > 1 && b.back() == L'/') b.pop_back();
    return a == b;
}

static bool ParseSettingsBundle(const std::string& input, std::map<std::wstring, std::string>* out) {
    if (!out) return false;
    out->clear();
    constexpr std::string_view header = "PDF_NOTE_SETTINGS_BUNDLE_V1\n";
    if (input.rfind(header, 0) != 0) return false;
    size_t pos = header.size();
    constexpr size_t kMaxEntryBytes = 512 * 1024;
    while (pos < input.size()) {
        const size_t lineEnd = input.find('\n', pos);
        if (lineEnd == std::string::npos) return false;
        const std::string line = input.substr(pos, lineEnd - pos);
        pos = lineEnd + 1;
        if (line == "END") return pos == input.size();
        const size_t tab = line.rfind('\t');
        if (tab == std::string::npos) return false;
        const std::string name = line.substr(0, tab);
        size_t size = 0;
        try { size = static_cast<size_t>(std::stoull(line.substr(tab + 1))); } catch (...) { return false; }
        if (size > kMaxEntryBytes || size > input.size() - pos) return false;
        const std::wstring wideName = UTF8ToWide(name);
        if (SettingsBundleEntryPath(wideName).empty()) return false;
        if (!out->emplace(wideName, input.substr(pos, size)).second) return false;
        pos += size;
        if (pos >= input.size() || input[pos++] != '\n') return false;
    }
    return false;
}

static bool BuildCurrentSettingsBundle(std::string* out, std::wstring* outErr) {
    if (!out) return false;
    out->clear();
    if (g_workspaceRoot.empty()) {
        if (outErr) *outErr = L"workspace is not open";
        return false;
    }
    PersistConfig();
    *out = "PDF_NOTE_SETTINGS_BUNDLE_V1\n";
    bool hasWorkspace = false;
    for (const auto& entry : CurrentSettingsBundleEntries()) {
        std::string bytes;
        if (!ReadBundleBytes(entry.path, &bytes)) continue;
        if (entry.name == L"workspace.json") hasWorkspace = true;
        *out += WideToUTF8(entry.name) + "\t" + std::to_string(bytes.size()) + "\n" + bytes + "\n";
    }
    if (!hasWorkspace) {
        if (outErr) *outErr = L"workspace.json was not available for export";
        out->clear();
        return false;
    }
    *out += "END\n";
    return true;
}

static bool WriteAtomicSettingsBytes(const std::filesystem::path& path,
                                     const std::string& bytes,
                                     std::wstring* outErr) {
    if (path.empty()) {
        if (outErr) *outErr = L"empty output path";
        return false;
    }
    if (!path.is_absolute() || path.parent_path().empty()) {
        if (outErr) *outErr = L"output path must be explicitly selected and absolute";
        return false;
    }
    const auto parent = path.parent_path();
    return write_checks::ObservedWriteBytes(g_workspaceRoot, path, bytes.data(), bytes.size(), parent, parent, outErr);
}

static bool PickSettingsPresetSavePath(HWND owner, std::filesystem::path* outPath) {
    if (!outPath) return false;
    outPath->clear();
    IFileSaveDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return false;
    const std::wstring title = localization::Text(L"workspace.config_io.cb4ee15d2058").c_str();
    const std::wstring settingsFilter = localization::Text(L"workspace.config_io.a22471ca2933").c_str();
    const std::wstring allFilesFilter = localization::Text(L"workspace.config_io.4de7ee3c7424").c_str();
    dialog->SetTitle(title.c_str());
    COMDLG_FILTERSPEC filters[] = {
        {settingsFilter.c_str(), L"*.pnssettings"},
        {allFilesFilter.c_str(), L"*.*"}
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(L"pnssettings");
    dialog->SetFileName(L"pdf_note_settings_preset.pnssettings");
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN | FOS_OVERWRITEPROMPT;
        dialog->SetOptions(options);
    }
    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return false;
    }
    if (FAILED(hr)) {
        dialog->Release();
        return false;
    }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item) {
        dialog->Release();
        return false;
    }
    PWSTR rawPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    item->Release();
    dialog->Release();
    if (FAILED(hr) || !rawPath) return false;
    *outPath = std::filesystem::path(rawPath);
    CoTaskMemFree(rawPath);
    return !outPath->empty();
}

static bool PickSettingsPresetOpenPath(HWND owner, std::filesystem::path* outPath) {
    if (!outPath) return false;
    outPath->clear();
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return false;
    const std::wstring title = localization::Text(L"workspace.config_io.84ddc690eb58").c_str();
    const std::wstring settingsFilter = localization::Text(L"workspace.config_io.a22471ca2933").c_str();
    const std::wstring allFilesFilter = localization::Text(L"workspace.config_io.4de7ee3c7424").c_str();
    dialog->SetTitle(title.c_str());
    // Choosing a preset is followed by a separate apply confirmation.
    (void)dialog->SetOkButtonLabel(localization::Text(L"ui.local_path.e5d546893d24").c_str());
    COMDLG_FILTERSPEC filters[] = {
        {settingsFilter.c_str(), L"*.pnssettings"},
        {allFilesFilter.c_str(), L"*.*"}
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        dialog->SetOptions(options);
    }
    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return false;
    }
    if (FAILED(hr)) {
        dialog->Release();
        return false;
    }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item) {
        dialog->Release();
        return false;
    }
    PWSTR rawPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    item->Release();
    dialog->Release();
    if (FAILED(hr) || !rawPath) return false;
    *outPath = std::filesystem::path(rawPath);
    CoTaskMemFree(rawPath);
    return !outPath->empty();
}

static bool ValidateSettingsBundleForImport(const std::map<std::wstring, std::string>& entries,
                                            WorkspaceConfig* outConfig,
                                            bool* outCanUseRawWorkspaceJson,
                                            std::wstring* outErr) {
    if (!outConfig) return false;
    if (outCanUseRawWorkspaceJson) *outCanUseRawWorkspaceJson = false;
    const auto ws = entries.find(L"workspace.json");
    if (ws == entries.end()) {
        if (outErr) *outErr = L"workspace.json is missing from the settings bundle";
        return false;
    }
    if (!IsSettingsJsonObjectSyntaxValid(ws->second)) {
        if (outErr) *outErr = L"workspace.json is not valid JSON";
        return false;
    }

    std::filesystem::path settingsDir;
    if (!EnsureWorkspaceResourceDirs(&settingsDir)) {
        if (outErr) *outErr = L"could not create __settings__";
        return false;
    }
    const std::filesystem::path temp = settingsDir / (L".__import_workspace__." +
        std::to_wstring(static_cast<unsigned long long>(GetTickCount64())) + L".json");
    std::wstring err;
    if (!write_checks::ObservedWriteUtf8(g_workspaceRoot, temp, ws->second, settingsDir, settingsDir, &err)) {
        if (outErr) *outErr = L"could not stage imported workspace.json: " + err;
        return false;
    }
    auto imported = LoadWorkspaceConfigFromFile(temp, &err);
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    if (!imported) {
        if (outErr) *outErr = err.empty() ? L"imported workspace.json was rejected" : err;
        return false;
    }

    for (const auto& kv : entries) {
        if (kv.first == L"workspace.json") continue;
        if (!IsSettingsJsonObjectSyntaxValid(kv.second)) {
            if (outErr) *outErr = kv.first + L" is not valid JSON";
            return false;
        }
    }
    const auto rawClassesDir = ExtractSettingsJsonStringField(ws->second, "classesDir");
    const auto rawCacheDir = ExtractSettingsJsonStringField(ws->second, "cacheDir");
    const bool classesDirEquivalent = rawClassesDir && EquivalentSettingsPathText(*rawClassesDir, g_config.classesDir);
    const bool cacheDirEquivalent = rawCacheDir &&
        (EquivalentSettingsPathText(*rawCacheDir, g_config.cacheDir) ||
         (IsDefaultCacheDir(*rawCacheDir) && IsDefaultCacheDir(g_config.cacheDir)));
    const bool canUseRawWorkspaceJson = classesDirEquivalent && cacheDirEquivalent;
    imported->classesDir = g_config.classesDir;
    imported->cacheDir = g_config.cacheDir;
    *outConfig = *imported;
    if (outCanUseRawWorkspaceJson) *outCanUseRawWorkspaceJson = canUseRawWorkspaceJson;
    return true;
}

static std::vector<SettingsFileSnapshot> CaptureSettingsFileSnapshot(const std::map<std::wstring, std::string>& incoming) {
    std::vector<SettingsFileSnapshot> snapshot;
    std::set<std::wstring> seen;
    for (const auto& entry : CurrentSettingsBundleEntries()) {
        SettingsFileSnapshot s;
        s.name = entry.name;
        s.path = entry.path;
        std::string bytes;
        if (ReadBundleBytes(entry.path, &bytes)) s.bytes = std::move(bytes);
        snapshot.push_back(std::move(s));
        seen.insert(entry.name);
    }
    for (const auto& kv : incoming) {
        if (!seen.insert(kv.first).second) continue;
        const std::filesystem::path path = SettingsBundleEntryPath(kv.first);
        if (path.empty()) continue;
        SettingsFileSnapshot s;
        s.name = kv.first;
        s.path = path;
        std::string bytes;
        if (ReadBundleBytes(path, &bytes)) s.bytes = std::move(bytes);
        snapshot.push_back(std::move(s));
    }
    return snapshot;
}

static std::filesystem::path CreateSettingsImportBackupDir(const std::vector<SettingsFileSnapshot>& snapshot,
                                                           std::wstring* outErr) {
    const std::filesystem::path root(g_workspaceRoot);
    if (root.empty()) return {};
    const std::filesystem::path escapeRoot = root / L"__pdf_note_workspace__" / L"__escape__";
    std::error_code ec;
    std::filesystem::create_directories(escapeRoot, ec);
    if (ec) {
        if (outErr) *outErr = L"could not create settings import backup root";
        return {};
    }
    std::filesystem::path backupDir;
    for (int attempt = 0; attempt < 100; ++attempt) {
        backupDir = escapeRoot / (L"settings_import_" +
            std::to_wstring(static_cast<unsigned long long>(GetTickCount64())) +
            L"_" + std::to_wstring(attempt));
        std::filesystem::create_directory(backupDir, ec);
        if (!ec) break;
        ec.clear();
    }
    if (backupDir.empty() || !std::filesystem::is_directory(backupDir, ec) || ec) {
        if (outErr) *outErr = L"could not create settings import backup directory";
        return {};
    }

    std::ostringstream manifest;
    manifest << "PDF_NOTE_SETTINGS_IMPORT_BACKUP_V1\n";
    for (const auto& s : snapshot) {
        manifest << WideToUTF8(s.name) << "\t" << WideToUTF8(s.path.wstring()) << "\t"
                 << (s.bytes ? "present" : "missing") << "\n";
        if (!s.bytes) continue;
        std::wstring writeErr;
        std::wstring backupName = s.name;
        std::replace(backupName.begin(), backupName.end(), L'/', L'_');
        const std::filesystem::path backupFile = backupDir / backupName;
        if (!write_checks::ObservedWriteBytes(g_workspaceRoot, backupFile, s.bytes->data(), s.bytes->size(),
                                            backupDir, backupDir, &writeErr)) {
            if (outErr) *outErr = L"could not write settings import backup: " + writeErr;
            return {};
        }
    }
    std::wstring writeErr;
    const std::string manifestBytes = manifest.str();
    if (!write_checks::ObservedWriteUtf8(g_workspaceRoot, backupDir / L"manifest.txt", manifestBytes, backupDir, backupDir, &writeErr)) {
        if (outErr) *outErr = L"could not write settings import backup manifest: " + writeErr;
        return {};
    }
    return backupDir;
}

static bool RestoreSettingsFileSnapshot(const std::vector<SettingsFileSnapshot>& snapshot,
                                        std::wstring* outErr) {
    bool ok = true;
    std::wstring details;
    for (const auto& s : snapshot) {
        if (s.bytes) {
            std::wstring err;
            if (!WriteAtomicSettingsBytes(s.path, *s.bytes, &err)) {
                ok = false;
                details += L"\n" + s.path.wstring() + L": " + err;
            }
        } else {
            std::error_code ec;
            std::filesystem::remove(s.path, ec);
            if (ec) {
                ok = false;
                details += L"\n" + s.path.wstring() + L": remove failed";
            }
        }
    }
    if (!ok && outErr) *outErr = details;
    return ok;
}
} // namespace

bool ExportSettingsPresetToFile(const std::filesystem::path& outputPath, std::wstring* outErr) {
    std::string bundle;
    if (!BuildCurrentSettingsBundle(&bundle, outErr)) return false;
    return WriteAtomicSettingsBytes(outputPath, bundle, outErr);
}

bool ImportSettingsPresetFromFile(const std::filesystem::path& inputPath, std::wstring* outErr,
                                  std::filesystem::path* outRecoveryBackup) {
    if (outRecoveryBackup) outRecoveryBackup->clear();
    if (g_workspaceRoot.empty()) {
        if (outErr) *outErr = L"workspace is not open";
        return false;
    }
    std::string raw;
    std::map<std::wstring, std::string> entries;
    if (!ReadBundleBytes(inputPath, &raw) || !ParseSettingsBundle(raw, &entries)) {
        if (outErr) *outErr = L"settings bundle format is invalid";
        return false;
    }
    WorkspaceConfig imported;
    bool canUseRawWorkspaceJson = false;
    if (!ValidateSettingsBundleForImport(entries, &imported, &canUseRawWorkspaceJson, outErr)) return false;

    const auto snapshot = CaptureSettingsFileSnapshot(entries);
    const std::filesystem::path backupDir = CreateSettingsImportBackupDir(snapshot, outErr);
    if (backupDir.empty()) return false;
    if (outRecoveryBackup) *outRecoveryBackup = backupDir;

    std::wstring applyErr;
    bool applied = false;
    try {
        int auxWrites = 0;
        for (const auto& entry : CurrentSettingsBundleEntries()) {
            const std::wstring& name = entry.name;
            // Theme entries are additive. Removing a custom theme merely because a
            // different preset does not reference it would lose user data.
            if (name == L"workspace.json" || IsThemeBundleEntryName(name)) continue;
            const auto importedEntry = entries.find(name);
            fault_injection::MaybeThrow(L"settings_import_before_aux_write");
            if (importedEntry == entries.end()) {
                std::error_code ec;
                std::filesystem::remove(entry.path, ec);
                if (ec) {
                    applyErr = L"could not remove omitted settings file: " + entry.path.wstring();
                    break;
                }
            } else if (!WriteAtomicSettingsBytes(entry.path, importedEntry->second, &applyErr)) {
                applyErr = L"could not write imported settings file: " + entry.path.wstring() + L"\n" + applyErr;
                break;
            }
            ++auxWrites;
            if (auxWrites == 1) fault_injection::MaybeThrow(L"settings_import_after_first_aux_write");
        }
        if (applyErr.empty()) {
            for (const auto& kv : entries) {
                if (!IsThemeBundleEntryName(kv.first)) continue;
                const std::filesystem::path target = SettingsBundleEntryPath(kv.first);
                fault_injection::MaybeThrow(L"settings_import_before_aux_write");
                if (target.empty() || !WriteAtomicSettingsBytes(target, kv.second, &applyErr)) {
                    applyErr = L"could not write imported theme file: " + target.wstring() + L"\n" + applyErr;
                    break;
                }
                ++auxWrites;
            }
        }
        if (applyErr.empty()) {
            fault_injection::MaybeThrow(L"settings_import_before_workspace_write");
            const std::filesystem::path workspaceJsonPath = std::filesystem::path(g_workspaceRoot) / L"workspace.json";
            if (canUseRawWorkspaceJson) {
                const auto workspaceEntry = entries.find(L"workspace.json");
                if (workspaceEntry == entries.end() ||
                    !WriteAtomicSettingsBytes(workspaceJsonPath, workspaceEntry->second, &applyErr)) {
                    applyErr = L"could not write imported workspace.json";
                } else {
                    applied = true;
                }
            } else if (!SaveWorkspaceConfigToFile(workspaceJsonPath, imported)) {
                applyErr = L"could not write imported workspace.json";
            } else {
                applied = true;
            }
        }
    } catch (const std::exception& ex) {
        applyErr = UTF8ToWide(ex.what());
    } catch (...) {
        applyErr = L"unknown exception while applying imported settings";
    }

    if (!applied) {
        std::wstring rollbackErr;
        const bool rollbackOk = RestoreSettingsFileSnapshot(snapshot, &rollbackErr);
        if (outErr) {
            *outErr = L"settings import failed; recovery backup is in:\n" + backupDir.wstring();
            if (!applyErr.empty()) *outErr += L"\n\nreason:\n" + applyErr;
            if (!rollbackOk) *outErr += L"\n\nrollback failure:\n" + rollbackErr;
        }
        return false;
    }

    LoadThemeConfig(g_workspaceRoot);
    g_config = LoadWorkspaceConfig(g_workspaceRoot);
    ApplyConfigToUI(nullptr);
    return true;
}

void SaveAllManual(HWND hWnd) {
    try {
    // A manual save is an explicit boundary: commit the active TextBox before
    // taking the annotation snapshot, including any active IME composition.
    CommitActiveTextEditing(true);
    preview_trace::Append(
        L"SaveAllManual",
        L"begin noteDirty=" + preview_trace::Bool(g_noteDirty) +
        L" noteNeedsIntegrate=" + preview_trace::Bool(g_noteNeedsIntegrate) +
        L" annotsDirty=" + preview_trace::Bool(g_annotsDirty) +
        L" annotsNeedsIntegrate=" + preview_trace::Bool(g_annotsNeedsIntegrate) +
        L" notePath=" + g_currentNotePath +
        L" logicalPdfPath=" + CurrentLogicalPdfPath());
    file_output::SaveTransactionStartResult start =
        file_output::StartBackgroundSaveAndIntegrateTransaction(hWnd);
    const bool ok = (start != file_output::SaveTransactionStartResult::Failed);
    preview_trace::Append(
        L"SaveAllManual",
        L"transaction_start=" + std::to_wstring(static_cast<int>(start)) +
        L" ok=" + preview_trace::Bool(ok) +
        L" noteDirty=" + preview_trace::Bool(g_noteDirty) +
        L" noteNeedsIntegrate=" + preview_trace::Bool(g_noteNeedsIntegrate) +
        L" annotsDirty=" + preview_trace::Bool(g_annotsDirty) +
        L" annotsNeedsIntegrate=" + preview_trace::Bool(g_annotsNeedsIntegrate));
    if (!ok) {
        RefreshMainMenuBar(hWnd);
        RefreshStatusDisplay(hWnd);
        preview_trace::Append(L"SaveAllManual", L"end_without_finalize");
        return;
    }
    if (start == file_output::SaveTransactionStartResult::Started) {
        preview_trace::Append(L"SaveAllManual", L"background_started");
        return;
    }
    FinalizeManualSaveUi(hWnd, /*updateWindowTitleAfterSave=*/true);
    preview_trace::Append(
        L"SaveAllManual",
        L"end notePath=" + g_currentNotePath +
        L" pdfPath=" + g_pdf.path);
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("SaveAllManual", ex.what());
        preview_trace::Append(L"SaveAllManual", L"exception=std");
        ReportMainOperationException(hWnd, L"統合保存");
    } catch (...) {
        AppendMainOperationExceptionLog("SaveAllManual", nullptr);
        preview_trace::Append(L"SaveAllManual", L"exception=unknown");
        ReportMainOperationException(hWnd, L"統合保存");
    }
}

void SaveCurrentNoteManual(HWND hWnd) {
    try {
    if (g_currentNotePath.empty()) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.config_io.eb5c57ff8257").c_str(),
                       SoftNoticeKind::Warning);
        return;
    }

    if (!file_output::SaveNoteFile(hWnd)) {
        RefreshMainMenuBar(hWnd);
        RefreshStatusDisplay(hWnd);
        return;
    }
    FinalizeManualSaveUi(hWnd, /*updateWindowTitleAfterSave=*/true);

    std::wstring msg = localization::Text(L"workspace.config_io.6c9e238c789c").c_str();
    if (g_annotsDirty || g_annotsNeedsIntegrate) {
        msg += localization::Text(L"workspace.config_io.e270e475dd85").c_str();
    }
    ShowSoftNotice(hWnd, msg);
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("SaveCurrentNoteManual", ex.what());
        ReportMainOperationException(hWnd, L"ノート直接保存");
    } catch (...) {
        AppendMainOperationExceptionLog("SaveCurrentNoteManual", nullptr);
        ReportMainOperationException(hWnd, L"ノート直接保存");
    }
}

void ShowRecoveryDialog(HWND hWnd) {
    const auto& ui = GetUiText();
    if (g_workspaceRoot.empty()) {
        const std::wstring msg = localization::Text(L"workspace.config_io.55c168692754").c_str();
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__pdf_note_workspace__";
    std::filesystem::path backupRoot = resource / L"__escape__" / L"backup";

    std::wstring msg = localization::Text(L"workspace.recovery.action_prompt");

    SilentDialogOptions dialog;
    dialog.title = ui.menuRecovery;
    dialog.message = msg;
    dialog.kind = SoftNoticeKind::Warning;
    dialog.buttons = SilentDialogButtons::YesNoCancel;
    dialog.yesLabel = localization::Text(L"dialog.action.restore");
    dialog.noLabel = localization::Text(L"dialog.action.save");
    dialog.defaultResult = SilentDialogResult::Cancel;
    dialog.escapeResult = SilentDialogResult::Cancel;
    SilentDialogResult res = ShowSilentDialog(hWnd, dialog);
    if (res == SilentDialogResult::Cancel || res == SilentDialogResult::None) return;

    if (res == SilentDialogResult::No) {
        file_output::IntegrateStagedNoteAndAnnotations(hWnd);
        RefreshStatusDisplay(hWnd);
        return;
    }

    // Backup restore path
    if (g_noteDirty || g_annotsDirty || g_noteNeedsIntegrate || g_annotsNeedsIntegrate) {
        const std::wstring warn = localization::Text(L"workspace.config_io.6df3218d1994").c_str();
        SilentDialogOptions confirm;
        confirm.title = ui.menuRecovery;
        confirm.message = warn;
        confirm.kind = SoftNoticeKind::Warning;
        confirm.buttons = SilentDialogButtons::YesNo;
        confirm.defaultResult = SilentDialogResult::No;
        confirm.escapeResult = SilentDialogResult::No;
        if (ShowSilentDialog(hWnd, confirm) != SilentDialogResult::Yes) return;
    }

    std::filesystem::path pickedMeta;
    if (!PromptBackupList(hWnd, backupRoot, ui.menuRecovery,
                          localization::Text(L"dialog.common.575a7e91c663"), pickedMeta)) {
        return;
    }

    std::filesystem::path restoredDest;
    if (!file_output::RestoreFromBackupMeta(hWnd, pickedMeta, &restoredDest)) {
        return;
    }

    // If the restored file is currently open, reload views so the UI reflects disk state.
    if (!restoredDest.empty()) {
        if (restoredDest.extension() == L".clrop") {
            InvalidateAnnotHistoryForPath(restoredDest.wstring());
        }
        std::wstring destKey = NormalizePathKey(restoredDest.wstring());
        if (!g_currentNotePath.empty() && destKey == NormalizePathKey(g_currentNotePath)) {
            LoadNoteFile(hWnd, restoredDest.wstring());
            SyncBottomPaneAfterNoteLoad(hWnd);
        } else if (g_pdf.kind == DocKind::Pdf && !CurrentLogicalPdfPath().empty()) {
            std::wstring curClropKey = NormalizePathKey(clrop_bridge::ClropPathForPdf(CurrentLogicalPdfPath()));
            if (destKey == curClropKey) {
                LoadAnnotationsForCurrentPdf(hWnd);
            }
        }
    }
    RefreshStatusDisplay(hWnd);
    ShowSoftNotice(hWnd, localization::Text(L"workspace.config_io.6d9231d0b1c3").c_str());
}

void ShowRestoreBackupListDialogAndExecute(HWND hWnd) {
    if (g_workspaceRoot.empty()) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.config_io.55c168692754").c_str(),
                       SoftNoticeKind::Warning);
        return;
    }
    
    if (g_noteDirty || g_annotsDirty || g_noteNeedsIntegrate || g_annotsNeedsIntegrate) {
        const std::wstring warn = localization::Text(L"workspace.config_io.6df3218d1994").c_str();
        SilentDialogOptions confirm;
        confirm.title = localization::Text(L"workspace.config_io.98d1854bd23d").c_str();
        confirm.message = warn;
        confirm.kind = SoftNoticeKind::Warning;
        confirm.buttons = SilentDialogButtons::YesNo;
        confirm.defaultResult = SilentDialogResult::No;
        confirm.escapeResult = SilentDialogResult::No;
        if (ShowSilentDialog(hWnd, confirm) != SilentDialogResult::Yes) return;
    }

    std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__pdf_note_workspace__";
    std::filesystem::path backupRoot = resource / L"__escape__" / L"backup";
    std::filesystem::path pickedMeta;
    
    if (PromptBackupList(hWnd, backupRoot,
                         localization::Text(L"dialog.common.95d9abd66237"),
                         localization::Text(L"dialog.common.575a7e91c663"),
                         pickedMeta)) {
        std::filesystem::path restoredDest;
        if (!file_output::RestoreFromBackupMeta(hWnd, pickedMeta, &restoredDest)) {
            return;
        }

        if (!restoredDest.empty()) {
            if (restoredDest.extension() == L".clrop") {
                InvalidateAnnotHistoryForPath(restoredDest.wstring());
            }
            std::wstring destKey = NormalizePathKey(restoredDest.wstring());
            if (!g_currentNotePath.empty() && destKey == NormalizePathKey(g_currentNotePath)) {
                LoadNoteFile(hWnd, restoredDest.wstring());
                SyncBottomPaneAfterNoteLoad(hWnd);
            } else if (g_pdf.kind == DocKind::Pdf && !CurrentLogicalPdfPath().empty()) {
                std::wstring curClropKey = NormalizePathKey(clrop_bridge::ClropPathForPdf(CurrentLogicalPdfPath()));
                if (destKey == curClropKey) {
                    LoadAnnotationsForCurrentPdf(hWnd);
                }
            }
        }
        RefreshStatusDisplay(hWnd);
        ShowSoftNotice(hWnd, localization::Text(L"workspace.config_io.6d9231d0b1c3").c_str());
    }
}

void ShowDeleteSavedBackupDialog(HWND hWnd) {
    const auto& ui = GetUiText();
    if (g_workspaceRoot.empty()) {
        const std::wstring msg = localization::Text(L"workspace.config_io.55c168692754").c_str();
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Warning);
        return;
    }

    std::filesystem::path backupRoot = std::filesystem::path(g_workspaceRoot) / L"__pdf_note_workspace__" / L"__escape__" / L"backup";
    std::filesystem::path selected;
    if (!PromptBackupList(hWnd, backupRoot, ui.menuDeleteBackup,
                          localization::Text(L"menu.common.delete"), selected)) {
        return;
    }
    const std::wstring fileName = selected.filename().wstring();
    if (fileName.size() < 9 || fileName.rfind(L".meta.txt") != fileName.size() - 9) {
        ShowMainMessageDialog(
            hWnd,
            ui.menuDeleteBackup,
            localization::Text(L"workspace.config_io.76a39734f407").c_str(),
            SoftNoticeKind::Warning);
        return;
    }

    const std::wstring confirm = localization::Text(L"workspace.config_io.bdd6d772f9d4").c_str();
    if (!ConfirmMainYesNo(hWnd, ui.menuDeleteBackup, confirm, SoftNoticeKind::Warning,
                          SilentDialogResult::No, SilentDialogResult::No)) {
        return;
    }

    std::wstring err;
    if (!file_output::DeleteBackupMeta(selected, &err)) {
        if (err.empty()) {
            err = localization::Text(L"workspace.config_io.cb5f4466e22d").c_str();
        }
        ShowMainMessageDialog(hWnd, ui.menuDeleteBackup, err, SoftNoticeKind::Warning);
        return;
    }

    ShowSoftNotice(hWnd,
                   localization::Text(L"workspace.config_io.ad8507f8d221").c_str());
    RefreshMainMenuBar(hWnd);
}
