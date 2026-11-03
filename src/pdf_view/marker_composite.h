#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace pdf_marker {

// Match standard PDF Highlight annotations: interpolate between the backdrop
// and its product with the marker color. Black stays black; white takes the
// marker tint. No glyph detection or source-document mutation is involved.
[[nodiscard]] inline uint32_t Blend(uint32_t backdrop, COLORREF color, BYTE alpha) noexcept {
    const auto channel = [alpha](unsigned base, unsigned marker) {
        return (base * (65025u - alpha * (255u - marker)) + 32512u) / 65025u;
    };
    return (backdrop & 0xff000000u) |
        (channel((backdrop >> 16) & 255, GetRValue(color)) << 16) |
        (channel((backdrop >> 8) & 255, GetGValue(color)) << 8) |
        channel(backdrop & 255, GetBValue(color));
}

namespace detail {
struct Surface {
    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    ~Surface() {
        if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};
} // namespace detail

// Work only on the visible logical rectangle, including a translated viewport.
// Strip composition caps each temporary DIB at 4 MiB even at extreme zoom.
[[nodiscard]] inline bool Paint(HDC dc, const RECT& rectangle, COLORREF color, BYTE alpha) noexcept {
    if (!dc) return false;
    if (alpha == 0 || rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) return true;
    RECT clip{}, visible{};
    if (GetClipBox(dc, &clip) == ERROR) return false;
    if (!IntersectRect(&visible, &rectangle, &clip)) return true;
    constexpr int kMaxPixels = 1024 * 1024;
    const int width = visible.right - visible.left;
    if (width <= 0 || width > kMaxPixels) return false;
    const int rows = std::min<int>(visible.bottom - visible.top, kMaxPixels / width);
    detail::Surface surface;
    surface.dc = CreateCompatibleDC(dc);
    if (!surface.dc) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -rows;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    surface.bitmap = CreateDIBSection(surface.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!surface.bitmap || !bits) return false;
    surface.previous = SelectObject(surface.dc, surface.bitmap);
    if (!surface.previous || surface.previous == HGDI_ERROR) return false;
    for (LONG top = visible.top; top < visible.bottom; top += rows) {
        const int height = std::min<LONG>(rows, visible.bottom - top);
        if (!BitBlt(surface.dc, 0, 0, width, height, dc, visible.left, top, SRCCOPY) || !GdiFlush()) return false;
        auto* pixels = static_cast<uint32_t*>(bits);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
            pixels[i] = Blend(pixels[i], color, alpha);
        if (!BitBlt(dc, visible.left, top, width, height, surface.dc, 0, 0, SRCCOPY)) return false;
    }
    return true;
}

} // namespace pdf_marker
