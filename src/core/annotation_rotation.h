#pragma once

#include "core/annot_types.h"
#include "core/page_rotation.h"
#include <optional>

namespace annotation_rotation {

[[nodiscard]] inline bool HasOrientedContent(const Annotation& a) noexcept {
    return a.type == Annotation::Type::TextBox || a.type == Annotation::Type::MathBox || a.type == Annotation::Type::Shape;
}

// A pure boundary: caller supplies the original page extent. All vertices,
// paths and quads are transformed in the same basis. Saving first returns to
// basis zero; viewport changes only create disposable projections.
[[nodiscard]] inline std::optional<Annotation> InBasis(const Annotation& source, int target,
                                                      double width, double height,
                                                      double originX = 0.0, double originY = 0.0,
                                                      bool canonicalTarget = false) {
    const int from = source.projectedCoordinates ? page_rotation::Normalize(source.coordinateRotation) : 0;
    target = page_rotation::Normalize(target);
    if ((from != target || source.projectedCoordinates != !canonicalTarget) &&
        (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0))
        return std::nullopt;
    if (!std::isfinite(originX) || !std::isfinite(originY) ||
        (source.projectedCoordinates && (!std::isfinite(source.basisWidthPt) || !std::isfinite(source.basisHeightPt) ||
         source.basisWidthPt <= 0 || source.basisHeightPt <= 0 || !std::isfinite(source.basisOriginX) ||
         !std::isfinite(source.basisOriginY)))) return std::nullopt;
    Annotation result = source;
    result.coordinateRotation = canonicalTarget ? 0 : target;
    result.projectedCoordinates = !canonicalTarget;
    result.basisWidthPt = width;
    result.basisHeightPt = height;
    result.basisOriginX = originX;
    result.basisOriginY = originY;
    if (from == target && source.projectedCoordinates == result.projectedCoordinates &&
        source.basisOriginX == originX && source.basisOriginY == originY) return result;
    auto point = [&](double& x, double& y) {
        auto p = page_rotation::Point{x, y};
        if (source.projectedCoordinates) {
            p = page_rotation::ToCanonical(p, source.basisWidthPt, source.basisHeightPt, from);
            p.x += source.basisOriginX; p.y += source.basisOriginY;
        }
        if (!canonicalTarget) p = page_rotation::FromCanonical({p.x - originX, p.y - originY}, width, height, target);
        x = p.x; y = p.y;
    };
    point(result.x1, result.y1);
    point(result.x2, result.y2);
    // Retain endpoint identity. Normalizing a box here would change legacy
    // exact journal preconditions even when projection is only temporary.
    for (auto& p : result.path) point(p.x, p.y);
    for (size_t i = 0; i + 1 < result.quads.size(); i += 2) point(result.quads[i], result.quads[i + 1]);
    if (HasOrientedContent(result))
        result.contentRotation = page_rotation::Normalize(source.contentRotation + target - from);
    return result;
}

[[nodiscard]] inline std::optional<Annotation> Canonical(const Annotation& source) {
    if (!source.projectedCoordinates) return source;
    return InBasis(source, 0, source.basisWidthPt, source.basisHeightPt,
                   source.basisOriginX, source.basisOriginY, true);
}

[[nodiscard]] inline page_rotation::Size ContentExtent(const Annotation& a) noexcept {
    return page_rotation::Extent(std::abs(a.x2 - a.x1), std::abs(a.y2 - a.y1), a.contentRotation);
}

[[nodiscard]] inline page_rotation::Point ContentOrigin(const Annotation& a) noexcept {
    const double left = std::min(a.x1, a.x2), right = std::max(a.x1, a.x2);
    const double bottom = std::min(a.y1, a.y2), top = std::max(a.y1, a.y2);
    switch (page_rotation::Normalize(a.contentRotation)) {
    case 1: return {right, top};
    case 2: return {right, bottom};
    case 3: return {left, bottom};
    default: return {left, top};
    }
}

inline void SetContentBounds(Annotation& a, page_rotation::Point origin, double width, double height) noexcept {
    switch (page_rotation::Normalize(a.contentRotation)) {
    case 1: a.x1 = origin.x - height; a.x2 = origin.x; a.y1 = origin.y; a.y2 = origin.y - width; break;
    case 2: a.x1 = origin.x - width; a.x2 = origin.x; a.y1 = origin.y + height; a.y2 = origin.y; break;
    case 3: a.x1 = origin.x; a.x2 = origin.x + height; a.y1 = origin.y + width; a.y2 = origin.y; break;
    default: a.x1 = origin.x; a.x2 = origin.x + width; a.y1 = origin.y; a.y2 = origin.y - height; break;
    }
}
} // namespace annotation_rotation
