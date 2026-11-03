#pragma once
#include "clrop/types.h"
#include "core/page_rotation.h"

namespace clrop {
// Disposable projection of PDF-native coordinates; the stored document is never modified.
inline Item ProjectItem(const Item& source, double width, double height, int turns,
                        double originX = 0, double originY = 0) {
    Item result = source;
    auto point = [&](double x, double y) {
        return page_rotation::FromCanonical({x - originX, y - originY}, width, height, turns);
    };
    if (source.bbox) {
        const auto& b = *source.bbox;
        const auto a = point(b[0], b[1]), z = point(b[0] + b[2], b[1] - b[3]);
        result.bbox = std::array<double, 4>{std::min(a.x,z.x), std::max(a.y,z.y), std::abs(z.x-a.x), std::abs(z.y-a.y)};
    }
    if (source.bboxEndpoints) {
        const auto& b = *source.bboxEndpoints;
        const auto a = point(b[0],b[1]), z = point(b[2],b[3]);
        result.bboxEndpoints = std::array<double,4>{a.x,a.y,z.x,z.y};
    }
    for (auto* value : {&result.p1, &result.p2}) if (*value) {
        const auto p = point((**value)[0],(**value)[1]); **value = {p.x,p.y};
    }
    for (auto* values : {&result.path, &result.quads})
        for (size_t i=0; i+1<values->size(); i+=2) {
            const auto p = point((*values)[i],(*values)[i+1]); (*values)[i]=p.x; (*values)[i+1]=p.y;
        }
    if (source.kind == Item::Kind::Text || source.kind == Item::Kind::Math || source.kind == Item::Kind::Shape)
        result.contentRotation = page_rotation::Normalize(source.contentRotation / 90 + turns) * 90;
    return result;
}
} // namespace clrop
