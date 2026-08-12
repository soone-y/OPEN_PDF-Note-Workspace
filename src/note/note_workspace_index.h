#pragma once

#include "note/note_kernel.h"

#include <optional>

namespace note {

using NoteQueryGroups = std::vector<std::vector<std::wstring>>;

struct NoteTextRange {
    size_t start = 0;
    size_t len = 0;
};

struct NoteQueryMatch {
    bool matched = false;
    size_t firstPos = std::wstring::npos;
    size_t firstLen = 0;
    int totalHits = 0;
    std::vector<NoteTextRange> ranges;
};

struct WorkspaceNoteLine {
    int line_number = 1;
    Span raw_span{};
    std::wstring text;
    SemanticNormalizedTextIndex normalized_text;
};

struct WorkspaceNoteLineMatch {
    int line_number = 1;
    Span raw_span{};
    std::wstring text;
    NoteQueryMatch match;
};

struct WorkspaceNoteIndexSnapshot {
    bool valid = false;
    SnapshotIdentity snapshot_identity{};
    std::wstring target_path;
    NoteTextModel text_model;
    NoteDocument document;
    SemanticIndexSnapshot semantic_index;
    std::vector<WorkspaceNoteLine> lines;
};

WorkspaceNoteIndexSnapshot BuildWorkspaceNoteIndex(
    SnapshotIdentity snapshotIdentity,
    std::wstring targetPath,
    NoteMetadata metadata,
    std::wstring raw,
    NoteContentKind contentKind);
WorkspaceNoteIndexSnapshot BuildWorkspaceNoteIndexFromKernel(
    SnapshotIdentity snapshotIdentity,
    std::wstring targetPath,
    const LocalNoteKernel& kernel);

NoteQueryMatch MatchNoteNormalizedText(
    const SemanticNormalizedTextIndex& normalized,
    const NoteQueryGroups& queryGroups);
NoteQueryMatch MatchNoteText(std::wstring_view text,
                             const NoteQueryGroups& queryGroups,
                             SemanticSearchOptions options = {});
std::vector<WorkspaceNoteLineMatch> SearchWorkspaceNoteIndex(
    const WorkspaceNoteIndexSnapshot& index,
    const NoteQueryGroups& queryGroups,
    SemanticSearchOptions options = {});

std::optional<size_t> FindWorkspaceLinkIdAnchor(
    const WorkspaceNoteIndexSnapshot& index,
    std::wstring_view linkId,
    std::optional<size_t> excludedSourcePosition = std::nullopt);

} // namespace note
