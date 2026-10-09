#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace note {

// Pure syntax generation. The UI owns selection, IME, history and insertion;
// this boundary never reads globals or changes the document.
struct InputAssistFormat {
    int heading_level = 0;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    std::vector<std::wstring> attributes;

    [[nodiscard]] bool has_formatting() const noexcept {
        return heading_level > 0 || bold || italic || strike || !attributes.empty();
    }
};

[[nodiscard]] inline std::wstring BuildInputAssistSnippet(
    std::wstring_view body, const InputAssistFormat& format) {
    std::wstring opening;
    if (!format.attributes.empty()) {
        opening = L"<";
        for (const auto& attribute : format.attributes) {
            if (opening.size() > 1) opening += L", ";
            opening += attribute;
        }
        opening += L">";
    }
    std::wstring delimiter;
    if (format.strike) delimiter += L"~~";
    if (format.bold) delimiter += L"**";
    if (format.italic) delimiter += L"*";
    std::wstring closing;
    if (format.italic) closing += L"*";
    if (format.bold) closing += L"**";
    if (format.strike) closing += L"~~";

    std::wstring result;
    size_t start = 0;
    while (start < body.size()) {
        const size_t found = body.find_first_of(L"\r\n", start);
        const size_t end = found == std::wstring_view::npos ? body.size() : found;
        size_t first = start;
        size_t last = end;
        const auto space = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\x3000'; };
        while (first < last && space(body[first])) ++first;
        while (last > first && space(body[last - 1])) --last;
        if (first < last && format.heading_level > 0) {
            result.append(static_cast<size_t>(std::clamp(format.heading_level, 1, 6)), L'#');
            result += L' ';
        }
        result.append(body.substr(start, first - start));
        if (first < last) {
            result += opening + delimiter;
            result.append(body.substr(first, last - first));
            result += closing;
            if (!opening.empty()) result += L"</>";
        }
        result.append(body.substr(last, end - last));
        if (found == std::wstring_view::npos) break;
        result += body[found];
        start = found + 1;
        if (body[found] == L'\r' && start < body.size() && body[start] == L'\n') {
            result += body[start++];
        }
    }
    return result;
}

// Block snippets must remain separate from the surrounding paragraph. Read
// only the adjacent native characters in the UI; no full-note copy is needed.
[[nodiscard]] inline std::wstring FrameInputAssistBlock(
    std::wstring text, wchar_t before, wchar_t after) {
    const auto boundary = [](wchar_t ch) { return ch == 0 || ch == L'\r' || ch == L'\n'; };
    if (!boundary(before)) text.insert(0, L"\r\n");
    if (!boundary(after) && !text.empty() && !boundary(text.back())) text += L"\r\n";
    return text;
}

} // namespace note
