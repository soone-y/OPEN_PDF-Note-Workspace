#include "note/note_influence.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string_view>

namespace note {
namespace {

[[nodiscard]] bool ValidLineStarts(const std::vector<size_t>& starts,
                                   size_t textSize) {
    if (starts.empty() || starts.front() != 0 || starts.back() > textSize) {
        return false;
    }
    return std::is_sorted(starts.begin(), starts.end()) &&
        std::adjacent_find(starts.begin(), starts.end()) == starts.end();
}

[[nodiscard]] size_t FindLineByOffset(const std::vector<size_t>& starts,
                                      size_t offset) {
    const auto upper = std::upper_bound(starts.begin(), starts.end(), offset);
    return upper == starts.begin()
        ? 0
        : static_cast<size_t>(std::distance(starts.begin(), upper) - 1);
}

[[nodiscard]] bool SpanContainsOrIntersects(const Span& span,
                                             const TextEdit& edit) {
    if (span.end < span.start) return false;
    const size_t start = edit.start.value;
    if (edit.deleted_len == 0) {
        return span.start.value <= start && start <= span.end.value;
    }
    if (start > std::numeric_limits<size_t>::max() - edit.deleted_len) {
        return false;
    }
    const size_t end = start + edit.deleted_len;
    return start < span.end.value && end > span.start.value;
}

[[nodiscard]] bool ContainsPotentialMarkup(std::wstring_view text) {
    return text.find_first_of(L"\r\n`*_~[]()<>#!$\\:|>") !=
        std::wstring_view::npos;
}

[[nodiscard]] bool IsLineWhitespace(wchar_t ch) {
    return ch == L' ' || ch == L'\t';
}

[[nodiscard]] bool IsContainerOrDelimiterLine(std::wstring_view text) {
    size_t first = 0;
    while (first < text.size() && text[first] == L' ') ++first;
    // A leading tab changes Markdown indentation semantics.  The renderer
    // preserves tabs, but it must not infer a local parser patch around one.
    if (first < text.size() && text[first] == L'\t') return true;
    if (first == text.size()) return false;

    const wchar_t marker = text[first];
    if (marker == L'#' || marker == L'>' ||
        (marker == L':' && text.substr(first, 3) == L":::")) return true;
    if ((marker == L'-' || marker == L'+' || marker == L'*') &&
        (first + 1 == text.size() || IsLineWhitespace(text[first + 1]))) {
        return true;
    }

    // Keep setext/thematic delimiter rows on the conservative path.  A row
    // composed of at least three '-' or '=' characters (with spaces allowed)
    // can change its own and the preceding block's meaning.
    if (marker == L'-' || marker == L'=') {
        size_t delimiters = 0;
        for (size_t index = first; index < text.size(); ++index) {
            if (text[index] == marker) {
                ++delimiters;
            } else if (!IsLineWhitespace(text[index])) {
                break;
            }
        }
        if (delimiters >= 3) return true;
    }

    // Ordered list markers are structural only at a Markdown list prefix;
    // ordinary dots elsewhere retain the inexpensive local path.
    if (std::iswdigit(marker)) {
        size_t cursor = first;
        while (cursor < text.size() && std::iswdigit(text[cursor])) ++cursor;
        if (cursor < text.size() && (text[cursor] == L'.' || text[cursor] == L')') &&
            (cursor + 1 == text.size() || IsLineWhitespace(text[cursor + 1]))) {
            return true;
        }
    }
    return false;
}

// Keep this intentionally aligned with the render-line fence recognizer:
// fences begin in column zero and require three identical backticks or tildes.
// This conservative predicate is used only to revoke a local code-body proof;
// unfamiliar fence variants continue through the existing full-parse path.
[[nodiscard]] bool IsCodeFenceDelimiterLine(std::wstring_view text) {
    if (text.size() < 3) return false;
    const wchar_t marker = text[0];
    return (marker == L'`' || marker == L'~') &&
        text[1] == marker && text[2] == marker;
}

// Returns the first literal body position after a complete sequence of quote
// and list markers.  The immutable NoteDocument has already proved that this
// row belongs to a container; this lexical check merely proves that the edit
// stays after every marker and its required whitespace.  If the prefix is
// ambiguous, callers retain the container-wide conservative path.
[[nodiscard]] size_t FindContainerBodyStart(std::wstring_view text) {
    size_t cursor = 0;
    bool consumedMarker = false;
    while (cursor < text.size()) {
        while (cursor < text.size() && text[cursor] == L' ') ++cursor;
        if (cursor >= text.size()) return 0;

        if (text[cursor] == L'>') {
            consumedMarker = true;
            ++cursor;
            while (cursor < text.size() && IsLineWhitespace(text[cursor])) ++cursor;
            continue;
        }

        const wchar_t marker = text[cursor];
        if ((marker == L'-' || marker == L'+' || marker == L'*') &&
            cursor + 1 < text.size() && IsLineWhitespace(text[cursor + 1])) {
            consumedMarker = true;
            ++cursor;
            while (cursor < text.size() && IsLineWhitespace(text[cursor])) ++cursor;
            continue;
        }

        if (std::iswdigit(marker)) {
            size_t digitsEnd = cursor;
            while (digitsEnd < text.size() && std::iswdigit(text[digitsEnd])) ++digitsEnd;
            if (digitsEnd < text.size() &&
                (text[digitsEnd] == L'.' || text[digitsEnd] == L')') &&
                digitsEnd + 1 < text.size() && IsLineWhitespace(text[digitsEnd + 1])) {
                consumedMarker = true;
                cursor = digitsEnd + 1;
                while (cursor < text.size() && IsLineWhitespace(text[cursor])) ++cursor;
                continue;
            }
        }
        return consumedMarker ? cursor : 0;
    }
    return 0;
}

[[nodiscard]] NoteInfluenceLineRange LinesForSpan(
    const std::vector<size_t>& starts,
    size_t textSize,
    Span span) {
    if (starts.empty() || span.end < span.start || span.start.value > textSize) {
        return {};
    }
    const size_t start = std::min(span.start.value, textSize);
    const size_t end = std::min(span.end.value, textSize);
    const size_t inclusiveEnd = end > start ? end - 1 : start;
    return {true, FindLineByOffset(starts, start),
            FindLineByOffset(starts, inclusiveEnd)};
}

[[nodiscard]] std::wstring_view LineTextAt(const NoteTextModel& source,
                                            size_t line) {
    if (line >= source.line_starts.size()) return {};
    const size_t start = source.line_starts[line];
    size_t end = line + 1 < source.line_starts.size()
        ? source.line_starts[line + 1]
        : source.raw.size();
    // Line starts point immediately after a logical line break.  Newline
    // characters themselves are transport, not source syntax belonging to
    // this row; keeping them here would make every non-final plain row look
    // structural and silently disable the local path.
    while (end > start && (source.raw[end - 1] == L'\r' || source.raw[end - 1] == L'\n')) {
        --end;
    }
    return std::wstring_view(source.raw).substr(start, end - start);
}

[[nodiscard]] bool EditLeavesOnlyWhitespaceOnLine(const NoteTextModel& source,
                                                   size_t line,
                                                   const TextEdit& edit) {
    const std::wstring_view text = LineTextAt(source, line);
    const size_t lineStart = source.line_starts[line];
    if (edit.start.value < lineStart || edit.start.value > lineStart + text.size() ||
        edit.deleted_len > lineStart + text.size() - edit.start.value) {
        return false;
    }
    const size_t editStart = edit.start.value - lineStart;
    const size_t editEnd = editStart + edit.deleted_len;
    const auto hasNonWhitespace = [](std::wstring_view candidate) {
        return std::any_of(candidate.begin(), candidate.end(), [](wchar_t ch) {
            return !IsLineWhitespace(ch);
        });
    };
    return !hasNonWhitespace(text.substr(0, editStart)) &&
        !hasNonWhitespace(edit.inserted_text) &&
        !hasNonWhitespace(text.substr(editEnd));
}

[[nodiscard]] NoteInfluenceScope ScopeForSpan(
    NoteInfluenceScopeKind kind,
    const NoteTextModel& source,
    Span span) {
    NoteInfluenceScope result;
    result.kind = kind;
    result.source_revision = source.revision;
    result.source_lines = LinesForSpan(source.line_starts, source.raw.size(), span);
    // Finding a container is useful information for the later checkpoint
    // planner, but is not a proof that the existing parser snapshot can be
    // patched locally.  In particular, a marker, nesting-depth, or lazy
    // continuation change can alter following siblings without containing a
    // delimiter in the inserted text.  Preserve the current conservative
    // full-parse route until that fixed-point proof exists.
    result.permits_local_dirty_graph = result.source_lines.valid &&
        kind != NoteInfluenceScopeKind::Container &&
        kind != NoteInfluenceScopeKind::Unknown;
    return result;
}

[[nodiscard]] NoteInfluenceScope ScopeForCodeBlockEdit(
    const NoteTextModel& source,
    size_t line,
    const TextEdit& edit) {
    const std::wstring_view beforeLine = LineTextAt(source, line);
    if (line >= source.line_starts.size() ||
        !NoteCodeBlockContentEditPreservesFenceState(
            beforeLine, Utf16CodeUnitOffset{source.line_starts[line]}, edit)) {
        return {};
    }

    const Span localLine{{source.line_starts[line]},
                         {line + 1 < source.line_starts.size()
                              ? source.line_starts[line + 1]
                              : source.raw.size()}};
    return ScopeForSpan(NoteInfluenceScopeKind::CodeBlockContent, source, localLine);
}

[[nodiscard]] NoteInfluenceScope ScopeForContainerContentEdit(
    const NoteTextModel& source,
    size_t line,
    const TextEdit& edit) {
    if (line >= source.line_starts.size()) return {};
    const std::wstring_view beforeLine = LineTextAt(source, line);
    const size_t protectedPrefixEnd = NoteContainerContentProtectedPrefixEnd(
        beforeLine, Utf16CodeUnitOffset{source.line_starts[line]}, edit);
    if (protectedPrefixEnd == 0) return {};

    const Span localLine{{source.line_starts[line]},
                         {line + 1 < source.line_starts.size()
                              ? source.line_starts[line + 1]
                              : source.raw.size()}};
    NoteInfluenceScope result = ScopeForSpan(
        NoteInfluenceScopeKind::ContainerContent, source, localLine);
    result.protected_prefix_end = protectedPrefixEnd;
    return result;
}

[[nodiscard]] NoteInfluenceScope ScopeForInlineMathContentEdit(
    const NoteTextModel& source,
    const MathSpan& math,
    size_t mathIndex,
    size_t line,
    const TextEdit& edit) {
    if (math.kind != MathKind::Inline ||
        math.span.end < math.span.start ||
        math.content_span.end <= math.content_span.start ||
        math.content_span.start < math.span.start ||
        math.content_span.end > math.span.end ||
        !math.diagnostic_ids.empty() ||
        math.content_span.end.value > source.raw.size() ||
        line >= source.line_starts.size()) {
        return {};
    }

    const NoteInfluenceLineRange mathLines = LinesForSpan(
        source.line_starts, source.raw.size(), math.span);
    if (!mathLines.valid || mathLines.first != line || mathLines.last != line ||
        edit.start.value < math.content_span.start.value ||
        edit.start.value > math.content_span.end.value ||
        edit.deleted_len > math.content_span.end.value - edit.start.value) {
        return {};
    }

    const std::wstring_view content = std::wstring_view(source.raw).substr(
        math.content_span.start.value,
        math.content_span.end.value - math.content_span.start.value);
    // NormalizeMathText is the identity exactly for this deliberately small
    // subset. TeX commands and Markdown-significant punctuation may change
    // diagnostics or delimiter meaning, so they retain the full-parse path.
    if (content.empty() || ContainsPotentialMarkup(content) ||
        ContainsPotentialMarkup(edit.inserted_text) || math.normalized_tex != content) {
        return {};
    }
    size_t firstLiteral = 0;
    while (firstLiteral < content.size() && IsLineWhitespace(content[firstLiteral])) {
        ++firstLiteral;
    }
    if (firstLiteral == content.size() ||
        firstLiteral == std::numeric_limits<size_t>::max()) {
        return {};
    }
    const size_t protectedPrefixEnd = math.content_span.start.value + firstLiteral + 1;
    if (edit.start.value < protectedPrefixEnd) return {};

    const Span localLine{{source.line_starts[line]},
                         {line + 1 < source.line_starts.size()
                              ? source.line_starts[line + 1]
                              : source.raw.size()}};
    NoteInfluenceScope result = ScopeForSpan(
        NoteInfluenceScopeKind::InlineMathContent, source, localLine);
    result.protected_prefix_end = protectedPrefixEnd;
    result.source_node_index = mathIndex;
    return result;
}

[[nodiscard]] bool IsContainerBlock(const BlockNode& block) {
    return block.kind == BlockKind::List || block.kind == BlockKind::Quote ||
           block.kind == BlockKind::FencedContainer;
}

// MD4C's content span for a list/quote can begin after its source marker.
// The marker itself is structural: an insertion immediately before it must
// resolve to the same container, not be misclassified as an unrelated
// paragraph edit.  Keep this normalization local to the influence index;
// the parsed document's text-content spans remain unchanged.
[[nodiscard]] Span ContainerOwnerSpan(const NoteTextModel& source,
                                      const BlockNode& block) {
    Span result = block.span;
    if (result.end < result.start) return {};
    result.start.value = std::min(result.start.value, source.raw.size());
    result.end.value = std::min(result.end.value, source.raw.size());
    if (block.loc.line > 0) {
        const size_t line = static_cast<size_t>(block.loc.line - 1);
        if (line < source.line_starts.size()) {
            result.start.value = std::min(result.start.value, source.line_starts[line]);
        }
    }
    if (result.end < result.start) result.end = result.start;
    return result;
}

[[nodiscard]] bool IsNarrowerContainerSpan(const Span& candidate,
                                            const Span& current) {
    if (candidate.end < candidate.start || current.end < current.start) {
        return false;
    }
    const size_t candidateLength = candidate.end.value - candidate.start.value;
    const size_t currentLength = current.end.value - current.start.value;
    if (candidateLength != currentLength) return candidateLength < currentLength;
    // Equal source spans can occur in nested syntactic wrappers.  Prefer the
    // later start, which is the inner owner and never expands the scope.
    return candidate.start.value > current.start.value;
}

void AddIndexEntry(NoteInfluenceIndex* index,
                   NoteInfluenceScopeKind kind,
                   const NoteTextModel& source,
                   Span span,
                   size_t sourceNodeIndex = static_cast<size_t>(-1)) {
    if (!index) return;
    const NoteInfluenceLineRange lines =
        LinesForSpan(source.line_starts, source.raw.size(), span);
    if (!lines.valid) return;
    index->entries.push_back(
        NoteInfluenceIndexEntry{kind, span, lines, sourceNodeIndex});
}

} // namespace

NoteInfluenceIndex BuildNoteInfluenceIndex(const NoteTextModel& source,
                                           const NoteDocument& document) {
    NoteInfluenceIndex index;
    if (source.revision == 0 || !NoteDocumentMatchesTextModel(document, source) ||
        !ValidLineStarts(source.line_starts, source.raw.size())) {
        return index;
    }

    index.source_revision = source.revision;

    // Preserve this priority in each row's candidate list: a table owns its
    // shared geometry before nested inline syntax can claim one cell.
    for (const BlockNode& block : document.blocks) {
        if (block.kind == BlockKind::Table) {
            AddIndexEntry(&index, NoteInfluenceScopeKind::SharedTableGeometry,
                          source, block.span);
        }
    }
    for (size_t mathIndex = 0; mathIndex < document.math_spans.size(); ++mathIndex) {
        const MathSpan& math = document.math_spans[mathIndex];
        AddIndexEntry(&index,
                      math.kind == MathKind::Block
                          ? NoteInfluenceScopeKind::BlockMath
                          : NoteInfluenceScopeKind::InlineMath,
                      source, math.span, mathIndex);
    }
    for (const BlockNode& block : document.blocks) {
        if (block.kind == BlockKind::CodeBlock) {
            AddIndexEntry(&index, NoteInfluenceScopeKind::CodeBlock,
                          source, block.span);
        }
    }
    for (const InlineNode& inlineNode : document.inlines) {
        if (inlineNode.kind == InlineKind::Code) {
            AddIndexEntry(&index, NoteInfluenceScopeKind::InlineCode,
                          source, inlineNode.span);
        } else if (inlineNode.kind == InlineKind::Link ||
                   inlineNode.kind == InlineKind::Image) {
            AddIndexEntry(&index, NoteInfluenceScopeKind::InlineLink,
                          source, inlineNode.span);
        }
    }
    for (const BlockNode& block : document.blocks) {
        if (IsContainerBlock(block)) {
            AddIndexEntry(&index, NoteInfluenceScopeKind::Container,
                          source, ContainerOwnerSpan(source, block));
        }
    }

    // Materialize a compact row-to-entry index after candidates are complete.
    // The build is paid only for a published syntax snapshot; input lookup
    // becomes a bounded scan of the current row's candidates.
    index.line_entry_offsets.assign(source.line_starts.size() + 1, 0);
    for (const NoteInfluenceIndexEntry& entry : index.entries) {
        if (!entry.source_lines.valid ||
            entry.source_lines.last >= source.line_starts.size()) {
            return {};
        }
        for (size_t line = entry.source_lines.first; line <= entry.source_lines.last; ++line) {
            ++index.line_entry_offsets[line + 1];
        }
    }
    for (size_t line = 1; line < index.line_entry_offsets.size(); ++line) {
        index.line_entry_offsets[line] += index.line_entry_offsets[line - 1];
    }
    index.line_entry_indices.resize(index.line_entry_offsets.back());
    std::vector<size_t> nextOffset = index.line_entry_offsets;
    for (size_t entryIndex = 0; entryIndex < index.entries.size(); ++entryIndex) {
        const NoteInfluenceLineRange lines = index.entries[entryIndex].source_lines;
        for (size_t line = lines.first; line <= lines.last; ++line) {
            index.line_entry_indices[nextOffset[line]++] = entryIndex;
        }
    }
    return index;
}

bool NoteInfluenceIndexMatchesTextModel(const NoteInfluenceIndex& index,
                                        const NoteTextModel& source) noexcept {
    return source.revision != 0 && index.source_revision == source.revision &&
        index.line_entry_offsets.size() == source.line_starts.size() + 1 &&
        !index.line_entry_offsets.empty() &&
        index.line_entry_offsets.back() == index.line_entry_indices.size();
}

NoteInfluenceScope ResolveNoteInfluenceScope(const NoteTextModel& source,
                                              const NoteDocument& document,
                                              const TextEdit& edit,
                                              const NoteInfluenceIndex* index) {
    NoteInfluenceScope unknown;
    if (source.revision == 0 || !NoteDocumentMatchesTextModel(document, source) ||
        !ValidLineStarts(source.line_starts, source.raw.size()) ||
        edit.start.value > source.raw.size() ||
        edit.deleted_len > source.raw.size() - edit.start.value) {
        return unknown;
    }

    const std::wstring_view removed = source.raw.substr(edit.start.value, edit.deleted_len);
    if (ContainsPotentialMarkup(removed) || ContainsPotentialMarkup(edit.inserted_text)) {
        return unknown;
    }

    const size_t line = FindLineByOffset(source.line_starts, edit.start.value);
    const Span localLine{{source.line_starts[line]},
                         {line + 1 < source.line_starts.size()
                              ? source.line_starts[line + 1]
                              : source.raw.size()}};

    if (index) {
        // A stale index is never used and must not silently trigger a
        // document-wide scan while handling input.  The caller falls back to
        // the conservative dirty-graph/reparse route instead.
        if (!NoteInfluenceIndexMatchesTextModel(*index, source) ||
            line + 1 >= index->line_entry_offsets.size()) {
            return unknown;
        }
        const size_t firstEntry = index->line_entry_offsets[line];
        const size_t afterLastEntry = index->line_entry_offsets[line + 1];
        if (firstEntry > afterLastEntry || afterLastEntry > index->line_entry_indices.size()) {
            return unknown;
        }
        const NoteInfluenceIndexEntry* narrowestContainer = nullptr;
        for (size_t entryOffset = firstEntry; entryOffset < afterLastEntry; ++entryOffset) {
            const size_t entryIndex = index->line_entry_indices[entryOffset];
            if (entryIndex >= index->entries.size()) return unknown;
            const NoteInfluenceIndexEntry& entry = index->entries[entryIndex];
            if (!SpanContainsOrIntersects(entry.span, edit)) continue;
            if (entry.kind == NoteInfluenceScopeKind::Container) {
                if (!narrowestContainer ||
                    IsNarrowerContainerSpan(entry.span, narrowestContainer->span)) {
                    narrowestContainer = &entry;
                }
                continue;
            }
            if (entry.kind == NoteInfluenceScopeKind::CodeBlock) {
                return ScopeForCodeBlockEdit(source, line, edit);
            }
            if (entry.kind == NoteInfluenceScopeKind::InlineMath) {
                if (entry.source_node_index >= document.math_spans.size()) {
                    return unknown;
                }
                const NoteInfluenceScope local = ScopeForInlineMathContentEdit(
                    source, document.math_spans[entry.source_node_index],
                    entry.source_node_index, line, edit);
                return local.kind == NoteInfluenceScopeKind::InlineMathContent
                    ? local
                    : ScopeForSpan(entry.kind, source, entry.span);
            }
            return entry.kind == NoteInfluenceScopeKind::InlineLink
                ? ScopeForSpan(entry.kind, source, localLine)
                : ScopeForSpan(entry.kind, source, entry.span);
        }
        if (narrowestContainer) {
            const NoteInfluenceScope local = ScopeForContainerContentEdit(
                source, line, edit);
            if (local.kind == NoteInfluenceScopeKind::ContainerContent) return local;
            return ScopeForSpan(NoteInfluenceScopeKind::Container,
                                source, narrowestContainer->span);
        }
    } else {

        // Compatibility path for pure callers that do not retain the
        // committed index.  The editor kernel always supplies one.
        // Table cell width/align and borders are shared by every table row.
        for (const BlockNode& block : document.blocks) {
            if (block.kind == BlockKind::Table && SpanContainsOrIntersects(block.span, edit)) {
                return ScopeForSpan(NoteInfluenceScopeKind::SharedTableGeometry,
                                    source, block.span);
            }
        }

        for (size_t mathIndex = 0; mathIndex < document.math_spans.size(); ++mathIndex) {
            const MathSpan& math = document.math_spans[mathIndex];
            if (!SpanContainsOrIntersects(math.span, edit)) continue;
            if (math.kind == MathKind::Inline) {
                const NoteInfluenceScope local = ScopeForInlineMathContentEdit(
                    source, math, mathIndex, line, edit);
                if (local.kind == NoteInfluenceScopeKind::InlineMathContent) return local;
            }
            return ScopeForSpan(math.kind == MathKind::Block
                                    ? NoteInfluenceScopeKind::BlockMath
                                    : NoteInfluenceScopeKind::InlineMath,
                                source, math.span);
        }

        for (const BlockNode& block : document.blocks) {
            if (block.kind == BlockKind::CodeBlock && SpanContainsOrIntersects(block.span, edit)) {
                return ScopeForCodeBlockEdit(source, line, edit);
            }
        }

        for (const InlineNode& inlineNode : document.inlines) {
            if (!SpanContainsOrIntersects(inlineNode.span, edit)) continue;
            if (inlineNode.kind == InlineKind::Code) {
                return ScopeForSpan(NoteInfluenceScopeKind::InlineCode,
                                    source, inlineNode.span);
            }
            if (inlineNode.kind == InlineKind::Link || inlineNode.kind == InlineKind::Image) {
                return ScopeForSpan(NoteInfluenceScopeKind::InlineLink,
                                    source, localLine);
            }
        }

        const BlockNode* narrowestContainer = nullptr;
        Span narrowestContainerSpan{};
        for (const BlockNode& block : document.blocks) {
            if (!IsContainerBlock(block)) {
                continue;
            }
            const Span containerSpan = ContainerOwnerSpan(source, block);
            if (!SpanContainsOrIntersects(containerSpan, edit)) continue;
            if (!narrowestContainer ||
                IsNarrowerContainerSpan(containerSpan, narrowestContainerSpan)) {
                narrowestContainer = &block;
                narrowestContainerSpan = containerSpan;
            }
        }
        if (narrowestContainer) {
            const NoteInfluenceScope local = ScopeForContainerContentEdit(
                source, line, edit);
            if (local.kind == NoteInfluenceScopeKind::ContainerContent) return local;
            return ScopeForSpan(NoteInfluenceScopeKind::Container,
                                source, narrowestContainerSpan);
        }
    }

    // If the edited source row contains syntax delimiters but does not belong
    // to one of the exact groups above, an ordinary character can still alter
    // a list, quote, setext heading or legacy-tag interpretation.  Let the
    // conservative dirty-graph path inspect the post-edit text in that case.
    if (ContainsPotentialMarkup(LineTextAt(source, line)) ||
        IsContainerOrDelimiterLine(LineTextAt(source, line)) ||
        EditLeavesOnlyWhitespaceOnLine(source, line, edit)) {
        return unknown;
    }
    return ScopeForSpan(NoteInfluenceScopeKind::LocalLine, source, localLine);
}

NoteInfluenceScope ResolveNoteInfluenceContinuationScope(
    const NoteTextModel& source,
    const NoteInfluenceScope& previous,
    const TextEdit& edit) {
    NoteInfluenceScope unknown;
    if ((previous.kind != NoteInfluenceScopeKind::LocalLine &&
         previous.kind != NoteInfluenceScopeKind::CodeBlockContent &&
         previous.kind != NoteInfluenceScopeKind::ContainerContent &&
         previous.kind != NoteInfluenceScopeKind::InlineMathContent) ||
        previous.source_revision == 0 ||
        previous.source_revision == std::numeric_limits<uint64_t>::max() ||
        source.revision != previous.source_revision + 1 ||
        !previous.permits_local_dirty_graph || !previous.source_lines.valid ||
        previous.source_lines.first != previous.source_lines.last ||
        !ValidLineStarts(source.line_starts, source.raw.size()) ||
        edit.start.value > source.raw.size() ||
        edit.deleted_len > source.raw.size() - edit.start.value) {
        return unknown;
    }

    const std::wstring_view removed = source.raw.substr(edit.start.value, edit.deleted_len);
    if (ContainsPotentialMarkup(removed) || ContainsPotentialMarkup(edit.inserted_text)) {
        return unknown;
    }
    const size_t line = FindLineByOffset(source.line_starts, edit.start.value);
    if (line != previous.source_lines.first) {
        return unknown;
    }
    if (previous.kind == NoteInfluenceScopeKind::CodeBlockContent) {
        return ScopeForCodeBlockEdit(source, line, edit);
    }
    if (previous.kind == NoteInfluenceScopeKind::ContainerContent) {
        return ScopeForContainerContentEdit(source, line, edit);
    }
    if (previous.kind == NoteInfluenceScopeKind::InlineMathContent) {
        if (previous.protected_prefix_end == 0 ||
            edit.start.value < previous.protected_prefix_end) {
            return unknown;
        }
        const Span localLine{{source.line_starts[line]},
                             {line + 1 < source.line_starts.size()
                                  ? source.line_starts[line + 1]
                                  : source.raw.size()}};
        NoteInfluenceScope result = ScopeForSpan(
            NoteInfluenceScopeKind::InlineMathContent, source, localLine);
        result.protected_prefix_end = previous.protected_prefix_end;
        result.source_node_index = previous.source_node_index;
        return result;
    }
    if (ContainsPotentialMarkup(LineTextAt(source, line)) ||
        IsContainerOrDelimiterLine(LineTextAt(source, line)) ||
        EditLeavesOnlyWhitespaceOnLine(source, line, edit)) {
        return unknown;
    }
    const Span localLine{{source.line_starts[line]},
                         {line + 1 < source.line_starts.size()
                              ? source.line_starts[line + 1]
                              : source.raw.size()}};
    return ScopeForSpan(NoteInfluenceScopeKind::LocalLine, source, localLine);
}

bool NoteCodeBlockContentEditPreservesFenceState(
    std::wstring_view beforeLine,
    Utf16CodeUnitOffset lineStart,
    const TextEdit& edit) {
    if (edit.start.value < lineStart.value ||
        edit.start.value > lineStart.value + beforeLine.size() ||
        edit.deleted_len > lineStart.value + beforeLine.size() - edit.start.value ||
        IsCodeFenceDelimiterLine(beforeLine)) {
        return false;
    }

    // Normal characters can still turn a line such as "x```" into a closing
    // fence by deleting the prefix.  Evaluate only the edited source row;
    // this is bounded by the row length and never copies or scans the note.
    const size_t localStart = edit.start.value - lineStart.value;
    std::wstring afterLine;
    afterLine.reserve(beforeLine.size() - edit.deleted_len + edit.inserted_text.size());
    afterLine.append(beforeLine.substr(0, localStart));
    afterLine.append(edit.inserted_text);
    afterLine.append(beforeLine.substr(localStart + edit.deleted_len));
    return !IsCodeFenceDelimiterLine(afterLine);
}

size_t NoteContainerContentProtectedPrefixEnd(
    std::wstring_view beforeLine,
    Utf16CodeUnitOffset lineStart,
    const TextEdit& edit) {
    if (edit.start.value < lineStart.value ||
        edit.start.value > lineStart.value + beforeLine.size() ||
        edit.deleted_len > lineStart.value + beforeLine.size() - edit.start.value) {
        return 0;
    }
    const size_t bodyStart = FindContainerBodyStart(beforeLine);
    if (bodyStart == 0 || bodyStart >= beforeLine.size() ||
        ContainsPotentialMarkup(beforeLine.substr(bodyStart))) {
        return 0;
    }
    size_t firstBody = bodyStart;
    while (firstBody < beforeLine.size() && IsLineWhitespace(beforeLine[firstBody])) {
        ++firstBody;
    }
    if (firstBody == beforeLine.size() ||
        firstBody == std::numeric_limits<size_t>::max() ||
        ContainsPotentialMarkup(edit.inserted_text) ||
        edit.start.value < lineStart.value + firstBody + 1) {
        return 0;
    }
    return lineStart.value + firstBody + 1;
}

bool NoteInfluenceScopeAllowsIncrementalSyntaxPatch(
    const NoteInfluenceScope& scope) noexcept {
    if (!scope.permits_local_dirty_graph || !scope.source_lines.valid) return false;
    switch (scope.kind) {
    case NoteInfluenceScopeKind::LocalLine:
    case NoteInfluenceScopeKind::InlineLink:
    case NoteInfluenceScopeKind::CodeBlock:
    case NoteInfluenceScopeKind::CodeBlockContent:
    case NoteInfluenceScopeKind::ContainerContent:
    case NoteInfluenceScopeKind::InlineMathContent:
    case NoteInfluenceScopeKind::SharedTableGeometry:
        return true;
    case NoteInfluenceScopeKind::Unknown:
    case NoteInfluenceScopeKind::Container:
    case NoteInfluenceScopeKind::InlineCode:
    case NoteInfluenceScopeKind::InlineMath:
    case NoteInfluenceScopeKind::BlockMath:
        return false;
    }
    return false;
}

} // namespace note
