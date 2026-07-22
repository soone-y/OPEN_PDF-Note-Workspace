#include "note/note_text_boundaries.h"

#include <algorithm>

namespace note {
namespace {

bool IsHigh(wchar_t ch) { return ch >= 0xD800 && ch <= 0xDBFF; }
bool IsLow(wchar_t ch) { return ch >= 0xDC00 && ch <= 0xDFFF; }
bool IsCombining(wchar_t ch) {
    return (ch >= 0x0300 && ch <= 0x036F) || (ch >= 0x1AB0 && ch <= 0x1AFF) ||
           (ch >= 0x1DC0 && ch <= 0x1DFF) || (ch >= 0x20D0 && ch <= 0x20FF) ||
           (ch >= 0xFE20 && ch <= 0xFE2F);
}
bool IsVariationSelector16(wchar_t ch) { return ch >= 0xFE00 && ch <= 0xFE0F; }
bool IsSupplementaryVariationSelector(std::wstring_view text, size_t pos) {
    if (pos + 1 >= text.size() || !IsHigh(text[pos]) || !IsLow(text[pos + 1])) return false;
    const unsigned scalar = 0x10000u + ((static_cast<unsigned>(text[pos]) - 0xD800u) << 10) +
                            (static_cast<unsigned>(text[pos + 1]) - 0xDC00u);
    return scalar >= 0xE0100u && scalar <= 0xE01EFu;
}

void ConsumeExtenders(std::wstring_view text, size_t* end) {
    while (*end < text.size()) {
        if (IsCombining(text[*end]) || IsVariationSelector16(text[*end])) {
            ++*end;
        } else if (IsSupplementaryVariationSelector(text, *end)) {
            *end += 2;
        } else {
            break;
        }
    }
}

TextUnitClass Classify(wchar_t ch) {
    if (ch == L' ' || ch == L'\t' || ch == 0x3000) return TextUnitClass::Whitespace;
    if ((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
        ch == L'_' || (ch >= 0xFF10 && ch <= 0xFF19) ||
        (ch >= 0xFF21 && ch <= 0xFF3A) || (ch >= 0xFF41 && ch <= 0xFF5A)) return TextUnitClass::LatinNumber;
    if (ch >= 0x3040 && ch <= 0x309F) return TextUnitClass::Hiragana;
    if ((ch >= 0x30A0 && ch <= 0x30FF) || (ch >= 0xFF66 && ch <= 0xFF9F)) return TextUnitClass::Katakana;
    if (ch >= 0x3400 && ch <= 0x9FFF) return TextUnitClass::Kanji;
    if ((ch >= 0x2000 && ch <= 0x206F) || (ch >= 0x3000 && ch <= 0x303F) ||
        (ch >= 0xFF00 && ch <= 0xFF65) || (ch >= 0x21 && ch <= 0x2F) ||
        (ch >= 0x3A && ch <= 0x40) || (ch >= 0x5B && ch <= 0x60) ||
        (ch >= 0x7B && ch <= 0x7E)) return TextUnitClass::Punctuation;
    return TextUnitClass::Other;
}

size_t NextUnitEnd(std::wstring_view text, size_t start) {
    if (start >= text.size()) return start;
    if (text[start] == L'\r') return (start + 1 < text.size() && text[start + 1] == L'\n') ? start + 2 : start + 1;
    if (text[start] == L'\n') return start + 1;
    size_t end = start + ((IsHigh(text[start]) && start + 1 < text.size() && IsLow(text[start + 1])) ? 2 : 1);
    ConsumeExtenders(text, &end);
    while (end < text.size() && text[end] == 0x200D) {
        ++end;
        if (end >= text.size()) break;
        end += (IsHigh(text[end]) && end + 1 < text.size() && IsLow(text[end + 1])) ? 2 : 1;
        ConsumeExtenders(text, &end);
    }
    return end;
}

} // namespace

std::vector<TextUnit> BuildTextUnits(std::wstring_view text) {
    std::vector<TextUnit> out;
    for (size_t start = 0; start < text.size();) {
        const size_t end = NextUnitEnd(text, start);
        const TextUnitClass klass = (text[start] == L'\r' || text[start] == L'\n')
            ? TextUnitClass::LineBreak : Classify(text[start]);
        out.push_back({{{start}, {end}}, klass});
        start = end;
    }
    return out;
}

Span PreviousTextUnit(std::wstring_view text, Utf16CodeUnitOffset caret) {
    const auto units = BuildTextUnits(text);
    const size_t pos = std::min(caret.value, text.size());
    size_t index = 0;
    while (index < units.size() && units[index].span.end.value <= pos) ++index;
    return index == 0 ? Span{{pos}, {pos}} : units[index - 1].span;
}

Span NextTextUnit(std::wstring_view text, Utf16CodeUnitOffset caret) {
    const auto units = BuildTextUnits(text);
    const size_t pos = std::min(caret.value, text.size());
    size_t index = 0;
    while (index < units.size() && units[index].span.end.value <= pos) ++index;
    return index == units.size() ? Span{{pos}, {pos}} : units[index].span;
}

Span PreviousTextUnitRun(std::wstring_view text, Utf16CodeUnitOffset caret) {
    const auto units = BuildTextUnits(text);
    const size_t pos = std::min(caret.value, text.size());
    size_t index = 0;
    while (index < units.size() && units[index].span.end.value <= pos) ++index;
    if (index == 0) return {{pos}, {pos}};
    --index;
    const TextUnitClass klass = units[index].klass;
    size_t start = units[index].span.start.value;
    while (klass != TextUnitClass::LineBreak && index > 0 && units[index - 1].klass == klass && units[index - 1].span.end.value == start) {
        start = units[--index].span.start.value;
    }
    return {{start}, {pos}};
}

Span NextTextUnitRun(std::wstring_view text, Utf16CodeUnitOffset caret) {
    const auto units = BuildTextUnits(text);
    const size_t pos = std::min(caret.value, text.size());
    size_t index = 0;
    while (index < units.size() && units[index].span.end.value <= pos) ++index;
    if (index >= units.size()) return {{pos}, {pos}};
    const size_t start = units[index].span.start.value;
    const TextUnitClass klass = units[index].klass;
    size_t end = units[index].span.end.value;
    while (klass != TextUnitClass::LineBreak && ++index < units.size() && units[index].klass == klass && units[index].span.start.value == end) end = units[index].span.end.value;
    return {{start}, {end}};
}

} // namespace note
