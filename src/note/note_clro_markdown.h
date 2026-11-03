#pragma once

#include "note/note_model.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace note {

[[nodiscard]] inline bool IsClroMarkdownFile(std::wstring_view name) noexcept {
    if (name.size() < 5) return false;
    const auto suffix = name.substr(name.size() - 5);
    constexpr std::wstring_view expected = L".clro";
    for (size_t i = 0; i < expected.size(); ++i) {
        if (towlower(suffix[i]) != expected[i]) return false;
    }
    return true;
}

// Syntax-only, one UTF-16 unit for one unit. Never fold letters, ordinary
// numbers, currency signs, or a TeX dollar delimiter.
[[nodiscard]] inline wchar_t ClroMarkdownCharacter(wchar_t ch) noexcept {
    if (ch == L'\u3000') return L' ';
    if (ch < L'\uFF01' || ch > L'\uFF5E') return ch;
    const wchar_t ascii = ch - 0xFEE0;
    if ((ascii >= L'0' && ascii <= L'9') || (ascii >= L'A' && ascii <= L'Z') ||
        (ascii >= L'a' && ascii <= L'z') || ascii == L'$') return ch;
    return ascii;
}

// Only style-tag names use this mapping; prose and attribute data keep their
// original letters. Like punctuation folding, it preserves UTF-16 offsets.
[[nodiscard]] inline wchar_t FoldClroMarkupNameLetter(wchar_t ch) noexcept {
    if ((ch >= L'\uFF21' && ch <= L'\uFF3A') || (ch >= L'\uFF41' && ch <= L'\uFF5A')) return ch - 0xFEE0;
    return ch;
}

// Attribute data may contain Japanese (for example a font family). Only
// syntax outside values, including whitespace and quote delimiters, is ASCII.
[[nodiscard]] inline bool IsAsciiClroTagSyntax(std::wstring_view tag) noexcept {
    enum class Part { Name, BeforeValue, Value, Quoted };
    Part part = Part::Name;
    wchar_t quote = 0;
    for (wchar_t ch : tag) {
        if (part == Part::Quoted) {
            if (ch == quote) part = Part::Name;
            continue;
        }
        if (part == Part::Value) {
            if (iswspace(ch)) {
                if (ch > 0x7f) return false;
                part = Part::Name;
            } else if (ch == L',') part = Part::Name;
            continue;
        }
        if (part == Part::BeforeValue) {
            if (iswspace(ch)) { if (ch > 0x7f) return false; continue; }
            if (ch == L'\uFF02' || ch == L'\uFF07') return false;
            if (ch == L'\'' || ch == L'"') { quote = ch; part = Part::Quoted; }
            else part = Part::Value;
            continue;
        }
        if (ch > 0x7f) return false;
        if (ch == L'=') part = Part::BeforeValue;
    }
    return true;
}

[[nodiscard]] inline bool HasClroFullwidthMarkdown(const NoteTextModel& model) noexcept {
    if (model.meta.clro_ascii_only || !IsClroMarkdownFile(model.meta.file_name)) return false;
    enum class TagPart { Name, BeforeValue, Value, QuotedValue };
    bool inTag = false;
    TagPart part = TagPart::Name;
    wchar_t quote = 0;
    for (wchar_t ch : model.raw) {
        if (inTag && ClroMarkdownCharacter(ch) != ch) return true;
        // Only tag syntax can request a projection. Prose punctuation and
        // fullwidth list/task markers never activate Markdown conversion.
        // Known tags and code/math protection are validated by the parser.
        if (inTag && part == TagPart::QuotedValue && ch != L'\r' && ch != L'\n') {
            if (ch == quote) part = TagPart::Name;
            continue;
        }
        if (ch == L'\uFF1C') return true;
        if (ch == L'<' || ch == L'>' || ch == L'\r' || ch == L'\n') {
            inTag = ch == L'<';
            part = TagPart::Name;
            continue;
        }
        if (!inTag) continue;
        if (part == TagPart::BeforeValue) {
            if (iswspace(ch)) continue;
            quote = ch == L'"' || ch == L'\'' ? ch : 0;
            part = quote ? TagPart::QuotedValue : (ch == L',' ? TagPart::Name : TagPart::Value);
        } else if (part == TagPart::Value) {
            if (iswspace(ch) || ch == L',') part = TagPart::Name;
        } else if (ch == L'=') {
            part = TagPart::BeforeValue;
        } else if (FoldClroMarkupNameLetter(ch) != ch) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool ClroRangeContains(const std::vector<Span>& ranges, size_t position) noexcept {
    return std::any_of(ranges.begin(), ranges.end(), [position](Span range) {
        return range.start.value <= position && position < range.end.value;
    });
}

inline void RestoreClroSourceRange(std::wstring_view raw, Span span, std::wstring* syntax) {
    if (!syntax || span.end < span.start || span.end.value > raw.size()) return;
    syntax->replace(span.start.value, span.end - span.start, raw.substr(span.start.value, span.end - span.start));
}

// The parser, not storage, owns this disposable projection. Link destinations
// retain their exact spelling: an IME punctuation mark in a local path must
// never redirect a link to a different file.
[[nodiscard]] inline std::wstring BuildClroMarkdownCharacters(std::wstring_view raw,
                                                            std::vector<Span>* destinations) {
    std::wstring syntax(raw);
    std::transform(syntax.begin(), syntax.end(), syntax.begin(), ClroMarkdownCharacter);
    for (size_t i = 0; i < raw.size(); ++i) {
        if ((raw[i] == L'\uFF3C' && i + 1 < raw.size() &&
             (ClroMarkdownCharacter(raw[i + 1]) == L'(' || ClroMarkdownCharacter(raw[i + 1]) == L'[')) ||
            (i > 0 && raw[i - 1] == L'\\' &&
             (raw[i] == L'\uFF08' || raw[i] == L'\uFF3B'))) syntax[i] = raw[i];
        // Fullwidth angle brackets do not introduce auxiliary tags or math.
        if (raw[i] == L'\uFF1C') syntax[i] = raw[i];
        if (raw[i] >= L'\uFF10' && raw[i] <= L'\uFF19') {
            size_t end = i;
            while (end < raw.size() && ((raw[end] >= L'\uFF10' && raw[end] <= L'\uFF19') ||
                                       (raw[end] >= L'0' && raw[end] <= L'9'))) ++end;
            if (end - i <= 9 && end < raw.size() && (syntax[end] == L'.' || syntax[end] == L')') &&
                end + 1 < raw.size() && (syntax[end + 1] == L' ' || syntax[end + 1] == L'\t')) {
                for (size_t digit = i; digit < end; ++digit) {
                    if (raw[digit] >= L'\uFF10' && raw[digit] <= L'\uFF19') syntax[digit] = raw[digit] - 0xFEE0;
                }
            }
            i = end - 1;
        }
    }
    // Task-box X is syntax only inside the box; body letters stay untouched.
    for (size_t i = 1; i + 1 < syntax.size(); ++i) {
        if (syntax[i - 1] == L'[' && syntax[i + 1] == L']' &&
            (raw[i] == L'\uFF58' || raw[i] == L'\uFF38')) syntax[i] = L'x';
    }
    for (size_t i = 0; i + 1 < syntax.size(); ++i) {
        if (syntax[i] != L']' || (syntax[i + 1] != L'(' && syntax[i + 1] != L':')) continue;
        const bool reference = syntax[i + 1] == L':';
        const size_t start = i + 2;
        size_t end = start;
        size_t depth = 1;
        for (; end < syntax.size(); ++end) {
            if (syntax[end] == L'\r' || syntax[end] == L'\n') break;
            if (!reference && syntax[end] == L'(') ++depth;
            if (!reference && syntax[end] == L')' && --depth == 0) break;
        }
        if (reference || (end < syntax.size() && syntax[end] == L')' && depth == 0)) {
            const Span target{{start}, {end}};
            RestoreClroSourceRange(raw, target, &syntax);
            size_t first = start;
            while (first < end && iswspace(syntax[first])) ++first;
            if (first < end && raw[first] == L'\uFF1C') {
                const size_t close = raw.find(L'\uFF1E', first + 1);
                if (close < end) { syntax[first] = L'<'; syntax[close] = L'>'; }
            }
            size_t last = end;
            while (last > start && iswspace(syntax[last - 1])) --last;
            if (last > start && (raw[last - 1] == L'\uFF02' || raw[last - 1] == L'\uFF07')) {
                const size_t quote = raw.find(raw[last - 1], start);
                if (quote < last - 1 && quote > start && iswspace(raw[quote - 1])) {
                    syntax[quote] = syntax[last - 1] = raw[quote] - 0xFEE0;
                }
            }
            if (destinations) destinations->push_back(target);
            i = end;
        } else {
            // An unfinished destination cannot consume later rows. Avoid
            // rescanning an adversarial row once per unmatched opener.
            i = end;
        }
    }
    for (size_t i = 0; i < syntax.size(); ++i) {
        if ((raw[i] != L'<' && raw[i] != L'\uFF1C') ||
            (destinations && ClroRangeContains(*destinations, i))) continue;
        const size_t end = syntax.find_first_of(L"><\uFF1C\r\n", i + 1);
        if (end == std::wstring::npos) break;
        if (syntax[end] != L'>') continue;
        const auto body = raw.substr(i + 1, end - i - 1);
        if (body.find_first_of(L" \t\r\n") != std::wstring_view::npos ||
            (body.find(L"://") == std::wstring_view::npos && body.find(L'@') == std::wstring_view::npos)) continue;
        const Span target{{i + 1}, {end}};
        RestoreClroSourceRange(raw, target, &syntax);
        syntax[i] = L'<';
        if (destinations) destinations->push_back(target);
        i = end;
    }
    return syntax;
}

// Permissive ATX recognition is used for fullwidth ＃見出し. Hide only ASCII
// no-space hash runs from that parser option; their original visible text and
// every source offset remain intact. U+E001 is confined to this syntax copy.
inline void MaskClroAsciiNoSpaceHeadings(std::wstring_view raw, const std::vector<Span>& protectedRanges,
                                       std::wstring* syntax) {
    for (size_t line = 0; line < syntax->size();) {
        const size_t newline = syntax->find_first_of(L"\r\n", line);
        const size_t end = newline == std::wstring::npos ? syntax->size() : newline;
        size_t i = line;
        line = end < syntax->size() ? end + 1 : end;
        for (;;) {
            while (i < end && ((*syntax)[i] == L' ' || (*syntax)[i] == L'\t')) ++i;
            if (i < end && (*syntax)[i] == L'>') { ++i; continue; }
            if (i + 1 < end && std::wstring_view(L"-*+").find((*syntax)[i]) != std::wstring_view::npos &&
                iswspace((*syntax)[i + 1])) { i += 2; continue; }
            size_t digit = i;
            while (digit < end && (*syntax)[digit] >= L'0' && (*syntax)[digit] <= L'9') ++digit;
            if (digit > i && digit + 1 < end && ((*syntax)[digit] == L'.' || (*syntax)[digit] == L')') &&
                iswspace((*syntax)[digit + 1])) { i = digit + 2; continue; }
            break;
        }
        if (i >= end || (*syntax)[i] != L'#' || ClroRangeContains(protectedRanges, i)) continue;
        const size_t start = i;
        bool fullwidth = false;
        while (i < end && (*syntax)[i] == L'#') {
            fullwidth = fullwidth || raw[i] == L'\uFF03';
            ++i;
        }
        if (!fullwidth && i < end && !iswspace((*syntax)[i])) (*syntax)[start] = L'\uE001';
    }
}

} // namespace note
