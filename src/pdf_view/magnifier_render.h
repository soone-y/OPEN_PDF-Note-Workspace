#pragma once

#include "fpdfview.h"
#include "pdf_view/text_coverage.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// Display-only rendering shared by the editor and reader. The caller owns the
// pixels and serializes PDFium access. Only the visible part of a page is
// allocated; the source document and the ordinary page cache stay unchanged.
namespace pdf_magnifier {
inline constexpr int kGuardPixels = 64;
struct Transform {
    double zoom = 1.0;
    double sourceX = 0.0, sourceY = 0.0;
    [[nodiscard]] double X(double x) const { return (x-sourceX)*zoom; }
    [[nodiscard]] double Y(double y) const { return (y-sourceY)*zoom; }
    [[nodiscard]] RECT Page(double x, double y, int width, int height) const {
        const double scaledW = width*zoom, scaledH = height*zoom;
        if (width <= 0 || height <= 0 || !std::isfinite(X(x)) || !std::isfinite(Y(y)) ||
            !std::isfinite(scaledW) || !std::isfinite(scaledH) || scaledW <= 0 || scaledH <= 0 ||
            X(x) < -1'000'000'000 || Y(y) < -1'000'000'000 ||
            X(x)+scaledW > 1'000'000'000 || Y(y)+scaledH > 1'000'000'000) return {};
        const auto left = static_cast<LONG>(std::lround(X(x)));
        const auto top = static_cast<LONG>(std::lround(Y(y)));
        return {left, top, left+static_cast<LONG>(std::lround(width*zoom)),
                           top+static_cast<LONG>(std::lround(height*zoom))};
    }
};

struct Region {
    RECT rect{};
    std::vector<uint8_t> pixels;
};

[[nodiscard]] inline bool Render(FPDF_DOCUMENT document, int index, const RECT& pageRect,
    const RECT& viewport, int rotation, Region& output) {
    const int64_t pageW = static_cast<int64_t>(pageRect.right)-pageRect.left;
    const int64_t pageH = static_cast<int64_t>(pageRect.bottom)-pageRect.top;
    RECT visible{};
    if (!document || index < 0 || rotation < 0 || rotation > 3 || pageW <= 0 || pageH <= 0 ||
        pageW > std::numeric_limits<int>::max() || pageH > std::numeric_limits<int>::max() ||
        !IntersectRect(&visible, &pageRect, &viewport)) return false;
    const int64_t width = static_cast<int64_t>(visible.right)-visible.left;
    const int64_t height = static_cast<int64_t>(visible.bottom)-visible.top;
    // Cap each temporary buffer at 16 MiB, including malformed/oversized input.
    if (width <= 0 || height <= 0 || width > 4'194'304 || height > 4'194'304 ||
        width*height > 4'194'304) return false;
    Region result;
    result.rect = visible;
    // Keep glyph antialiasing away from the bitmap boundary, then crop the
    // guard pixels. PDFium clips glyph coverage at that boundary.
    constexpr int pad = kGuardPixels;
    const int bitmapW = static_cast<int>(width)+pad*2;
    const int bitmapH = static_cast<int>(height)+pad*2;
    if (static_cast<int64_t>(bitmapW)*bitmapH > 4'194'304) return false;
    std::vector<uint8_t> guarded;
    try {
        result.pixels.resize(static_cast<size_t>(width*height)*4);
        guarded.assign(static_cast<size_t>(bitmapW)*bitmapH*4, 255);
    }
    catch (...) { return false; }
    struct Page {
        FPDF_PAGE handle;
        ~Page() { if (handle) FPDF_ClosePage(handle); }
    } page{FPDF_LoadPage(document, index)};
    if (!page.handle) return false;
    struct Bitmap {
        FPDF_BITMAP handle;
        ~Bitmap() { if (handle) FPDFBitmap_Destroy(handle); }
    } bitmap{FPDFBitmap_CreateEx(bitmapW, bitmapH,
        FPDFBitmap_BGRA, guarded.data(), bitmapW*4)};
    if (!bitmap.handle) return false;
    FPDF_RenderPageBitmap(bitmap.handle, page.handle, pageRect.left-visible.left+pad,
        pageRect.top-visible.top+pad, static_cast<int>(pageW), static_cast<int>(pageH),
        rotation, FPDF_ANNOT | FPDF_LCD_TEXT);
    for (int row = 0; row < height; ++row)
        std::copy_n(guarded.data()+(static_cast<size_t>(row+pad)*bitmapW+pad)*4,
            static_cast<size_t>(width)*4, result.pixels.data()+static_cast<size_t>(row)*width*4);
    output = std::move(result);
    return true;
}

struct Coverage {
    RECT rect{};
    std::vector<uint8_t> alpha;
};

[[nodiscard]] inline bool RenderCoverage(FPDF_DOCUMENT document, int index, const RECT& pageRect,
    const RECT& viewport, int rotation, Coverage& output) {
    const int64_t pageW = static_cast<int64_t>(pageRect.right)-pageRect.left;
    const int64_t pageH = static_cast<int64_t>(pageRect.bottom)-pageRect.top;
    RECT visible{};
    if (pageW <= 0 || pageH <= 0 || pageW > 1'000'000'000 || pageH > 1'000'000'000 ||
        !IntersectRect(&visible, &pageRect, &viewport) ||
        visible.left < -1'000'000'000 || visible.top < -1'000'000'000 ||
        visible.right > 1'000'000'000 || visible.bottom > 1'000'000'000) return false;
    const int64_t width = static_cast<int64_t>(visible.right)-visible.left+kGuardPixels*2;
    const int64_t height = static_cast<int64_t>(visible.bottom)-visible.top+kGuardPixels*2;
    if (width > 4'194'304 || height > 4'194'304 || width*height > 4'194'304) return false;
    Coverage result;
    result.rect = {visible.left-kGuardPixels, visible.top-kGuardPixels,
                   visible.right+kGuardPixels, visible.bottom+kGuardPixels};
    if (!pdf_text_coverage::RenderWith(document, index, static_cast<int>(width), static_cast<int>(height), rotation, result.alpha,
        [&](FPDF_BITMAP bitmap, FPDF_PAGE page, int, int, int turn) {
            FPDF_RenderPageBitmap(bitmap, page, pageRect.left-result.rect.left, pageRect.top-result.rect.top,
                pageRect.right-pageRect.left, pageRect.bottom-pageRect.top, turn, 0);
            return true;
        })) return false;
    output = std::move(result);
    return true;
}

inline void Paint(HDC dc, const Region& region) {
    const int width = region.rect.right-region.rect.left, height = region.rect.bottom-region.rect.top;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    StretchDIBits(dc, region.rect.left, region.rect.top, width, height, 0, 0, width, height,
        region.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}
} // namespace pdf_magnifier
