#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

struct ByteRange {
    uintmax_t start = 0;
    uintmax_t end = 0; // Exclusive; offsets always refer to the original file.
};

namespace readonly_viewer::file_range {
inline constexpr uintmax_t kMaximumBytes = 256ull * 1024 * 1024;
inline constexpr uintmax_t kDefaultBytes = 64ull * 1024;

// Pure calculations: no file reads, allocations, UI state or persistence.
[[nodiscard]] inline uintmax_t OffsetAt(uintmax_t size, int position) {
    const auto part = static_cast<uintmax_t>(std::clamp(position, 0, 1000));
    return (size / 1000) * part + (size % 1000) * part / 1000;
}

[[nodiscard]] inline int PositionOf(uintmax_t size, uintmax_t offset) {
    if (!size) return 0;
    offset = std::min(offset, size);
    int low = 0, high = 1000;
    while (low < high) {
        const int middle = (low + high + 1) / 2;
        // ceil(size * middle / 1000), without overflowing the byte offset.
        const uintmax_t threshold = OffsetAt(size, middle) +
            ((size % 1000) * static_cast<uintmax_t>(middle) % 1000 != 0 ? 1 : 0);
        if (threshold <= offset) low = middle;
        else high = middle - 1;
    }
    return low;
}

[[nodiscard]] inline std::optional<uintmax_t> ParseBytes(std::wstring_view text) {
    if (text.empty() || text.size() > 32) return std::nullopt;
    unsigned base = 10;
    if (text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    uintmax_t value = 0;
    for (wchar_t c : text) {
        unsigned digit = 16;
        if (c >= L'0' && c <= L'9') digit = static_cast<unsigned>(c - L'0');
        else if (c >= L'a' && c <= L'f') digit = static_cast<unsigned>(c - L'a') + 10;
        else if (c >= L'A' && c <= L'F') digit = static_cast<unsigned>(c - L'A') + 10;
        if (digit >= base || value > (std::numeric_limits<uintmax_t>::max() - digit) / base)
            return std::nullopt;
        value = value * base + digit;
    }
    return value;
}

[[nodiscard]] inline std::optional<ByteRange> Make(uintmax_t size, uintmax_t start, uintmax_t count) {
    if (!count || start >= size || count > size - start || count > kMaximumBytes)
        return std::nullopt;
    return ByteRange{start, start + count};
}

[[nodiscard]] inline ByteRange Move(uintmax_t size, ByteRange range, bool forward) {
    if (range.end <= range.start || range.end > size) return range;
    const uintmax_t count = range.end - range.start;
    if (forward) {
        if (range.end == size) return range;
        range.start = range.end;
    } else range.start = range.start > count ? range.start - count : 0;
    range.end = range.start + std::min(count, size - range.start);
    return range;
}
} // namespace readonly_viewer::file_range
