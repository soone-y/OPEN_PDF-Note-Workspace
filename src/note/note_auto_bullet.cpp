#include "note/note_auto_bullet.h"

#include <algorithm>

namespace note {

bool IsNoteBulletIndentChar(wchar_t ch) {
    // Markdown list syntax accepts an ASCII space or tab. Treating an ideographic
    // space as indentation made the input helper create lists which MD4C rightly
    // continued to parse as ordinary text.
    return ch == L' ' || ch == L'\t';
}

bool IsMarkdownBulletMarker(wchar_t ch) {
    return ch == L'-' || ch == L'*' || ch == L'+';
}

bool IsMarkdownOrderedListDigit(wchar_t ch) {
    return ch >= L'0' && ch <= L'9';
}

size_t FindNoteLineStart(const std::wstring& text, size_t pos) {
    pos = std::min(pos, text.size());
    while (pos > 0) {
        wchar_t prev = text[pos - 1];
        if (prev == L'\r' || prev == L'\n') break;
        --pos;
    }
    return pos;
}

size_t FindNoteLineEnd(const std::wstring& text, size_t pos) {
    pos = std::min(pos, text.size());
    size_t lineEnd = text.find_first_of(L"\r\n", pos);
    return (lineEnd == std::wstring::npos) ? text.size() : lineEnd;
}

static size_t AdvanceMarkdownQuotePrefix(const std::wstring& text, size_t pos, size_t lineEnd) {
    while (pos < lineEnd && text[pos] == L'>') {
        ++pos;
        while (pos < lineEnd && IsNoteBulletIndentChar(text[pos])) {
            ++pos;
        }
    }
    return pos;
}

static bool TryParseMarkdownOrderedListMarker(const std::wstring& text,
                                              size_t pos,
                                              size_t lineEnd,
                                              unsigned long long* outNumber,
                                              wchar_t* outDelimiter,
                                              size_t* outAfterMarker) {
    if (outNumber) *outNumber = 0;
    if (outDelimiter) *outDelimiter = L'.';
    if (outAfterMarker) *outAfterMarker = pos;
    if (pos >= lineEnd || !IsMarkdownOrderedListDigit(text[pos])) return false;

    size_t scan = pos;
    unsigned long long number = 0;
    while (scan < lineEnd && IsMarkdownOrderedListDigit(text[scan])) {
        unsigned digit = static_cast<unsigned>(text[scan] - L'0');
        if (number < 999999999ULL) {
            number = std::min(999999999ULL, number * 10ULL + digit);
        }
        ++scan;
    }
    if (scan == pos || scan >= lineEnd) return false;
    wchar_t delimiter = text[scan];
    if (delimiter != L'.' && delimiter != L')') return false;
    ++scan;
    if (scan >= lineEnd || !IsNoteBulletIndentChar(text[scan])) return false;
    while (scan < lineEnd && IsNoteBulletIndentChar(text[scan])) {
        ++scan;
    }

    if (outNumber) *outNumber = std::max(1ULL, number);
    if (outDelimiter) *outDelimiter = delimiter;
    if (outAfterMarker) *outAfterMarker = scan;
    return true;
}

static bool TryParseMarkdownChecklistMarker(const std::wstring& text,
                                            size_t pos,
                                            size_t lineEnd,
                                            size_t* outAfterMarker) {
    if (outAfterMarker) *outAfterMarker = pos;
    if (pos + 2 >= lineEnd) return false;
    if (text[pos] != L'[' || text[pos + 2] != L']') return false;
    wchar_t state = text[pos + 1];
    if (!(state == L' ' || state == L'x' || state == L'X')) return false;

    size_t scan = pos + 3;
    if (scan >= lineEnd || !IsNoteBulletIndentChar(text[scan])) return false;
    while (scan < lineEnd && IsNoteBulletIndentChar(text[scan])) {
        ++scan;
    }
    if (outAfterMarker) *outAfterMarker = scan;
    return true;
}

AutoBulletLineContext AnalyzeAutoBulletLine(const std::wstring& text, size_t caret,
                                          bool continueTabSeparatedOrderedList) {
    AutoBulletLineContext ctx{};
    if (caret > text.size()) caret = text.size();

    size_t lineStart = FindNoteLineStart(text, caret);
    size_t lineEnd = FindNoteLineEnd(text, lineStart);
    if (lineStart >= lineEnd && lineStart >= text.size()) return ctx;

    size_t pos = lineStart;
    while (pos < lineEnd && IsNoteBulletIndentChar(text[pos])) {
        ++pos;
    }
    const size_t afterIndent = pos;
    size_t afterQuotes = AdvanceMarkdownQuotePrefix(text, pos, lineEnd);
    const bool hasQuotePrefix = afterQuotes > afterIndent;
    std::wstring prefixBase = text.substr(lineStart, afterQuotes - lineStart);

    bool valid = (afterQuotes > lineStart);
    size_t markerPos = afterQuotes;
    bool hasListMarker = false;
    bool hasChecklist = false;
    bool ordered = false;
    unsigned long long orderedNumber = 1;
    wchar_t orderedDelimiter = L'.';

    pos = afterQuotes;
    if (pos < lineEnd && IsMarkdownBulletMarker(text[pos])) {
        markerPos = pos;
        ++pos;
        if (pos >= lineEnd || !IsNoteBulletIndentChar(text[pos])) return ctx;
        while (pos < lineEnd && IsNoteBulletIndentChar(text[pos])) {
            ++pos;
        }
        hasListMarker = true;
        valid = true;
        // Preserve the author-entered separator after the marker. Besides
        // keeping nested list text aligned, this avoids collapsing a deliberate
        // tab or multi-space indent to one ordinary space on the next item.
        ctx.continuationPrefix = prefixBase + text.substr(markerPos, pos - markerPos);
    } else {
        size_t orderedAfter = pos;
        if (TryParseMarkdownOrderedListMarker(text, pos, lineEnd,
                                              &orderedNumber, &orderedDelimiter, &orderedAfter)) {
            markerPos = pos;
            pos = orderedAfter;
            ordered = true;
            hasListMarker = true;
            valid = true;
            size_t separatorStart = markerPos;
            while (separatorStart < pos && IsMarkdownOrderedListDigit(text[separatorStart])) {
                ++separatorStart;
            }
            if (separatorStart < pos &&
                (text[separatorStart] == L'.' || text[separatorStart] == L')')) {
                ++separatorStart;
            }
            const std::wstring nextNumber = std::to_wstring(orderedNumber + 1ULL);
            const std::wstring separator = text.substr(separatorStart, pos - separatorStart);
            const bool spaceOnlySeparator = !separator.empty() &&
                std::all_of(separator.begin(), separator.end(), [](wchar_t ch) { return ch == L' '; });
            if (spaceOnlySeparator) {
                // Keep the text column aligned at a 9 -> 10 (or 99 -> 100)
                // transition. The preceding item's separator is extended only
                // when its marker becomes shorter than the next marker.
                const size_t currentFieldWidth = pos - markerPos;
                const size_t nextMarkerWidth = nextNumber.size() + 1; // number + '.' or ')'
                const size_t targetFieldWidth = std::max(currentFieldWidth, nextMarkerWidth + 1);
                ctx.orderedPaddingInsertPos = pos;
                ctx.orderedPaddingCount = targetFieldWidth - currentFieldWidth;
                ctx.continuationPrefix = prefixBase + nextNumber +
                                         std::wstring(1, orderedDelimiter) +
                                         std::wstring(targetFieldWidth - nextMarkerWidth, L' ');
            } else if (continueTabSeparatedOrderedList) {
                // Tabs are visually width-dependent, so preserve them exactly
                // rather than guessing an alignment column.
                ctx.continuationPrefix = prefixBase + nextNumber +
                                         std::wstring(1, orderedDelimiter) + separator;
            }
        } else {
            ctx.continuationPrefix = prefixBase;
        }
    }

    if (hasListMarker) {
        size_t checklistAfter = pos;
        if (TryParseMarkdownChecklistMarker(text, pos, lineEnd, &checklistAfter)) {
            hasChecklist = true;
            pos = checklistAfter;
            ctx.continuationPrefix += L"[ ] ";
        }
    }
    if (!valid) return ctx;

    bool hasContent = false;
    for (size_t scan = pos; scan < lineEnd; ++scan) {
        if (!IsNoteBulletIndentChar(text[scan])) {
            hasContent = true;
            break;
        }
    }

    ctx.lineStart = lineStart;
    ctx.lineEnd = lineEnd;
    ctx.markerPos = markerPos;
    ctx.prefixEnd = pos;
    ctx.prefix = text.substr(lineStart, pos - lineStart);
    if (!hasListMarker && !hasChecklist && ctx.continuationPrefix.empty()) {
        ctx.continuationPrefix = ctx.prefix;
    }
    if (hasListMarker && !ordered && ctx.continuationPrefix.empty()) {
        ctx.continuationPrefix = prefixBase + text[markerPos] + L' ';
    }
    ctx.valid = valid;
    ctx.hasContent = hasContent;
    ctx.structuralPrefix = hasQuotePrefix || hasListMarker || hasChecklist;
    return ctx;
}

bool IsMarkdownListLine(const std::wstring& text, size_t lineStart, size_t lineEnd) {
    size_t marker = lineStart;
    while (marker < lineEnd && IsNoteBulletIndentChar(text[marker])) {
        ++marker;
    }
    if (marker + 1 < lineEnd && IsMarkdownBulletMarker(text[marker]) &&
        IsNoteBulletIndentChar(text[marker + 1])) {
        return true;
    }
    return TryParseMarkdownOrderedListMarker(text, marker, lineEnd, nullptr, nullptr, nullptr);
}

} // namespace note
