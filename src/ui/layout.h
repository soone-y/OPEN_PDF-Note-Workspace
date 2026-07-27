#pragma once

#include <windows.h>

namespace ui {

enum class LayoutPass {
    Live,
    Commit,
};

struct LayoutState {
    int leftWidth{};
    int rightWidth{};
    int topHeight{};
    int leftSplit1{};
    int leftSplit2{};
};

struct SplitBands {
    RECT vLeft{};
    RECT vRight{};
    RECT hMain{};
    RECT hLeftTop{};
    RECT hLeftMid{};
};

struct LayoutApplyResult {
    bool anyBoundsChanged{};
    bool anyVisibilityChanged{};
    bool hasRootDirtyRect{};
    RECT rootDirtyRect{};
    bool pdfViewBoundsChanged{};
    bool pdfToolbarBoundsChanged{};
    bool noteEditorBoundsChanged{};
    bool noteRenderBoundsChanged{};
    bool bottomPaneBoundsChanged{};
    bool annotPanelBoundsChanged{};
    bool shortcutPanelBoundsChanged{};
};

int SplitBandThickness();

LayoutState GetLayoutState();
void SetLayoutState(const LayoutState& state);

SplitBands GetSplitBands(HWND hWnd);

// Positions child HWNDs according to the current globals (g_leftWidth, ...),
// applies view-specific bounds notifications, and requests the necessary redraws.
LayoutApplyResult ApplyLayout(HWND hWnd, LayoutPass pass);

// Repaints the complete current layout, including child non-client frames.
// Use this for non-layout state changes that require a settled workspace redraw.
void RedrawLayoutTree(HWND hWnd);

} // namespace ui


