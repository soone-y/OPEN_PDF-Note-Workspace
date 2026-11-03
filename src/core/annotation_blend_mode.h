#pragma once
#include <string_view>

// v2 display metadata for shapes and free markers. Missing metadata preserves
// the normal translucent rendering used by existing files of either version.
enum class AnnotationBlendMode { Normal, Multiply };
[[nodiscard]] inline const char* AnnotationBlendModeName(AnnotationBlendMode mode) noexcept {
    switch(mode) {
    case AnnotationBlendMode::Normal: return "normal";
    case AnnotationBlendMode::Multiply: return "multiply";
    default: return "invalid";
    }
}
[[nodiscard]] inline bool ParseAnnotationBlendMode(std::string_view name, AnnotationBlendMode& mode) noexcept {
    if (name == "normal") mode = AnnotationBlendMode::Normal;
    else if (name == "multiply") mode = AnnotationBlendMode::Multiply;
    else return false;
    return true;
}
