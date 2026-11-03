#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure, bounded suffix codec for pdf_view_positions.txt. Each digit is the
// current clockwise quarter-turn state, not an accumulated operation count.
namespace view_rotation_record {
inline constexpr std::size_t kMaxPages = 1000000;
[[nodiscard]] inline std::optional<std::string> Encode(const std::vector<int>& turns) try {
    if (turns.size() > kMaxPages) return std::nullopt;
    std::string text = "R1:";
    for (int turn : turns) {
        if (turn < 0 || turn > 3) return std::nullopt;
        text += static_cast<char>('0' + turn);
    }
    return text;
} catch (...) {
    return std::nullopt;
}
[[nodiscard]] inline bool Decode(std::string_view text, std::vector<int>& out) try {
    if (text.size() < 3 || text.substr(0, 3) != "R1:" || text.size() - 3 > kMaxPages) return false;
    for (std::size_t i = 3; i < text.size(); ++i) if (text[i] < '0' || text[i] > '3') return false;
    std::vector<int> parsed;
    parsed.reserve(text.size() - 3);
    for (std::size_t i = 3; i < text.size(); ++i) parsed.push_back(text[i] - '0');
    out = std::move(parsed);
    return true;
} catch (...) {
    return false;
}
} // namespace view_rotation_record
