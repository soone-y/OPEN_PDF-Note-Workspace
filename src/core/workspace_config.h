// file: core/workspace_config.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <array>
#include "core/constants.h"

struct ShortcutItem {
    int id;
    std::wstring label;
    std::wstring snippet;
    COLORREF color = RGB(0,0,0); // 0 = unspecified
};

struct AppDebugLogConfig {
    bool previewTrace = false;
    bool switchTiming = false;
    bool crash = true;
    bool startupWatchdog = true;
    bool officeConversion = true;
};

struct WorkspaceConfig {
    std::wstring classesDir = kDefaultClassesDir;
    std::wstring cacheDir   = kDefaultCacheDir;
    bool showAnnots = true;
    std::wstring pdfFlowMode = L"v_ttb"; // "v_ttb" | "v_btu" | "h_ltr" | "h_rtl"
    int pdfBitmapBudgetMiB = kPdfBitmapBudgetMiBDefault;
    bool pdfSinglePageMode = false;
    bool mouseWheelInvertVertical = false;
    bool mouseWheelInvertHorizontal = false;
    bool touchpadInvertVertical = false;
    bool touchpadInvertHorizontal = false;
    int leftWidth  = kDefaultLeftPane;
    int rightWidth = kDefaultRightPane;
    int topHeight  = 520;
    int windowWidth = 0;
    int windowHeight = 0;
    int defaultWindowWidth = 0;
    int defaultWindowHeight = 0;
    int leftSplit1 = 0;
    int leftSplit2 = 0;
    int defaultLeftWidth = 0;
    int defaultRightWidth = 0;
    int defaultTopHeight = 0;
    int defaultLeftSplit1 = 0;
    int defaultLeftSplit2 = 0;
    bool leftPaneCollapsed = false;
    std::wstring language = L"ja"; // "ja" or "en"
    std::wstring bottomPanePin = L"note"; // "note"(extend) / "math"(preview)
    std::wstring bottomNoteMode = L"legacy"; // "legacy"(note preview) / "headings"
    std::wstring notePlacement = L"bottom"; // "bottom" | "top"; dual notes are not enabled yet
    std::wstring colorTone = L"default";
    // Theme variant applied on top of the selected tone theme:
    // - "pure": use theme colors as-is
    // - "guard": keep the palette while guarding text contrast
    // - "white": shift toward a white-based palette while keeping the accent
    // - "black": shift toward a dark palette while keeping the accent
    std::wstring toneVariant = L"pure"; // "pure" | "guard" | "emphasis" | "white" | "black"
    std::wstring quickAnnotPopupPlacement = L"auto"; // "auto" | "boundary" | "up" | "down"
    bool ownerDrawUi = false;
    bool useNativeFileDialogs = false;
    bool developerMode = false;
    bool studentMode = true;
    bool showMathList = true;
    std::wstring downKeyLastLineAction = L"lineend";
    std::wstring leftRightLineMoveAction = L"allow";
    bool autoPairBrackets = true;
    bool fullWidthParenCaretInside = false;
    bool fullWidthParenCancelNextLeft = false;
    double noteFontPt = 10.0;
    std::wstring noteFontName;
    double noteRenderFontPt = 10.0;
    std::wstring noteRenderFontName;
    std::wstring noteRenderJpFontName;
    std::wstring noteSystem = L"legacy"; // "legacy" only
    std::wstring lectureSortMode = L"recent"; // "recent" | "name" | "schedule"
    std::wstring sessionSortMode = L"numeric_asc"; // "numeric_asc" | "numeric_desc" | "name"
    std::wstring sessionNumberingMode = L"count"; // "count" | "max_number"
    std::wstring sessionAutoOpenMode = L"off"; // legacy setting, auto-open is disabled
    bool sessionAutoOpenPairLinked = false;
    bool noteRenderEnabled = true;
    bool noteRawOnly = false;
    bool noteRenderMath = false;
    bool noteWrapEnabled = true;
    bool noteVimModeEnabled = true;
    bool noteVimCaretLineRawTextVisible = false;
    bool noteVimClickEntersInsertMode = true;
    int noteMathMarginTopPercent = 75;
    // 0 = auto, 5..95 = allocate this percent of additional super/sub gap to superscripts.
    int noteMathSupSubGapSupPercent = 0;
    bool noteGridEnabled = false;
    int noteGridPitch = 24;
    std::wstring selectionStyle = L"windows"; // "windows" | "theme"
    int pointerOffsetX = 0; // PDF click correction in client pixels
    int pointerOffsetY = 0;
    COLORREF noteBgColor = RGB(255, 255, 255);
    COLORREF noteFgColor = RGB(0, 0, 0);
    // Note background source:
    // - "explicit": use noteBgColor
    // - "theme_current": follow current theme's noteBg
    // - "theme_named": use noteBg of noteBgThemeName (even if current theme differs)
    std::wstring noteBgSource = L"explicit";
    std::wstring noteBgThemeName;
    COLORREF noteShortcutBackColor = RGB(0xFF, 0xF7, 0xE8);
    COLORREF noteShortcutTextColor = RGB(0x00, 0xAA, 0x7B);
    std::wstring noteShortcutTextTagKey = L"char"; // "char" | "c"
    bool noteShortcutHeadingArrowInvert = false;
    std::wstring noteCustomTagKey = L"c"; // e.g. "c"
    bool noteCustomTagBold = false;
    bool noteCustomTagItalic = false;
    bool noteCustomTagUnderline = true;
    bool noteCustomTagStrike = false;
    bool noteCustomTagBackColor = false;
    bool noteCustomTagTextColor = false;
    std::array<int, kNoteCustomTagPresetCount> noteCustomTagPresetMasks{};
    int noteFontCustomization = 0; // 0: Off, 1: Simple, 2: Advanced
    int noteFontDigitTarget = 0;   // 0: Latin font, 1: Japanese font
    int autoSaveSeconds = kDefaultAutoSaveSeconds;
    // 0 = automatic integration off, -2 = custom minutes, >0 = explicit seconds.
    int autoIntegrateSeconds = kAutoIntegrateModeOffSwitchExit;
    int autoIntegrateCustomMinutes = kAutoIntegrateCustomMinutesDefault;
    std::wstring clroNamePattern;
    // tool options (persisted in workspace.json)
    std::wstring textFontName = L"Meiryo";
    double textFontPt = 14.0;
    bool textFontUseA4Scale = true;
    int textFontActiveSizeSlot = 0;
    double textFontPtSlotA = 14.0;
    double textFontPtSlotB = 20.0;
    bool textFontUseA4ScaleSlotA = true;
    bool textFontUseA4ScaleSlotB = true;
    bool textBoxReadableBackground = false;
    bool textBoxReadableBackgroundInverted = false;
    // Line-like tools (Line / Arrow / Wave)
    // Default behavior: share style across these tools.
    bool lineToolsShareStyle = true;
    double lineWidthPt = 2.5;
    double arrowWidthPt = 2.5;
    std::wstring arrowHead = L"single"; // "single" | "double"
    double waveWidthPt = 2.5;
    std::wstring lineDashStyle = L"solid"; // "solid" | "dash"
    double freehandWidthPt = 2.5;
    double markerFreeWidthPt = 8.0;
    double markerTextWidthPt = 4.0;
    bool markerTextUnderline = false;
    double eraserWidthPt = 4.0;
    double markerAlpha = kMarkerAlphaDefault;
    double lineAlpha = kLineAlphaDefault;
    double arrowAlpha = kLineAlphaDefault;
    double waveAlpha = kLineAlphaDefault;
    double freehandAlpha = 1.0;
    double shapeAlpha = 0.35;
    COLORREF textColor = RGB(255, 140, 0);
    COLORREF lineColor = RGB(255, 140, 0);
    COLORREF arrowColor = RGB(255, 140, 0);
    COLORREF waveColor = RGB(255, 140, 0);
    COLORREF freehandColor = RGB(255, 140, 0);
    COLORREF markerFreeColor = RGB(255, 140, 0);
    COLORREF markerTextColor = RGB(255, 140, 0);
    COLORREF paletteCustomColor = RGB(128, 128, 128);
    std::wstring magnifierShape = L"circle"; // "circle" | "square"
    COLORREF shapeColor = RGB(255, 140, 0);
    std::wstring shapeDetail = L"line";
    std::wstring shapeKind = L"rectangle";
    std::wstring shapeDrawMode = L"outline";
    // Stable detail keys selected most recently inside each multi-detail category.
    std::wstring annotLastMarkerDetail = L"marker_text";
    std::wstring annotLastPenDetail = L"freehand";
    // Structured Shape-family selection. annotLastShapeDetail remains a
    // compatibility projection for older workspaces.
    std::wstring annotLastShapePresentation = L"stroke";
    std::wstring annotLastShapeGeometry = L"line";
    std::wstring annotLastShapeDetail = L"line";
    std::wstring freehandCorrection = L"off";       // "off" | "smooth" | "hold" | "auto"
    std::wstring freehandCorrectionStyle = L"auto"; // "auto" | "pen" | "shape"
    std::wstring freehandCorrectionFill = L"off";   // "off" | "use_shape_setting" | "always_fill"
    int scheduleDayMask = 0x1F; // Mon-Fri
    int schedulePeriods = 6;
    std::vector<std::wstring> scheduleCells;
    std::vector<std::wstring> scheduleStartTimes;
    std::vector<ShortcutItem> shortcuts;
    int markFontPx = 0;     // 0 = follow UI font
    int headingFontPx = 0;  // 0 = follow UI font
    int markColor = -1;     // 0xRRGGBB or -1 default
    int headingColor = -1;  // 0xRRGGBB or -1 default
    bool headingBold = true;
    bool headingUnderline = true;
    bool headingLeftBar = true;

    // Export options
    // When true, try to export TextBox annotations as PDF text objects (selectable/searchable).
    // Default is true. If font embedding/rendering cannot be resolved, export falls back to images.
    bool exportStandardTextAnnots = true;

    // Debug log outputs under __resource__/__log__/*.log.
    // Changes are persisted immediately, but applied to logging on the next launch.
    AppDebugLogConfig debugLogs;
};
