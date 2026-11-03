#pragma once
#include <cstdint>
#include <string_view>

// v2 shape metadata. Omission keeps existing same-color shape rendering.
enum class ShapeBoundary { None, Black, White };
// UI choices combine the existing v2 boundary and dash fields.
enum class ShapeLineStyle { Solid, Dashed, BlackBoundary, WhiteBoundary };
[[nodiscard]] inline ShapeLineStyle ShapeLineStyleFor(ShapeBoundary boundary,bool dashed) noexcept {
    if(boundary==ShapeBoundary::Black)return ShapeLineStyle::BlackBoundary;
    if(boundary==ShapeBoundary::White)return ShapeLineStyle::WhiteBoundary;
    return dashed ? ShapeLineStyle::Dashed : ShapeLineStyle::Solid;
}
[[nodiscard]] inline ShapeBoundary BoundaryForLineStyle(ShapeLineStyle style) noexcept {
    return style==ShapeLineStyle::BlackBoundary ? ShapeBoundary::Black :
        style==ShapeLineStyle::WhiteBoundary ? ShapeBoundary::White : ShapeBoundary::None;
}
[[nodiscard]] inline const char* ShapeBoundaryName(ShapeBoundary value) noexcept {
    switch (value) {
    case ShapeBoundary::None: return "none";
    case ShapeBoundary::Black: return "black";
    case ShapeBoundary::White: return "white";
    default: return "invalid";
    }
}
[[nodiscard]] inline bool ParseShapeBoundary(std::string_view name, ShapeBoundary& value) noexcept {
    if (name == "none") value = ShapeBoundary::None;
    else if (name == "black") value = ShapeBoundary::Black;
    else if (name == "white") value = ShapeBoundary::White;
    else return false;
    return true;
}
[[nodiscard]] inline std::uint32_t ShapeBoundaryColor(ShapeBoundary value) noexcept {
    return value == ShapeBoundary::White ? 0x00ffffffu : 0;
}
