#include "workspace/workspace_config_io.h"
#include "core/app_core.h"
#include "ui/core/main_window_api.h"
#include "ui/dialogs/dialogs.h"
#include "clrop/bridge.h"
#include "file_output/file_output.h"
#include "pdf_view/pdf_view.h"
#include "core/preview_trace.h"
#include "core/atomic_write.h"
#include "note_view/note_view.h"
#include "settings/settings.h"
#include "bridge/view_bridge.h"
#include "core/font_list.h"
#include "math/math_render.h"
#include "core/fault_injection.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <shlobj.h>
#include <shobjidl.h>
#include <sstream>
#include <unordered_set>

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
        OnExitNoteNormalMode();
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
    RefreshBottomPaneView();
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
    RefreshBottomPaneView();
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
    ClearCurrentNoteUndoHistory();
    preview_trace::Append(
        L"FinalizeManualSaveUi",
        L"after_clear_note_undo elapsed_ms=" + preview_trace::ElapsedMs(startTick));
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

    std::wstring msg = IsEnglishUi()
        ? L"Temporary external lecture path cannot be used because access failed.\n\npath:\n" + dir.wstring()
        : std::wstring(g_config.studentMode
                           ? L"一時外部授業パスにアクセスできないため、このパスは今回使用しません。\n\npath:\n"
                           : L"一時外部上位項目パスにアクセスできないため、このパスは今回使用しません。\n\npath:\n") + dir.wstring();
    msg += IsEnglishUi()
        ? L"\n\nThe registration is not removed automatically. Check folder permissions, removable drive state, or security policy. If this path is no longer needed, remove it from the temporary external lecture list."
        : (g_config.studentMode
               ? L"\n\n登録は自動削除しません。フォルダ権限、外部ドライブの接続状態、セキュリティ制御を確認してください。不要な場合は一時外部授業リストから削除してください。"
               : L"\n\n登録は自動削除しません。フォルダ権限、外部ドライブの接続状態、セキュリティ制御を確認してください。不要な場合は一時外部上位項目リストから削除してください。");
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
    std::filesystem::path resource = std::filesystem::path(root) / L"__resource__";
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
    std::wstring msg = IsEnglishUi()
        ? L"Workspace is not writable.\nThis operation is canceled and no edit/open action is performed.\n\nroot:\n" + g_workspaceRoot
        : L"ワークスペースに書き込みできません。\nこの操作は中断され、編集/オープン処理は行いません。\n\nroot:\n" + g_workspaceRoot;
    msg += IsEnglishUi()
        ? L"\n\nCheck access control / security policy for this folder."
        : L"\n\nこのフォルダのアクセス権・セキュリティ制御を確認してください。";
    if (!err.empty()) msg += L"\n\n" + err;
    ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
    return false;
}

bool VerifyDirReadableWritableForEditing(HWND owner, const std::filesystem::path& dir, const wchar_t* labelJa, const wchar_t* labelEn) {
    std::wstring readErr;
    if (!TryOpenDirForList(dir, &readErr)) {
        ForgetWritableProbe(dir);
        std::wstring msg = IsEnglishUi()
            ? std::wstring(labelEn ? labelEn : L"Folder") + L" is not readable.\nThis operation is canceled and no edit/open action is performed.\n\npath:\n" + dir.wstring()
            : std::wstring(labelJa ? labelJa : L"フォルダ") + L"を読み込めません。\nこの操作は中断され、編集/オープン処理は行いません。\n\npath:\n" + dir.wstring();
        msg += IsEnglishUi()
            ? L"\n\nCheck access control / security policy for this folder."
            : L"\n\nこのフォルダのアクセス権・セキュリティ制御を確認してください。";
        if (!readErr.empty()) msg += L"\n\n" + readErr;
        if (IsTempExternalLecturePath(dir.wstring())) {
            msg += IsEnglishUi()
                ? L"\n\nThis temporary external lecture registration is not removed automatically."
                : (g_config.studentMode ? L"\n\nこの一時外部授業の登録は自動削除しません。"
                                        : L"\n\nこの一時外部上位項目の登録は自動削除しません。");
        }
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    std::wstring writeErr;
    if (!IsWritableProbeCached(dir) && !TryWriteTempDeleteOnCloseFile(dir, &writeErr)) {
        ForgetWritableProbe(dir);
        std::wstring msg = IsEnglishUi()
            ? std::wstring(labelEn ? labelEn : L"Folder") + L" is not writable.\nThis operation is canceled and no edit/open action is performed.\n\npath:\n" + dir.wstring()
            : std::wstring(labelJa ? labelJa : L"フォルダ") + L"に書き込みできません。\nこの操作は中断され、編集/オープン処理は行いません。\n\npath:\n" + dir.wstring();
        msg += IsEnglishUi()
            ? L"\n\nCheck access control / security policy for this folder."
            : L"\n\nこのフォルダのアクセス権・セキュリティ制御を確認してください。";
        if (!writeErr.empty()) msg += L"\n\n" + writeErr;
        if (IsTempExternalLecturePath(dir.wstring())) {
            msg += IsEnglishUi()
                ? L"\n\nThis temporary external lecture registration is not removed automatically."
                : (g_config.studentMode ? L"\n\nこの一時外部授業の登録は自動削除しません。"
                                        : L"\n\nこの一時外部上位項目の登録は自動削除しません。");
        }
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    RememberWritableProbe(dir);
    return true;
}

static std::filesystem::path WeaklyCanonicalNoThrow(const std::filesystem::path& p) {
    if (p.empty()) return {};
    std::error_code ec;
    auto can = std::filesystem::weakly_canonical(p, ec);
    if (ec) return {};
    return can;
}

static bool IsPathUnderDir(const std::filesystem::path& dir, const std::filesystem::path& path) {
    if (dir.empty() || path.empty()) return false;
    auto dirCan = WeaklyCanonicalNoThrow(dir);
    auto pathCan = WeaklyCanonicalNoThrow(path);
    if (dirCan.empty() || pathCan.empty()) return false;

    std::filesystem::path rel = pathCan.lexically_relative(dirCan);
    if (rel.empty()) return false;
    if (rel.is_absolute()) return false;

    auto it = rel.begin();
    if (it == rel.end()) return false;
    if (*it == L"..") return false;
    return true;
}

static std::optional<std::wstring> PickFileUnderLocked(HWND owner,
                                                       const std::filesystem::path& dir,
                                                       const std::wstring& title) {
    auto picked = PickFileUnder(owner, dir, title);
    if (!picked) return std::nullopt;
    std::filesystem::path pickedPath(*picked);
    if (!IsPathUnderDir(dir, pickedPath)) {
        std::wstring msg = L"指定フォルダの外のファイルは選択できません。\n\nroot:\n" +
                           dir.wstring() + L"\n\npicked:\n" + pickedPath.wstring();
        const std::wstring dialogTitle = title.empty() ? GetUiText().menuSettings : title;
        ShowSilentMessageDialog(owner, dialogTitle, msg, SoftNoticeKind::Warning);
        return std::nullopt;
    }
    return pickedPath.wstring();
}

static std::wstring SanitizePresetName(const std::wstring& name) {
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t c : name) {
        switch (c) {
        case L'\\': case L'/': case L':': case L'*':
        case L'?': case L'"': case L'<': case L'>': case L'|':
            out.push_back(L'_');
            break;
        default:
            if (c < 0x20) {
                out.push_back(L'_');
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out = TrimWhitespace(out);
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) {
        out.pop_back();
    }
    if (out == L"." || out == L"..") out.clear();
    return out;
}

void SaveSettingsPreset(HWND hWnd) {
    try {
    const auto& ui = GetUiText();
    if (g_workspaceRoot.empty()) {
        ShowSoftNotice(hWnd, L"ワークスペースが開かれていません。", SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path settingsDir;
    if (!EnsureWorkspaceResourceDirs(&settingsDir)) {
        ShowSoftNotice(hWnd, L"__resource__ の作成に失敗しました。", SoftNoticeKind::Warning);
        return;
    }
    std::wstring name;
    if (!PromptSimpleText(hWnd, L"プリセット名", L"", name)) return;
    name = SanitizePresetName(name);
    if (name.empty()) {
        ShowSoftNotice(hWnd, L"プリセット名が無効です。", SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path fileName(name);
    if (!fileName.has_extension()) fileName += L".json";
    std::filesystem::path presetPath = settingsDir / fileName;
    std::error_code existsEc;
    if (std::filesystem::exists(presetPath, existsEc) && !existsEc) {
        std::wstring msg = L"既に存在します。上書きしますか？\n\n" + presetPath.wstring();
        SilentDialogOptions options;
        options.title = ui.menuSettings;
        options.message = msg;
        options.kind = SoftNoticeKind::Warning;
        options.buttons = SilentDialogButtons::YesNo;
        options.defaultResult = SilentDialogResult::No;
        options.escapeResult = SilentDialogResult::No;
        if (ShowSilentDialog(hWnd, options) != SilentDialogResult::Yes) {
            return;
        }
    }
    PersistConfig();
    if (!SaveWorkspaceConfigToFile(presetPath, g_config)) {
        ShowSoftNotice(hWnd, L"設定プリセットを保存できませんでした。", SoftNoticeKind::Warning);
        return;
    }
    ShowSoftNotice(hWnd, L"設定プリセットを保存しました。");
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
        ShowSoftNotice(hWnd, L"ワークスペースが開かれていません。", SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path settingsDir;
    if (!EnsureWorkspaceResourceDirs(&settingsDir)) {
        ShowSoftNotice(hWnd, L"__resource__ の作成に失敗しました。", SoftNoticeKind::Warning);
        return;
    }
    auto picked = PickFileUnderLocked(hWnd, settingsDir, L"設定プリセットを選択");
    if (!picked) return;
    std::wstring err;
    auto loaded = LoadWorkspaceConfigFromFile(std::filesystem::path(*picked), &err);
    if (!loaded) {
        std::wstring msg = L"設定プリセットの読み込みに失敗しました。\n\n" + std::filesystem::path(*picked).wstring();
        if (!err.empty()) msg += L"\n\n理由:\n" + err;
        ShowSilentMessageDialog(hWnd, ui.menuSettings, msg, SoftNoticeKind::Warning);
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
    ShowSoftNotice(hWnd, L"設定プリセットを読み込みました。");
    } catch (const std::exception& ex) {
        AppendMainOperationExceptionLog("LoadSettingsPreset", ex.what());
        ReportMainOperationException(hWnd, L"設定プリセット読込");
    } catch (...) {
        AppendMainOperationExceptionLog("LoadSettingsPreset", nullptr);
        ReportMainOperationException(hWnd, L"設定プリセット読込");
    }
}

namespace {
struct SettingsBundleEntry { const wchar_t* name; std::filesystem::path path; };
struct SettingsFileSnapshot {
    const wchar_t* name = L"";
    std::filesystem::path path;
    std::optional<std::string> bytes;
};

static std::vector<SettingsBundleEntry> CurrentSettingsBundleEntries() {
    const std::filesystem::path root(g_workspaceRoot);
    const std::filesystem::path settings = root / L"__resource__" / L"__settings__";
    return {{L"workspace.json", root / L"workspace.json"},
            {L"user_palette.json", settings / L"user_palette.json"},
            {L"tool_shortcuts.json", settings / L"tool_shortcuts.json"},
            {L"schedule.json", settings / L"schedule.json"}};
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
    if (!pos || *pos >= json.size() || json[*pos] != '"') return false;
    ++(*pos);
    while (*pos < json.size()) {
        const char ch = json[*pos];
        ++(*pos);
        if (ch == '"') return true;
        if (ch != '\\') continue;
        if (*pos >= json.size()) return false;
        const char esc = json[*pos];
        ++(*pos);
        if (esc == 'u') {
            if (json.size() - *pos < 4) return false;
            for (size_t i = 0; i < 4; ++i) {
                if (!std::isxdigit(static_cast<unsigned char>(json[*pos + i]))) return false;
            }
            *pos += 4;
            continue;
        }
        switch (esc) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
            break;
        default:
            return false;
        }
    }
    return false;
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
    try {
        const std::regex re(std::string("\"") + key + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch m;
        if (std::regex_search(rawJson, m, re)) return UTF8ToWide(m[1].str());
    } catch (...) {
    }
    return std::nullopt;
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
        if (name != "workspace.json" && name != "user_palette.json" &&
            name != "tool_shortcuts.json" && name != "schedule.json") return false;
        if (!out->emplace(UTF8ToWide(name), input.substr(pos, size)).second) return false;
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
        if (std::wstring(entry.name) == L"workspace.json") hasWorkspace = true;
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
    std::error_code cwdEc;
    const std::filesystem::path parent = path.parent_path().empty()
        ? std::filesystem::current_path(cwdEc)
        : path.parent_path();
    if (cwdEc) {
        if (outErr) *outErr = L"could not resolve current directory";
        return false;
    }
    return atomic_write::AtomicWriteBytes(path, bytes.data(), bytes.size(), parent, parent, outErr);
}

static bool PickSettingsBundleSavePath(HWND owner, std::filesystem::path* outPath) {
    if (!outPath) return false;
    outPath->clear();
    IFileSaveDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return false;
    dialog->SetTitle(IsEnglishUi()
        ? L"Export user settings for version migration"
        : L"バージョン更新用にユーザー設定を書き出し");
    COMDLG_FILTERSPEC filters[] = {
        {IsEnglishUi() ? L"PDF Note settings transfer bundle (*.pnssettings)" : L"PDF Note 設定引き継ぎファイル (*.pnssettings)", L"*.pnssettings"},
        {IsEnglishUi() ? L"All files (*.*)" : L"すべてのファイル (*.*)", L"*.*"}
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(L"pnssettings");
    dialog->SetFileName(L"pdf_note_user_settings_transfer.pnssettings");
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

static bool PickSettingsBundleOpenPath(HWND owner, std::filesystem::path* outPath) {
    if (!outPath) return false;
    outPath->clear();
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return false;
    dialog->SetTitle(IsEnglishUi()
        ? L"Import user settings from a previous version"
        : L"以前のバージョンのユーザー設定を読み込み");
    COMDLG_FILTERSPEC filters[] = {
        {IsEnglishUi() ? L"PDF Note settings transfer bundle (*.pnssettings)" : L"PDF Note 設定引き継ぎファイル (*.pnssettings)", L"*.pnssettings"},
        {IsEnglishUi() ? L"All files (*.*)" : L"すべてのファイル (*.*)", L"*.*"}
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
    if (!atomic_write::AtomicWriteUtf8(temp, ws->second, settingsDir, settingsDir, &err)) {
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

static std::vector<SettingsFileSnapshot> CaptureSettingsFileSnapshot() {
    std::vector<SettingsFileSnapshot> snapshot;
    for (const auto& entry : CurrentSettingsBundleEntries()) {
        SettingsFileSnapshot s;
        s.name = entry.name;
        s.path = entry.path;
        std::string bytes;
        if (ReadBundleBytes(entry.path, &bytes)) s.bytes = std::move(bytes);
        snapshot.push_back(std::move(s));
    }
    return snapshot;
}

static std::filesystem::path CreateSettingsImportBackupDir(const std::vector<SettingsFileSnapshot>& snapshot,
                                                           std::wstring* outErr) {
    const std::filesystem::path root(g_workspaceRoot);
    if (root.empty()) return {};
    const std::filesystem::path escapeRoot = root / L"__resource__" / L"__escape__";
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
        const std::filesystem::path backupFile = backupDir / s.name;
        if (!atomic_write::AtomicWriteBytes(backupFile, s.bytes->data(), s.bytes->size(),
                                            backupDir, backupDir, &writeErr)) {
            if (outErr) *outErr = L"could not write settings import backup: " + writeErr;
            return {};
        }
    }
    std::wstring writeErr;
    const std::string manifestBytes = manifest.str();
    if (!atomic_write::AtomicWriteUtf8(backupDir / L"manifest.txt", manifestBytes, backupDir, backupDir, &writeErr)) {
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

bool ExportAllUserSettingsToFile(const std::filesystem::path& outputPath, std::wstring* outErr) {
    std::string bundle;
    if (!BuildCurrentSettingsBundle(&bundle, outErr)) return false;
    return WriteAtomicSettingsBytes(outputPath, bundle, outErr);
}

bool ImportAllUserSettingsFromFile(const std::filesystem::path& inputPath, std::wstring* outErr) {
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

    const auto snapshot = CaptureSettingsFileSnapshot();
    const std::filesystem::path backupDir = CreateSettingsImportBackupDir(snapshot, outErr);
    if (backupDir.empty()) return false;

    std::wstring applyErr;
    bool applied = false;
    try {
        int auxWrites = 0;
        for (const auto& entry : CurrentSettingsBundleEntries()) {
            const std::wstring name(entry.name);
            if (name == L"workspace.json") continue;
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

    g_config = LoadWorkspaceConfig(g_workspaceRoot);
    ApplyConfigToUI(nullptr);
    return true;
}

void ExportAllUserSettings(HWND hWnd) {
    if (g_workspaceRoot.empty()) {
        ShowSoftNotice(hWnd, IsEnglishUi() ? L"No workspace is open." : L"ワークスペースが開かれていません。",
                       SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path outputPath;
    if (!PickSettingsBundleSavePath(hWnd, &outputPath)) return;
    std::wstring err;
    if (!ExportAllUserSettingsToFile(outputPath, &err)) {
        std::wstring msg = IsEnglishUi()
            ? L"Could not export the user settings transfer file."
            : L"設定引き継ぎファイルを書き出せませんでした。";
        if (!err.empty()) msg += L"\n\n" + err;
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Warning);
        return;
    }
    ShowSoftNotice(hWnd,
                   (IsEnglishUi()
                        ? L"Exported a settings transfer file for version updates:\n"
                        : L"バージョン更新時に引き継げる設定ファイルを書き出しました:\n") +
                   outputPath.wstring());
}

void ImportAllUserSettings(HWND hWnd) {
    if (g_workspaceRoot.empty()) {
        ShowSoftNotice(hWnd, IsEnglishUi() ? L"No workspace is open." : L"ワークスペースが開かれていません。",
                       SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path inputPath;
    if (!PickSettingsBundleOpenPath(hWnd, &inputPath)) return;
    std::wstring err;
    if (!ImportAllUserSettingsFromFile(inputPath, &err)) {
        std::wstring msg = IsEnglishUi()
            ? L"Could not import the user settings transfer file. Existing settings were kept or restored from backup."
            : L"設定引き継ぎファイルを読み込めませんでした。既存設定は保持またはバックアップから復旧されています。";
        if (!err.empty()) msg += L"\n\n" + err;
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Warning);
        return;
    }
    ShowSoftNotice(hWnd,
                   IsEnglishUi()
                       ? L"Imported user settings from the transfer file."
                       : L"引き継ぎファイルからユーザー設定を読み込みました。");
}

void SaveAllManual(HWND hWnd) {
    try {
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
                       IsEnglishUi() ? L"No note is open." : L"ノートが開かれていません。",
                       SoftNoticeKind::Warning);
        return;
    }

    if (!file_output::SaveNoteFile(hWnd)) {
        RefreshMainMenuBar(hWnd);
        RefreshStatusDisplay(hWnd);
        return;
    }
    FinalizeManualSaveUi(hWnd, /*updateWindowTitleAfterSave=*/true);

    std::wstring msg = IsEnglishUi()
        ? L"Saved the current note directly to its original file."
        : L"現在のノートを原本ファイルへ直接保存しました。";
    if (g_annotsDirty || g_annotsNeedsIntegrate) {
        msg += IsEnglishUi()
            ? L"\nAnnotation staged diffs were not integrated."
            : L"\n注釈の未統合差分はそのままです。";
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
        const wchar_t* msg = IsEnglishUi()
            ? L"No workspace is open."
            : L"ワークスペースが開かれていません。";
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__resource__";
    std::filesystem::path backupRoot = resource / L"__escape__" / L"backup";

    std::wstring msg = IsEnglishUi()
        ? (std::wstring(
            L"Recovery / Backups\n\n"
            L"[Yes] Restore from backup\n"
            L"  - Select a .meta.txt under __resource__/__escape__/backup\n"
            L"  - Destination is taken from meta (dest=...)\n\n"
            L"[No] Integrate staged diffs now (stage -> original)\n"
            L"  - Integrates the currently adopted staged diffs\n"
            L"  - Same as Ctrl+S integrate\n\n"
            L"[Cancel] Do nothing"))
        : (std::wstring(
            L"復元/バックアップ\n\n"
            L"[はい] バックアップから復元する\n"
            L"  - __resource__/__escape__/backup の .meta.txt を選択\n"
            L"  - 復元先は meta の dest に従います\n\n"
            L"[いいえ] 未統合の差分をいま統合する（ステージ→原本）\n"
            L"  - 現在採用されている stage を原本へ反映します\n"
            L"  - Ctrl+S と同じ統合処理です\n\n"
            L"[キャンセル] 何もしない"));

    SilentDialogOptions dialog;
    dialog.title = ui.menuRecovery;
    dialog.message = msg;
    dialog.kind = SoftNoticeKind::Warning;
    dialog.buttons = SilentDialogButtons::YesNoCancel;
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
        const wchar_t* warn = IsEnglishUi()
            ? L"There are unintegrated staged changes.\n\nRestoring a backup may revert to an older version.\nContinue?"
            : L"未統合の差分があります。\n\nバックアップから復元すると、現在の編集内容より古い版へ戻る可能性があります。\n続行しますか？";
        SilentDialogOptions confirm;
        confirm.title = ui.menuRecovery;
        confirm.message = warn;
        confirm.kind = SoftNoticeKind::Warning;
        confirm.buttons = SilentDialogButtons::YesNo;
        confirm.defaultResult = SilentDialogResult::No;
        confirm.escapeResult = SilentDialogResult::No;
        if (ShowSilentDialog(hWnd, confirm) != SilentDialogResult::Yes) return;
    }

    auto pickedMeta = PickFileUnderLocked(hWnd, backupRoot,
        IsEnglishUi() ? L"Select backup .meta.txt" : L"バックアップ(meta.txt)を選択");
    if (!pickedMeta) return;

    std::filesystem::path restoredDest;
    if (!file_output::RestoreFromBackupMeta(hWnd, std::filesystem::path(*pickedMeta), &restoredDest)) {
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
    ShowSoftNotice(hWnd, IsEnglishUi() ? L"Restored." : L"復元しました。");
}

void ShowRestoreBackupListDialogAndExecute(HWND hWnd) {
    if (g_workspaceRoot.empty()) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi() ? L"No workspace is open." : L"ワークスペースが開かれていません。",
                       SoftNoticeKind::Warning);
        return;
    }
    
    if (g_noteDirty || g_annotsDirty || g_noteNeedsIntegrate || g_annotsNeedsIntegrate) {
        const wchar_t* warn = IsEnglishUi()
            ? L"There are unintegrated staged changes.\n\nRestoring a backup may revert to an older version.\nContinue?"
            : L"未統合の差分があります。\n\nバックアップから復元すると、現在の編集内容より古い版へ戻る可能性があります。\n続行しますか？";
        SilentDialogOptions confirm;
        confirm.title = IsEnglishUi() ? L"Restore Backup" : L"バックアップ復元";
        confirm.message = warn;
        confirm.kind = SoftNoticeKind::Warning;
        confirm.buttons = SilentDialogButtons::YesNo;
        confirm.defaultResult = SilentDialogResult::No;
        confirm.escapeResult = SilentDialogResult::No;
        if (ShowSilentDialog(hWnd, confirm) != SilentDialogResult::Yes) return;
    }

    std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__resource__";
    std::filesystem::path backupRoot = resource / L"__escape__" / L"backup";
    std::filesystem::path pickedMeta;
    
    if (PromptRestoreBackupList(hWnd, backupRoot, pickedMeta)) {
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
        ShowSoftNotice(hWnd, IsEnglishUi() ? L"Restored." : L"復元しました。");
    }
}

void ShowDeleteSavedBackupDialog(HWND hWnd) {
    const auto& ui = GetUiText();
    if (g_workspaceRoot.empty()) {
        const wchar_t* msg = IsEnglishUi()
            ? L"No workspace is open."
            : L"ワークスペースが開かれていません。";
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Warning);
        return;
    }

    std::filesystem::path backupRoot = std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__escape__" / L"backup";
    auto pickedMeta = PickFileUnderLocked(hWnd, backupRoot,
        IsEnglishUi() ? L"Select backup .meta.txt to delete" : L"削除するバックアップ(meta.txt)を選択");
    if (!pickedMeta) return;

    std::filesystem::path selected(*pickedMeta);
    const std::wstring fileName = selected.filename().wstring();
    if (fileName.size() < 9 || fileName.rfind(L".meta.txt") != fileName.size() - 9) {
        ShowMainMessageDialog(
            hWnd,
            ui.menuDeleteBackup,
            IsEnglishUi()
                ? L"Select a backup .meta.txt file."
                : L"バックアップの .meta.txt ファイルを選択してください。",
            SoftNoticeKind::Warning);
        return;
    }

    const wchar_t* confirm = IsEnglishUi()
        ? L"Delete the selected backup and its metadata?"
        : L"選択したバックアップ本体と metadata を削除しますか？";
    if (!ConfirmMainYesNo(hWnd, ui.menuDeleteBackup, confirm, SoftNoticeKind::Warning,
                          SilentDialogResult::No, SilentDialogResult::No)) {
        return;
    }

    std::wstring err;
    if (!file_output::DeleteBackupMeta(selected, &err)) {
        if (err.empty()) {
            err = IsEnglishUi()
                ? L"Failed to delete backup."
                : L"バックアップの削除に失敗しました。";
        }
        ShowMainMessageDialog(hWnd, ui.menuDeleteBackup, err, SoftNoticeKind::Warning);
        return;
    }

    ShowSoftNotice(hWnd,
                   IsEnglishUi() ? L"Deleted backup." : L"バックアップを削除しました。");
    RefreshMainMenuBar(hWnd);
}
