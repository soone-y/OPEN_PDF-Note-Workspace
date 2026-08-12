// file: core/constants.h
// アプリ全体で使われる定数群。app_core.h から分離。
// Win32 型 (UINT_PTR) に依存するため <windows.h> が必要。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>
#include <cstdint>

// ---------------------------------------------------------------------
// AllUniqueValues（static_assert ヘルパ）
// ---------------------------------------------------------------------
template <typename T, std::size_t N>
constexpr bool AllUniqueValues(const std::array<T, N>& values) {
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = i + 1; j < N; ++j) {
            if (values[i] == values[j]) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------
// 表示・スケール定数
// ---------------------------------------------------------------------
inline constexpr double kDpi = 96.0;
inline constexpr double kMinScale = 0.2;
inline constexpr double kMaxScale = 8.0;
inline constexpr double kMarkerAlphaThin = 0.25;
inline constexpr double kMarkerAlphaDefault =
    1.0 - (1.0 - kMarkerAlphaThin) * (1.0 - kMarkerAlphaThin);
inline constexpr double kMarkerAlphaDark =
    1.0 - (1.0 - kMarkerAlphaThin) * (1.0 - kMarkerAlphaThin) * (1.0 - kMarkerAlphaThin);
inline constexpr double kLineAlphaDefault = 1.0;

// ---------------------------------------------------------------------
// レイアウト定数
// ---------------------------------------------------------------------
inline constexpr double kTopSplitRatio   = 0.55;
inline constexpr double kTopLeftRatio    = 0.22;
inline constexpr double kTopRightRatio   = 0.22;
inline constexpr double kBottomLeftRatio = 0.22;
inline constexpr double kBottomRightRatio= 0.20;
inline constexpr double kPageGapPx       = 16.0;
inline constexpr int    kSplitGrip       = 6;
inline constexpr int    kMinPane         = 160;
inline constexpr int    kDefaultLeftPane = 280;
inline constexpr int    kDefaultRightPane= 240;
inline constexpr int    kDefaultWindowWidth = 1280;
inline constexpr int    kDefaultWindowHeight = 900;
inline constexpr int    kMinListHeight   = 60;

// ---------------------------------------------------------------------
// Win32 タイマーID レジストリ
// Win32 timer IDs are scoped per HWND. Keep IDs unique within the target WindowProc.
// PdfViewProc timers:
//   0x2001 kScrollTimerId
//   0x2002 kPdfVirtualRenderTimerId
//   0x2005 kZoomTimerId (defined in pdf_view/view_state.cppinc)
//   0x504  kAutoScrollTimerId
// MainWndProc timers:
//   0x502  kAutoSaveTimerId
//   0x505  kAutoIntegrateTimerId
//   0x506  kNoteStageSaveTimerId
//   0x507  kAutoSaveExecuteTimerId
//   0x508  kSetupJsonExistenceVerifyTimerId (defined in main/main_window_proc.cppinc)
//   0x509  kNoteOverlayRefreshTimerId
//   0x50A  kNoteFullReparseTimerId
// NoteEditProc timers:
//   0x50B  kNoteLinkRenderGraceTimerId
//   0x5E11 kExitRetryTimerId (defined in main.cpp)
//   0x6A11 ui::kSplitDragTimerId (defined in ui/splitter.h)
// ---------------------------------------------------------------------
inline constexpr UINT_PTR kScrollTimerId = 0x2001;
inline constexpr UINT_PTR kPdfVirtualRenderTimerId = 0x2002;
inline constexpr int kPdfBitmapBudgetMiBDefault = 128;
inline constexpr int kPdfBitmapBudgetMiBMin = 32;
inline constexpr int kPdfBitmapBudgetMiBMax = 1024;
inline constexpr UINT_PTR kAutoSaveTimerId = 0x502;
inline constexpr UINT_PTR kAutoIntegrateTimerId = 0x505;
inline constexpr UINT_PTR kAutoScrollTimerId = 0x504;
inline constexpr UINT_PTR kNoteStageSaveTimerId = 0x506;
inline constexpr UINT_PTR kAutoSaveExecuteTimerId = 0x507;
inline constexpr UINT_PTR kNoteOverlayRefreshTimerId = 0x509;
inline constexpr UINT_PTR kNoteFullReparseTimerId = 0x50A;
inline constexpr UINT_PTR kNoteLinkRenderGraceTimerId = 0x50B;

static_assert(AllUniqueValues(std::array<UINT_PTR, 3>{
    kScrollTimerId,
    kPdfVirtualRenderTimerId,
    kAutoScrollTimerId,
}));
static_assert(AllUniqueValues(std::array<UINT_PTR, 7>{
    kAutoSaveTimerId,
    kAutoIntegrateTimerId,
    kNoteStageSaveTimerId,
    kAutoSaveExecuteTimerId,
    kNoteOverlayRefreshTimerId,
    kNoteFullReparseTimerId,
    kNoteLinkRenderGraceTimerId,
}));

// ---------------------------------------------------------------------
// オートセーブ・自動統合モード定数
// ---------------------------------------------------------------------
// Background annotation stage-save modes for WorkspaceConfig::autoSaveSeconds.
inline constexpr int kAutoSaveModeOff = 0;
inline constexpr int kAutoSaveModeEveryStep = -1;      // legacy import only
inline constexpr int kAutoSaveModeStep20Or1Min = -2;   // legacy import only
inline constexpr int kDefaultAutoSaveSeconds = 5;
inline int NormalizeAutoStageSaveSeconds(int seconds) {
    if (seconds == kAutoSaveModeOff) return kAutoSaveModeOff;
    if (seconds <= 0) return kDefaultAutoSaveSeconds;
    return seconds;
}
// Auto integrate modes for WorkspaceConfig::autoIntegrateSeconds.
// Zero keeps staged diffs until an explicit save or exit flow. Positive values
// schedule an automatic integration after the selected idle period.
inline constexpr int kAutoIntegrateModeOffSwitchExit = 0;
inline constexpr int kAutoIntegrateModeCustom = -2;
inline constexpr int kAutoIntegrateCustomMinutesMin = 1;
inline constexpr int kAutoIntegrateCustomMinutesMax = 1440;
inline constexpr int kAutoIntegrateCustomMinutesDefault = 10;

// ---------------------------------------------------------------------
// ノートカスタムタグ定数
// ---------------------------------------------------------------------
inline constexpr int kNoteCustomTagPresetCount = 9; //設定できるcタグの数
inline constexpr int kNoteCustomTagStyleBold = 1 << 0;
inline constexpr int kNoteCustomTagStyleItalic = 1 << 1;
inline constexpr int kNoteCustomTagStyleUnderline = 1 << 2;
inline constexpr int kNoteCustomTagStyleStrike = 1 << 3;
inline constexpr int kNoteCustomTagStyleBackColor = 1 << 4;
inline constexpr int kNoteCustomTagStyleTextColor = 1 << 5;
inline constexpr int kNoteCustomTagStyleAllMask =
    kNoteCustomTagStyleBold |
    kNoteCustomTagStyleItalic |
    kNoteCustomTagStyleUnderline |
    kNoteCustomTagStyleStrike |
    kNoteCustomTagStyleBackColor |
    kNoteCustomTagStyleTextColor;

// ---------------------------------------------------------------------
// スクロール定数
// ---------------------------------------------------------------------
inline constexpr double kScrollDamp      = 0.90;
inline constexpr double kScrollStepPx    = 60.0;
inline constexpr double kScrollOvershootPx = 80.0;

// ---------------------------------------------------------------------
// ウィンドウクラス名
// ---------------------------------------------------------------------
inline constexpr wchar_t kMainClass[]   = L"PdfWorkspaceMainWnd";
inline constexpr wchar_t kPdfViewClass[] = L"PdfWorkspacePdfView";

// ---------------------------------------------------------------------
// ワークスペース既定ディレクトリ名
// ---------------------------------------------------------------------
inline constexpr wchar_t kDefaultClassesDir[] = L"classes";
inline constexpr wchar_t kDefaultCacheDir[]        = L"__resource__/__tmp__";

// ---------------------------------------------------------------------
// ツールパレット定数
// ---------------------------------------------------------------------
// (moved to command_ids.h)

// ---------------------------------------------------------------------
// WM_APP メッセージレジストリ
// WM_APP messages are process-local but still scoped by target WindowProc.
//
// MainWndProc:
//   WM_APP+1   kMsgInitLayout (defined in main/main_window_proc.cppinc)
//   WM_APP+2   kMsgNoteOverlayUpdate
//   WM_APP+3   kMsgReloadLectureList
//   WM_APP+11  kMsgReloadSessionList
//   WM_APP+7   kMsgEnterNoteNormalMode
//   WM_APP+8   kMsgNoteEnterInsertMode
//   WM_APP+9   kMsgLectureContentOpened
//   WM_APP+120 kMsgListMaybeScroll (defined in main.cpp)
//   WM_APP+200 kMsgStartupWatchdogAbort (defined in main.cpp)
//   WM_APP+201 kMsgSingleInstanceShutdownRequest (defined in main.cpp)
//   WM_APP+210 kMsgRunUiAutomation (defined in main/main_window_proc.cppinc)
//   WM_APP+211 kMsgOpenStartupDocument (defined in main.cpp)
//   WM_APP+212 kMsgBackgroundSaveComplete
//
// PdfViewProc:
//   WM_APP+4   kMsgTextBoxFontUpdate
//   WM_APP+5   kMsgApplyInitialPdfFitWidth
//   WM_APP+6   kMsgApplyInitialPdfFitHeight
//   WM_APP+10  kMsgPdfVirtualRenderComplete
//
// Local dialog/window procedures:
//   WM_APP+91..92   Settings palette ChooseColor hook
//   WM_APP+140      Unified settings dialog navigation
//   WM_APP+141..142 Shared PickColor hook in app_core.cpp
//   WM_APP+420      Search window result open
// ---------------------------------------------------------------------
inline constexpr UINT kMsgNoteOverlayUpdate  = WM_APP + 2;
inline constexpr UINT kMsgReloadLectureList = WM_APP + 3;
inline constexpr UINT kMsgTextBoxFontUpdate = WM_APP + 4;
inline constexpr UINT kMsgApplyInitialPdfFitWidth = WM_APP + 5;
inline constexpr UINT kMsgApplyInitialPdfFitHeight = WM_APP + 6;
inline constexpr UINT kMsgEnterNoteNormalMode = WM_APP + 7;
inline constexpr UINT kMsgNoteEnterInsertMode = WM_APP + 8;
// Current lecture was meaningfully "opened" (e.g. user opened a PDF/note under it).
// Used to update recent-sort ordering without re-sorting on mere selection moves.
inline constexpr UINT kMsgLectureContentOpened = WM_APP + 9;
inline constexpr UINT kMsgPdfVirtualRenderComplete = WM_APP + 10;
inline constexpr UINT kMsgReloadSessionList = WM_APP + 11;
inline constexpr UINT kMsgBackgroundSaveComplete = WM_APP + 212;
static_assert(AllUniqueValues(std::array<UINT, 10>{
    kMsgNoteOverlayUpdate,
    kMsgReloadLectureList,
    kMsgTextBoxFontUpdate,
    kMsgApplyInitialPdfFitWidth,
    kMsgApplyInitialPdfFitHeight,
    kMsgEnterNoteNormalMode,
    kMsgNoteEnterInsertMode,
    kMsgLectureContentOpened,
    kMsgPdfVirtualRenderComplete,
    kMsgReloadSessionList,
}));
inline constexpr UINT kNoteShortcutInputCodeClickRendered = 1;
inline constexpr UINT kNoteShortcutInputCodeClickRaw = 2;
