#pragma once

#include <string>
#include <string_view>

// Pure preference validation shared by config I/O and the existing UI owners.
// Unknown hand-edited values fall back without permitting arbitrary file types.
namespace open_preferences {
[[nodiscard]] inline std::wstring NewNoteExtension(std::wstring_view value) {
    std::wstring lower(value);
    for (auto& ch : lower) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    if (lower == L".md" || lower == L".txt" || lower == L".csv") return lower;
    return L".clro";
}

[[nodiscard]] inline std::wstring PdfViewMode(std::wstring_view value) {
    if (value == L"previous" || value == L"width" || value == L"page") return std::wstring(value);
    return L"auto"; // Preserve the existing flow-based fit and saved position.
}
} // namespace open_preferences
