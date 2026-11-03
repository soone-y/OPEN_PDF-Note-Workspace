#pragma once

#include <cctype>
#include <cstddef>
#include <string>

// Skip one JSON number token without converting its value. On failure, keep
// the caller's cursor unchanged; the enclosing parser checks the delimiter.
namespace json_number_token {

[[nodiscard]] inline bool Skip(const std::string& json, size_t* pos) {
    if (!pos || *pos >= json.size()) return false;
    size_t i = *pos;
    if (json[i] == '-') ++i;
    if (i >= json.size()) return false;
    if (json[i] == '0') {
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(json[i]))) {
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    } else {
        return false;
    }
    if (i < json.size() && json[i] == '.') {
        ++i;
        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) return false;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    }
    if (i < json.size() && (json[i] == 'e' || json[i] == 'E')) {
        ++i;
        if (i < json.size() && (json[i] == '+' || json[i] == '-')) ++i;
        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) return false;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    }
    *pos = i;
    return true;
}

} // namespace json_number_token
