#pragma once

#include "note/note_workspace_index.h"

#include <string>

namespace note {

enum class ExportTextMathMode {
    Raw,
    Placeholder,
    Simplified,
};

enum class ExportTextMarkupMode {
    Raw,
    Placeholder,
    Simplified,
};

struct TextExportConfig {
    ExportTextMathMode mathMode = ExportTextMathMode::Raw;
    std::string mathPlaceholder = "[math]";
    ExportTextMarkupMode markupMode = ExportTextMarkupMode::Simplified;
    std::string markupPlaceholder = "[mark]";
};

enum class ExportMarkupFormat {
    Markdown,
    Html,
};

enum class ExportMarkupMathMode {
    Format,
    Placeholder,
};

struct MarkupExportConfig {
    ExportMarkupFormat format = ExportMarkupFormat::Markdown;
    ExportMarkupMathMode mathMode = ExportMarkupMathMode::Format;
    std::string mathPlaceholder = "[math]";
    bool includeTitleHeading = false;
    bool shiftHeadingLevels = true;
    std::wstring title;
};

struct WorkspaceNoteExportResult {
    bool ok = false;
    SnapshotIdentity snapshot_identity{};
    std::string bytes;
};

std::string ExportPlainText(const NoteTextModel& model,
                            const NoteDocument& doc,
                            const TextExportConfig& config);

std::string ExportMarkdown(const NoteTextModel& model,
                           const NoteDocument& doc,
                           const MarkupExportConfig& config);

std::string ExportHtml(const NoteTextModel& model,
                       const NoteDocument& doc,
                       const MarkupExportConfig& config);

bool WorkspaceNoteIndexCanExport(const WorkspaceNoteIndexSnapshot& index);
WorkspaceNoteExportResult ExportWorkspacePlainText(
    const WorkspaceNoteIndexSnapshot& index,
    const TextExportConfig& config);
WorkspaceNoteExportResult ExportWorkspaceMarkdown(
    const WorkspaceNoteIndexSnapshot& index,
    const MarkupExportConfig& config);
WorkspaceNoteExportResult ExportWorkspaceHtml(
    const WorkspaceNoteIndexSnapshot& index,
    const MarkupExportConfig& config);

} // namespace note
