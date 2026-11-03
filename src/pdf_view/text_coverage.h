#pragma once

#include <windows.h>
#include "fpdfview.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pdf_text_coverage {

// A display-only, one-byte coverage plane. The caller owns the PDFium lock,
// worker scheduling and cache lifetime. Only an imported memory copy is edited;
// the source document, annotations and disk are never changed. In particular,
// invisible/transparent text and text behind opaque page objects remain usable
// for the application's explicit text-reveal annotation.
inline constexpr uint64_t kMaxPixels = 8ull * 1024 * 1024;

// Keep visible fill/stroke geometry. Clip-only and invisible text need a
// visible fill for reveal, but must not carry their clipping onto other text.
[[nodiscard]] inline FPDF_TEXT_RENDERMODE RevealMode(FPDF_TEXT_RENDERMODE mode) noexcept {
    switch (mode) {
    case FPDF_TEXTRENDERMODE_FILL:
    case FPDF_TEXTRENDERMODE_FILL_CLIP:
    case FPDF_TEXTRENDERMODE_INVISIBLE:
    case FPDF_TEXTRENDERMODE_CLIP: return FPDF_TEXTRENDERMODE_FILL;
    case FPDF_TEXTRENDERMODE_STROKE:
    case FPDF_TEXTRENDERMODE_STROKE_CLIP: return FPDF_TEXTRENDERMODE_STROKE;
    case FPDF_TEXTRENDERMODE_FILL_STROKE:
    case FPDF_TEXTRENDERMODE_FILL_STROKE_CLIP: return FPDF_TEXTRENDERMODE_FILL_STROKE;
    default: return FPDF_TEXTRENDERMODE_UNKNOWN;
    }
}

inline void LimitSize(int& width, int& height) noexcept {
    if (width <= 0 || height <= 0) { width = height = 0; return; }
    const auto pixels = static_cast<uint64_t>(width) * height;
    if (pixels > kMaxPixels) {
        const double factor = std::sqrt(static_cast<double>(kMaxPixels) / pixels);
        width = std::max(1, static_cast<int>(width * factor));
        height = std::max(1, static_cast<int>(height * factor));
    }
}

namespace detail {
struct Document {
    FPDF_DOCUMENT value = FPDF_CreateNewDocument();
    ~Document() { if (value) FPDF_CloseDocument(value); }
};
struct Page {
    FPDF_PAGE value = nullptr;
    ~Page() { if (value) FPDF_ClosePage(value); }
};
struct Bitmap {
    FPDF_BITMAP value = nullptr;
    ~Bitmap() { if (value) FPDFBitmap_Destroy(value); }
};

[[nodiscard]] inline bool PrepareObject(FPDF_PAGEOBJECT object, unsigned depth, size_t& remaining) {
    if (!object || depth > 32 || remaining == 0) return false;
    --remaining;
    const int type = FPDFPageObj_GetType(object);
    if (type == FPDF_PAGEOBJ_TEXT) {
        // Reveal Tr=3, zero alpha and clip-only text, preserving stroke geometry.
        // Do not substitute Unicode into a font: the original encoded glyphs,
        // font resources, text matrices and character spacing stay intact.
        const auto mode = RevealMode(FPDFTextObj_GetTextRenderMode(object));
        if (mode == FPDF_TEXTRENDERMODE_UNKNOWN || !FPDFTextObj_SetTextRenderMode(object, mode) ||
            !FPDFPageObj_SetFillColor(object, 255, 255, 255, 255) ||
            !FPDFPageObj_SetStrokeColor(object, 255, 255, 255, 255)) return false;
        FPDFPageObj_SetBlendMode(object, "Normal");
        return FPDFPageObj_SetIsActive(object, true) != 0;
    }
    if (type == FPDF_PAGEOBJ_FORM) {
        if (!FPDFPageObj_SetFillColor(object, 255, 255, 255, 255) ||
            !FPDFPageObj_SetStrokeColor(object, 255, 255, 255, 255)) return false;
        FPDFPageObj_SetBlendMode(object, "Normal");
        const int count = FPDFFormObj_CountObjects(object);
        if (count < 0) return false;
        for (int i = 0; i < count; ++i)
            if (!PrepareObject(FPDFFormObj_GetObject(object, i), depth + 1, remaining)) return false;
        return FPDFPageObj_SetIsActive(object, true) != 0;
    }
    return FPDFPageObj_SetIsActive(object, false) != 0;
}
} // namespace detail

template<class Draw>
[[nodiscard]] inline bool RenderWith(FPDF_DOCUMENT source, int index, int width, int height,
                                  int rotation, std::vector<uint8_t>& output, Draw draw) noexcept {
    if (!source || index < 0 || index >= FPDF_GetPageCount(source) || width <= 0 || height <= 0 ||
        static_cast<uint64_t>(width) * height > kMaxPixels || rotation < 0 || rotation > 3) return false;
    try {
        detail::Document copy;
        if (!copy.value || !FPDF_ImportPagesByIndex(copy.value, source, &index, 1, 0)) return false;
        detail::Page page{FPDF_LoadPage(copy.value, 0)};
        if (!page.value) return false;
        const int count = FPDFPage_CountObjects(page.value);
        if (count < 0) return false;
        size_t remaining = 200000;
        for (int i = 0; i < count; ++i)
            if (!detail::PrepareObject(FPDFPage_GetObject(page.value, i), 0, remaining)) return false;
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 0);
        detail::Bitmap bitmap{FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, rgba.data(), width * 4)};
        if (!bitmap.value) return false;
        // Grayscale alpha coverage avoids LCD color fringes. No native
        // annotations or opaque page background belong in this plane.
        if (!draw(bitmap.value, page.value, width, height, rotation)) return false;
        std::vector<uint8_t> coverage(static_cast<size_t>(width) * height);
        for (size_t i = 0; i < coverage.size(); ++i) coverage[i] = rgba[i * 4 + 3];
        output = std::move(coverage);
        return true;
    } catch (...) {
        // Worker failure cannot publish partial coverage or escape the thread.
        return false;
    }
}

[[nodiscard]] inline bool Render(FPDF_DOCUMENT source, int index, int width, int height,
                                  int rotation, std::vector<uint8_t>& output) noexcept {
    return RenderWith(source, index, width, height, rotation, output,
        [](FPDF_BITMAP bitmap, FPDF_PAGE page, int w, int h, int rotate) {
            FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, rotate, 0);
            return true;
        });
}

// Blit only the visible annotation rectangle. Work is O(visible pixels), with
// no character-box scan and no PDFium calls during painting.
[[nodiscard]] inline bool Paint(HDC dc, const RECT& annotation, COLORREF color,
                                 const RECT& page, const std::vector<uint8_t>& coverage,
                                 int width, int height) noexcept {
    if (!dc || width <= 0 || height <= 0 || page.right <= page.left || page.bottom <= page.top ||
        coverage.size() != static_cast<size_t>(width) * height) return false;
    RECT clip{}, area{}, visible{};
    if (GetClipBox(dc, &clip) == ERROR) return false;
    if (!IntersectRect(&area, &annotation, &page) || !IntersectRect(&visible, &area, &clip)) return true;
    const int w = visible.right - visible.left, h = visible.bottom - visible.top;
    if (static_cast<uint64_t>(w) * h > kMaxPixels) {
        if (static_cast<uint64_t>(w) > kMaxPixels) return false;
        const int rows = static_cast<int>(kMaxPixels / w);
        for (int y = visible.top; y < visible.bottom; y += rows) {
            const RECT strip{visible.left, y, visible.right, std::min<LONG>(visible.bottom, y + rows)};
            if (!Paint(dc, strip, color, page, coverage, width, height)) return false;
        }
        return true;
    }
    try {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        HDC memory = CreateCompatibleDC(dc);
        if (!memory) return false;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); DeleteDC(memory); return false; }
        auto* pixels = static_cast<uint32_t*>(bits);
        for (int y = 0; y < h; ++y) {
            const int sy = std::clamp(static_cast<int>((static_cast<int64_t>(visible.top + y - page.top) * height) /
                                                      (page.bottom - page.top)), 0, height - 1);
            for (int x = 0; x < w; ++x) {
                const int sx = std::clamp(static_cast<int>((static_cast<int64_t>(visible.left + x - page.left) * width) /
                                                          (page.right - page.left)), 0, width - 1);
                const unsigned a = coverage[static_cast<size_t>(sy) * width + sx];
                pixels[static_cast<size_t>(y) * w + x] = (a << 24) |
                    ((GetRValue(color) * a / 255) << 16) | ((GetGValue(color) * a / 255) << 8) |
                    (GetBValue(color) * a / 255);
            }
        }
        const auto old = SelectObject(memory, bitmap);
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const bool ok = AlphaBlend(dc, visible.left, visible.top, w, h, memory, 0, 0, w, h, blend) != 0;
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
        return ok;
    } catch (...) { return false; }
}

} // namespace pdf_text_coverage
