#pragma once

#include "note/note_export.h"
#include "note/note_parser.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace note {

// A pure proposal in the caller's UTF-16 source coordinates. The UI owns
// application, IME exclusion and the single, non-coalescing history entry.
struct MarkdownBoldEdit {
    TextEdit edit;
    Span selection; // relative to edit.inserted_text
};

namespace markdown_bold_detail {
struct Pair { size_t open; size_t close; };
inline bool Escaped(std::wstring_view raw, size_t at) {
    size_t count = 0;
    while (at > 0 && raw[--at] == L'\\') ++count;
    return count % 2 != 0;
}

// The AST proves emphasis; source spelling proves that it uses asterisks.
// A snippet proof handles hidden syntax between markers and text, such as
// **[label](target)**, without mistaking stars in a link destination for closes.
[[nodiscard]] inline std::optional<Pair> FindPair(const NoteTextModel& model,
                                                const std::vector<Span>& leaves,
                                                Span content, size_t* proofBudget, bool* budgetExceeded) {
    const auto& raw = model.raw;
    const size_t start = content.start.value, end = content.end.value;
    if (start >= end || end > raw.size()) return std::nullopt;
    if (start >= 2 && end + 2 <= raw.size() &&
        raw.compare(start - 2, 2, L"**") == 0 && !Escaped(raw, start - 2) &&
        raw.compare(end, 2, L"**") == 0 && !Escaped(raw, end)) return Pair{start - 2, end};
    size_t left = 0, right = raw.size();
    const auto prior = std::lower_bound(leaves.begin(), leaves.end(), start,
        [](Span span, size_t at) { return span.end.value <= at; });
    if (prior != leaves.begin()) left = std::prev(prior)->end.value;
    const auto next = std::lower_bound(leaves.begin(), leaves.end(), end,
        [](Span span, size_t at) { return span.start.value < at; });
    if (next != leaves.end()) right = next->start.value;
    for (size_t open = start; open > left;) {
        --open;
        if (raw.compare(open, 2, L"**") != 0 || Escaped(raw, open)) continue;
        for (size_t close = end; close + 2 <= right; ++close) {
            if (raw.compare(close, 2, L"**") != 0 || Escaped(raw, close)) continue;
            const size_t length = close + 2 - open;
            // Bound all snippet re-parsing for one command, including link
            // targets containing many stars. Failure leaves source untouched.
            if (length > *proofBudget) { *budgetExceeded = true; return std::nullopt; }
            *proofBudget -= length;
            const auto local = MakeNoteTextModel(model.meta, raw.substr(open, close + 2 - open), 1);
            const auto parsed = ParseNoteDocument(local);
            if (NoteDocumentHasFatalParseFailure(parsed)) return std::nullopt;
            const bool exact = std::any_of(parsed.inlines.begin(), parsed.inlines.end(), [&](const auto& node) {
                return node.kind == InlineKind::Strong && node.span.start.value == start - open &&
                    node.span.end.value == end - open;
            });
            const bool extraText = std::any_of(parsed.inlines.begin(), parsed.inlines.end(), [&](const auto& node) {
                return node.kind == InlineKind::Text &&
                    (node.span.start.value < start - open || node.span.end.value > end - open);
            });
            if (exact && !extraText) return Pair{open, close};
        }
    }
    return std::nullopt;
}

inline void Merge(std::vector<Span>* ranges) {
    std::sort(ranges->begin(), ranges->end(), [](Span a, Span b) { return a.start < b.start; });
    size_t count = 0;
    for (Span span : *ranges) {
        if (span.end <= span.start) continue;
        if (count && span.start <= (*ranges)[count - 1].end)
            (*ranges)[count - 1].end = std::max((*ranges)[count - 1].end, span.end);
        else (*ranges)[count++] = span;
    }
    ranges->resize(count);
}
} // namespace markdown_bold_detail

[[nodiscard]] inline std::optional<MarkdownBoldEdit> BuildMarkdownBoldToggle(
    const NoteTextModel& model, Span selection) {
    using namespace markdown_bold_detail;
    const auto& raw = model.raw;
    if (selection.start > selection.end || selection.end.value > raw.size()) return std::nullopt;
    const auto doc = ParseNoteDocument(model);
    if (NoteDocumentHasFatalParseFailure(doc)) return std::nullopt;
    const bool caret = selection.start == selection.end;
    // Never edit a literal's interior. Selecting the entire inline-code
    // notation can still wrap it, while its internal asterisks stay untouched.
    for (const auto& block : doc.blocks) {
        if (block.kind == BlockKind::CodeBlock && block.span.start <= selection.start &&
            selection.end <= block.span.end) return std::nullopt;
    }
    for (const auto& node : doc.inlines) {
        if (node.kind == InlineKind::Code && node.span.start <= selection.start &&
            selection.end <= node.span.end) return std::nullopt;
    }
    for (const auto& math : doc.math_spans) {
        if (math.span.start < selection.start && selection.end < math.span.end) return std::nullopt;
    }
    std::vector<Span> leaves;
    for (const auto& node : doc.inlines) if (node.kind == InlineKind::Text) leaves.push_back(node.span);
    Merge(&leaves);
    std::vector<Pair> pairs;
    size_t proofBudget = std::max<size_t>(4096, raw.size());
    bool budgetExceeded = false;
    for (const auto& node : doc.inlines) {
        if (node.kind != InlineKind::Strong ||
            node.span.start.value > selection.end.value + std::min<size_t>(2, raw.size() - selection.end.value)) continue;
        const auto pair = FindPair(model, leaves, node.span, &proofBudget, &budgetExceeded);
        if (budgetExceeded) return std::nullopt;
        if (!pair) continue;
        size_t extentEnd = pair->close + 2;
        if (caret) while (extentEnd < raw.size() && raw[extentEnd] == L'*') ++extentEnd;
        if (pair->open <= selection.end.value && selection.start.value <= extentEnd) pairs.push_back(*pair);
    }
    if (caret) {
        if (!pairs.empty()) {
            const auto pair = *std::min_element(pairs.begin(), pairs.end(), [](Pair a, Pair b) {
                return a.close - a.open < b.close - b.open;
            });
            size_t end = std::max(pair.close + 2, selection.end.value);
            std::wstring body = raw.substr(pair.open, end - pair.open);
            body.erase(pair.close - pair.open, 2);
            body.erase(0, 2);
            const size_t old = selection.start.value;
            const size_t next = old <= pair.open + 2 ? 0 : old <= pair.close ? old - pair.open - 2 :
                old - pair.open - 2 - std::min<size_t>(2, old - pair.close);
            return MarkdownBoldEdit{{{pair.open}, end - pair.open, std::move(body)}, {{next}, {next}}};
        }
        const size_t at = selection.start.value;
        // Empty **|** is a pending shortcut pair, not parsed strong emphasis.
        if (at >= 2 && at + 2 <= raw.size() && raw.compare(at - 2, 4, L"****") == 0 &&
            !Escaped(raw, at - 2) && (at == 2 || raw[at - 3] != L'*') &&
            (at + 2 == raw.size() || raw[at + 2] != L'*'))
            return MarkdownBoldEdit{{{at - 2}, 4, L""}, {{0}, {0}}};
        return MarkdownBoldEdit{{selection.start, 0, L"****"}, {{2}, {2}}};
    }
    // Include crossing pairs so the unselected prefix/suffix keep their bold.
    size_t first = selection.start.value, last = selection.end.value;
    for (const auto pair : pairs) { first = std::min(first, pair.open); last = std::max(last, pair.close + 2); }
    std::vector<Span> removed;
    for (const auto pair : pairs) {
        removed.push_back({{pair.open}, {pair.open + 2}});
        removed.push_back({{pair.close}, {pair.close + 2}});
    }
    Merge(&removed);
    std::wstring body;
    std::vector<size_t> offsets(last - first + 1);
    size_t token = 0;
    for (size_t at = first; at < last; ++at) {
        while (token < removed.size() && removed[token].end.value <= at) ++token;
        offsets[at - first] = body.size();
        if (token == removed.size() || at < removed[token].start.value) body += raw[at];
    }
    offsets.back() = body.size();
    const auto mapped = [&](size_t at) { return offsets[at - first]; };
    const size_t selectedStart = mapped(selection.start.value), selectedEnd = mapped(selection.end.value);
    std::vector<Span> bold;
    for (const auto pair : pairs) bold.push_back({{mapped(pair.open + 2)}, {mapped(pair.close)}});
    Merge(&bold);
    bool anyText = false, allBold = true;
    size_t covering = 0;
    for (const Span leaf : leaves) {
        const size_t a = std::max(selection.start.value, leaf.start.value);
        const size_t b = std::min(selection.end.value, leaf.end.value);
        for (size_t at = a; at < b; ++at) {
            if (std::iswspace(raw[at])) continue;
            anyText = true;
            const size_t position = mapped(at);
            while (covering < bold.size() && bold[covering].end.value <= position) ++covering;
            allBold = allBold && covering < bold.size() && bold[covering].start.value <= position;
        }
    }
    if (!anyText) return std::nullopt;
    std::vector<Span> desired;
    if (allBold) {
        for (Span span : bold) {
            if (span.start.value < selectedStart) desired.push_back({span.start, {std::min(span.end.value, selectedStart)}});
            if (span.end.value > selectedEnd) desired.push_back({{std::max(span.start.value, selectedEnd)}, span.end});
        }
    } else {
        desired = bold;
        desired.push_back({{selectedStart}, {selectedEnd}});
        Merge(&desired);
    }
    // Newline and surrounding whitespace stay outside generated emphasis.
    std::vector<Span> lines;
    for (Span span : desired) {
        for (size_t at = span.start.value; at < span.end.value;) {
            size_t end = body.find_first_of(L"\r\n", at);
            if (end == std::wstring::npos || end > span.end.value) end = span.end.value;
            size_t a = at, b = end;
            while (a < b && std::iswspace(body[a])) ++a;
            while (b > a && std::iswspace(body[b - 1])) --b;
            if (a < b) lines.push_back({{a}, {b}});
            at = end < span.end.value ? end + 1 : end;
        }
    }
    std::wstring replacement;
    size_t line = 0, nextStart = 0, nextEnd = 0;
    for (size_t at = 0; at <= body.size(); ++at) {
        if (line < lines.size() && lines[line].end.value == at) { replacement += L"**"; ++line; }
        if (at == selectedEnd) nextEnd = replacement.size();
        if (line < lines.size() && lines[line].start.value == at) replacement += L"**";
        if (at == selectedStart) nextStart = replacement.size();
        if (at < body.size()) replacement += body[at];
    }
    std::wstring changed = raw;
    changed.replace(first, last - first, replacement);
    const auto afterModel = MakeNoteTextModel(model.meta, std::move(changed), model.revision);
    const auto afterDoc = ParseNoteDocument(afterModel);
    // Reject ambiguous delimiter combinations rather than expose or consume
    // literal punctuation. The original document/history is still untouched.
    if (NoteDocumentHasFatalParseFailure(afterDoc) ||
        ExportPlainText(model, doc, {}) != ExportPlainText(afterModel, afterDoc, {})) return std::nullopt;
    if (!allBold) nextStart = nextEnd; // retain the established append-after-format behaviour
    if (replacement == raw.substr(first, last - first)) return std::nullopt;
    return MarkdownBoldEdit{{{first}, last - first, std::move(replacement)}, {{nextStart}, {nextEnd}}};
}
} // namespace note
