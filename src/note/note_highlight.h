#pragma once

#include "core/app_core.h"
#include "note/note_model.h"

#include <vector>

namespace note {

std::vector<HighlightRange> BuildHighlightRanges(const NoteDocument& doc);

} // namespace note
