#pragma once
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace readonly_viewer::byte_dump {
inline constexpr size_t kMaximumPreviewBytes = 65536;
enum class Format { Hex, Binary };

// Pure bounded formatting of already loaded bytes. Neither the bytes nor the
// original file are changed; every address is relative to the original file.
[[nodiscard]] inline std::wstring Rows(const std::vector<unsigned char>& bytes,
                                      uintmax_t base, Format format) {
    const size_t count = std::min(bytes.size(), kMaximumPreviewBytes);
    if (count && count - 1 > std::numeric_limits<uintmax_t>::max() - base) return {};
    const bool binary = format == Format::Binary;
    const size_t per_row = binary ? 8 : 16;
    std::wostringstream output;
    for (size_t i = 0; i < count; i += per_row) {
        output << std::hex << std::setw(8) << std::setfill(L'0') << base + i << L"  ";
        for (size_t j = 0; j < per_row; ++j) {
            if (i + j >= count) output << (binary ? L"         " : L"   ");
            else if (binary) {
                for (int bit = 7; bit >= 0; --bit)
                    output << static_cast<wchar_t>(L'0' + ((bytes[i + j] >> bit) & 1));
                output << L' ';
            } else output << std::setw(2) << static_cast<unsigned>(bytes[i + j]) << L' ';
            if (j == per_row / 2 - 1) output << L' ';
        }
        output << L" |";
        for (size_t j = 0; j < per_row && i + j < count; ++j) {
            const unsigned char c = bytes[i + j];
            output << (c >= 32 && c <= 126 ? static_cast<wchar_t>(c) : L'.');
        }
        output << L"|\n";
    }
    return output.str();
}
} // namespace readonly_viewer::byte_dump
