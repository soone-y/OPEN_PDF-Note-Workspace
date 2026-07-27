// file: ui/splitter.cpp
#include "ui/splitter.h"

#include "core/app_core.h"

#include <algorithm>

namespace ui {

static bool PtInRectClient(const RECT& r, POINT pt) {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

SplitHit HitTest(const SplitBands& b, POINT pt) {
    // Priority: inner left splits, then vertical splits, then main horizontal.
    if (!g_leftPaneCollapsed) {
        if (PtInRectClient(b.hLeftTop, pt)) return SplitHit::HLeftTop;
        if (PtInRectClient(b.hLeftMid, pt)) return SplitHit::HLeftMid;
        if (PtInRectClient(b.vLeft, pt)) return SplitHit::VLeft;
    }
    if (PtInRectClient(b.vRight, pt)) return SplitHit::VRight;
    if (PtInRectClient(b.hMain, pt)) return SplitHit::HMain;
    return SplitHit::None;
}

static HCURSOR CursorForHit(SplitHit hit) {
    switch (hit) {
    case SplitHit::VLeft:
    case SplitHit::VRight:
        return LoadCursorW(nullptr, IDC_SIZEWE);
    case SplitHit::HMain:
    case SplitHit::HLeftTop:
    case SplitHit::HLeftMid:
        return LoadCursorW(nullptr, IDC_SIZENS);
    default:
        return nullptr;
    }
}

bool HandleSetCursor(HWND hWnd) {
    DWORD pos = GetMessagePos();
    POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
    ScreenToClient(hWnd, &pt);
    SplitBands bands = GetSplitBands(hWnd);
    SplitHit hit = HitTest(bands, pt);
    if (hit == SplitHit::None) return false;
    HCURSOR cur = CursorForHit(hit);
    if (cur) {
        SetCursor(cur);
        return true;
    }
    return false;
}

void PaintSplitBands(HDC hdc, const SplitBands& bands) {
    HBRUSH br = CreateSolidBrush(g_theme.splitterBg);
    HPEN pen = CreatePen(PS_DOT, 1, g_theme.splitterLine);
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    auto paintBand = [&](const RECT& rc, bool vertical) {
        if (rc.right <= rc.left || rc.bottom <= rc.top) return;
        FillRect(hdc, &rc, br);
        if (vertical) {
            int cx = (rc.left + rc.right) / 2;
            MoveToEx(hdc, cx, rc.top + 6, nullptr);
            LineTo(hdc, cx, rc.bottom - 6);
        } else {
            int cy = (rc.top + rc.bottom) / 2;
            MoveToEx(hdc, rc.left + 6, cy, nullptr);
            LineTo(hdc, rc.right - 6, cy);
        }
    };

    paintBand(bands.vLeft, true);
    paintBand(bands.vRight, true);
    paintBand(bands.hMain, false);
    paintBand(bands.hLeftTop, false);
    paintBand(bands.hLeftMid, false);

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    DeleteObject(br);
}

static LayoutState s_pending{};
static bool s_hasPending = false;
static SplitHit s_dragHit = SplitHit::None;

static bool AnyDragging() {
    return g_draggingLeft || g_draggingRight || g_draggingHoriz || g_draggingLeftTop || g_draggingLeftMid;
}

static void ClearDraggingFlags() {
    g_draggingLeft = g_draggingRight = g_draggingHoriz = g_draggingLeftTop = g_draggingLeftMid = false;
    s_dragHit = SplitHit::None;
}

bool BeginDrag(HWND hWnd, POINT pt) {
    SplitBands bands = GetSplitBands(hWnd);
    SplitHit hit = HitTest(bands, pt);
    if (hit == SplitHit::None) return false;

    s_dragHit = hit;
    g_dragStart = pt;
    g_leftStart = g_leftWidth;
    g_rightStart = g_rightWidth;
    g_topStart = g_topHeight;
    g_leftSplitStart1 = g_leftSplit1;
    g_leftSplitStart2 = g_leftSplit2;

    ClearDraggingFlags();
    switch (hit) {
    case SplitHit::VLeft:    g_draggingLeft = true; break;
    case SplitHit::VRight:   g_draggingRight = true; break;
    case SplitHit::HMain:    g_draggingHoriz = true; break;
    case SplitHit::HLeftTop: g_draggingLeftTop = true; break;
    case SplitHit::HLeftMid: g_draggingLeftMid = true; break;
    default: break;
    }

    s_pending = GetLayoutState();
    s_hasPending = false;
    SetCapture(hWnd);
    SetTimer(hWnd, kSplitDragTimerId, 16, nullptr); // ~60fps coalescing
    return true;
}

void UpdateDrag(HWND hWnd, POINT pt) {
    if (!AnyDragging()) return;

    RECT rc{};
    GetClientRect(hWnd, &rc);
    int cw = std::max(0, static_cast<int>(rc.right - rc.left));
    int ch = std::max(0, static_cast<int>(rc.bottom - rc.top));
    int split = SplitBandThickness();

    LayoutState next = GetLayoutState(); // applied state as base
    int dx = pt.x - g_dragStart.x;
    int dy = pt.y - g_dragStart.y;

    if (g_draggingLeft) {
        int want = g_leftStart + dx;
        int maxLeft = cw - g_rightWidth - kMinPane - split * 2;
        next.leftWidth = std::clamp(want, kMinPane, std::max(kMinPane, maxLeft));
    } else if (g_draggingRight) {
        int want = g_rightStart - dx;
        int maxRight = cw - g_leftWidth - kMinPane - split * 2;
        next.rightWidth = std::clamp(want, kMinPane, std::max(kMinPane, maxRight));
    } else if (g_draggingHoriz) {
        int want = g_topStart + dy;
        int maxTop = ch - split - kMinPane;
        next.topHeight = std::clamp(want, kMinPane, std::max(kMinPane, maxTop));
    } else if (g_draggingLeftTop || g_draggingLeftMid) {
        // Internal splits live only in the top-left column.
        constexpr int kNewBtnH = 28;
        constexpr int kNewBtnGap = 6;
        constexpr int kNewBtnTop = 6;
        int listTopOffset = kNewBtnTop + kNewBtnH + kNewBtnGap;
        int topH = g_topHeight;
        int available = std::max(0, topH - listTopOffset);
        int minLH = kMinListHeight;
        if (available < (minLH * 3 + split * 2)) {
            minLH = std::max(0, (available - split * 2) / 3);
        }
        int minS1 = listTopOffset + split + minLH;
        int maxS2 = std::max(minS1, topH - minLH);
        int maxS1 = std::max(minS1, maxS2 - (split + minLH));

        if (g_draggingLeftTop) {
            int want = g_leftSplitStart1 + dy;
            want = std::clamp(want, minS1, maxS1);
            next.leftSplit1 = want;
            int minS2 = next.leftSplit1 + split + minLH;
            next.leftSplit2 = std::max(next.leftSplit2, minS2);
        } else {
            int minS2 = g_leftSplit1 + split + minLH;
            int want = g_leftSplitStart2 + dy;
            want = std::clamp(want, minS2, maxS2);
            next.leftSplit2 = want;
            next.leftSplit1 = std::min(next.leftSplit1, next.leftSplit2 - (split + minLH));
            next.leftSplit1 = std::clamp(next.leftSplit1, minS1, maxS1);
        }
    }

    s_pending = next;
    s_hasPending = true;
}

void OnTimer(HWND hWnd) {
    if (!AnyDragging()) return;
    if (!s_hasPending) return;

    SetLayoutState(s_pending);
    ApplyLayout(hWnd, LayoutPass::Live);
    s_hasPending = false;
}

bool EndDrag(HWND hWnd) {
    if (!AnyDragging()) return false;

    if (s_hasPending) {
        SetLayoutState(s_pending);
        s_hasPending = false;
    }
    ApplyLayout(hWnd, LayoutPass::Commit);

    ClearDraggingFlags();
    KillTimer(hWnd, kSplitDragTimerId);
    ReleaseCapture();
    return true;
}

void CancelDrag(HWND hWnd) {
    if (!AnyDragging()) return;
    s_hasPending = false;
    ClearDraggingFlags();
    KillTimer(hWnd, kSplitDragTimerId);
    ReleaseCapture();
}

} // namespace ui


