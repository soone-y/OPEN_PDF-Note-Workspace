#pragma once

#include <cstddef>
#include <string>

namespace note {

// Offsets count wchar_t units in the caller's text (UTF-16 in the Windows UI).
// Results own their prefix strings; parsing never changes text, selection or history.
struct AutoBulletLineContext {
    size_t lineStart = 0;
    size_t lineEnd = 0;
    size_t markerPos = 0;
    size_t prefixEnd = 0;
    std::wstring prefix;
    std::wstring continuationPrefix;
    size_t orderedPaddingInsertPos = 0;
    size_t orderedPaddingCount = 0;
    bool valid = false;
    bool hasContent = false;
    bool structuralPrefix = false;
};

[[nodiscard]] bool IsNoteBulletIndentChar(wchar_t ch);
[[nodiscard]] bool IsMarkdownBulletMarker(wchar_t ch);
[[nodiscard]] bool IsMarkdownOrderedListDigit(wchar_t ch);
// Positions past the end are clamped. CR and LF each delimit a line, as in the
// existing native editing route; a position between CR and LF starts an empty line.
[[nodiscard]] size_t FindNoteLineStart(const std::wstring& text, size_t pos);
[[nodiscard]] size_t FindNoteLineEnd(const std::wstring& text, size_t pos);
// The caller supplies the legacy policy for tab-separated ordered continuation.
// Invalid lines return valid=false; callers must check it before applying edits.
[[nodiscard]] AutoBulletLineContext AnalyzeAutoBulletLine(
    const std::wstring& text, size_t caret, bool continueTabSeparatedOrderedList);
// Requires lineStart <= lineEnd <= text.size(). This recognizes direct list
// markers for indentation; quoted lists retain their existing separate treatment.
[[nodiscard]] bool IsMarkdownListLine(const std::wstring& text, size_t lineStart, size_t lineEnd);

} // namespace note
