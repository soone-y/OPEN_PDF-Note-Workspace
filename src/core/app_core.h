// file: core/app_core.h
#pragma once

#if !defined(MAYBE_UNUSED)
#  if defined(__has_cpp_attribute)
#    if __has_cpp_attribute(maybe_unused)
#      define MAYBE_UNUSED [[maybe_unused]]
#    endif
#  endif
#  if !defined(MAYBE_UNUSED)
#    if defined(__GNUC__) || defined(__clang__)
#      define MAYBE_UNUSED __attribute__((unused))
#    else
#      define MAYBE_UNUSED
#    endif
#  endif
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include "core/cache_dir_policy.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <optional>
#include <functional>
#include <cmath>
#include <limits>
#include <fstream>
#include <cwctype>
#include <wingdi.h>
#include <atomic>
#include <mutex>

#include "core/path_safety.h"
#include "core/ui_notify.h"
#include "core/app_log.h"
#include "core/constants.h"
#include "fpdfview.h"
#include "fpdf_text.h"
#include "clrop/types.h"

#ifndef PDF_NOTE_LITE_EDITION
#define PDF_NOTE_LITE_EDITION 0
#endif

#if PDF_NOTE_LITE_EDITION != 0 && PDF_NOTE_LITE_EDITION != 1
#error "PDF_NOTE_LITE_EDITION must be 0 (Full) or 1 (Lite)."
#endif

inline constexpr bool kIsLiteEdition = (PDF_NOTE_LITE_EDITION == 1);




// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Enums / structs
// ---------------------------------------------------------------------
#include "core/command_ids.h"


#include "core/annot_types.h"

#include "core/workspace_config.h"


#include "core/theme_types.h"

LONG WINAPI CrashLogFilter(EXCEPTION_POINTERS* ep);

enum class ToolMode {
    Select,
    Pan,
    Magnifier,
    MarkerText,
    MarkerTextUnderline,
    MarkerTextColor,
    MarkerFree,
    MarkerLine,
    MarkerArrow,
    MarkerWave,
    TextBox,
    Line,
    Arrow,
    Wave,
    Freehand,
    Shape,
    Eraser
};
inline constexpr int kToolModeCount = static_cast<int>(ToolMode::Eraser) + 1;

enum class AnnotToolFamily {
    Select,
    Pan,
    Magnifier,
    Text,
    Marker,
    Pen,
    Shape,
    Eraser
};
inline constexpr int kAnnotToolFamilyCount = static_cast<int>(AnnotToolFamily::Eraser) + 1;

enum class AnnotToolUiState { Enabled, Disabled, Hidden };
enum class AnnotToolShortcutTargetKind { Category, Detail };
enum class AnnotToolGeometry { None, Shape, Line, Wave, Arrow };
enum class AnnotToolPresentation { None, Stroke, Emphasis };
enum class ShapeDetail { Line, Arrow, Wave, Rectangle, Ellipse, Triangle, Diamond };
struct ShapeToolSelection {
    AnnotToolPresentation presentation = AnnotToolPresentation::Stroke;
    AnnotToolGeometry geometry = AnnotToolGeometry::Line;

    constexpr bool operator==(const ShapeToolSelection& other) const {
        return presentation == other.presentation && geometry == other.geometry;
    }
};
enum class AnnotToolOptionGroup {
    None,
    Magnifier,
    Text,
    MarkerText,
    MarkerFree,
    Line,
    Arrow,
    Wave,
    Pen,
    Shape,
};
struct AnnotToolDetailDescriptor {
    ToolMode mode = ToolMode::Select;
    AnnotToolFamily family = AnnotToolFamily::Select;
    const char* key = "select";
    AnnotToolOptionGroup optionGroup = AnnotToolOptionGroup::None;
    AnnotToolGeometry geometry = AnnotToolGeometry::None;
    AnnotToolPresentation presentation = AnnotToolPresentation::None;
};
struct AnnotToolShortcutBinding {
    std::wstring key;
    AnnotToolShortcutTargetKind targetKind = AnnotToolShortcutTargetKind::Detail;
    AnnotToolFamily family = AnnotToolFamily::Select;
    ToolMode mode = ToolMode::Select;
    UINT vk = 0;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};
const std::vector<AnnotToolFamily>& AnnotToolFamilyUiOrder();
const std::vector<AnnotToolDetailDescriptor>& AnnotToolDetailDescriptors();
const AnnotToolDetailDescriptor* FindAnnotToolDetailDescriptor(ToolMode mode);
AnnotToolGeometry AnnotToolGeometryForMode(ToolMode mode);
AnnotToolPresentation AnnotToolPresentationForMode(ToolMode mode, ShapeDrawMode shapeDrawMode);
ShapeToolSelection ShapeToolSelectionForMode(ToolMode mode, ShapeDrawMode shapeDrawMode);
std::optional<ToolMode> ToolModeForShapeToolSelection(ShapeToolSelection selection);
const char* ShapeToolPresentationKey(AnnotToolPresentation presentation);
bool ShapeToolPresentationFromKey(const std::string& key, AnnotToolPresentation& out);
const char* ShapeToolGeometryKey(AnnotToolGeometry geometry);
bool ShapeToolGeometryFromKey(const std::string& key, AnnotToolGeometry& out);
std::vector<AnnotToolGeometry> OrderedShapeToolGeometries();
const char* ShapeDetailKey(ShapeDetail detail);
bool ShapeDetailFromKey(const std::string& key, ShapeDetail& out);
std::vector<ShapeDetail> OrderedShapeDetails();
bool ShapeDetailIsLinear(ShapeDetail detail);
bool ShapeDetailIsClosed(ShapeDetail detail);
ToolMode ToolModeForShapeDetail(ShapeDetail detail);
ShapeKind ShapeKindForShapeDetail(ShapeDetail detail);
ShapeDetail ShapeDetailForLegacyState(ToolMode mode, ShapeKind kind);
void SyncLegacyShapeStateFromDetail();
bool AnnotToolUsesGeometry(ToolMode mode, AnnotToolGeometry geometry);
bool IsLineLikeAnnotToolMode(ToolMode mode);
bool IsArrowAnnotToolMode(ToolMode mode);
bool IsWaveAnnotToolMode(ToolMode mode);
bool IsEmphasisAnnotToolMode(ToolMode mode, ShapeDrawMode shapeDrawMode);
const char* AnnotToolFamilyKey(AnnotToolFamily family);
bool AnnotToolFamilyFromKey(const std::string& key, AnnotToolFamily& out);
const char* AnnotToolModeKey(ToolMode mode);
bool AnnotToolModeFromKey(const std::string& key, ToolMode& out);
bool ParseAnnotToolShortcutKeyForUser(const std::wstring& text, AnnotToolShortcutBinding& out);
bool IsFixedAnnotToolNavigationShortcut(const AnnotToolShortcutBinding& binding);
AnnotToolFamily AnnotToolFamilyForMode(ToolMode mode);
HWND AnnotToolFamilyButtonHwnd(AnnotToolFamily family);
int AnnotToolFamilyCommand(AnnotToolFamily family);
std::wstring AnnotToolFamilyLabel(AnnotToolFamily family);
AnnotToolUiState AnnotToolFamilyUiStateFor(AnnotToolFamily family);
const std::vector<ToolMode>& AnnotToolModeUiOrder();
AnnotToolUiState AnnotToolModeUiStateFor(ToolMode mode);
std::vector<AnnotToolUiState> GetAnnotToolModeUiStates();
void ApplyAnnotToolModeUiConfig(const std::vector<ToolMode>& order,
                                const std::vector<AnnotToolUiState>& states,
                                bool persistSetupJson);
void LoadAnnotToolUiConfigFromSetupJson(const std::string& setupJson);
bool PersistAnnotToolUiConfigToSetupJson();
HWND ToolModeButtonHwnd(ToolMode mode);
void RecordRecentMouseMove(HWND host, const POINT* clientPt);
bool HandleQuickAnnotRightButtonDown(HWND host, const POINT* clientPt, bool allowWithoutCtrl = false);
bool HandleQuickAnnotContextMenu(HWND host, const POINT* screenPt, bool allowWithoutCtrl);
inline bool ToolbarHasFontOptions(ToolMode mode) {
    return mode == ToolMode::TextBox;
}
inline bool ToolbarHasWidthOptions(ToolMode mode) {
    switch (mode) {
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerFree:
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::MarkerWave:
    case ToolMode::Line:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Freehand:
    case ToolMode::Eraser:
        return true;
    default:
        return false;
    }
}
inline bool ToolbarHasMarkerAlphaOptions(ToolMode mode) {
    switch (mode) {
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerFree:
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::MarkerWave:
    case ToolMode::Line:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Freehand:
    case ToolMode::Shape:
        return true;
    default:
        return false;
    }
}
inline int ToolAlphaOptionCountForMode(ToolMode mode) {
    if (!ToolbarHasMarkerAlphaOptions(mode)) return 0;
    return mode == ToolMode::Shape ? 4 : 3;
}
inline double ToolAlphaOptionValueForMode(ToolMode mode, int index) {
    if (index < 0) return 0.0;
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Line) ||
        AnnotToolUsesGeometry(mode, AnnotToolGeometry::Arrow) ||
        AnnotToolUsesGeometry(mode, AnnotToolGeometry::Wave) ||
        mode == ToolMode::Freehand) {
        constexpr double values[] = { 0.50, 0.75, 1.00 };
        return values[std::min(index, 2)];
    }
    if (mode == ToolMode::Shape) {
        constexpr double values[] = { 0.25, 0.50, 0.80, 1.00 };
        return values[std::min(index, 3)];
    }
    constexpr double values[] = { kMarkerAlphaThin, kMarkerAlphaDefault, kMarkerAlphaDark };
    return values[std::min(index, 2)];
}
inline bool ToolbarHasColorOptions(ToolMode mode) {
    switch (mode) {
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor:
    case ToolMode::MarkerFree:
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::MarkerWave:
    case ToolMode::TextBox:
    case ToolMode::Line:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Freehand:
    case ToolMode::Shape:
        return true;
    default:
        return false;
    }
}
inline bool ToolbarHasShapeOptions(ToolMode mode) {
    (void)mode;
    return false;
}
inline bool ToolbarHasShapeDrawModeOptions(ToolMode mode) {
    return mode == ToolMode::Shape;
}
inline bool ToolbarHasLineDashOptions(ToolMode mode) {
    return mode == ToolMode::MarkerLine ||
           mode == ToolMode::MarkerArrow ||
           mode == ToolMode::Line ||
           mode == ToolMode::Arrow ||
           mode == ToolMode::Wave;
}
inline bool ToolbarHasAnnotMethodOptions(ToolMode mode) {
    return AnnotToolFamilyForMode(mode) == AnnotToolFamily::Marker ||
           AnnotToolFamilyForMode(mode) == AnnotToolFamily::Pen ||
           AnnotToolFamilyForMode(mode) == AnnotToolFamily::Shape;
}
inline bool ToolbarHasMarkerTextStyleOptions(ToolMode mode) {
    (void)mode;
    return false;
}
inline bool ToolbarHasMagnifierShapeOptions(ToolMode mode) {
    return mode == ToolMode::Magnifier;
}
inline bool IsMarkerGroupMode(ToolMode mode) {
    switch (mode) {
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor:
    case ToolMode::MarkerFree:
        return true;
    default:
        return false;
    }
}
inline bool IsMarkerTextMode(ToolMode mode) {
    return mode == ToolMode::MarkerText ||
           mode == ToolMode::MarkerTextUnderline ||
           mode == ToolMode::MarkerTextColor;
}
inline bool MarkerTextModeUsesUnderline(ToolMode mode) {
    return mode == ToolMode::MarkerTextUnderline;
}
inline bool MarkerTextModeUsesTextColor(ToolMode mode) {
    return mode == ToolMode::MarkerTextColor;
}
inline bool MarkerTextModeStoresUnderlineOption(ToolMode mode) {
    return mode == ToolMode::MarkerText || mode == ToolMode::MarkerTextUnderline;
}
inline bool IsPenGroupMode(ToolMode mode) {
    switch (mode) {
    case ToolMode::Freehand:
        return true;
    default:
        return false;
    }
}
inline bool IsShapeGroupMode(ToolMode mode) {
    return AnnotToolGeometryForMode(mode) != AnnotToolGeometry::None;
}
enum class MagnifierShape { Circle, Square };
enum class BottomPanePin { Note, Math };
enum class BottomNoteMode { Legacy, Headings, Assist };
enum class NotePlacement { Bottom, Top };
enum class DownKeyLastLineAction { None, InsertNewline, MoveLineEnd };
enum class LeftRightLineMoveAction { Allow, Stay };

inline MagnifierShape ParseMagnifierShape(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"square" || t == L"rect" || t == L"rectangle") return MagnifierShape::Square;
    return MagnifierShape::Circle;
}

inline std::wstring MagnifierShapeToString(MagnifierShape s) {
    switch (s) {
    case MagnifierShape::Square: return L"square";
    case MagnifierShape::Circle:
    default:
        return L"circle";
    }
}

inline BottomPanePin ParseBottomPanePin(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"note") return BottomPanePin::Note;
    if (t == L"math") return BottomPanePin::Math;
    return BottomPanePin::Math;
}

inline std::wstring BottomPanePinToString(BottomPanePin p) {
    switch (p) {
    case BottomPanePin::Note: return L"note";
    case BottomPanePin::Math: return L"math";
    default: return L"math";
    }
}

inline BottomNoteMode ParseBottomNoteMode(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"headings" || t == L"heading") return BottomNoteMode::Headings;
    if (t == L"assist" || t == L"noteassist" || t == L"note-assist") return BottomNoteMode::Assist;
    return BottomNoteMode::Legacy;
}

inline std::wstring BottomNoteModeToString(BottomNoteMode m) {
    switch (m) {
    case BottomNoteMode::Headings: return L"headings";
    case BottomNoteMode::Assist: return L"assist";
    case BottomNoteMode::Legacy:
    default: return L"legacy";
    }
}

inline NotePlacement ParseNotePlacement(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"top" || t == L"above") return NotePlacement::Top;
    return NotePlacement::Bottom;
}

inline std::wstring NotePlacementToString(NotePlacement p) {
    switch (p) {
    case NotePlacement::Top: return L"top";
    case NotePlacement::Bottom:
    default: return L"bottom";
    }
}

inline DownKeyLastLineAction ParseDownKeyLastLineAction(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"none" || t == L"off" || t == L"disabled") return DownKeyLastLineAction::None;
    if (t == L"newline") return DownKeyLastLineAction::InsertNewline;
    if (t == L"lineend" || t == L"line_end") return DownKeyLastLineAction::MoveLineEnd;
    return DownKeyLastLineAction::MoveLineEnd;
}

inline std::wstring DownKeyLastLineActionToString(DownKeyLastLineAction a) {
    switch (a) {
    case DownKeyLastLineAction::InsertNewline:
        return L"newline";
    case DownKeyLastLineAction::MoveLineEnd:
        return L"lineend";
    case DownKeyLastLineAction::None:
    default:
        return L"none";
    }
}

inline LeftRightLineMoveAction ParseLeftRightLineMoveAction(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"stay" || t == L"clamp" || t == L"line") return LeftRightLineMoveAction::Stay;
    return LeftRightLineMoveAction::Allow;
}

inline std::wstring LeftRightLineMoveActionToString(LeftRightLineMoveAction a) {
    switch (a) {
    case LeftRightLineMoveAction::Stay:
        return L"stay";
    case LeftRightLineMoveAction::Allow:
    default:
        return L"allow";
    }
}



// ---------------------------------------------------------------------
// Globals (extern)
// ---------------------------------------------------------------------
extern HINSTANCE g_hInst;
extern HFONT g_hUIFont;
extern HFONT g_hNoteFont;
extern HFONT g_hNoteRenderFont;

extern HWND g_hLectureList;
extern HWND g_hSessionList;
extern HWND g_hPdfList;
extern HWND g_hNoteList;
extern HWND g_hMainWnd;
extern HWND g_hPdfView;
extern HWND g_hPdfToolbar;
extern HWND g_hBtnClearAnn;
extern HWND g_hBtnToggleAnn;
extern HWND g_hBtnNewLecture;
extern HWND g_hBtnNewSession;
extern HWND g_hBtnNewNote;
extern HWND g_hBtnToggleLeftPane;
extern HWND g_hBtnModeSelect;
extern HWND g_hBtnModePan;
extern HWND g_hBtnModeMagnifier;
extern HWND g_hBtnModeMarker;
extern HWND g_hBtnModeMarkerFree;
extern HWND g_hBtnModeMarkerLine;
extern HWND g_hBtnModeMarkerArrow;
extern HWND g_hBtnModeMarkerWave;
extern HWND g_hBtnModeText;
extern HWND g_hBtnModeLine;
extern HWND g_hBtnModeArrow;
extern HWND g_hBtnModeWave;
extern HWND g_hBtnModeFreehand;
extern HWND g_hBtnModeShape;
extern HWND g_hBtnModeEraser;
extern HWND g_hComboFont;
extern HWND g_hComboFontSize;
extern HWND g_hComboFontSizeAlt;
extern HWND g_hRadioFontSizeSlotA;
extern HWND g_hRadioFontSizeSlotB;
extern HWND g_hChkTextReadableBackground;
extern HWND g_hRadioTextReadableBackgroundNormal;
extern HWND g_hRadioTextReadableBackgroundInverted;
extern HWND g_hComboWidth;
extern HWND g_hComboMarkerAlpha;
extern HWND g_hComboAnnotMethod;
extern HWND g_hComboFreehandCorrection;
extern HWND g_hComboMarkerTextStyle;
extern HWND g_hComboLineDashStyle;
extern HWND g_hComboShapeKind;
extern HWND g_hComboShapeGeometry;
extern HWND g_hComboShapeDrawMode;
extern HWND g_hComboMagnifierShape;
extern HWND g_hComboStrokeWidth; // legacy
extern HWND g_hAnnotShow;
extern HWND g_hAnnotSettings;
extern HWND g_hAnnotClear;
extern HWND g_hAnnotList;
extern HWND g_hAnnotSummary;
extern HWND g_hNoteEdit;
extern HWND g_hNoteRender;
extern HWND g_hBottomNote;
extern HWND g_hBottomMath;
extern HWND g_hBottomRight; // legacy alias
extern HMENU g_hMainMenu;
extern HMENU g_hBottomPaneMenu;
extern HMENU g_hBottomPaneMenuSettings;
extern HMENU g_hScrollDirectionMenu;
extern HACCEL g_hAccel;
extern std::vector<HWND> g_colorButtons;
extern HWND g_hBtnPaletteCustom;
extern HWND g_hChkShortcutHeading1;
extern HWND g_hChkShortcutHeading2;
extern HWND g_hShortcutHeadingLevelLabel;
extern HWND g_hBtnShortcutHeadingLevelUp;
extern HWND g_hChkShortcutBack;
extern HWND g_hChkShortcutChar;
extern HWND g_hChkShortcutBold;
extern HWND g_hChkShortcutItalic;
extern HWND g_hChkShortcutStrike;
extern HWND g_hChkShortcutUnderline;
extern HWND g_hChkShortcutLinkDecor;
extern HWND g_hBtnShortcutBackPreview;
extern HWND g_hBtnShortcutCharPreview;
extern HWND g_hBtnShortcutBackPalette;
extern HWND g_hBtnShortcutCharPalette;
extern HWND g_hShortcutIndentLabel;
extern HWND g_hShortcutIndentEdit;
extern HWND g_hShortcutMarginLabel;
extern HWND g_hShortcutMarginEdit;
extern HWND g_hShortcutFontSizeLabel;
extern HWND g_hShortcutFontSizeEdit;
extern HWND g_hShortcutTagEdit;
extern HWND g_hBtnShortcutInput;
extern HWND g_hBtnShortcutPdfLink;
extern HWND g_hBtnNoteAssistBullet;
extern HWND g_hBtnNoteAssistQuote;
extern HWND g_hBtnNoteAssistPageRef;
extern int g_noteShortcutHeadingLevel;
extern COLORREF g_noteShortcutBackColor;
extern COLORREF g_noteShortcutTextColor;
extern double g_markerAlpha;
extern double g_lineAlpha;
extern double g_arrowAlpha;
extern double g_waveAlpha;
extern double g_freehandAlpha;
extern double g_shapeAlpha;

// Keyboard "normal mode" for note input (Esc to exit, 'i' to re-enter note typing).
extern bool g_noteNormalMode;
extern DWORD g_noteNormalCaret;
extern bool g_noteVimModeEnabled;
extern bool g_noteVimCaretLineRawTextVisible;
extern bool g_noteVimClickEntersInsertMode;

extern PdfViewState g_pdf;
extern std::recursive_mutex g_pdfiumMutex;
extern std::wstring g_workspaceRoot;
extern std::wstring g_currentNotePath;
extern std::wstring g_currentSessionPath;
extern std::wstring g_currentLecturePath;
extern bool g_noteDirty;
extern bool g_noteNeedsIntegrate;
extern bool g_annotsDirty;
extern bool g_annotsNeedsIntegrate;
extern bool g_annotsRequireStrongValidation;
extern bool g_annotsLoadedPdfIdValid;
extern clrop::PdfId g_annotsLoadedPdfId;
extern std::vector<MathEntry> g_mathEntries;
extern std::vector<HighlightRange> g_highlightRanges;
extern COLORREF g_highlightMarkColor;
extern COLORREF g_highlightHeadingColor;
extern int g_markFontPx;
extern int g_headingFontPx;

void SetHighlightColors(COLORREF mark, COLORREF heading);

// bottom-right note assist
extern std::wstring g_previewNote;
extern bool g_pdfPreviewEnabled;
extern bool g_pdfPreviewActive;
extern std::wstring g_pdfPreviewOriginalPdfPath;
extern std::wstring g_pdfPreviewTempPath;
extern std::vector<Annotation> g_pdfPreviewOriginalAnnotations;
extern HANDLE g_pdfPreviewSourceFileHandle;
extern FPDF_DOCUMENT g_pdfPreviewSourceDoc;
extern BottomPanePin g_bottomPanePin;
extern BottomNoteMode g_bottomNoteMode;
extern NotePlacement g_notePlacement;
extern std::vector<std::unique_ptr<NodeData>> g_nodeStore;
extern std::vector<ShortcutItem> g_shortcuts;
extern std::vector<Annotation> g_annots;
extern LinkPending g_linkPending;
extern std::vector<std::wstring> g_lectures;
extern std::vector<SessionEntry> g_sessions;
extern std::vector<FileEntry> g_pdfFiles;
extern std::vector<FileEntry> g_noteFiles;
extern WorkspaceConfig g_config;

// layout / state
extern int g_leftWidth;
extern int g_rightWidth;
extern int g_topHeight;
extern int g_leftSplit1;
extern int g_leftSplit2;
extern bool g_leftPaneCollapsed;
extern bool g_draggingLeft;
extern bool g_draggingRight;
extern bool g_draggingHoriz;
extern bool g_draggingLeftTop;
extern bool g_draggingLeftMid;
extern POINT g_dragStart;
extern int g_leftStart;
extern int g_rightStart;
extern int g_topStart;
extern int g_leftSplitStart1;
extern int g_leftSplitStart2;
extern double g_scrollVelocity;
extern bool g_showAnnots;
extern bool g_readableTextOverlay;
extern bool g_showMathList;
extern ToolMode g_toolMode;
extern MagnifierShape g_magnifierShape;
extern ShapeKind g_shapeKind;
extern ShapeDrawMode g_shapeDrawMode;
extern ShapeDetail g_shapeDetail;
extern ShapeToolSelection g_shapeToolSelection;
extern ToolMode g_markerGroupMode;
extern ToolMode g_penGroupMode;
extern ToolMode g_shapeGroupMode;
extern std::vector<COLORREF> g_palette;
extern COLORREF g_paletteCustomColor;
extern COLORREF g_activeColor;
extern WNDPROC g_oldNoteProc;
extern std::wstring g_textFontName;
extern double g_textFontPt;
extern bool g_textFontUseA4Scale;
extern int g_textFontActiveSizeSlot;
extern double g_textFontPtSlotA;
extern double g_textFontPtSlotB;
extern bool g_textFontUseA4ScaleSlotA;
extern bool g_textFontUseA4ScaleSlotB;
extern bool g_textBoxReadableBackground;
extern bool g_textBoxReadableBackgroundInverted;

struct SavedToolbarState {
    bool valid = false;
    ToolMode toolMode = ToolMode::Select;
    std::wstring textFontName;
    double textFontPt = 0.0;
    COLORREF textColor = 0;
    bool readableBackground = false;
    bool readableBackgroundInverted = false;
};
extern SavedToolbarState g_preEditToolbarState;
extern bool g_lineToolsShareStyle;
extern double g_lineWidthPt;
extern double g_arrowWidthPt;
extern ArrowHead g_arrowHead;
extern double g_waveWidthPt;
extern double g_markerFreeWidthPt;
extern double g_markerTextWidthPt;
extern bool g_markerTextUnderline;
extern std::wstring g_lineDashStyle;
extern double g_freehandWidthPt;
extern double g_eraserWidthPt;
extern std::wstring g_noteFontName;
extern double g_noteFontPt;
extern std::wstring g_noteRenderFontName;
extern std::wstring g_noteRenderJpFontName;
extern double g_noteRenderFontPt;
extern NoteSystem g_noteSystem;
extern bool g_noteRenderEnabled;
extern bool g_noteRawOnly;
extern bool g_noteRenderMath;
extern bool g_noteWrapEnabled;
extern bool g_noteGridEnabled;
extern int g_noteGridPitch;
extern COLORREF g_noteBgColor;
extern COLORREF g_noteFgColor;
extern COLORREF g_textColor;
extern COLORREF g_lineColor;
extern COLORREF g_arrowColor;
extern COLORREF g_waveColor;
extern COLORREF g_freehandColor;
extern COLORREF g_markerFreeColor;
extern COLORREF g_markerTextColor;
extern COLORREF g_shapeColor;
extern ThemeColors g_theme;
extern std::wstring g_themeName;
extern std::vector<ThemeColors> g_themeCatalog;
extern HBRUSH g_hThemeWindowBrush;
extern HBRUSH g_hThemePanelBrush;
extern HBRUSH g_hThemeNoteBrush;
extern HBRUSH g_hThemeToolbarBrush;
extern HBRUSH g_hThemeMenuBrush;

// ---------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------
std::string  WideToUTF8(const std::wstring& w);
std::wstring UTF8ToWide(const std::string& s);
std::wstring TrimWhitespace(const std::wstring& s);
bool IsEnglishUi();
void UpdateLinkModeButtonState(bool active);
inline bool IsDefaultCacheDir(const std::wstring& cacheDir) {
    return cache_dir_policy::IsManagedCacheDir(cacheDir);
}

void SetUIFont(HWND hWnd);
HFONT CreateUIFont();
HFONT CreateUIFontVariant(double pt);
HFONT CreateFontFromFaceName(const std::wstring& faceName, double pt);
std::wstring GetDefaultFontFaceName();
bool IsJapaneseRenderCharForNoteFont(wchar_t ch);
std::wstring ResolveNoteRenderBaseFace();
std::wstring ResolveNoteRenderJpFace();
void SetPaletteCustomColor(COLORREF color);
bool PickColorDialog(HWND owner, COLORREF initial, COLORREF* outColor);
void SyncUserPaletteToRuntime();
void LoadUserPaletteColorsForSettings(COLORREF* custom, size_t count);
void SaveUserPaletteColorsForSettings(const COLORREF* custom, size_t count);
bool ApplyActivePaletteColorStep(HWND hwnd, int direction, bool focusPdfView);
const std::vector<AnnotToolShortcutBinding>& AnnotToolShortcutBindings();
bool ResolveAnnotToolShortcut(const MSG& msg, AnnotToolShortcutBinding* outBinding);
bool ReplaceAnnotToolShortcutBindings(const std::vector<AnnotToolShortcutBinding>& bindings);
void SyncUserToolShortcutsToRuntime();
bool LoadThemeConfig(const std::wstring& root);
bool PersistThemeSelection(const std::wstring& name);
bool ApplyThemeByName(const std::wstring& name, HWND owner, bool persist);
bool ApplyThemeByNameForSettingsBatch(const std::wstring& name, HWND owner, bool persist);
void ApplyThemeToUI(HWND owner);
bool DrawThemeButton(const DRAWITEMSTRUCT* dis);
LRESULT ThemeCtlColorPanel(HWND ctl, HDC hdc);
void ApplyThemeToDialog(HWND hWnd);
void EnsureOwnerDrawUi(HWND owner);
// Theme helpers (safe, local-only)
bool CreateThemeFromCurrent(HWND owner, const std::wstring& displayName, std::wstring* outThemeId, std::wstring* err);

COLORREF ToolColorForMode(ToolMode mode);
void StoreToolColorForMode(ToolMode mode, COLORREF color);
double ToolWidthPtForMode(ToolMode mode);
void StoreToolWidthPtForMode(ToolMode mode, double pt);
double ToolAlphaForMode(ToolMode mode);
void StoreToolAlphaForMode(ToolMode mode, double alpha);

void UpdateWindowTitle(HWND hWnd);
void RefreshStatusDisplay(HWND hWnd);
void UpdateToolbarUI(HWND hWnd);
void PumpSaveScrollMessages(HWND owner);
void SetAppLogWorkspaceRoot(const std::wstring& root);
void ApplyRuntimeLogConfigFromWorkspace(const std::wstring& root, const WorkspaceConfig& cfg);
bool IsAppLogEnabled(AppLogKind kind);
std::filesystem::path AppLogPath(AppLogKind kind);
void AppendAppLogLine(AppLogKind kind, const std::wstring& line);
void AppendAppLogLineUtf8(AppLogKind kind, const std::string& line);
void AppendCrashLogLine(const char* area, const char* detail = nullptr);
void AppendCrashLogWide(const std::wstring& title, const std::wstring& detail = L"");
void RefreshMainMenuBar(HWND hWnd);
void RefreshMainWindowUiState(HWND hWnd);
void RequestAnnotPanelRevealLatest();
void RefreshAnnotPanel();
bool SyncTextBoxToolFontFromAnnotationIndex(int index);
const std::wstring& CurrentLogicalPdfPath();
void UpdateSessionLastOpenTargetsAfterOpen();
FPDF_DOCUMENT CurrentLogicalPdfDocument();
const std::vector<Annotation>* CurrentLogicalPdfAnnotations();
bool IsPdfPreviewReadOnlyActive();
void EndIntegratedPdfPreviewMode(HWND owner, bool restoreOriginalPdf);

// 数値ソート系（テンプレートなのでヘッダに実装）
int KanjiDigit(wchar_t ch);
int FullwidthDigit(wchar_t ch);
int RomanDigit(wchar_t ch);
std::optional<int> ParseRomanNumber(const std::wstring& s);
std::optional<int> ParseKanjiNumber(const std::wstring& s);
std::optional<int> ExtractNumericKey(const std::wstring& name);

template <typename Vec, typename Getter>
void SortByNumericThenName(Vec& vec, Getter getter) {
    std::sort(vec.begin(), vec.end(), [&](const auto& a, const auto& b) {
        auto na = ExtractNumericKey(getter(a));
        auto nb = ExtractNumericKey(getter(b));
        if (na && nb && na.value() != nb.value()) return na.value() < nb.value();
        if (na && !nb) return true;
        if (!na && nb) return false;
        return getter(a) < getter(b);
    });
}

void SetListWide(HWND hList);
bool IsPdfFile(const std::filesystem::path& p);
bool IsImageFile(const std::filesystem::path& p);
bool IsPdfOrImageFile(const std::filesystem::path& p);
bool IsNoteFile(const std::filesystem::path& p);
bool OpenSearchResultFileInCurrentFileList(HWND owner, const std::wstring& path);
bool LaunchReadOnlyViewerForPdfAt(HWND owner,
                                  const std::wstring& pdfPath,
                                  int pageIndex,
                                  double yPt,
                                  bool hasY);
void SyncLeftPaneSelection();
void RefreshLeftPaneOpenState();
void BeginDeferredLeftPaneSelection();
void FlushDeferredLeftPaneSelection();
void EndDeferredLeftPaneSelection();

// workspace helpers
WorkspaceConfig DefaultWorkspaceConfig();
WorkspaceConfig LoadWorkspaceConfig(const std::wstring& root);
std::optional<WorkspaceConfig> LoadWorkspaceConfigFromFile(const std::filesystem::path& path, std::wstring* outErr = nullptr);
void SaveWorkspaceConfig(const std::wstring& root, const WorkspaceConfig& cfg);
bool SaveWorkspaceConfigToFile(const std::filesystem::path& path, const WorkspaceConfig& cfg);
void PersistConfig();
void SyncToolbarFontSizeCombo();
std::filesystem::path WorkspaceClassesPath(const std::wstring& root, const WorkspaceConfig& cfg);
std::filesystem::path WorkspaceCachePath(const std::wstring& root, const WorkspaceConfig& cfg);
const UiText& GetUiText();
std::wstring BuildAboutDialogText();
std::optional<std::wstring> LoadSetupWorkspaceRoot();
bool ConsumePendingStartupNotice(std::wstring* outText, SoftNoticeKind* outKind);
// If a setup JSON file was created during startup, returns its path once (then clears).
std::optional<std::wstring> ConsumePendingSetupJsonExistenceCheckPath();
// Best-effort existence/readability check for the setup JSON file (e.g. antivirus deletion).
bool VerifySetupJsonStillExistsReadable(const std::filesystem::path& setupPath, std::wstring* outErr = nullptr);
void UpdateSetupJsonWorkspaceRoot(const std::wstring& newRoot);
std::vector<std::wstring> LoadSetupTempExternalLectureDirs();
bool PersistSetupTempExternalLectureDirs(const std::vector<std::wstring>& dirs);
void CheckAndPromptClassdirMismatch(HWND hWnd, const std::wstring& workspaceRoot);

// ---------------------------------------------------------------------
// Save operation tracking (for safe shutdown)
// ---------------------------------------------------------------------
void EnterSaveOperation();
void LeaveSaveOperation();
bool IsSaveOperationInProgress();
void NotifyEditRevisionChanged();
uint64_t CurrentEditRevision();
bool TryBeginSaveTransaction(uint64_t* outSnapshotRevision);
void RequestQueuedSaveTransaction();
bool ShouldRunQueuedSaveTransaction(uint64_t snapshotRevision);
void EndSaveTransaction();
bool IsSaveTransactionRunning();

struct SaveOperationGuard {
    SaveOperationGuard() { EnterSaveOperation(); }
    ~SaveOperationGuard() { LeaveSaveOperation(); }
    SaveOperationGuard(const SaveOperationGuard&) = delete;
    SaveOperationGuard& operator=(const SaveOperationGuard&) = delete;
};
void InstallGlobalBeepFilter();
