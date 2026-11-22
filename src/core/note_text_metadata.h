#pragma once

#include "core/workspace_config.h"
#include "note/note_clro_markdown.h"

#include <utility>

// One application boundary for editor, link, export and search parsing.
// Preferences affect derived interpretation only. Source text and history
// remain owned by the caller, and other formats keep their own grammar.
[[nodiscard]] inline note::NoteMetadata BuildNoteTextMetadata(
    const WorkspaceConfig& config, std::wstring fileName) {
    note::NoteMetadata metadata;
    metadata.file_name = std::move(fileName);
    metadata.title = note::DeriveTitleFromFileName(metadata.file_name);
    if (note::IsClroMarkdownFile(metadata.file_name)) {
        metadata.clro_ascii_only = config.clroAsciiOnly;
        metadata.quote_lazy_continuation = config.noteQuoteLazyContinuation;
        metadata.clro_list_implicit_continuation = config.clroListImplicitContinuation;
    }
    return metadata;
}
