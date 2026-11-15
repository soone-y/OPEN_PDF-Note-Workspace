#pragma once
#include "annotation_rotation.h"
#include <limits>
#include <string_view>

namespace annotation_page_target {
// UI numbers are one-based; model numbers are zero-based. An unchanged legacy
// out-of-range page is retained, so editing other properties never drops it.
[[nodiscard]] inline std::optional<int> Parse(std::wstring_view text, int current, int pageCount) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
    if (text.empty() || text.size() > 10 || pageCount <= 0) return std::nullopt;
    unsigned long long value = 0;
    constexpr auto maximum = static_cast<unsigned long long>(std::numeric_limits<int>::max()) + 1;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + static_cast<unsigned>(c - L'0');
        if (value > maximum) return std::nullopt;
    }
    if (!value) return std::nullopt;
    const int index = static_cast<int>(value - 1);
    if (index != current && index >= pageCount) return std::nullopt;
    return index;
}

// Preserve PDF-space coordinates, content, IDs and styles. A source projection
// belongs to its old page and must be removed before changing page ownership.
[[nodiscard]] inline std::optional<Annotation> Retarget(const Annotation& source, int target, int pageCount) {
    if (target < 0 || pageCount <= 0 || (target != source.pageIndex && target >= pageCount)) return std::nullopt;
    if (target == source.pageIndex) return source;
    auto canonical = annotation_rotation::Canonical(source);
    if (!canonical) return std::nullopt;
    canonical->pageIndex = target;
    return canonical;
}
} // namespace annotation_page_target
