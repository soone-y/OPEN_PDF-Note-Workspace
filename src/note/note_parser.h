#pragma once

#include "note/note_model.h"
#include <optional>

namespace note {

NoteDocument ParseNoteDocument(const NoteTextModel& model);
// Export-only conversion of accepted CLRO syntax; never a storage mutation.
// A failed parse returns no candidate output.
[[nodiscard]] std::optional<std::wstring> BuildClroMarkdownExportSource(const NoteTextModel& model);
// Ordinary syntax errors remain diagnostics. A failed MD4C run cannot provide
// a complete structural document for publication or derived consumers.
[[nodiscard]] bool NoteDocumentHasFatalParseFailure(const NoteDocument& document) noexcept;
// TeX source keeps every non-math token literal. Only completed, supported
// TeX math delimiters contribute derived math spans.
NoteDocument ParseTeXMathDocument(const NoteTextModel& model);

} // namespace note
