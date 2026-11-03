#pragma once

#include <string_view>

// Display-only metadata. PDF/PNG exporters deliberately ignore this choice.
// Inherit is transient while a legacy file's document metadata is still read.
enum class MarkerDisplayMode { Inherit, Legacy, Multiply };

[[nodiscard]] inline const char* MarkerDisplayModeName(MarkerDisplayMode mode) noexcept {
    switch (mode) {
    case MarkerDisplayMode::Legacy: return "legacy";
    case MarkerDisplayMode::Multiply: return "multiply";
    default: return "inherit";
    }
}

[[nodiscard]] inline bool ParseMarkerDisplayMode(std::string_view name, MarkerDisplayMode& mode) noexcept {
    if (name.empty()) mode = MarkerDisplayMode::Inherit;
    else if (name == "legacy") mode = MarkerDisplayMode::Legacy;
    else if (name == "multiply") mode = MarkerDisplayMode::Multiply;
    else return false;
    return true;
}
