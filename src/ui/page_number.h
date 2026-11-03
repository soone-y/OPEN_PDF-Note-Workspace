#pragma once
#include <string_view>

namespace ui {
// One-based page number. Reject incomplete/overflowing input without changing
// the output; callers navigate only after confirmation and successful parsing.
[[nodiscard]] constexpr bool ParsePageNumber(std::wstring_view text, int maximum, int& result) {
    if (text.empty() || text.size() > 10 || maximum < 1) return false;
    int value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        const int digit = character - L'0';
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + digit;
    }
    if (value < 1) return false;
    result = value;
    return true;
}
} // namespace ui
