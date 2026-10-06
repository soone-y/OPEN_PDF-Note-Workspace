#include "note/note_syntax_lexical.h"

#include <cwctype>

namespace note {

namespace {
[[nodiscard]] bool MathTokenEquals(std::wstring_view text, std::wstring_view token) noexcept {
    if (text.size() != token.size()) return false;
    for (size_t index = 0; index < token.size(); ++index) {
        wchar_t ch = text[index];
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        if (ch != token[index]) return false;
    }
    return true;
}
[[nodiscard]] bool MathAttributeNameChar(wchar_t ch) noexcept {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
        (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'-' || ch == L':' || ch == L'.';
}
} // namespace

size_t NoteMathCloseTagLength(std::wstring_view suffix) noexcept {
    if (suffix.size() >= 3 && suffix.substr(0, 3) == L"</>") return 3;
    return suffix.size() >= 7 && MathTokenEquals(suffix.substr(0, 7), L"</math>") ? 7 : 0;
}

bool TryParseNoteMathOpenTag(std::wstring_view raw, Utf16CodeUnitOffset start,
                             NoteMathOpenTag* out) noexcept {
    if (!out || start.value > raw.size() || raw.size() - start.value < 5 ||
        raw[start.value] != L'<' || !MathTokenEquals(raw.substr(start.value + 1, 4), L"math")) return false;
    size_t cursor = start.value + 5;
    if (cursor < raw.size() && raw[cursor] != L'>' && raw[cursor] != L' ' && raw[cursor] != L'\t' &&
        raw[cursor] != L'\r' && raw[cursor] != L'\n' && raw[cursor] != L'/') return false;
    NoteMathOpenTag tag;
    bool hasDisplay = false;
    while (cursor < raw.size()) {
        const size_t beforeSpace = cursor;
        while (cursor < raw.size() && (raw[cursor] == L' ' || raw[cursor] == L'\t')) ++cursor;
        if (cursor == raw.size() || raw[cursor] == L'\r' || raw[cursor] == L'\n' || raw[cursor] == L'<') break;
        if (raw[cursor] == L'>') {
            tag.complete = true;
            ++cursor;
            break;
        }
        if (raw[cursor] == L'/' && cursor + 1 < raw.size() && raw[cursor + 1] == L'>') {
            tag.complete = true;
            tag.self_closing = true;
            cursor += 2;
            break;
        }
        if (cursor == beforeSpace) tag.valid_attributes = false;
        const size_t nameStart = cursor;
        while (cursor < raw.size() && MathAttributeNameChar(raw[cursor])) ++cursor;
        if (cursor == nameStart) {
            tag.valid_attributes = false;
            ++cursor;
            continue;
        }
        const auto name = raw.substr(nameStart, cursor - nameStart);
        const size_t nameEnd = cursor;
        while (cursor < raw.size() && (raw[cursor] == L' ' || raw[cursor] == L'\t')) ++cursor;
        std::wstring_view value;
        bool hasValue = false;
        if (cursor < raw.size() && raw[cursor] == L'=') {
            hasValue = true;
            ++cursor;
            while (cursor < raw.size() && (raw[cursor] == L' ' || raw[cursor] == L'\t')) ++cursor;
            if (cursor < raw.size() && (raw[cursor] == L'\'' || raw[cursor] == L'"')) {
                const wchar_t quote = raw[cursor++];
                const size_t first = cursor;
                while (cursor < raw.size() && raw[cursor] != quote && raw[cursor] != L'\r' && raw[cursor] != L'\n') ++cursor;
                if (cursor == raw.size() || raw[cursor] != quote) break;
                value = raw.substr(first, cursor - first);
                ++cursor;
            } else {
                const size_t first = cursor;
                while (cursor < raw.size() && raw[cursor] != L' ' && raw[cursor] != L'\t' &&
                       raw[cursor] != L'\r' && raw[cursor] != L'\n' && raw[cursor] != L'>' && raw[cursor] != L'<') {
                    if (raw[cursor] == L'/' && cursor + 1 < raw.size() && raw[cursor + 1] == L'>') break;
                    ++cursor;
                }
                value = raw.substr(first, cursor - first);
                if (value.empty() || value.find_first_of(L"\"'=`") != std::wstring_view::npos) tag.valid_attributes = false;
            }
        } else {
            cursor = nameEnd; // Let the next token prove its whitespace separator.
        }
        if (MathTokenEquals(name, L"display")) {
            if (hasDisplay || !hasValue) tag.valid_attributes = false;
            hasDisplay = true;
            if (MathTokenEquals(value, L"inline")) tag.display = NoteMathTagDisplay::Inline;
            else if (MathTokenEquals(value, L"block")) tag.display = NoteMathTagDisplay::Block;
            else tag.valid_attributes = false;
        } else {
            tag.unknown_attributes = true;
        }
    }
    tag.end = {cursor};
    *out = tag;
    return true;
}

bool TryParseNoteContainerFenceRun(std::wstring_view line,
                                   NoteContainerFenceRun* out) noexcept {
    if (!out) return false;
    size_t first = 0;
    while (first < line.size() && line[first] == L' ') ++first;
    if (first > 3 || first >= line.size() || line[first] != L':') return false;
    size_t end = first;
    while (end < line.size() && line[end] == L':') ++end;
    if (end - first < 3 ||
        (end < line.size() && line[end] != L' ' && line[end] != L'\t')) return false;
    const size_t count = end - first;
    while (end < line.size() && (line[end] == L' ' || line[end] == L'\t')) ++end;
    size_t infoEnd = line.size();
    while (infoEnd > end && (line[infoEnd - 1] == L' ' || line[infoEnd - 1] == L'\t')) --infoEnd;
    *out = {count, {{end}, {infoEnd}}};
    return true;
}

bool TryParseNoteCodeFenceRun(std::wstring_view raw,
                              Utf16CodeUnitOffset line_start,
                              Utf16CodeUnitOffset line_end,
                              bool closing_only,
                              NoteCodeFenceRun* out_run) noexcept {
    if (!out_run || line_start.value > line_end.value || line_end.value > raw.size()) {
        return false;
    }
    size_t start = line_start.value;
    while (start < line_end.value && (raw[start] == L' ' || raw[start] == L'\t')) {
        ++start;
    }
    if (start >= line_end.value) return false;
    const wchar_t marker = raw[start];
    if (marker != L'`' && marker != L'~') return false;

    size_t count = 0;
    while (start + count < line_end.value && raw[start + count] == marker) ++count;
    if (count < 3) return false;
    if (closing_only) {
        for (size_t cursor = start + count; cursor < line_end.value; ++cursor) {
            if (!std::iswspace(raw[cursor])) return false;
        }
    }
    *out_run = {marker, count};
    return true;
}

bool IsNoteBoundedLookBehindDelimiterLine(std::wstring_view line) noexcept {
    size_t dash_count = 0;
    size_t equal_count = 0;
    bool has_pipe = false;
    for (const wchar_t ch : line) {
        switch (ch) {
        case L'-': ++dash_count; break;
        case L'=': ++equal_count; break;
        case L'|': has_pipe = true; break;
        case L':':
        case L' ':
        case L'\t':
        case L'\r':
        case L'\n':
            break;
        default:
            return false;
        }
    }
    return has_pipe ? dash_count > 0 : (dash_count >= 3 || equal_count >= 3);
}

} // namespace note
