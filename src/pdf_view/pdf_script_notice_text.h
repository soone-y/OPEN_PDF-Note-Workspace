#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace pdf_script_notice {
// PDFium's lengths are bytes of UTF-16LE, including the terminator.
// A failed/inconsistent read publishes no partial text. Nothing is evaluated.
template <typename Reader>
[[nodiscard]] std::wstring Read(Reader reader) {
    constexpr unsigned long maxBytes = 1024 * 1024;
    const auto bytes = reader(nullptr, 0);
    if (bytes < 2 || bytes > maxBytes || bytes % 2 != 0) return {};
    std::vector<unsigned short> buffer(bytes / 2 + 1, 0xffff);
    const auto capacity = static_cast<unsigned long>(buffer.size() * 2);
    if (reader(buffer.data(), capacity) != bytes || buffer[bytes / 2 - 1] != 0) return {};
    return std::wstring(buffer.begin(), buffer.begin() + bytes / 2 - 1);
}

// Preserve visible text; make control/bidirectional characters visible rather
// than allowing document content to impersonate the surrounding notice.
[[nodiscard]] inline std::wstring Preview(const std::wstring& text, size_t limit) {
    std::wstring result;
    size_t count = std::min(text.size(), limit);
    if (count > 0 && count < text.size() && text[count - 1] >= 0xd800 &&
        text[count - 1] <= 0xdbff && text[count] >= 0xdc00 && text[count] <= 0xdfff)
        --count; // Do not split a UTF-16 surrogate pair in a preview.
    for (size_t i = 0; i < count; ++i) {
        const unsigned c = static_cast<unsigned>(text[i]);
        const bool pairedSurrogate =
            (c >= 0xd800 && c <= 0xdbff && i + 1 < count &&
             text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) ||
            (c >= 0xdc00 && c <= 0xdfff && i > 0 &&
             text[i - 1] >= 0xd800 && text[i - 1] <= 0xdbff);
        if ((c >= 0xd800 && c <= 0xdfff && !pairedSurrogate) || c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x061c ||
            c == 0x200e || c == 0x200f || (c >= 0x2028 && c <= 0x202e) ||
            (c >= 0x2066 && c <= 0x2069)) {
            result += L"\\u";
            for (int shift = 12; shift >= 0; shift -= 4)
                result += L"0123456789ABCDEF"[(c >> shift) & 15];
        } else {
            result += text[i];
        }
    }
    if (text.size() > count) result += L"…";
    return result;
}
} // namespace pdf_script_notice
