#include "note/note_workspace_index.h"

#include <algorithm>

namespace note {
namespace {

void MergeTextRanges(std::vector<NoteTextRange>* ranges) {
    if (!ranges || ranges->size() < 2) return;
    std::sort(ranges->begin(), ranges->end(),
              [](const NoteTextRange& lhs, const NoteTextRange& rhs) {
                  if (lhs.start != rhs.start) return lhs.start < rhs.start;
                  return lhs.len < rhs.len;
              });
    std::vector<NoteTextRange> merged;
    merged.reserve(ranges->size());
    for (const NoteTextRange& range : *ranges) {
        if (range.len == 0) continue;
        if (merged.empty()) {
            merged.push_back(range);
            continue;
        }
        NoteTextRange& tail = merged.back();
        const size_t tailEnd = tail.start + tail.len;
        const size_t rangeEnd = range.start + range.len;
        if (range.start <= tailEnd) {
            tail.len = std::max(tailEnd, rangeEnd) - tail.start;
        } else {
            merged.push_back(range);
        }
    }
    ranges->swap(merged);
}

void BuildLineIndex(WorkspaceNoteIndexSnapshot* index) {
    if (!index) return;
    const std::vector<size_t>& starts = index->text_model.line_starts;
    index->lines.clear();
    index->lines.reserve(std::max<size_t>(1, starts.size()));
    for (size_t lineIndex = 0; lineIndex < starts.size(); ++lineIndex) {
        const size_t start = starts[lineIndex];
        size_t end = lineIndex + 1 < starts.size()
            ? starts[lineIndex + 1]
            : index->text_model.raw.size();
        while (end > start &&
               (index->text_model.raw[end - 1] == L'\r' ||
                index->text_model.raw[end - 1] == L'\n')) {
            --end;
        }
        WorkspaceNoteLine line;
        line.line_number = static_cast<int>(lineIndex + 1);
        line.raw_span = Span{start, end};
        line.text = index->text_model.raw.substr(start, end - start);
        line.normalized_text = BuildSemanticNormalizedTextIndex(line.text);
        index->lines.push_back(std::move(line));
    }
    if (index->lines.empty()) index->lines.push_back(WorkspaceNoteLine{});
}

} // namespace

WorkspaceNoteIndexSnapshot BuildWorkspaceNoteIndex(
    SnapshotIdentity snapshotIdentity,
    std::wstring targetPath,
    NoteMetadata metadata,
    std::wstring raw,
    NoteContentKind contentKind) {
    WorkspaceNoteIndexSnapshot index;
    index.snapshot_identity = snapshotIdentity;
    index.target_path = std::move(targetPath);
    if (!snapshotIdentity.note_id.valid()) return index;

    const uint64_t contentRevision = snapshotIdentity.content_revision != 0
        ? snapshotIdentity.content_revision
        : 1;
    LocalNoteKernel kernel;
    kernel.Reset(snapshotIdentity.note_id,
                 std::move(metadata),
                 std::move(raw),
                 contentRevision,
                 snapshotIdentity.persistence_revision,
                 contentKind);
    (void)kernel.RefreshDerived();

    index.text_model = kernel.text_core().model();
    index.document = kernel.document();
    index.semantic_index = kernel.semantic_index();
    BuildLineIndex(&index);
    index.valid = snapshotIdentity.valid();
    return index;
}

WorkspaceNoteIndexSnapshot BuildWorkspaceNoteIndexFromKernel(
    SnapshotIdentity snapshotIdentity,
    std::wstring targetPath,
    const LocalNoteKernel& kernel) {
    WorkspaceNoteIndexSnapshot index;
    index.snapshot_identity = snapshotIdentity;
    index.target_path = std::move(targetPath);
    if (!snapshotIdentity.valid() || !kernel.valid() ||
        snapshotIdentity.note_id != kernel.text_core().note_id() ||
        (snapshotIdentity.content_revision != 0 &&
         snapshotIdentity.content_revision != kernel.text_core().content_revision()) ||
        (kernel.content_kind() == NoteContentKind::Markdown &&
         (!kernel.CanReadSyntax() || !kernel.CanReadSemantic()))) {
        return index;
    }
    index.text_model = kernel.text_core().model();
    index.document = kernel.document();
    index.semantic_index = kernel.semantic_index();
    BuildLineIndex(&index);
    index.valid = true;
    return index;
}

NoteQueryMatch MatchNoteNormalizedText(
    const SemanticNormalizedTextIndex& normalized,
    const NoteQueryGroups& queryGroups) {
    NoteQueryMatch match;
    if (normalized.text.empty() || queryGroups.empty()) return match;

    match.matched = true;
    for (const auto& group : queryGroups) {
        int groupHits = 0;
        for (const std::wstring& term : group) {
            if (term.empty()) continue;
            size_t pos = 0;
            while (pos < normalized.text.size()) {
                const size_t found = normalized.text.find(term, pos);
                if (found == std::wstring::npos) break;
                if (found + term.size() > normalized.source_end.size()) break;
                const size_t originalStart = normalized.source_start[found];
                const size_t originalEnd =
                    normalized.source_end[found + term.size() - 1];
                const size_t originalLength =
                    std::max<size_t>(1, originalEnd - originalStart);
                match.ranges.push_back(
                    NoteTextRange{originalStart, originalLength});
                ++match.totalHits;
                ++groupHits;
                if (match.firstPos == std::wstring::npos ||
                    originalStart < match.firstPos) {
                    match.firstPos = originalStart;
                    match.firstLen = originalLength;
                }
                pos = found + term.size();
            }
        }
        if (groupHits == 0) return NoteQueryMatch{};
    }
    MergeTextRanges(&match.ranges);
    return match;
}

NoteQueryMatch MatchNoteText(std::wstring_view text,
                             const NoteQueryGroups& queryGroups,
                             SemanticSearchOptions options) {
    return MatchNoteNormalizedText(
        BuildSemanticNormalizedTextIndex(text, options), queryGroups);
}

std::vector<WorkspaceNoteLineMatch> SearchWorkspaceNoteIndex(
    const WorkspaceNoteIndexSnapshot& index,
    const NoteQueryGroups& queryGroups,
    SemanticSearchOptions options) {
    std::vector<WorkspaceNoteLineMatch> matches;
    if (!index.valid || queryGroups.empty()) return matches;
    for (const WorkspaceNoteLine& line : index.lines) {
        NoteQueryMatch match = (options.normalizeWidthKana && options.ignoreCase && options.ignoreSeparators)
            ? MatchNoteNormalizedText(line.normalized_text, queryGroups)
            : MatchNoteText(line.text, queryGroups, options);
        if (!match.matched) continue;
        for (NoteTextRange& range : match.ranges) {
            range.start += line.raw_span.start.value;
        }
        if (match.firstPos != std::wstring::npos) {
            match.firstPos += line.raw_span.start.value;
        }
        WorkspaceNoteLineMatch result;
        result.line_number = line.line_number;
        result.raw_span = line.raw_span;
        result.text = line.text;
        result.match = std::move(match);
        matches.push_back(std::move(result));
    }
    return matches;
}

std::optional<size_t> FindWorkspaceLinkIdAnchor(
    const WorkspaceNoteIndexSnapshot& index,
    std::wstring_view linkId,
    std::optional<size_t> excludedSourcePosition) {
    if (!index.valid || !index.semantic_index.valid || linkId.empty()) {
        return std::nullopt;
    }
    std::optional<size_t> firstAny;
    std::optional<size_t> firstAfter;
    for (const SemanticLinkEntry& link : index.semantic_index.links) {
        if (link.kind != SemanticLinkKind::LinkId || link.target != linkId) continue;
        if (excludedSourcePosition.has_value() &&
            link.span.start.value <= *excludedSourcePosition &&
            *excludedSourcePosition < link.span.end.value) {
            continue;
        }
        if (!firstAny.has_value()) firstAny = link.span.start.value;
        if (excludedSourcePosition.has_value() &&
            link.span.start.value > *excludedSourcePosition &&
            !firstAfter.has_value()) {
            firstAfter = link.span.start.value;
        }
    }
    return firstAfter.has_value() ? firstAfter : firstAny;
}

} // namespace note
