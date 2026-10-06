#pragma once

#include "note/note_model.h"

#include <cstddef>
#include <string_view>

namespace note {

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

} // namespace note
