#pragma once

#include <algorithm>
#include <cmath>

namespace page_rotation {
struct Point { double x = 0.0; double y = 0.0; };
struct Size { double width = 0.0; double height = 0.0; };

[[nodiscard]] constexpr int Normalize(int turns) noexcept { return ((turns % 4) + 4) % 4; }
[[nodiscard]] constexpr Size Extent(double width, double height, int turns) noexcept {
    return Normalize(turns) % 2 ? Size{height, width} : Size{width, height};
}
// PDF-style, bottom-left origin; positive quarter turns are clockwise.
// Canonical page coordinates never change when the viewport rotates.
[[nodiscard]] constexpr Point FromCanonical(Point p, double width, double height, int turns) noexcept {
    switch (Normalize(turns)) {
    case 1: return {p.y, width - p.x};
    case 2: return {width - p.x, height - p.y};
    case 3: return {height - p.y, p.x};
    default: return p;
    }
}
[[nodiscard]] constexpr Point ToCanonical(Point p, double width, double height, int turns) noexcept {
    const auto extent = Extent(width, height, turns);
    return FromCanonical(p, extent.width, extent.height, -turns);
}
[[nodiscard]] constexpr Point Between(Point p, double width, double height, int from, int to) noexcept {
    return Normalize(from) == Normalize(to) ? p
        : FromCanonical(ToCanonical(p, width, height, from), width, height, to);
}
} // namespace page_rotation
