#pragma once

#include <cstdint>
#include <string>
#include <utility>

// JSON string conversion only; no file access or application state.
// DecodeToken commits its output and cursor only after a complete valid token.
namespace json_string {

[[nodiscard]] inline std::string Escape(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                escaped += "\\u00";
                escaped.push_back(hex[ch >> 4]);
                escaped.push_back(hex[ch & 15]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
        }
    }
    return escaped;
}

[[nodiscard]] inline bool DecodeToken(const std::string& json, size_t* cursor, std::string* output) {
    if (!cursor || !output || *cursor >= json.size() || json[*cursor] != '"') return false;
    size_t pos = *cursor + 1;
    std::string decoded;
    auto readHex = [&](std::uint32_t* value) -> bool {
        if (json.size() - pos < 4) return false;
        *value = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned char ch = json[pos++];
            const int digit = ch >= '0' && ch <= '9' ? ch - '0'
                : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (digit < 0) return false;
            *value = (*value << 4) | static_cast<std::uint32_t>(digit);
        }
        return true;
    };
    while (pos < json.size()) {
        const unsigned char ch = json[pos++];
        if (ch == '"') {
            *cursor = pos;
            *output = std::move(decoded);
            return true;
        }
        if (ch < 0x20) return false;
        if (ch != '\\') {
            decoded.push_back(static_cast<char>(ch));
            continue;
        }
        if (pos >= json.size()) return false;
        switch (json[pos++]) {
        case '"': decoded += '"'; break;
        case '\\': decoded += '\\'; break;
        case '/': decoded += '/'; break;
        case 'b': decoded += '\b'; break;
        case 'f': decoded += '\f'; break;
        case 'n': decoded += '\n'; break;
        case 'r': decoded += '\r'; break;
        case 't': decoded += '\t'; break;
        case 'u': {
            std::uint32_t code = 0;
            if (!readHex(&code)) return false;
            if (code >= 0xd800 && code <= 0xdbff) {
                if (json.size() - pos < 6 || json[pos] != '\\' || json[pos + 1] != 'u') return false;
                pos += 2;
                std::uint32_t low = 0;
                if (!readHex(&low) || low < 0xdc00 || low > 0xdfff) return false;
                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
            } else if (code >= 0xdc00 && code <= 0xdfff) {
                return false;
            }
            if (code <= 0x7f) decoded.push_back(static_cast<char>(code));
            else {
                if (code > 0xffff) decoded.push_back(static_cast<char>(0xf0 | (code >> 18)));
                if (code > 0x7ff) decoded.push_back(static_cast<char>(
                    (code > 0xffff ? 0x80 : 0xe0) | ((code >> 12) & 0x3f)));
                decoded.push_back(static_cast<char>(
                    (code > 0x7ff ? 0x80 : 0xc0) | ((code >> 6) & 0x3f)));
                decoded.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
            break;
        }
        default: return false;
        }
    }
    return false;
}

} // namespace json_string
