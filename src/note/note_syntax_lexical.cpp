#include "note/note_syntax_lexical.h"

#include <cwctype>

namespace note {

bool FindNoteMarkupTagEndingAt(std::wstring_view source, Utf16CodeUnitOffset end, Span* out, bool fullwidth) noexcept {
    const auto open = [fullwidth](wchar_t ch) { return ch == L'<' || (fullwidth && ch == L'\uFF1C'); };
    if (!out || end.value == 0 || end.value > source.size() ||
        (source[end.value - 1] != L'>' && !(fullwidth && source[end.value - 1] == L'\uFF1E'))) return false;
    size_t cursor = end.value - 1;
    while (cursor > 0 && !open(source[cursor - 1]) && source[cursor - 1] != L'\r' &&
           source[cursor - 1] != L'\n') --cursor;
    if (cursor == 0 || !open(source[cursor - 1])) return false;
    *out = {{cursor - 1}, end};
    return true;
}

bool FindNoteMarkupTagStartingAt(std::wstring_view source, Utf16CodeUnitOffset start, Span* out, bool fullwidth) noexcept {
    const auto close = [fullwidth](wchar_t ch) { return ch == L'>' || (fullwidth && ch == L'\uFF1E'); };
    if (!out || start.value >= source.size() ||
        (source[start.value] != L'<' && !(fullwidth && source[start.value] == L'\uFF1C'))) return false;
    size_t end = start.value + 1;
    while (end < source.size() && !close(source[end]) && source[end] != L'\r' && source[end] != L'\n') ++end;
    if (end >= source.size() || !close(source[end])) return false;
    *out = {start, {end + 1}};
    return true;
}

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

Span NoteFenceTitleSpan(std::wstring_view line, bool container) noexcept {
    size_t start = 0;
    size_t end = line.size();
    if (container) {
        NoteContainerFenceRun fence;
        if (!TryParseNoteContainerFenceRun(line, &fence)) return {};
        start = fence.info_span.start.value;
        end = fence.info_span.end.value;
    } else {
        // Standalone fences only; quoted/list prefixes are not rediscovered
        // here. The caller must already own a parser-proven code block.
        while (start < line.size() && line[start] == L' ') ++start;
        if (start > 3 || start == line.size() || (line[start] != L'`' && line[start] != L'~')) return {};
        const wchar_t marker = line[start];
        const size_t first = start;
        while (start < line.size() && line[start] == marker) ++start;
        if (start - first < 3) return {};
        while (start < end && (line[start] == L' ' || line[start] == L'\t')) ++start;
        while (end > start && (line[end - 1] == L' ' || line[end - 1] == L'\t')) --end;
    }
    if (line.substr(start, end - start).find_first_of(L"\r\n") != std::wstring_view::npos) return {};
    Span title{};
    bool explicitTitle = false;
    for (size_t cursor = start; cursor < end;) {
        while (cursor < end && (line[cursor] == L' ' || line[cursor] == L'\t')) ++cursor;
        if (cursor == end) break;
        const bool attribute = line.substr(cursor, 6) == L"title=";
        if (attribute) {
            if (explicitTitle) return {};
            explicitTitle = true;
            cursor += 6;
        }
        const size_t valueStart = cursor;
        if (cursor < end && (line[cursor] == L'\"' || line[cursor] == L'\'')) {
            const wchar_t quote = line[cursor++];
            const size_t body = cursor;
            while (cursor < end && line[cursor] != quote) ++cursor;
            if (cursor == end) return {};
            if (attribute) title = {{body}, {cursor}};
            ++cursor;
            if (cursor < end && line[cursor] != L' ' && line[cursor] != L'\t') return {};
        } else {
            while (cursor < end && line[cursor] != L' ' && line[cursor] != L'\t') ++cursor;
            if (attribute) title = {{valueStart}, {cursor}};
        }
    }
    return explicitTitle ? title : container ? Span{{start}, {end}} : Span{};
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
