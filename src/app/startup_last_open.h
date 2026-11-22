#pragma once

#include <string>
#include <utility>
#include <vector>

// Restart state is independent of per-session file history. An empty path
// means that display was closed, never "choose a file from history".
namespace startup_last_open {
struct Target {
    std::wstring lecturePath;
    std::wstring sessionPath;
    std::wstring pdfPath;
    std::wstring notePath;
};

[[nodiscard]] inline std::wstring Escape(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': result += L"\\\\"; break;
        case L'\t': result += L"\\t"; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        default: result += ch; break;
        }
    }
    return result;
}

[[nodiscard]] inline std::wstring Unescape(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size());
    bool escaped = false;
    for (wchar_t ch : value) {
        if (!escaped) {
            if (ch == L'\\') escaped = true;
            else result += ch;
            continue;
        }
        switch (ch) {
        case L'\\': result += L'\\'; break;
        case L't': result += L'\t'; break;
        case L'n': result += L'\n'; break;
        case L'r': result += L'\r'; break;
        default: result += L'\\'; result += ch; break;
        }
        escaped = false;
    }
    if (escaped) result += L'\\';
    return result;
}

[[nodiscard]] inline std::wstring Serialize(Target target) {
    // Closing both files also clears the directory restore target.
    if (target.pdfPath.empty() && target.notePath.empty()) target = {};
    return L"v2\t" + Escape(target.lecturePath) + L"\t" + Escape(target.sessionPath) +
           L"\t" + Escape(target.pdfPath) + L"\t" + Escape(target.notePath) + L"\n";
}

[[nodiscard]] inline bool Parse(const std::wstring& line, Target* out) {
    if (!out) return false;
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (;;) {
        const size_t end = line.find(L'\t', start);
        fields.push_back(line.substr(start, end == std::wstring::npos ? end : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    // Legacy directory-only records cannot tell closed files from open ones.
    // Leave them untouched, but do not guess a restart target from history.
    if (fields.size() != 5 || fields[0] != L"v2" ||
        line.find_first_of(L"\r\n") != std::wstring::npos || line.find(L'\0') != std::wstring::npos)
        return false;
    Target target{Unescape(fields[1]), Unescape(fields[2]), Unescape(fields[3]), Unescape(fields[4])};
    if (target.pdfPath.empty() && target.notePath.empty()) target = {};
    *out = std::move(target);
    return true;
}
} // namespace startup_last_open
