#pragma once
#include <cwctype>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace lecture_priority {
inline constexpr int initial = 5000;
inline constexpr int maximum = 9999;
using Priorities = std::map<std::wstring, int>;

// Absolute, lexically normalized Windows paths are supplied by the caller.
[[nodiscard]] inline std::wstring PathKey(std::wstring path) {
    for (auto& c : path) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}
[[nodiscard]] inline std::optional<int> Next(const Priorities& managed, const Priorities& external) {
    std::set<int> used;
    for (const auto& entry : managed) used.insert(entry.second);
    for (const auto& entry : external) used.insert(entry.second);
    for (int value = initial; value <= maximum; ++value) if (!used.count(value)) return value;
    return std::nullopt;
}
} // namespace lecture_priority
