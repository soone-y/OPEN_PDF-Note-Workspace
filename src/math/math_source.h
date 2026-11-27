#pragma once

#include <string>
#include <string_view>

namespace mathrender {
// Legacy shorthand conversion. Preserve paired TeX row separators before
// removing a single backslash used to join physical source lines.
[[nodiscard]] std::wstring NormalizeMarkupSource(std::wstring_view source);
} // namespace mathrender
