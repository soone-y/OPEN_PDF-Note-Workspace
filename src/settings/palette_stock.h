#pragma once
#include "core/json_string.h"
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

// Pure stock format: no file access, globals or UI. Reject unsupported fields
// instead of silently losing them on a subsequent save. Failed parsing keeps out.
namespace palette_stock {
inline constexpr size_t kMaxColors = 128;
inline constexpr size_t kMaxBytes = 64 * 1024;

[[nodiscard]] inline bool ParseColors(const std::string& bytes, std::vector<COLORREF>& out, bool active) {
    if (bytes.size() > kMaxBytes) return false;
    size_t pos = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    const auto skip = [&]() {
        while (pos < bytes.size() && (bytes[pos] == ' ' || bytes[pos] == '\t' ||
               bytes[pos] == '\r' || bytes[pos] == '\n')) ++pos;
    };
    const auto take = [&](char ch) {
        skip();
        if (pos == bytes.size() || bytes[pos] != ch) return false;
        ++pos;
        return true;
    };
    std::string key;
    if (!take('{')) return false;
    skip();
    if (!json_string::DecodeToken(bytes, &pos, &key) || key != "colors" || !take(':') || !take('[')) return false;
    std::vector<COLORREF> colors;
    skip();
    if (pos < bytes.size() && bytes[pos] != ']') {
        for (;;) {
            skip();
            std::string hex;
            if (!json_string::DecodeToken(bytes, &pos, &hex)) return false;
            if (active && hex.size() == 6) hex.insert(hex.begin(), '#'); // Legacy active palettes.
            if (hex.size() != 7 || hex[0] != '#') return false;
            unsigned rgb = 0;
            for (size_t i = 1; i < hex.size(); ++i) {
                const char ch = hex[i];
                const int digit = ch >= '0' && ch <= '9' ? ch - '0'
                    : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                    : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (digit < 0) return false;
                rgb = (rgb << 4) | static_cast<unsigned>(digit);
            }
            const COLORREF color = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
            if (colors.size() == (active ? 24 : kMaxColors) ||
                (!active && std::find(colors.begin(), colors.end(), color) != colors.end())) return false;
            colors.push_back(color);
            skip();
            if (pos < bytes.size() && bytes[pos] == ']') break;
            if (!take(',')) return false;
        }
    }
    if (!take(']') || !take('}')) return false;
    skip();
    if (pos != bytes.size() || (active && colors.empty())) return false;
    out = std::move(colors);
    return true;
}

[[nodiscard]] inline bool SerializeColors(const std::vector<COLORREF>& colors, std::string& out, bool active) {
    if (colors.size() > (active ? 24 : kMaxColors) || (active && colors.empty())) return false;
    constexpr char hex[] = "0123456789ABCDEF";
    std::string bytes = "{\n  \"colors\": [";
    for (size_t i = 0; i < colors.size(); ++i) {
        const COLORREF color = colors[i];
        if ((color & 0xFF000000) != 0 || (!active && std::find(colors.begin(), colors.begin() + i, color) != colors.begin() + i)) return false;
        if (i) bytes += ", ";
        bytes += "\"#";
        for (BYTE channel : {GetRValue(color), GetGValue(color), GetBValue(color)}) {
            bytes += hex[channel >> 4];
            bytes += hex[channel & 15];
        }
        bytes += '"';
    }
    bytes += "]\n}\n";
    out = std::move(bytes);
    return true;
}
[[nodiscard]] inline bool Parse(const std::string& bytes, std::vector<COLORREF>& out) { return ParseColors(bytes, out, false); }
[[nodiscard]] inline bool Serialize(const std::vector<COLORREF>& colors, std::string& out) { return SerializeColors(colors, out, false); }
} // namespace palette_stock
