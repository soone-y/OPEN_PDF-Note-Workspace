// file: ui/layout.cpp
#include "ui/layout.h"
#include "ui/toolbar_scroll.h"

#include "core/app_core.h"
#include "core/preview_trace.h"
#include "bridge/view_bridge.h"

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace ui {

namespace {

constexpr int kCollapsedLeftPaneWidth = 32;
// Shared by toolbar height reservation and actual annotation-list placement.
const int panelPad = 8;
const int panelRowH = 24;
const int panelGap = 6;
const int panelSummaryH = 20;
const int panelListMinH = 48;

static bool RectEquals(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

static bool RectIsValid(const RECT& rc) {
    return rc.right > rc.left && rc.bottom > rc.top;
}

static void UnionRectInPlace(bool* hasRect, RECT* accum, const RECT& rc) {
    if (!hasRect || !accum) return;
    if (!RectIsValid(rc)) return;
    if (!*hasRect) {
        *accum = rc;
        *hasRect = true;
        return;
    }
    accum->left = std::min(accum->left, rc.left);
    accum->top = std::min(accum->top, rc.top);
    accum->right = std::max(accum->right, rc.right);
    accum->bottom = std::max(accum->bottom, rc.bottom);
}

static RECT GetWindowRectInParentCoords(HWND hwnd) {
    RECT rc{};
    if (!hwnd || !GetWindowRect(hwnd, &rc)) return rc;
    if (HWND parent = GetParent(hwnd)) {
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rc), 2);
    }
    return rc;
}

static bool IsAnnotPanelControl(HWND hwnd) {
    return hwnd == g_hAnnotSettings ||
           hwnd == g_hAnnotClear ||
           hwnd == g_hAnnotList ||
           hwnd == g_hAnnotSummary;
}

static bool IsShortcutControl(HWND hwnd) {
    return hwnd == g_hChkShortcutHeading1 ||
           hwnd == g_hChkShortcutHeading2 ||
           hwnd == g_hShortcutHeadingLevelLabel ||
           hwnd == g_hBtnShortcutHeadingLevelUp ||
           hwnd == g_hChkShortcutBack ||
           hwnd == g_hChkShortcutChar ||
           hwnd == g_hChkShortcutBold ||
           hwnd == g_hChkShortcutItalic ||
           hwnd == g_hChkShortcutStrike ||
           hwnd == g_hChkShortcutUnderline ||
           hwnd == g_hChkShortcutLinkDecor ||
           hwnd == g_hBtnShortcutBackPreview ||
           hwnd == g_hBtnShortcutCharPreview ||
           hwnd == g_hBtnShortcutBackPalette ||
           hwnd == g_hBtnShortcutCharPalette ||
           hwnd == g_hShortcutIndentLabel ||
           hwnd == g_hShortcutIndentEdit ||
           hwnd == g_hShortcutMarginLabel ||
           hwnd == g_hShortcutMarginEdit ||
           hwnd == g_hShortcutFontSizeLabel ||
           hwnd == g_hShortcutFontSizeEdit ||
           hwnd == g_hShortcutTagEdit ||
           hwnd == g_hBtnShortcutInput ||
           hwnd == g_hBtnNoteWritingGuide ||
           hwnd == g_hBtnShortcutPdfLink ||
           hwnd == g_hBtnNoteAssistBullet ||
           hwnd == g_hBtnNoteAssistQuote ||
           hwnd == g_hBtnNoteAssistPageRef;
}

static void MarkChanged(LayoutApplyResult& result, HWND hwnd) {
    if (hwnd == g_hPdfView) result.pdfViewBoundsChanged = true;
    if (hwnd == g_hPdfToolbar || GetParent(hwnd) == g_hPdfToolbar) {
        result.pdfToolbarBoundsChanged = true;
    }
    if (hwnd == g_hNoteEdit) result.noteEditorBoundsChanged = true;
    if (hwnd == g_hNoteRender) result.noteRenderBoundsChanged = true;
    if (hwnd == g_hBottomNote || hwnd == g_hBottomMath) {
        result.bottomPaneBoundsChanged = true;
    }
    if (IsAnnotPanelControl(hwnd)) result.annotPanelBoundsChanged = true;
    if (IsShortcutControl(hwnd)) result.shortcutPanelBoundsChanged = true;
}

static void UpdateLayoutDependentViews(const LayoutApplyResult& result, LayoutPass pass) {
    if (result.bottomPaneBoundsChanged && pass == LayoutPass::Commit) {
        RefreshCurrentNoteBottomPane();
    }
}

static void RedrawLayoutRegion(HWND hWnd, const RECT* dirtyRect) {
    if (!hWnd) return;
    // Layout applies positions with SWP_NOREDRAW. Repainting the root and all
    // intersecting children in one pass makes the post-layout image
    // authoritative, including list blank areas and WS_EX_CLIENTEDGE frames.
    RedrawWindow(hWnd, dirtyRect, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                 RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void RedrawChangedLayout(HWND hWnd, const LayoutApplyResult& result,
                                const std::vector<HWND>& changedControls) {
    // Keep existing child pixels until their new contents are painted. Broad
    // child-tree erasure makes text disappear between erase and child paint.
    RedrawWindow(hWnd, result.hasRootDirtyRect ? &result.rootDirtyRect : nullptr, nullptr,
                 RDW_INVALIDATE | RDW_NOCHILDREN | RDW_UPDATENOW);
    const bool redrawToolbar = result.pdfToolbarBoundsChanged && g_hPdfToolbar;
    if (redrawToolbar) {
        // Moving children with SWP_NOREDRAW leaves their old pixels in the
        // host. Paint the host's vacated areas and all children as one tree;
        // repainting only the moved controls produces overlapping old text.
        RedrawWindow(g_hPdfToolbar, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    for (const HWND control : changedControls) {
        if (redrawToolbar && (control == g_hPdfToolbar || GetParent(control) == g_hPdfToolbar)) continue;
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

} // namespace

int SplitBandThickness() {
    // Keep the same visual thickness as the old overlay splitter (kSplitGrip*2).
    return kSplitGrip * 2;
}

void RedrawLayoutTree(HWND hWnd) {
    RedrawLayoutRegion(hWnd, nullptr);
}

LayoutState GetLayoutState() {
    LayoutState s{};
    s.leftWidth = g_leftWidth;
    s.rightWidth = g_rightWidth;
    s.topHeight = g_topHeight;
    s.leftSplit1 = g_leftSplit1;
    s.leftSplit2 = g_leftSplit2;
    return s;
}

void SetLayoutState(const LayoutState& s) {
    g_leftWidth = s.leftWidth;
    g_rightWidth = s.rightWidth;
    g_topHeight = s.topHeight;
    g_leftSplit1 = s.leftSplit1;
    g_leftSplit2 = s.leftSplit2;
}

static int ClampInt(int v, int lo, int hi) {
    if (hi < lo) hi = lo;
    return std::clamp(v, lo, hi);
}

template <typename PlaceFn>
static int LayoutAnnotToolbar(int baseX, int baseY, int areaW, PlaceFn place) {
    const int padding = 6;
    const int rowGap = 4;
    const int sectionGap = 8;
    const int btnH = 26;
    const int comboH = 24;
    const int colorSize = 20;
    const int gap = 6;
    // Keep the tool families in two columns in a narrow right pane. This leaves
    // vertical room for the annotation list instead of silently collapsing it.
    const int minColW = 80;

    int x = baseX + padding;
    int y = baseY + padding;
    int w = std::max(0, areaW - padding * 2);
    int maxY = y;

    auto placeCtl = [&](HWND h, int px, int py, int pw, int ph) {
        if (!h) return;
        place(h, px, py, pw, ph);
        maxY = std::max(maxY, py + ph);
    };
    auto placeFull = [&](HWND h, int hgt) {
        if (!h) return;
        placeCtl(h, x, y, w, hgt);
        y += hgt + rowGap;
    };

    std::vector<HWND> tools;
    tools.reserve(kAnnotToolFamilyCount);
    for (AnnotToolFamily family : AnnotToolFamilyUiOrder()) {
        bool allHidden = true;
        for (ToolMode mode : AnnotToolModeUiOrder()) {
            if (AnnotToolFamilyForMode(mode) != family) continue;
            if (AnnotToolModeUiStateFor(mode) != AnnotToolUiState::Hidden) {
                allHidden = false;
                break;
            }
        }
        if (allHidden) continue;
        HWND h = AnnotToolFamilyButtonHwnd(family);
        if (h && std::find(tools.begin(), tools.end(), h) == tools.end()) tools.push_back(h);
    }
    int toolCount = static_cast<int>(tools.size());
    if (w > 0 && toolCount > 0) {
        int cols = (w >= minColW * 2 + gap) ? 2 : 1;
        int colW = (cols == 2) ? (w - gap) / 2 : w;
        int rows = (toolCount + cols - 1) / cols;
        for (int i = 0; i < toolCount; ++i) {
            int row = i / cols;
            int col = i % cols;
            int px = x + col * (colW + gap);
            int py = y + row * (btnH + rowGap);
            placeCtl(tools[static_cast<size_t>(i)], px, py, colW, btnH);
        }
        y += rows * btnH + (rows - 1) * rowGap;
        y += sectionGap;
    }

    if (w > 0 && (g_hBtnToggleAnn || g_hAnnotShow)) {
        int cols = (w >= minColW * 2 + gap) ? 2 : 1;
        int colW = (cols == 2) ? (w - gap) / 2 : w;
        int rowCount = 0;

        if (cols == 2) {
            if (g_hBtnToggleAnn) {
                placeCtl(g_hBtnToggleAnn, x, y, colW, btnH);
            }
            if (g_hAnnotShow) {
                int px = g_hBtnToggleAnn ? (x + colW + gap) : x;
                placeCtl(g_hAnnotShow, px, y, colW, btnH);
            }
            rowCount = 1;
        } else {
            if (g_hBtnToggleAnn) {
                placeCtl(g_hBtnToggleAnn, x, y, colW, btnH);
                rowCount++;
            }
            if (g_hAnnotShow) {
                int py = y + rowCount * (btnH + rowGap);
                placeCtl(g_hAnnotShow, x, py, colW, btnH);
                rowCount++;
            }
        }

        if (rowCount > 0) {
            y += rowCount * btnH + (rowCount - 1) * rowGap;
            y += sectionGap;
        }
    }

    bool showColor = ToolbarHasColorOptions(g_toolMode);
    if (showColor) {
        int colorMaxY = y;
        int cx = x;
        int cy = y;
        bool anyColor = false;
        for (HWND cb : g_colorButtons) {
            if (!cb) continue;
            anyColor = true;
            if (cx + colorSize > x + w && cx > x) {
                cx = x;
                cy += colorSize + rowGap;
            }
            placeCtl(cb, cx, cy, colorSize, colorSize);
            cx += colorSize + gap;
            colorMaxY = std::max(colorMaxY, cy + colorSize);
        }
        if (g_hBtnPaletteCustom) {
            anyColor = true;
            if (cx + colorSize > x + w && cx > x) {
                cx = x;
                cy += colorSize + rowGap;
            }
            placeCtl(g_hBtnPaletteCustom, cx, cy, colorSize, colorSize);
            cx += colorSize + gap;
            colorMaxY = std::max(colorMaxY, cy + colorSize);
        }
        if (anyColor) {
            y = colorMaxY + sectionGap;
        }
    }

    // Top options stay close to the color row because they affect the primary
    // appearance of the selected annotation tool.
    bool showFont = ToolbarHasFontOptions(g_toolMode);
    if (showFont) {
        const int radioW = 34;
        const int slotGap = 4;
        auto placeFontSizeSlot = [&](HWND radio, HWND combo) {
            if (radio) placeCtl(radio, x, y, radioW, comboH);
            if (combo) {
                int comboX = x + radioW + slotGap;
                int comboW = std::max(0, w - radioW - slotGap);
                placeCtl(combo, comboX, y, comboW, comboH);
            }
            y += comboH + rowGap;
        };
        placeFontSizeSlot(g_hRadioFontSizeSlotA, g_hComboFontSize);
        placeFontSizeSlot(g_hRadioFontSizeSlotB, g_hComboFontSizeAlt);
        placeFull(g_hComboFont, comboH);
        if (g_toolMode == ToolMode::TextBox) {
            const int assistCheckW = std::min(120, std::max(72, w / 2));
            const int assistRadioW = std::max(0, (w - assistCheckW) / 2);
            placeCtl(g_hChkTextReadableBackground, x, y, assistCheckW, comboH);
            placeCtl(g_hRadioTextReadableBackgroundNormal, x + assistCheckW, y, assistRadioW, comboH);
            placeCtl(g_hRadioTextReadableBackgroundInverted, x + assistCheckW + assistRadioW, y,
                     std::max(0, w - assistCheckW - assistRadioW), comboH);
            y += comboH + rowGap;
            placeFull(g_hChkTextAutoWrap, comboH);
        }
    }

    if (g_toolMode == ToolMode::Pan) {
        placeFull(g_hChkPanMouseWheelZoom, comboH);
    }

    bool showAnnotMethod = ToolbarHasAnnotMethodOptions(g_toolMode);
    if (showAnnotMethod) {
        placeFull(g_hComboAnnotMethod, comboH);
    }

    bool showFreehandCorrection =
        g_toolMode == ToolMode::Freehand && FreehandCorrectionPenActive(g_config.freehandCorrection);
    if (showFreehandCorrection) {
        placeFull(g_hComboFreehandCorrection, comboH);
    }

    bool showMarkerTextStyle = ToolbarHasMarkerTextStyleOptions(g_toolMode);
    if (showMarkerTextStyle) {
        placeFull(g_hComboMarkerTextStyle, comboH);
    }

    bool showLineDash = ToolbarHasLineDashOptions(g_toolMode,g_shapeKind);
    if (showLineDash) {
        placeFull(g_hComboLineDashStyle, comboH);
    }

    bool showShape = ToolbarHasShapeOptions(g_toolMode);
    bool showShapeDrawMode = ToolbarHasShapeDrawModeOptions(g_toolMode,g_shapeKind);
    if (showShape) {
        placeFull(g_hComboShapeKind, comboH);
    }
    if (showShapeDrawMode) {
        placeFull(g_hComboShapeDrawMode, comboH);
    }

    if (ToolbarHasBlendModeOptions(g_toolMode,g_shapeKind)) {
        const auto radioWidth = [](HWND control) {
            wchar_t label[64]{};GetWindowTextW(control,label,64);
            HDC dc=GetDC(control);SIZE extent{128,0};
            if(dc){
                auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
                GetTextExtentPoint32W(dc,label,static_cast<int>(wcslen(label)),&extent);
                if(old)SelectObject(dc,old);
                ReleaseDC(control,dc);
            }
            return static_cast<int>(extent.cx)+GetSystemMetrics(SM_CXMENUCHECK)+12;
        };
        const int normalWidth=radioWidth(g_hRadioBlendNormal),multiplyWidth=radioWidth(g_hRadioBlendMultiply);
        if(w < normalWidth+multiplyWidth) {
            placeFull(g_hRadioBlendNormal,comboH);placeFull(g_hRadioBlendMultiply,comboH);
        } else {
            placeCtl(g_hRadioBlendNormal,x,y,normalWidth,comboH);
            placeCtl(g_hRadioBlendMultiply,x+normalWidth,y,w-normalWidth,comboH);
            y += comboH + rowGap;
        }
    }
    bool showWidth = ToolbarHasWidthOptions(g_toolMode);
    if (showWidth) {
        placeFull(g_hComboWidth, comboH);
    }
    bool showMarkerAlpha = ToolbarHasMarkerAlphaOptions(g_toolMode);
    if (showMarkerAlpha) {
        placeFull(g_hComboMarkerAlpha, comboH);
    }
    bool showMagnifierShape = ToolbarHasMagnifierShapeOptions(g_toolMode);
    if (showMagnifierShape) {
        placeFull(g_hComboMagnifierShape, comboH);
    }

    int height = std::max(0, (maxY + padding) - baseY);
    return height;
}

struct LayoutCalc {
    LayoutState state{};
    int cw{};
    int ch{};
    int split{};

    int topH{};
    int bottomY{};
    int bottomH{};
    int leftW{};
    int rightW{};
    int centerX{};
    int centerW{};
    int rightX{};

    int toolbarH{};
    int toolbarContentH{}, toolbarContentW{};
    int listTopOffset{};

    SplitBands bands{};
};

static LayoutCalc ComputeLayout(HWND hWnd, LayoutState inState, bool forApply) {
    LayoutCalc c{};
    c.state = inState;
    c.split = SplitBandThickness();

    RECT rc{};
    GetClientRect(hWnd, &rc);
    c.cw = std::max(0, static_cast<int>(rc.right - rc.left));
    c.ch = std::max(0, static_cast<int>(rc.bottom - rc.top));

    // Buttons + list layout constants (must match CreateChildren and existing UI sizing).
    constexpr int kNewBtnH = 28;
    constexpr int kNewBtnGap = 6;
    constexpr int kNewBtnTop = 6;
    c.listTopOffset = kNewBtnTop + kNewBtnH + kNewBtnGap;

    // Clamp widths/heights (bands are dedicated gaps; keep center >= kMinPane).
    int maxLeft = c.cw - kMinPane - kMinPane - c.split * 2;
    c.state.leftWidth = ClampInt(c.state.leftWidth, kMinPane, std::max(kMinPane, maxLeft));

    c.leftW = g_leftPaneCollapsed
        ? std::min(kCollapsedLeftPaneWidth, std::max(0, c.cw))
        : c.state.leftWidth;

    int maxRight = c.cw - c.leftW - kMinPane - c.split * 2;
    c.state.rightWidth = ClampInt(c.state.rightWidth, kMinPane, std::max(kMinPane, maxRight));

    int maxTop = c.ch - c.split - kMinPane;
    c.state.topHeight = ClampInt(c.state.topHeight, kMinPane, std::max(kMinPane, maxTop));

    c.rightW = c.state.rightWidth;
    c.topH = c.state.topHeight;
    c.bottomY = c.topH + c.split;
    c.bottomH = std::max(0, c.ch - c.bottomY);

    c.centerX = c.leftW + c.split;
    c.centerW = std::max(0, c.cw - c.leftW - c.rightW - c.split * 2);
    c.rightX = c.centerX + c.centerW + c.split;

    // Initialize/clamp left internal splits (g_leftSplit1/2 are "start of lower pane", i.e. after the band).
    int available = std::max(0, c.topH - c.listTopOffset);
    int minLH = kMinListHeight;
    if (available < (minLH * 3 + c.split * 2)) {
        minLH = std::max(0, (available - c.split * 2) / 3);
    }

    int listSpace = std::max(0, available - c.split * 2);
    int seg = (listSpace > 0) ? (listSpace / 3) : 0;
    int defS1 = c.listTopOffset + seg + c.split;
    int defS2 = c.listTopOffset + seg * 2 + c.split * 2;

    if (c.state.leftSplit1 <= c.listTopOffset ||
        c.state.leftSplit2 <= c.listTopOffset ||
        c.state.leftSplit1 >= c.state.leftSplit2) {
        c.state.leftSplit1 = defS1;
        c.state.leftSplit2 = defS2;
    }

    int minS1 = c.listTopOffset + c.split + minLH;
    int maxS2 = std::max(minS1, c.topH - minLH);
    int maxS1 = std::max(minS1, maxS2 - (c.split + minLH));
    c.state.leftSplit1 = ClampInt(c.state.leftSplit1, minS1, maxS1);
    int minS2 = c.state.leftSplit1 + c.split + minLH;
    c.state.leftSplit2 = ClampInt(c.state.leftSplit2, minS2, maxS2);

    // Split bands (these are real gaps, not overlay windows).
    c.bands.vLeft = RECT{ c.leftW, 0, c.leftW + c.split, c.ch };
    c.bands.vRight = RECT{ c.rightX - c.split, 0, c.rightX, c.ch };
    c.bands.hMain = RECT{ 0, c.topH, c.cw, c.topH + c.split };
    if (!g_leftPaneCollapsed) {
        c.bands.hLeftTop = RECT{ 0, c.state.leftSplit1 - c.split, c.leftW, c.state.leftSplit1 };
        c.bands.hLeftMid = RECT{ 0, c.state.leftSplit2 - c.split, c.leftW, c.state.leftSplit2 };
    }

    int toolbarH = LayoutAnnotToolbar(0, 0, c.rightW,
                                      [](HWND, int, int, int, int) {});
    // The annotation list is the recovery surface for existing work. Reserve
    // enough room for its status and at least two visible rows before allowing
    // optional toolbar controls to consume the complete top pane.
    constexpr int kMinAnnotPanelHeight = panelPad * 2 + panelSummaryH + panelGap + panelListMinH;
    const int reservedPanelH = g_showMathList ? kMinAnnotPanelHeight : 0;
    const int maxToolbarH = std::max(32, c.topH - reservedPanelH);
    c.toolbarH = std::max(32, toolbarH);
    c.toolbarH = std::min(c.toolbarH, maxToolbarH);
    c.toolbarContentW = c.rightW;
    if (toolbarH > c.toolbarH) {
        // Account for the scrollbar before wrapping rows, including palettes
        // and the shape blend options which grow taller in a narrow pane.
        c.toolbarContentW = std::max(0, c.rightW - GetSystemMetrics(SM_CXVSCROLL));
        toolbarH = LayoutAnnotToolbar(0, 0, c.toolbarContentW, [](HWND, int, int, int, int) {});
    }
    c.toolbarContentH = toolbarH;

    if (forApply) {
        // Apply only the current user-selected state. A layout refresh must not
        // rescale split positions and make boundaries drift when the client
        // size changes transiently (for example, after a frame update).
        SetLayoutState(c.state);
    }
    return c;
}

SplitBands GetSplitBands(HWND hWnd) {
    LayoutCalc c = ComputeLayout(hWnd, GetLayoutState(), false);
    return c.bands;
}

LayoutApplyResult ApplyLayout(HWND hWnd, LayoutPass pass) {
    LayoutCalc c = ComputeLayout(hWnd, GetLayoutState(), true);
    LayoutApplyResult result{};
    UINT baseFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
    if (pass == LayoutPass::Live) {
        baseFlags |= SWP_DEFERERASE;
    }

    struct Pos {
        HWND hwnd{};
        int x{};
        int y{};
        int w{};
        int h{};
        bool visible{};
        bool manageVisibility{};
    };
    struct ApplyOp {
        HWND hwnd{};
        HWND parent{};
        int x{};
        int y{};
        int w{};
        int h{};
        UINT flags{};
    };
    struct ParentBatch {
        HWND parent{};
        std::vector<ApplyOp> ops;
    };
    std::vector<Pos> positions;
    positions.reserve(55 + g_colorButtons.size());

    auto addPos = [&](HWND w, int x, int y, int ww, int hh,
                      bool visible = true, bool manageVisibility = false) {
        if (!w) return;
        positions.push_back({ w, x, y, std::max(0, ww), std::max(0, hh), visible, manageVisibility });
    };

    auto layoutShortcutPanel = [&](int areaX, int areaY, int areaW, int areaH) {
        const HWND shortcuts[] = {
            g_hChkShortcutHeading1,
            g_hChkShortcutHeading2,
            g_hShortcutHeadingLevelLabel,
            g_hBtnShortcutHeadingLevelUp,
            g_hChkShortcutBack,
            g_hChkShortcutChar,
            g_hChkShortcutBold,
            g_hChkShortcutItalic,
            g_hChkShortcutStrike,
            g_hChkShortcutUnderline,
            g_hChkShortcutLinkDecor,
            g_hBtnShortcutBackPreview,
            g_hBtnShortcutCharPreview,
            g_hBtnShortcutBackPalette,
             g_hBtnShortcutCharPalette,
             g_hShortcutIndentLabel,
             g_hShortcutIndentEdit,
             g_hShortcutMarginLabel,
             g_hShortcutMarginEdit,
             g_hShortcutFontSizeLabel,
             g_hShortcutFontSizeEdit,
             g_hShortcutTagEdit,
             g_hBtnShortcutInput,
             g_hBtnNoteWritingGuide,
             g_hBtnShortcutPdfLink,
             g_hBtnNoteAssistBullet,
             g_hBtnNoteAssistQuote,
             g_hBtnNoteAssistPageRef
         };
        if (areaW <= 0 || areaH <= 0) {
            for (HWND h : shortcuts) {
                addPos(h, areaX, areaY, 0, 0, false, true);
            }
            return;
        }
        const int padding = 6;
        HDC dc = GetDC(hWnd);
        struct FontMeasureGuard {
            HWND owner;
            HDC dc;
            HGDIOBJ previous;
            ~FontMeasureGuard() {
                if (dc && previous) SelectObject(dc, previous);
                if (dc) ReleaseDC(owner, dc);
            }
        } measureGuard{hWnd, dc, dc && g_hUIFont ? SelectObject(dc, g_hUIFont) : nullptr};
        TEXTMETRICW metrics{};
        const int fontHeight = dc && GetTextMetricsW(dc, &metrics) ? metrics.tmHeight : 16;
        const int rowH = std::max(26, fontHeight + 8);
        const int gap = 6;
        const int previewSize = 20;
        const int paletteW = 30;
        int x = areaX + padding;
        int y = areaY + padding;
        int innerW = std::max(0, areaW - padding * 2);
        if (innerW <= 0) {
            for (HWND h : shortcuts) {
                addPos(h, areaX, areaY, 0, 0, false, true);
            }
            return;
        }

        auto place = [&](HWND h, int px, int py, int pw, int ph) {
            // Short panes must not expose partial controls outside the palette.
            // Keep the input row first; expanding the pane reveals later rows.
            const bool fits = pw > 0 && ph > 0 && px >= areaX && py >= areaY &&
                px + pw <= areaX + areaW && py + ph <= areaY + areaH;
            addPos(h, fits ? px : areaX, fits ? py : areaY,
                   fits ? pw : 0, fits ? ph : 0, fits, true);
        };

        const auto textWidth = [&](HWND control, bool checkbox = false) {
            const int length = GetWindowTextLengthW(control);
            std::wstring label(static_cast<size_t>(length) + 1, L'\0');
            label.resize(static_cast<size_t>(GetWindowTextW(control, label.data(), length + 1)));
            SIZE size{};
            if (!dc || !GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size)) size.cx = 32;
            return static_cast<int>(size.cx) + (checkbox ? fontHeight + 8 : 8);
        };
        auto layoutPaletteRow = [&](std::initializer_list<HWND> buttons, bool checkbox = false) {
            std::vector<HWND> items(buttons);
            std::vector<int> widths;
            for (HWND control : items) widths.push_back(std::max(30, textWidth(control, checkbox)));
            for (size_t first = 0; first < items.size();) {
                size_t last = first + 1;
                int used = widths[first];
                while (last < items.size() && used + gap + widths[last] <= innerW) {
                    used += gap + widths[last++];
                }
                const int extra = std::max(0, innerW - used) / static_cast<int>(last - first);
                int curX = x;
                for (size_t index = first; index < last; ++index) {
                    const int width = widths[index] + extra;
                    place(items[index], curX, y, width, rowH);
                    curX += width + gap;
                }
                y += rowH + gap;
                first = last;
            }
        };

        // Keep the reference reachable even with no note or a short pane.
        place(g_hBtnNoteWritingGuide, x, y, innerW, rowH);
        y += rowH + gap;

        int inputRowW = std::max(0, innerW);
        int inputBtnW = std::max(textWidth(g_hBtnShortcutInput), std::min(96, std::max(60, inputRowW / 4)));
        if (inputBtnW + gap + 48 > inputRowW) {
            place(g_hBtnShortcutInput, x, y, inputRowW, rowH);
            y += rowH + gap;
            inputBtnW = 0;
        } else {
            place(g_hBtnShortcutInput, x, y, inputBtnW, rowH);
        }
        int inputEditW = std::max(0, inputRowW - inputBtnW - gap);
        place(g_hShortcutTagEdit, inputBtnW ? x + inputBtnW + gap : x, y,
              inputBtnW ? inputEditW : innerW, rowH);
        y += rowH + gap;

        const int linkDecorW = textWidth(g_hChkShortcutLinkDecor, true);
        const int linkBtnW = std::max(90, textWidth(g_hBtnShortcutPdfLink));
        place(g_hChkShortcutLinkDecor, x, y, linkDecorW, rowH);
        if (linkDecorW + gap + linkBtnW > innerW) {
            y += rowH + gap;
            place(g_hBtnShortcutPdfLink, x, y, innerW, rowH);
        } else {
            place(g_hBtnShortcutPdfLink, x + linkDecorW + gap, y, linkBtnW, rowH);
        }
        y += rowH + gap;

        layoutPaletteRow({ g_hBtnNoteAssistBullet, g_hBtnNoteAssistQuote, g_hBtnNoteAssistPageRef });

        const int arrowW = std::max({22, textWidth(g_hBtnShortcutHeadingLevelUp), textWidth(g_hChkShortcutHeading2)});
        const int levelW = std::max(38, textWidth(g_hShortcutHeadingLevelLabel));
        const int headingMinW = textWidth(g_hChkShortcutHeading1, true);
        const int headingW = innerW - (arrowW * 2 + levelW + gap * 3);
        const bool headingFits = headingW >= headingMinW;
        place(g_hChkShortcutHeading1, x, y, headingFits ? headingW : std::max(innerW, headingMinW), rowH);
        if (!headingFits) y += rowH + gap;
        int headingX = headingFits ? x + headingW + gap : x;
        place(g_hBtnShortcutHeadingLevelUp, headingX, y, arrowW, rowH);
        headingX += arrowW + gap;
        place(g_hChkShortcutHeading2, headingX, y, arrowW, rowH);
        headingX += arrowW + gap;
        place(g_hShortcutHeadingLevelLabel, headingX, y, levelW, rowH);
        y += rowH + gap;

        auto layoutColorRow = [&](HWND checkbox, HWND preview, HWND palette) {
            const int pickerW = std::max(paletteW, textWidth(palette));
            const int minimumCheckW = textWidth(checkbox, true);
            int controlW = innerW - (previewSize + gap + pickerW + gap);
            const bool together = controlW >= minimumCheckW;
            if (!together) controlW = std::max(innerW, minimumCheckW);
            place(checkbox, x, y, controlW, rowH);
            if (!together) y += rowH + gap;
            int previewX = together ? x + controlW + gap : x;
            int previewY = y + (rowH - previewSize) / 2;
            place(preview, previewX, previewY, previewSize, previewSize);
            int paletteX = previewX + previewSize + gap;
            place(palette, paletteX, y, pickerW, rowH);
            y += rowH + gap;
        };

        layoutColorRow(g_hChkShortcutBack, g_hBtnShortcutBackPreview, g_hBtnShortcutBackPalette);
        layoutColorRow(g_hChkShortcutChar, g_hBtnShortcutCharPreview, g_hBtnShortcutCharPalette);

        layoutPaletteRow({g_hChkShortcutBold, g_hChkShortcutItalic, g_hChkShortcutStrike, g_hChkShortcutUnderline}, true);

        {
            constexpr int kNumFields = 3; // d, m, s
            const int labelW = std::max({26, textWidth(g_hShortcutIndentLabel),
                textWidth(g_hShortcutMarginLabel), textWidth(g_hShortcutFontSizeLabel)});
            int labelGap = 2;
            int editMinW = 28;
            const int columns = std::clamp((innerW + gap) / (labelW + labelGap + editMinW + gap), 1, kNumFields);
            const int columnW = (innerW - gap * (columns - 1)) / columns;
            const int editW = std::max(0, columnW - labelW - labelGap);
            const HWND labels[] = {g_hShortcutIndentLabel, g_hShortcutMarginLabel, g_hShortcutFontSizeLabel};
            const HWND edits[] = {g_hShortcutIndentEdit, g_hShortcutMarginEdit, g_hShortcutFontSizeEdit};
            for (int i = 0; i < kNumFields; ++i) {
                const int curX = x + (i % columns) * (columnW + gap);
                place(labels[i], curX, y, labelW, rowH);
                place(edits[i], curX + labelW + labelGap, y, editW, rowH);
                if ((i + 1) % columns == 0 || i + 1 == kNumFields) y += rowH + gap;
            }
        }

    };

    // Left column (directory/session lists)
    constexpr int kNewBtnH = 28;
    constexpr int kNewBtnGap = 6;
    constexpr int kNewBtnTop = 6;
    const bool leftCollapsed = g_leftPaneCollapsed;
    if (g_hBtnToggleLeftPane) {
        SetWindowTextW(g_hBtnToggleLeftPane, leftCollapsed ? L">>" : L"<<");
    }
    int toggleW = std::min(28, std::max(0, c.leftW - kNewBtnGap));
    int toggleX = leftCollapsed
        ? std::max(0, (c.leftW - toggleW) / 2)
        : std::max(kNewBtnGap, c.leftW - kNewBtnGap - toggleW);
    addPos(g_hBtnToggleLeftPane, toggleX, kNewBtnTop, toggleW, kNewBtnH, true, true);

    int newBtnAreaW = std::max(0, c.leftW - toggleW - kNewBtnGap * 4);
    int newBtnW = std::max(0, newBtnAreaW / 2);
    if (leftCollapsed) {
        addPos(g_hBtnNewSession, 0, 0, 0, 0, false, true);
        addPos(g_hBtnNewNote, 0, 0, 0, 0, false, true);
    } else {
        addPos(g_hBtnNewSession, kNewBtnGap, kNewBtnTop, newBtnW, kNewBtnH, true, true);
        addPos(g_hBtnNewNote, kNewBtnGap * 2 + newBtnW, kNewBtnTop, newBtnW, kNewBtnH, true, true);
    }

    int hLecture = std::max(0, c.state.leftSplit1 - c.split - c.listTopOffset);
    int hSession = std::max(0, c.state.leftSplit2 - c.split - c.state.leftSplit1);
    int hPdfNote = std::max(0, c.topH - c.state.leftSplit2);
    int hPdf = hPdfNote / 2;
    int hNote = hPdfNote - hPdf;

    if (leftCollapsed) {
        addPos(g_hLectureList, 0, c.listTopOffset, 0, 0, false, true);
        addPos(g_hSessionList, 0, c.state.leftSplit1, 0, 0, false, true);
        addPos(g_hPdfList, 0, c.state.leftSplit2, 0, 0, false, true);
        addPos(g_hNoteList, 0, c.state.leftSplit2, 0, 0, false, true);
    } else {
        addPos(g_hLectureList, 0, c.listTopOffset, c.leftW, hLecture, true, true);
        addPos(g_hSessionList, 0, c.state.leftSplit1, c.leftW, hSession, true, true);
        addPos(g_hPdfList, 0, c.state.leftSplit2, c.leftW, hPdf, true, true);
        addPos(g_hNoteList, 0, c.state.leftSplit2 + hPdf, c.leftW, hNote, true, true);
    }

    const bool noteAtTop = (g_notePlacement == NotePlacement::Top);
    const int pdfY = noteAtTop ? c.bottomY : 0;
    const int pdfH = noteAtTop ? c.bottomH : c.topH;
    const int noteY = noteAtTop ? 0 : c.bottomY;
    const int noteH = noteAtTop ? c.topH : c.bottomH;

    // Main center: PDF view and primary note pane can swap vertically.
    addPos(g_hPdfView, c.centerX, pdfY, c.centerW, pdfH);

    // Right column: tool selection toolbar
    addPos(g_hPdfToolbar, c.rightX, 0, c.rightW, c.toolbarH);
    std::vector<HWND> placedToolbarControls;
    placedToolbarControls.reserve(32 + g_colorButtons.size());
    const int toolbarOffset = toolbar_scroll::Offset(g_hPdfToolbar, c.toolbarContentH,
        c.toolbarH, static_cast<int>(g_toolMode));
    LayoutAnnotToolbar(0, 0, c.toolbarContentW, [&](HWND w, int x, int y, int ww, int hh) {
        if (w) placedToolbarControls.push_back(w);
        if (!toolbar_scroll::InstallChild(w, g_hPdfToolbar))
            preview_trace::Append(L"ToolbarScroll", L"control scroll/focus installation failed");
        // Keep every applicable control reachable. The host clips only the
        // current viewport, rather than removing options below its lower edge.
        addPos(w, x, y - toolbarOffset, ww, hh, true, true);
    });

    bool mergeNote = (!noteAtTop &&
                      g_bottomPanePin == BottomPanePin::Note &&
                      g_bottomNoteMode == BottomNoteMode::Legacy);
    bool showAnnotPanel = g_showMathList;
    bool showMathPane = (g_bottomPanePin == BottomPanePin::Math);
    bool showBottomNotePane = !showMathPane && ShouldShowBottomNotePane();

    // Top/right: annotation panel (extend down when pinned)
    int panelY = c.toolbarH;
    int panelTopH = std::max(0, c.topH - c.toolbarH);
    int panelX = c.rightX;
    int panelW = c.rightW;
    auto hidePos = [&](HWND w) {
        if (!w) return;
        addPos(w, panelX, panelY, 0, 0);
    };
    auto hideToolbarOptionIfUnused = [&](HWND w) {
        if (!w) return;
        if (std::find(placedToolbarControls.begin(), placedToolbarControls.end(), w) != placedToolbarControls.end()) {
            return;
        }
        addPos(w, 0, 0, 0, 0, false, true);
    };
    hideToolbarOptionIfUnused(g_hComboAnnotMethod);
    hideToolbarOptionIfUnused(g_hComboShapeGeometry);
    hideToolbarOptionIfUnused(g_hComboFreehandCorrection);
    hideToolbarOptionIfUnused(g_hComboMarkerTextStyle);
    hideToolbarOptionIfUnused(g_hComboLineDashStyle);
    hideToolbarOptionIfUnused(g_hComboShapeKind);
    hideToolbarOptionIfUnused(g_hComboShapeDrawMode);
    for (HWND cb : g_colorButtons) {
        hideToolbarOptionIfUnused(cb);
    }
    hideToolbarOptionIfUnused(g_hBtnPaletteCustom);
    hideToolbarOptionIfUnused(g_hComboFontSize);
    hideToolbarOptionIfUnused(g_hComboFontSizeAlt);
    hideToolbarOptionIfUnused(g_hRadioFontSizeSlotA);
    hideToolbarOptionIfUnused(g_hRadioFontSizeSlotB);
    hideToolbarOptionIfUnused(g_hComboFont);
    hideToolbarOptionIfUnused(g_hChkTextReadableBackground);
    hideToolbarOptionIfUnused(g_hRadioTextReadableBackgroundNormal);
    hideToolbarOptionIfUnused(g_hRadioTextReadableBackgroundInverted);
    hideToolbarOptionIfUnused(g_hChkTextAutoWrap);
    hideToolbarOptionIfUnused(g_hChkPanMouseWheelZoom);
    hideToolbarOptionIfUnused(g_hComboWidth);
    hideToolbarOptionIfUnused(g_hComboMarkerAlpha);
    hideToolbarOptionIfUnused(g_hRadioBlendNormal);
    hideToolbarOptionIfUnused(g_hRadioBlendMultiply);
    hideToolbarOptionIfUnused(g_hComboMagnifierShape);

    // Never allow annotation controls to overlap split bands. If there's not enough
    // vertical space (e.g. toolbar takes most of the top pane), collapse controls
    // to 0-size so the splitter band remains visible/clickable.
    if (!showAnnotPanel || panelTopH < panelPad * 2 + panelSummaryH + panelGap + panelListMinH) {
        hidePos(g_hAnnotSettings);
        hidePos(g_hAnnotClear);
        hidePos(g_hAnnotList);
        hidePos(g_hAnnotSummary);
    } else {
        int innerTop = panelY + panelPad;
        int innerBottom = panelY + panelTopH - panelPad; // == c.topH - panelPad
        int x = panelX + panelPad;
        int y = innerTop;
        int w = std::max(0, panelW - panelPad * 2);
        int buttonW = w;

        auto canReserveList = [&](int needH) {
            return needH > 0 && y + needH + panelListMinH <= innerBottom;
        };

        addPos(g_hAnnotSummary, x, y, w, panelSummaryH);
        y += panelSummaryH + panelGap;

        // Settings and clear are convenient, but the status and list are the
        // controls that prevent work from becoming invisible in a short pane.
        if (canReserveList(panelRowH + panelGap)) {
            addPos(g_hAnnotSettings, x, y, buttonW, panelRowH);
            y += panelRowH + panelGap;
        } else {
            hidePos(g_hAnnotSettings);
        }

        if (canReserveList(panelRowH + panelGap)) {
            addPos(g_hAnnotClear, x, y, buttonW, panelRowH);
            y += panelRowH + panelGap;
        } else {
            hidePos(g_hAnnotClear);
        }

        int listH = std::max(0, innerBottom - y);
        if (listH >= panelListMinH) {
            addPos(g_hAnnotList, x, y, w, listH);
        } else {
            hidePos(g_hAnnotList);
            hidePos(g_hAnnotSummary);
        }
    }

    // Bottom: shortcut / note / bottom-right overlay
    int bottomRightX = c.rightX;
    int bottomRightY = c.bottomY;
    int bottomRightW = c.rightW;
    int bottomRightH = c.bottomH;
    if (mergeNote) {
        bottomRightX -= c.split;
        bottomRightW += c.split;
    }

    // The shortcut panel is fixed in the bottom-left column.
    const bool shortcutRight = false;
    const bool returnHiddenBottomPaneToNote = !noteAtTop && !showMathPane && !showBottomNotePane && !shortcutRight;
    int noteW = c.centerW + ((mergeNote || returnHiddenBottomPaneToNote) ? (c.rightW + c.split) : 0);
    addPos(g_hNoteEdit, c.centerX, noteY, noteW, noteH);
    addPos(g_hNoteRender, c.centerX, noteY, noteW, noteH);
    int bottomNoteH = showBottomNotePane ? bottomRightH : 0;
    int bottomMathH = showMathPane ? bottomRightH : 0;
    addPos(g_hBottomNote, bottomRightX, bottomRightY, bottomRightW, bottomNoteH, showBottomNotePane, true);
    addPos(g_hBottomMath, bottomRightX, bottomRightY, bottomRightW, bottomMathH, showMathPane, true);
    g_hBottomRight = showMathPane ? g_hBottomMath : (showBottomNotePane ? g_hBottomNote : nullptr);
    int shortcutAreaX = shortcutRight ? bottomRightX : 0;
    int shortcutAreaY = shortcutRight ? bottomRightY : c.bottomY;
    int shortcutAreaW = shortcutRight ? bottomRightW : (leftCollapsed ? 0 : c.leftW);
    int shortcutAreaH = shortcutRight ? bottomRightH : c.bottomH;
    layoutShortcutPanel(shortcutAreaX, shortcutAreaY, shortcutAreaW, shortcutAreaH);

    std::vector<ParentBatch> batches;
    batches.reserve(4);
    std::vector<HWND> changedControls;
    changedControls.reserve(positions.size());
    auto batchForParent = [&](HWND parent) -> ParentBatch& {
        for (auto& batch : batches) {
            if (batch.parent == parent) return batch;
        }
        batches.push_back(ParentBatch{ parent, {} });
        return batches.back();
    };
    for (const auto& p : positions) {
        if (!p.hwnd) continue;
        RECT desired{ p.x, p.y, p.x + p.w, p.y + p.h };
        RECT current = GetWindowRectInParentCoords(p.hwnd);
        const bool boundsChanged = !RectEquals(current, desired);
        const bool visibilityChanged =
            p.manageVisibility && (((GetWindowLongPtrW(p.hwnd, GWL_STYLE) & WS_VISIBLE) != 0) != p.visible);
        
        if (boundsChanged) result.anyBoundsChanged = true;
        if (visibilityChanged) result.anyVisibilityChanged = true;
        if (!boundsChanged && !visibilityChanged) continue;
        if (boundsChanged || visibilityChanged) {
            changedControls.push_back(p.hwnd);
            MarkChanged(result, p.hwnd);
            RECT currentInRoot = current;
            RECT desiredInRoot = desired;
            HWND parent = GetParent(p.hwnd);
            if (parent && parent != hWnd) {
                MapWindowPoints(parent, hWnd, reinterpret_cast<POINT*>(&currentInRoot), 2);
                MapWindowPoints(parent, hWnd, reinterpret_cast<POINT*>(&desiredInRoot), 2);
            }
            UnionRectInPlace(&result.hasRootDirtyRect, &result.rootDirtyRect, currentInRoot);
            UnionRectInPlace(&result.hasRootDirtyRect, &result.rootDirtyRect, desiredInRoot);
        }

        UINT flags = baseFlags;
        if (!boundsChanged) flags |= SWP_NOMOVE | SWP_NOSIZE;
        if (visibilityChanged) {
            flags |= p.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        }

        ParentBatch& batch = batchForParent(GetParent(p.hwnd));
        batch.ops.push_back(ApplyOp{ p.hwnd, batch.parent, p.x, p.y, p.w, p.h, flags });
    }

    for (const auto& batch : batches) {
        if (batch.ops.empty()) continue;

        HDWP hdwp = BeginDeferWindowPos(static_cast<int>(batch.ops.size()));
        if (hdwp) {
            for (const auto& op : batch.ops) {
                hdwp = DeferWindowPos(hdwp, op.hwnd, nullptr, op.x, op.y, op.w, op.h, op.flags);
                if (!hdwp) break;
            }
        }

        if (hdwp && EndDeferWindowPos(hdwp)) {
            continue;
        }

        for (const auto& op : batch.ops) {
            SetWindowPos(op.hwnd, nullptr, op.x, op.y, op.w, op.h, op.flags);
        }
    }

    toolbar_scroll::SetRange(g_hPdfToolbar, c.toolbarContentH, c.toolbarH, static_cast<int>(g_toolMode));
    EnsureInactiveCachedNoteEditWindowsParked();
    UpdateLayoutDependentViews(result, pass);
    if (result.anyBoundsChanged || result.anyVisibilityChanged) {
        RedrawChangedLayout(hWnd, result, changedControls);
    }

    // Bottom-left area currently contains only shortcut buttons, so we don't need a container window.
    return result;
}

} // namespace ui


