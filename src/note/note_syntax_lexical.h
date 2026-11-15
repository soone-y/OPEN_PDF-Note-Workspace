#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <algorithm>
#include <string_view>

namespace note {

// Standalone heading names. <m> is tier one, <m1>...<m5> are tiers two...six.
// Values such as m=5 and relative line-height shorthand are not heading names.
[[nodiscard]] inline int ParseNoteHeadingTagLevel(std::wstring_view name) noexcept {
    if (name.empty()) return 0;
    const wchar_t first = name[0];
    if (first == L'm' || first == L'M') {
        if (name.size() == 1) return 1;
        return name.size() == 2 && name[1] >= L'1' && name[1] <= L'5'
            ? name[1] - L'0' + 1 : 0;
    }
    if ((first != L'h' && first != L'H') || name.size() < 2) return 0;
    int level = 0;
    for (size_t i = 1; i < name.size(); ++i) {
        if (name[i] < L'0' || name[i] > L'9') return 0;
        // Legacy h names clamp at tier six without overflowing on long input.
        level = std::min(6, level * 10 + static_cast<int>(name[i] - L'0'));
    }
    return level;
}

// Recover a token only at a parser-accepted style boundary. These helpers
// do not decide whether arbitrary angle-bracket text is valid markup.
[[nodiscard]] bool FindNoteMarkupTagEndingAt(std::wstring_view source,
    Utf16CodeUnitOffset end, Span* out, bool fullwidth = false) noexcept;
[[nodiscard]] bool FindNoteMarkupTagStartingAt(std::wstring_view source,
    Utf16CodeUnitOffset start, Span* out, bool fullwidth = false) noexcept;

enum class NoteMathTagDisplay { Automatic, Inline, Block };
struct NoteMathOpenTag {
    Utf16CodeUnitOffset end{};
    NoteMathTagDisplay display = NoteMathTagDisplay::Automatic;
    bool complete = false;
    bool self_closing = false;
    bool valid_attributes = true;
    bool unknown_attributes = false;
};
// Recognizes the math name, including an incomplete/malformed opening.
// `end` always advances, so callers can reject without rescanning the suffix.
// No allocation, source mutation, or interpretation of unknown attributes.
[[nodiscard]] bool TryParseNoteMathOpenTag(std::wstring_view raw,
    Utf16CodeUnitOffset start, NoteMathOpenTag* out) noexcept;
[[nodiscard]] size_t NoteMathCloseTagLength(std::wstring_view suffix) noexcept;

// Exact lexical form of a Markdown fenced-code delimiter.  This is shared by
// full parsing and checkpoint construction so both paths advance the same
// inherited code-fence state.
struct NoteCodeFenceRun {
    wchar_t marker = 0;
    size_t marker_count = 0;
};

// `line_end` is exclusive and must exclude CR/LF transport characters.  An
// opening run may carry an info string; a closing run accepts only trailing
// whitespace.  Invalid UTF-16 offsets simply return false.
[[nodiscard]] bool TryParseNoteCodeFenceRun(
    std::wstring_view raw,
    Utf16CodeUnitOffset line_start,
    Utf16CodeUnitOffset line_end,
    bool closing_only,
    NoteCodeFenceRun* out_run) noexcept;

// Setext and Markdown-table delimiter rows read the immediately preceding
// logical row. The input may include CR/LF transport characters.
[[nodiscard]] bool IsNoteBoundedLookBehindDelimiterLine(std::wstring_view line) noexcept;

// A standalone ::: fence (at most three leading spaces). An empty info
// string closes the innermost container, or opens an unnamed one outside it.
// The returned info range excludes surrounding spaces, never source units.
struct NoteContainerFenceRun {
    size_t marker_count = 0;
    Span info_span{};
};
[[nodiscard]] bool TryParseNoteContainerFenceRun(
    std::wstring_view line, NoteContainerFenceRun* out) noexcept;

// Literal title coordinates within a standalone opening fence. Containers
// use the whole info string unless title= is specified; code requires title=.
// Quotes/attributes are source-only. Empty, duplicate or malformed explicit
// titles produce no title, never a partially interpreted attribute.
[[nodiscard]] Span NoteFenceTitleSpan(std::wstring_view line, bool container) noexcept;

} // namespace note
