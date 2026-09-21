#pragma once

#include "note/note_model.h"

namespace note {

NoteDocument ParseNoteDocument(const NoteTextModel& model);
// TeX source keeps every non-math token literal. Only completed, supported
// TeX math delimiters contribute derived math spans.
NoteDocument ParseTeXMathDocument(const NoteTextModel& model);

} // namespace note
