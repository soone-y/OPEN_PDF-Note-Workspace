#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

// Pure color-space boundary: hue in 0..3599 (tenths of a degree), S/V in
// 0..1000. No UI, persistence, or application state. Gray retains chosen hue.
namespace palette_color {
struct Hsv { int h = 0, s = 1000, v = 1000; };
[[nodiscard]] inline COLORREF ToRgb(Hsv hsv) noexcept {
    const double h = std::clamp(hsv.h, 0, 3599) / 600.0;
    const double s = std::clamp(hsv.s, 0, 1000) / 1000.0;
    const double v = std::clamp(hsv.v, 0, 1000) / 1000.0;
    const int sector = static_cast<int>(h);
    const double f = h - sector;
    const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double r = v, g = t, b = p;
    switch (sector) {
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    default: break;
    }
    return RGB(static_cast<int>(std::lround(r * 255)), static_cast<int>(std::lround(g * 255)),
               static_cast<int>(std::lround(b * 255)));
}
[[nodiscard]] inline Hsv FromRgb(COLORREF rgb, int previousHue = 0) noexcept {
    const double r = GetRValue(rgb) / 255.0, g = GetGValue(rgb) / 255.0, b = GetBValue(rgb) / 255.0;
    const double high = std::max({r, g, b}), low = std::min({r, g, b}), delta = high - low;
    Hsv hsv{std::clamp(previousHue, 0, 3599), 0, static_cast<int>(std::lround(high * 1000))};
    if (delta > 0) {
        double h = high == r ? (g - b) / delta : high == g ? (b - r) / delta + 2 : (r - g) / delta + 4;
        if (h < 0) h += 6;
        hsv.h = static_cast<int>(std::lround(h * 600)) % 3600;
        hsv.s = static_cast<int>(std::lround(delta / high * 1000));
    }
    return hsv;
}
[[nodiscard]] inline int Position(int pixel, int extent, int maximum) noexcept {
    return extent <= 1 ? 0 : MulDiv(std::clamp(pixel, 0, extent - 1), maximum, extent - 1);
}
} // namespace palette_color
