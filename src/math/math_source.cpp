#include "math/math_source.h"

namespace mathrender {
std::wstring NormalizeMarkupSource(std::wstring_view source) {
    std::wstring normalized;
    normalized.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        if (source[i] == L'\\' && i + 1 < source.size()) {
            if (source[i + 1] == L'\\') {
                normalized += L"\\\\";
                i += 2;
                continue;
            }
            if (source[i + 1] == L'\r' || source[i + 1] == L'\n') {
                i += source[i + 1] == L'\r' && i + 2 < source.size() &&
                    source[i + 2] == L'\n' ? 3 : 2;
                continue;
            }
        }
        wchar_t ch = source[i++];
        if (ch >= 0xff01 && ch <= 0xff5e) ch -= 0xfee0;
        normalized += ch;
    }
    std::wstring result;
    result.reserve(normalized.size());
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i + 1 < normalized.size() &&
            ((normalized[i] == L'-' && normalized[i + 1] == L'>') ||
             (normalized[i] == L'\u304b' && normalized[i + 1] == L'\u3089'))) {
            result += L'\u2192';
            ++i;
        } else result += normalized[i];
    }
    return result;
}
} // namespace mathrender
