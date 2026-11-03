#pragma once

#include "core/page_rotation.h"
#include <windows.h>
#include <cstdint>

namespace ui {
// Source must be an owned top-down BGRA DIB. Returns a new top-down DIB.
// GetObject does not preserve the signed creation height. Source is unchanged on
// failure. Quarter turns copy pixels exactly without interpolation.
[[nodiscard]] inline HBITMAP RotateTopDownDibQuarterTurns(HDC reference, HBITMAP source, int turns,
                                                  int* outWidth, int* outHeight) noexcept {
    if (!source || !outWidth || !outHeight) return nullptr;
    GdiFlush(); // GDI may batch writes; CPU pixel reads must observe the completed drawing.
    DIBSECTION section{};
    if (GetObjectW(source, sizeof(section), &section) != sizeof(section) ||
        !section.dsBm.bmBits || section.dsBm.bmBitsPixel != 32) return nullptr;
    const int width = section.dsBm.bmWidth, height = section.dsBm.bmHeight;
    if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * height > 64'000'000) return nullptr;
    const int turn = page_rotation::Normalize(turns);
    const int targetW = turn % 2 ? height : width;
    const int targetH = turn % 2 ? width : height;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = targetW;
    info.bmiHeader.biHeight = -targetH;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(reference, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return nullptr; }
    const auto* input = static_cast<const uint8_t*>(section.dsBm.bmBits);
    auto* output = static_cast<uint32_t*>(bits);
    for (int y = 0; y < height; ++y) {
        const auto* pixels = reinterpret_cast<const uint32_t*>(input + static_cast<size_t>(y) * section.dsBm.bmWidthBytes);
        for (int x = 0; x < width; ++x) {
            int dx = x, dy = y;
            if (turn == 1) { dx = height - 1 - y; dy = x; }
            else if (turn == 2) { dx = width - 1 - x; dy = height - 1 - y; }
            else if (turn == 3) { dx = y; dy = width - 1 - x; }
            output[static_cast<size_t>(dy) * targetW + dx] = pixels[x];
        }
    }
    *outWidth = targetW;
    *outHeight = targetH;
    return bitmap;
}

// Paint in an upright local rectangle, then put those pixels back at the
// requested page angle. GDI alpha blending itself cannot rotate its target.
template<class Draw>
[[nodiscard]] bool DrawQuarterTurnBox(HDC target, const RECT& rectangle, int turns, Draw&& draw) {
    const int turn = page_rotation::Normalize(turns);
    if (turn == 0) { draw(target, rectangle); return true; }
    const int64_t width64 = static_cast<int64_t>(rectangle.right) - rectangle.left;
    const int64_t height64 = static_cast<int64_t>(rectangle.bottom) - rectangle.top;
    if (width64 <= 0 || height64 <= 0 || width64 > 16384 || height64 > 16384 ||
        width64 * height64 > 64'000'000) return false;
    const int width = static_cast<int>(width64), height = static_cast<int>(height64);
    HDC memory = CreateCompatibleDC(target);
    if (!memory) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP snapshot = CreateDIBSection(target, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!snapshot || !bits) { if (snapshot) DeleteObject(snapshot); DeleteDC(memory); return false; }
    auto old = SelectObject(memory, snapshot);
    const bool copied = BitBlt(memory, 0, 0, width, height, target, rectangle.left, rectangle.top, SRCCOPY) != 0;
    SelectObject(memory, old);
    int localW = 0, localH = 0;
    HBITMAP local = copied ? RotateTopDownDibQuarterTurns(target, snapshot, -turn, &localW, &localH) : nullptr;
    DeleteObject(snapshot);
    if (!local) { DeleteDC(memory); return false; }
    old = SelectObject(memory, local);
    bool painted = true;
    try { draw(memory, RECT{0, 0, localW, localH}); }
    catch (...) { painted = false; }
    SelectObject(memory, old);
    int finalW = 0, finalH = 0;
    HBITMAP final = painted ? RotateTopDownDibQuarterTurns(target, local, turn, &finalW, &finalH) : nullptr;
    DeleteObject(local);
    bool result = false;
    if (final) {
        old = SelectObject(memory, final);
        result = BitBlt(target, rectangle.left, rectangle.top, finalW, finalH, memory, 0, 0, SRCCOPY) != 0;
        SelectObject(memory, old);
        DeleteObject(final);
    }
    DeleteDC(memory);
    return result;
}
} // namespace ui
