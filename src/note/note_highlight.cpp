#include "note/note_highlight.h"

#include <algorithm>

namespace note {
namespace {

bool IsValidSpan(Span span) {
    return span.end > span.start;
}

void PushHighlight(std::vector<HighlightRange>* out,
                   Span span,
                   HighlightRange::Kind kind) {
    if (!out || !IsValidSpan(span)) return;
    HighlightRange range;
    range.start = span.start.value;
    range.end = span.end.value;
    range.kind = kind;
    out->push_back(std::move(range));
}

} // namespace

std::vector<HighlightRange> BuildHighlightRanges(const NoteDocument& doc) {
    std::vector<HighlightRange> out;
    out.reserve(doc.blocks.size() + doc.style_spans.size());

    for (const auto& block : doc.blocks) {
        if (block.kind == BlockKind::Heading) {
            PushHighlight(&out, block.span, HighlightRange::Kind::Heading);
        }
    }

    for (const auto& styleSpan : doc.style_spans) {
        PushHighlight(&out, styleSpan.span, HighlightRange::Kind::Mark);
    }

    std::sort(out.begin(), out.end(), [](const HighlightRange& lhs, const HighlightRange& rhs) {
        if (lhs.start != rhs.start) return lhs.start < rhs.start;
        if (lhs.end != rhs.end) return lhs.end < rhs.end;
        return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
    });
    out.erase(std::unique(out.begin(), out.end(), [](const HighlightRange& lhs, const HighlightRange& rhs) {
        return lhs.start == rhs.start &&
               lhs.end == rhs.end &&
               lhs.kind == rhs.kind;
    }), out.end());
    return out;
}

} // namespace note
