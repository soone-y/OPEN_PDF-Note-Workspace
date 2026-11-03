#pragma once

#include "core/shape_outline.h"

namespace ui {
// All dash runs share one mask and one blend, keeping translucent joins uniform.
inline void DrawDashedShapeOutline(HDC hdc, ShapeKind kind, const RECT& bounds, double rotation,
                                   int penWidth, COLORREF color, BYTE alpha,
                                   const std::vector<double>& dash, double ptToPx, int contentRotation = 0, double wholeAngle = 0.0) {
    if (!hdc || penWidth <= 0 || alpha == 0) return;
    const auto outline = shape_outline::BuildOriented(kind, bounds.left, bounds.top, bounds.right, bounds.bottom, rotation, contentRotation, wholeAngle);
    const auto runs = shape_outline::Dash(outline, dash, ptToPx);
    if (runs.empty()) return;
    RECT clip{};
    if (GetClipBox(hdc, &clip) == ERROR) return;
    const int margin = penWidth / 2 + 2;
    RECT area{bounds.left - margin, bounds.top - margin, bounds.right + margin, bounds.bottom + margin};
    if (wholeAngle != 0.0 && !outline.empty()) {
        double l=outline[0].x,t=outline[0].y,r=l,b=t;
        for (const auto& p:outline){l=std::min(l,p.x);r=std::max(r,p.x);t=std::min(t,p.y);b=std::max(b,p.y);}
        area={static_cast<LONG>(std::floor(l))-margin,static_cast<LONG>(std::floor(t))-margin,
            static_cast<LONG>(std::ceil(r))+margin,static_cast<LONG>(std::ceil(b))+margin};
    }
    if (!IntersectRect(&area, &area, &clip)) return;
    const int w = area.right - area.left, h = area.bottom - area.top;
    std::vector<POINT> points;
    std::vector<DWORD> counts;
    for (const auto& run : runs) {
        counts.push_back(static_cast<DWORD>(run.size()));
        for (const auto& p : run)
            points.push_back({static_cast<LONG>(std::lround(p.x - area.left)),
                              static_cast<LONG>(std::lround(p.y - area.top))});
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return; }
    HDC memory = CreateCompatibleDC(hdc);
    HPEN pen = CreatePen(PS_SOLID, penWidth, RGB(255, 255, 255));
    if (!memory || !pen) {
        if (pen) DeleteObject(pen);
        if (memory) DeleteDC(memory);
        DeleteObject(bitmap); return;
    }
    const auto oldBitmap = SelectObject(memory, bitmap), oldPen = SelectObject(memory, pen);
    std::fill_n(static_cast<DWORD*>(bits), static_cast<size_t>(w) * h, 0);
    PolyPolyline(memory, points.data(), counts.data(), static_cast<DWORD>(counts.size()));
    GdiFlush();
    const DWORD pixel = (static_cast<DWORD>(alpha) << 24) |
        (static_cast<DWORD>(GetRValue(color) * alpha / 255) << 16) |
        (static_cast<DWORD>(GetGValue(color) * alpha / 255) << 8) |
        static_cast<DWORD>(GetBValue(color) * alpha / 255);
    auto* pixels = static_cast<DWORD*>(bits);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) pixels[i] = pixels[i] ? pixel : 0;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(hdc, area.left, area.top, w, h, memory, 0, 0, w, h, blend);
    SelectObject(memory, oldPen); SelectObject(memory, oldBitmap);
    DeleteObject(pen); DeleteDC(memory); DeleteObject(bitmap);
}
} // namespace ui
