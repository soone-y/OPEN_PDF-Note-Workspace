#include "note/note_dirty_graph.h"

#include <algorithm>
#include <string>

namespace note {
namespace {

size_t CountLogicalLineBreaks(std::wstring_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            ++count;
        } else if (text[i] == L'\n') {
            ++count;
        }
    }
    return count;
}

bool ValidLineStarts(const std::vector<size_t>& starts, size_t textSize) {
    if (starts.empty() || starts.front() != 0 || starts.back() > textSize) return false;
    return std::is_sorted(starts.begin(), starts.end()) &&
           std::adjacent_find(starts.begin(), starts.end()) == starts.end();
}

size_t FindLineByOffset(const std::vector<size_t>& starts, size_t offset) {
    if (starts.empty()) return 0;
    const auto upper = std::upper_bound(starts.begin(), starts.end(), offset);
    if (upper == starts.begin()) return 0;
    return static_cast<size_t>(std::distance(starts.begin(), upper) - 1);
}

NoteDirtySyntaxFeature AddFeature(NoteDirtySyntaxFeature features,
                                  NoteDirtySyntaxFeature feature) {
    return static_cast<NoteDirtySyntaxFeature>(
        static_cast<uint32_t>(features) | static_cast<uint32_t>(feature));
}

bool ContainsAny(std::wstring_view text, std::wstring_view chars) {
    return text.find_first_of(chars) != std::wstring_view::npos;
}

bool ContainsFenceToken(std::wstring_view text) {
    return text.find(L"```") != std::wstring_view::npos ||
           text.find(L"~~~") != std::wstring_view::npos;
}

bool ContainsBlockMathBoundary(std::wstring_view text) {
    return text.find(L"$$") != std::wstring_view::npos ||
           text.find(L"\\[") != std::wstring_view::npos ||
           text.find(L"\\]") != std::wstring_view::npos;
}

bool ContainsMathToken(std::wstring_view text) {
    return text.find(L'$') != std::wstring_view::npos ||
           text.find(L"\\(") != std::wstring_view::npos ||
           text.find(L"\\)") != std::wstring_view::npos ||
           text.find(L"\\[") != std::wstring_view::npos ||
           text.find(L"\\]") != std::wstring_view::npos;
}

bool ContainsLegacyStyleToken(std::wstring_view text) {
    return text.find(L'<') != std::wstring_view::npos ||
           text.find(L'>') != std::wstring_view::npos;
}

bool ContainsLinkToken(std::wstring_view text) {
    return ContainsAny(text, L"[]") ||
           text.find(L"](") != std::wstring_view::npos ||
           text.find(L"<link=") != std::wstring_view::npos ||
           text.find(L"<id=") != std::wstring_view::npos;
}

NoteDirtySyntaxFeature ClassifySyntaxFeatures(std::wstring_view changedText,
                                              std::wstring_view affectedLine) {
    NoteDirtySyntaxFeature features = NoteDirtySyntaxFeature::None;
    if (ContainsFenceToken(changedText) || ContainsFenceToken(affectedLine)) {
        features = AddFeature(features, NoteDirtySyntaxFeature::CodeFence);
    }
    if (ContainsLegacyStyleToken(changedText) ||
        ContainsLegacyStyleToken(affectedLine)) {
        features = AddFeature(features, NoteDirtySyntaxFeature::LegacyStyle);
    }
    if (ContainsMathToken(changedText) || ContainsMathToken(affectedLine)) {
        features = AddFeature(features, NoteDirtySyntaxFeature::Math);
    }
    if (ContainsLinkToken(changedText) || ContainsLinkToken(affectedLine)) {
        features = AddFeature(features, NoteDirtySyntaxFeature::Link);
    }
    if (ContainsAny(changedText, L"#*_~:>!") ||
        ContainsAny(affectedLine, L"#*_~:>!")) {
        features = AddFeature(features, NoteDirtySyntaxFeature::BlockStructure);
    }
    return features;
}

std::wstring_view LineTextAt(std::wstring_view text,
                             const std::vector<size_t>& starts,
                             size_t line) {
    if (starts.empty() || line >= starts.size()) return {};
    const size_t start = starts[line];
    const size_t end = line + 1 < starts.size() ? starts[line + 1] : text.size();
    return text.substr(start, end - start);
}

bool IsInsideDelimitedLines(std::wstring_view text,
                            const std::vector<size_t>& starts,
                            size_t line,
                            NoteDirtySyntaxFeature feature) {
    size_t boundaries = 0;
    const size_t limit = std::min(line, starts.size());
    for (size_t cursor = 0; cursor < limit; ++cursor) {
        const std::wstring_view candidate = LineTextAt(text, starts, cursor);
        if ((feature == NoteDirtySyntaxFeature::CodeFence &&
             ContainsFenceToken(candidate)) ||
            (feature == NoteDirtySyntaxFeature::Math &&
             ContainsBlockMathBoundary(candidate))) {
            ++boundaries;
        }
    }
    return boundaries % 2 != 0;
}

bool InlineMathBalanced(std::wstring_view line) {
    size_t dollars = 0;
    size_t parenOpen = 0;
    size_t parenClose = 0;
    size_t bracketOpen = 0;
    size_t bracketClose = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L'$' && (i == 0 || line[i - 1] != L'\\')) ++dollars;
        if (line[i] != L'\\' || i + 1 >= line.size()) continue;
        switch (line[i + 1]) {
        case L'(': ++parenOpen; break;
        case L')': ++parenClose; break;
        case L'[': ++bracketOpen; break;
        case L']': ++bracketClose; break;
        default: break;
        }
        ++i;
    }
    return dollars % 2 == 0 &&
           parenOpen == parenClose &&
           bracketOpen == bracketClose;
}

bool LegacyStyleBalanced(std::wstring_view line) {
    return static_cast<size_t>(std::count(line.begin(), line.end(), L'<')) ==
           static_cast<size_t>(std::count(line.begin(), line.end(), L'>'));
}

size_t FindPreviousFeatureLine(std::wstring_view text,
                               const std::vector<size_t>& starts,
                               size_t line,
                               NoteDirtySyntaxFeature feature) {
    for (size_t cursor = std::min(line, starts.size() - 1);; --cursor) {
        const std::wstring_view candidate = LineTextAt(text, starts, cursor);
        const bool matches = feature == NoteDirtySyntaxFeature::CodeFence
            ? ContainsFenceToken(candidate)
            : feature == NoteDirtySyntaxFeature::Math
                ? ContainsMathToken(candidate)
                : ContainsLegacyStyleToken(candidate);
        if (matches) return cursor;
        if (cursor == 0) break;
    }
    return line;
}

size_t FindNextFeatureLine(std::wstring_view text,
                           const std::vector<size_t>& starts,
                           size_t line,
                           NoteDirtySyntaxFeature feature) {
    for (size_t cursor = std::min(line + 1, starts.size()); cursor < starts.size(); ++cursor) {
        const std::wstring_view candidate = LineTextAt(text, starts, cursor);
        const bool matches = feature == NoteDirtySyntaxFeature::CodeFence
            ? ContainsFenceToken(candidate)
            : feature == NoteDirtySyntaxFeature::Math
                ? ContainsMathToken(candidate)
                : ContainsLegacyStyleToken(candidate);
        if (matches) return cursor;
    }
    return starts.size() - 1;
}

} // namespace

bool ContainsNoteStructuralText(std::wstring_view text) {
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'\r':
        case L'\n':
        case L'`':
        case L'*':
        case L'_':
        case L'~':
        case L'[':
        case L']':
        case L'(':
        case L')':
        case L'<':
        case L'>':
        case L'#':
        case L'!':
        case L'$':
        case L'\\':
        case L':':
            return true;
        default:
            break;
        }
    }
    return false;
}

bool HasNoteDirtySyntaxFeature(NoteDirtySyntaxFeature features,
                               NoteDirtySyntaxFeature feature) {
    return (static_cast<uint32_t>(features) & static_cast<uint32_t>(feature)) != 0;
}

bool IsOnlyNoteLineBreakText(std::wstring_view text) {
    if (text.empty()) return false;
    for (const wchar_t ch : text) {
        if (ch != L'\r' && ch != L'\n') return false;
    }
    return true;
}

bool TextEditsEqual(const TextEdit& lhs, const TextEdit& rhs) {
    return lhs.start == rhs.start &&
           lhs.deleted_len == rhs.deleted_len &&
           lhs.inserted_text == rhs.inserted_text;
}

NoteDirtyGraph BuildNoteDirtyGraph(std::wstring_view beforeText,
                                   const std::vector<size_t>& beforeLineStarts,
                                   const TextEdit& edit,
                                   bool renderActive) {
    NoteDirtyGraph graph;
    if (edit.deleted_len == 0 && edit.inserted_text.empty()) return graph;

    graph.has_edit = true;
    graph.edit = edit;
    graph.content_dirty = true;
    graph.syntax_dirty = true;
    graph.render_dirty = true;
    graph.layout_dirty = true;

    const bool editRangeValid = edit.start.value <= beforeText.size() &&
        edit.deleted_len <= beforeText.size() - std::min(edit.start.value, beforeText.size());
    const std::wstring_view deletedText = editRangeValid
        ? beforeText.substr(edit.start.value, edit.deleted_len)
        : std::wstring_view{};
    if (!editRangeValid) {
        graph.edit_kind = NoteEditKind::StructuralText;
    } else {
        const bool deletedLineBreakOnly =
            edit.deleted_len == 0 || IsOnlyNoteLineBreakText(deletedText);
        const bool insertedLineBreakOnly =
            edit.inserted_text.empty() || IsOnlyNoteLineBreakText(edit.inserted_text);
        if (deletedLineBreakOnly && insertedLineBreakOnly) {
            graph.edit_kind = NoteEditKind::LineBreakOnly;
        } else if (ContainsNoteStructuralText(deletedText) ||
                   ContainsNoteStructuralText(edit.inserted_text)) {
            graph.edit_kind = NoteEditKind::StructuralText;
        } else {
            graph.edit_kind = NoteEditKind::PlainText;
        }
    }
    graph.structure_dirty = graph.edit_kind == NoteEditKind::LineBreakOnly ||
                            graph.edit_kind == NoteEditKind::StructuralText;

    std::wstring changedText;
    changedText.reserve(deletedText.size() + edit.inserted_text.size());
    changedText.append(deletedText);
    changedText.append(edit.inserted_text);
    std::wstring afterText;
    if (editRangeValid) {
        afterText.assign(beforeText);
        afterText.replace(edit.start.value, edit.deleted_len, edit.inserted_text);
    }
    const std::vector<size_t> afterStarts = editRangeValid
        ? note::BuildLineStarts(afterText)
        : std::vector<size_t>{};
    const size_t affectedAfterLine = afterStarts.empty()
        ? 0
        : FindLineByOffset(afterStarts, std::min(edit.start.value, afterText.size()));
    const std::wstring_view affectedLine = afterStarts.empty()
        ? std::wstring_view{}
        : LineTextAt(afterText, afterStarts, affectedAfterLine);
    graph.syntax_features = ClassifySyntaxFeatures(changedText, affectedLine);
    if (!editRangeValid) {
        graph.syntax_features = AddFeature(
            graph.syntax_features, NoteDirtySyntaxFeature::BlockStructure);
    }
    const bool insideCodeFence = !afterStarts.empty() &&
        IsInsideDelimitedLines(afterText, afterStarts, affectedAfterLine,
                               NoteDirtySyntaxFeature::CodeFence);
    const bool insideBlockMath = !afterStarts.empty() &&
        IsInsideDelimitedLines(afterText, afterStarts, affectedAfterLine,
                               NoteDirtySyntaxFeature::Math);
    if (insideCodeFence) {
        graph.syntax_features = AddFeature(
            graph.syntax_features, NoteDirtySyntaxFeature::CodeFence);
    }
    if (insideBlockMath) {
        graph.syntax_features = AddFeature(
            graph.syntax_features, NoteDirtySyntaxFeature::Math);
    }
    graph.propagation = NoteDirtyPropagation::LocalLine;

    if (!renderActive) return graph;
    const std::vector<size_t> fallbackStarts = ValidLineStarts(beforeLineStarts, beforeText.size())
        ? std::vector<size_t>{}
        : note::BuildLineStarts(beforeText);
    const std::vector<size_t>& starts = fallbackStarts.empty()
        ? beforeLineStarts
        : fallbackStarts;
    if (starts.empty()) return graph;

    const size_t maxLine = starts.size() - 1;
    const size_t safeStart = std::min(edit.start.value, beforeText.size());
    const size_t safeDeletedEnd = editRangeValid
        ? edit.start.value + edit.deleted_len
        : beforeText.size();
    size_t first = FindLineByOffset(starts, safeStart);
    size_t last = first;
    if (safeDeletedEnd > safeStart) {
        last = std::max(last, FindLineByOffset(starts, safeDeletedEnd - 1));
    }
    const size_t insertedBreaks = CountLogicalLineBreaks(edit.inserted_text);
    const size_t deletedBreaks = CountLogicalLineBreaks(deletedText);
    last = std::max(last, first + std::max(insertedBreaks, deletedBreaks));

    graph.line_count_may_change = !editRangeValid || insertedBreaks > 0 || deletedBreaks > 0;
    graph.render_stale = true;
    graph.stale_lines.valid = true;
    graph.stale_lines.first = std::min(first, maxLine);
    graph.stale_lines.last = std::min(last, maxLine);
    if (graph.line_count_may_change) {
        graph.propagation = NoteDirtyPropagation::FullDocument;
        graph.stale_lines.first = 0;
        graph.stale_lines.last = maxLine;
    } else if (!afterStarts.empty()) {
        const size_t afterMaxLine = afterStarts.size() - 1;
        size_t expandedFirst = std::min(first, afterMaxLine);
        size_t expandedLast = std::min(last, afterMaxLine);
        bool delimiterRegion = false;
        if (HasNoteDirtySyntaxFeature(
                graph.syntax_features, NoteDirtySyntaxFeature::CodeFence)) {
            expandedFirst = std::min(
                expandedFirst,
                FindPreviousFeatureLine(afterText, afterStarts, affectedAfterLine,
                                        NoteDirtySyntaxFeature::CodeFence));
            expandedLast = std::max(
                expandedLast,
                FindNextFeatureLine(afterText, afterStarts, affectedAfterLine,
                                    NoteDirtySyntaxFeature::CodeFence));
            delimiterRegion = true;
        }
        if (HasNoteDirtySyntaxFeature(
                graph.syntax_features, NoteDirtySyntaxFeature::Math) &&
            (insideBlockMath || !InlineMathBalanced(affectedLine))) {
            expandedFirst = std::min(
                expandedFirst,
                FindPreviousFeatureLine(afterText, afterStarts, affectedAfterLine,
                                        NoteDirtySyntaxFeature::Math));
            expandedLast = std::max(
                expandedLast,
                FindNextFeatureLine(afterText, afterStarts, affectedAfterLine,
                                    NoteDirtySyntaxFeature::Math));
            delimiterRegion = true;
        }
        if (HasNoteDirtySyntaxFeature(
                graph.syntax_features, NoteDirtySyntaxFeature::LegacyStyle) &&
            !LegacyStyleBalanced(affectedLine)) {
            expandedFirst = std::min(
                expandedFirst,
                FindPreviousFeatureLine(afterText, afterStarts, affectedAfterLine,
                                        NoteDirtySyntaxFeature::LegacyStyle));
            expandedLast = std::max(
                expandedLast,
                FindNextFeatureLine(afterText, afterStarts, affectedAfterLine,
                                    NoteDirtySyntaxFeature::LegacyStyle));
            delimiterRegion = true;
        }
        graph.stale_lines.first = std::min(expandedFirst, maxLine);
        graph.stale_lines.last = std::min(expandedLast, maxLine);
        if (delimiterRegion) {
            graph.propagation = NoteDirtyPropagation::DelimiterRegion;
        }
    }
    return graph;
}

bool NoteDirtyGraphAllowsRenderEarlyStop(const NoteDirtyGraph& graph,
                                         bool lineCountChanged) {
    return !lineCountChanged &&
           graph.edit_kind == NoteEditKind::PlainText &&
           graph.syntax_features == NoteDirtySyntaxFeature::None;
}

bool NoteDirtyGraphAllowsLineSpacingFastPath(const NoteDirtyGraph& graph) {
    return graph.edit_kind == NoteEditKind::PlainText &&
           graph.syntax_features == NoteDirtySyntaxFeature::None &&
           graph.edit.deleted_len == 0 &&
           graph.edit.inserted_text.size() <= 4;
}

} // namespace note
