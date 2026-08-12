#pragma once

#include "note/note_identity.h"
#include "note/note_model.h"

#include <string>
#include <vector>

namespace note {

struct SemanticHeadingEntry {
    int level = 0;
    Span block_span{};
    Span content_span{};
    Span line_span{};
    LineColumn loc{};
    std::wstring text;
};

enum class SemanticLinkKind {
    MarkdownTarget,
    LinkId,
};

struct SemanticLinkEntry {
    SemanticLinkKind kind = SemanticLinkKind::MarkdownTarget;
    Span span{};
    std::wstring target;
    std::wstring text;
};

struct SemanticLinkTargetResolution {
    std::wstring target_path;
    SnapshotIdentity snapshot_identity{};
    size_t anchor_position = 0;

    bool valid() const {
        return !target_path.empty() && snapshot_identity.valid();
    }
};

bool SemanticLinkTargetMatchesSnapshot(
    const SemanticLinkTargetResolution& target,
    const SnapshotIdentity& currentSnapshot);

struct SemanticMathEntry {
    MathKind kind = MathKind::Inline;
    MathDelimiter delimiter = MathDelimiter::Dollar;
    Span span{};
    Span content_span{};
    std::wstring normalized_tex;
};

struct SemanticSearchOptions {
    bool normalizeWidthKana = true;
    bool ignoreCase = true;
    bool ignoreSeparators = true;
};

struct SemanticNormalizedTextIndex {
    std::wstring text;
    std::vector<size_t> source_start;
    std::vector<size_t> source_end;
};

struct SemanticIndexSnapshot {
    bool valid = false;
    NoteDerivedSnapshotIdentity source_identity{};
    std::vector<SemanticHeadingEntry> headings;
    std::vector<SemanticLinkEntry> links;
    std::vector<SemanticMathEntry> math;
    SemanticNormalizedTextIndex normalized_text;
};

std::wstring NormalizeSemanticSearchTerm(std::wstring_view term, SemanticSearchOptions options = {});
SemanticNormalizedTextIndex BuildSemanticNormalizedTextIndex(std::wstring_view text, SemanticSearchOptions options = {});
SemanticIndexSnapshot BuildSemanticIndexSnapshot(NoteId ownerNoteId,
                                                 const NoteTextModel& model,
                                                 const NoteDocument& document);
bool SemanticIndexMatchesTextModel(const SemanticIndexSnapshot& index,
                                   NoteId ownerNoteId,
                                   const NoteTextModel& model);

} // namespace note
