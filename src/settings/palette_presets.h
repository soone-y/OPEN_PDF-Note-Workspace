#pragma once
#include "core/json_string.h"
#include <windows.h>
#include <array>
#include <optional>
#include <string>

// Six fixed sets: the immutable initial palette plus five optional user sets.
// Duplicate colors are valid within a palette. No file access or application state.
namespace palette_presets {
inline constexpr size_t kSlotCount = 8;
inline constexpr size_t kUserCount = 5;
inline constexpr size_t kMaxBytes = 64 * 1024;
using Colors = std::array<COLORREF, kSlotCount>;
inline constexpr Colors kInitial{{RGB(255,140,0), RGB(255,255,0), RGB(255,0,0), RGB(0,160,255),
    RGB(0,200,0), RGB(200,80,200), RGB(128,128,128), RGB(0,0,0)}};
struct Document { std::array<std::optional<Colors>, kUserCount> users{}; };

[[nodiscard]] inline bool Parse(const std::string& bytes, Document& out) try {
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
    const auto readColors = [&](Colors& colors) {
        if (!take('[')) return false;
        for (size_t i = 0; i < kSlotCount; ++i) {
            if (i && !take(',')) return false;
            skip();
            std::string hex;
            if (!json_string::DecodeToken(bytes, &pos, &hex) || hex.size() != 7 || hex[0] != '#') return false;
            unsigned rgb = 0;
            for (size_t j = 1; j < hex.size(); ++j) {
                const char ch = hex[j];
                const int digit = ch >= '0' && ch <= '9' ? ch - '0'
                    : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                    : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (digit < 0) return false;
                rgb = (rgb << 4) | static_cast<unsigned>(digit);
            }
            colors[i] = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
        }
        return take(']');
    };
    Document next;
    bool version = false, initial = false, users = false;
    if (!take('{')) return false;
    for (;;) {
        skip();
        std::string key;
        if (!json_string::DecodeToken(bytes, &pos, &key) || !take(':')) return false;
        if (key == "version" && !version) {
            if (!take('1')) return false;
            version = true;
        } else if (key == "initial" && !initial) {
            Colors colors{};
            if (!readColors(colors) || colors != kInitial) return false; // Never trust an altered initial set.
            initial = true;
        } else if (key == "users" && !users) {
            if (!take('[')) return false;
            for (size_t i = 0; i < kUserCount; ++i) {
                if (i && !take(',')) return false;
                skip();
                if (bytes.compare(pos, 4, "null") == 0) pos += 4;
                else {
                    Colors colors{};
                    if (!readColors(colors)) return false;
                    next.users[i] = colors;
                }
            }
            if (!take(']')) return false;
            users = true;
        } else return false; // Unknown/duplicate fields would be lost on save.
        skip();
        if (pos < bytes.size() && bytes[pos] == '}') break;
        if (!take(',')) return false;
    }
    if (!take('}') || !version || !initial || !users) return false;
    skip();
    if (pos != bytes.size()) return false;
    out = std::move(next);
    return true;
} catch (...) {
    return false;
}

[[nodiscard]] inline bool Serialize(const Document& doc, std::string& out) try {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string bytes = "{\n  \"version\": 1,\n  \"initial\": ";
    const auto appendColors = [&](const Colors& colors) {
        bytes += '[';
        for (size_t i = 0; i < colors.size(); ++i) {
            const COLORREF color = colors[i];
            if (color & 0xFF000000) return false;
            if (i) bytes += ", ";
            bytes += "\"#";
            for (BYTE channel : {GetRValue(color), GetGValue(color), GetBValue(color)}) {
                bytes += hex[channel >> 4]; bytes += hex[channel & 15];
            }
            bytes += '"';
        }
        bytes += ']';
        return true;
    };
    if (!appendColors(kInitial)) return false;
    bytes += ",\n  \"users\": [\n    ";
    for (size_t i = 0; i < kUserCount; ++i) {
        if (i) bytes += ",\n    ";
        if (doc.users[i]) { if (!appendColors(*doc.users[i])) return false; }
        else bytes += "null";
    }
    bytes += "\n  ]\n}\n";
    out = std::move(bytes);
    return true;
} catch (...) {
    return false;
}
} // namespace palette_presets
