#include "math/math_render.h"

#include <algorithm>

namespace mathrender {
bool DrawInBox(const Formula& formula, HDC dc, const RECT& bounds, int fontPx,
               COLORREF colour, RenderStyle style) noexcept {
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return false;
    const auto layout = Measure(formula, dc, fontPx, style);
    if (layout.width <= 0 || layout.height <= 0) return false;
    const double scale = std::min({1.0,
        (double(bounds.right) - bounds.left) / layout.width,
        (double(bounds.bottom) - bounds.top) / layout.height});
    const int saved = SaveDC(dc);
    if (!saved) return false;
    bool ok = false;
    if (IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom) != ERROR &&
        SetGraphicsMode(dc, GM_ADVANCED)) {
        // Compose with the caller's PDF zoom/rotation and viewport transform.
        const XFORM transform{float(scale), 0, 0, float(scale),
                              float(bounds.left), float(bounds.top)};
        if (ModifyWorldTransform(dc, &transform, MWT_LEFTMULTIPLY))
            ok = Draw(formula, layout, dc, 0, 0, fontPx, colour, style);
    }
    if (!RestoreDC(dc, saved)) return false;
    return ok;
}
} // namespace mathrender
