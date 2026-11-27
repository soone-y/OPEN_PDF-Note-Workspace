#pragma once

#include <windows.h>
#include <utility>

// Small owners for GDI objects used while painting. They delete what they
// created and put back what they replaced on every exit path, so drawing code
// does not need matched Create/Select/Delete calls before each `return`.
namespace ui {

// Owns one GDI object (pen, brush, font, bitmap, region) created by the caller.
template <class Handle>
class ScopedGdiObject final {
public:
    ScopedGdiObject() noexcept = default;
    explicit ScopedGdiObject(Handle handle) noexcept : handle_(handle) {}
    ~ScopedGdiObject() { Reset(); }

    ScopedGdiObject(const ScopedGdiObject&) = delete;
    ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;
    ScopedGdiObject(ScopedGdiObject&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    ScopedGdiObject& operator=(ScopedGdiObject&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] Handle get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    void Reset(Handle handle = nullptr) noexcept {
        if (handle_) (void)DeleteObject(handle_);
        handle_ = handle;
    }

private:
    Handle handle_ = nullptr;
};

using ScopedPen = ScopedGdiObject<HPEN>;
using ScopedBrush = ScopedGdiObject<HBRUSH>;
using ScopedFont = ScopedGdiObject<HFONT>;
using ScopedBitmap = ScopedGdiObject<HBITMAP>;

// Selects an object into a DC and restores the previous one on destruction.
// A null object or a failed selection leaves the DC unchanged.
class ScopedSelectObject final {
public:
    ScopedSelectObject(HDC dc, HGDIOBJ object) noexcept : dc_(dc) {
        if (!dc_ || !object) return;
        HGDIOBJ previous = SelectObject(dc_, object);
        if (previous && previous != HGDI_ERROR) previous_ = previous;
    }
    ~ScopedSelectObject() {
        if (previous_) (void)SelectObject(dc_, previous_);
    }

    ScopedSelectObject(const ScopedSelectObject&) = delete;
    ScopedSelectObject& operator=(const ScopedSelectObject&) = delete;

    [[nodiscard]] bool selected() const noexcept { return previous_ != nullptr; }

private:
    HDC dc_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

// Fills a rectangle with a solid color without creating a brush.
inline void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) noexcept {
    const COLORREF previous = SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (previous != CLR_INVALID) (void)SetDCBrushColor(dc, previous);
}

// Draws a rectangle outline with a solid pen; the interior is left untouched.
inline void FrameSolidRect(HDC dc, const RECT& rect, int width, COLORREF color) noexcept {
    ScopedPen pen(CreatePen(PS_SOLID, width, color));
    if (!pen) return;
    ScopedSelectObject penSelection(dc, pen.get());
    ScopedSelectObject brushSelection(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
}

} // namespace ui
