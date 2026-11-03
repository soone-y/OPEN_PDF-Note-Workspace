#pragma once

#include "note/note_model.h"

namespace note {

NoteDocument ParseNoteDocumentWithMd4c(const NoteTextModel& model, bool permissiveHeadings = false);

} // namespace note
