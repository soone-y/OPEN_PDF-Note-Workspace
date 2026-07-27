#pragma once

#include <string>
#include <string_view>

namespace note {

// Markdown defines a backslash escape only for ASCII punctuation (and line
// breaks). U+00A5 YEN SIGN is deliberately not included: it is ordinary text.
inline bool IsMarkdownEscapableCharacter(wchar_t ch) {
    switch (ch) {
    case L'!': case L'\"': case L'#': case L'$': case L'%': case L'&': case L'\'':
    case L'(': case L')': case L'*': case L'+': case L',': case L'-': case L'.':
    case L'/': case L':': case L';': case L'<': case L'=': case L'>': case L'?':
    case L'@': case L'[': case L'\\': case L']': case L'^': case L'_': case L'`':
    case L'{': case L'|': case L'}': case L'~': case L'\n': case L'\r':
        return true;
    default:
        return false;
    }
}

inline bool IsMarkdownBackslashEscapeAt(std::wstring_view text, size_t pos) {
    return pos + 1 < text.size() && text[pos] == L'\\' &&
           IsMarkdownEscapableCharacter(text[pos + 1]);
}

inline std::wstring DecodeMarkdownBackslashEscapes(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (IsMarkdownBackslashEscapeAt(text, i)) {
            out.push_back(text[i + 1]);
            ++i;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

// Finds a rendered MD4C text callback within raw Markdown while retaining the
// complete source span. This keeps escaped pairs (for example `\\*` and
// `\\\\`) together for selections, links, annotations, and exports.
inline bool MatchMarkdownEscapedTextAt(std::wstring_view raw,
                                       size_t start,
                                       std::wstring_view rendered,
                                       size_t* outEnd) {
    if (!outEnd || start > raw.size()) return false;
    size_t source = start;
    for (wchar_t expected : rendered) {
        if (source >= raw.size()) return false;
        if (IsMarkdownBackslashEscapeAt(raw, source)) {
            if (raw[source + 1] != expected) return false;
            source += 2;
            continue;
        }
        if (raw[source] != expected) return false;
        ++source;
    }
    *outEnd = source;
    return true;
}

} // namespace note
