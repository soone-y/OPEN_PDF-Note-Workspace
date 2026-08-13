#pragma once

#include "note/note_model.h"

#include <string_view>
#include <vector>

namespace note {

// A user-visible UTF-16 text unit. A unit never splits CRLF, a valid surrogate
// pair, combining marks, variation selectors, or a ZWJ emoji sequence.
enum class TextUnitClass {
    LineBreak,
    Whitespace,
    LatinNumber,
    Hiragana,
    Katakana,
    Kanji,
    Punctuation,
    Other,
};

struct TextUnit {
    Span span{};
    TextUnitClass klass = TextUnitClass::Other;
};

[[nodiscard]] std::vector<TextUnit> BuildTextUnits(std::wstring_view text);
[[nodiscard]] Span PreviousTextUnit(std::wstring_view text, Utf16CodeUnitOffset caret);
[[nodiscard]] Span NextTextUnit(std::wstring_view text, Utf16CodeUnitOffset caret);
[[nodiscard]] Span PreviousTextUnitRun(std::wstring_view text, Utf16CodeUnitOffset caret);
[[nodiscard]] Span NextTextUnitRun(std::wstring_view text, Utf16CodeUnitOffset caret);

} // namespace note
