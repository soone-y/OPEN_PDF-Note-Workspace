#include "note/note_semantic_index.h"

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <utility>

namespace note {

bool SemanticLinkTargetMatchesSnapshot(
    const SemanticLinkTargetResolution& target,
    const SnapshotIdentity& currentSnapshot) {
    return target.valid() && SameSnapshotContent(target.snapshot_identity, currentSnapshot);
}

namespace {

Span ClampSpan(Span span, size_t textSize) {
    span.start = {std::min(span.start.value, textSize)};
    span.end = {std::min(span.end.value, textSize)};
    if (span.end.value < span.start.value) span.end = span.start;
    return span;
}

bool IsIgnoredSearchPunctuation(wchar_t ch) {
    return ch == L',' || ch == L'，' || ch == L'、' || ch == L'､' ||
           ch == L'・' || ch == L'･';
}

bool IsIgnoredSearchSeparator(wchar_t ch) {
    return IsIgnoredSearchPunctuation(ch) || iswspace(static_cast<wint_t>(ch)) != 0;
}

wchar_t HalfwidthKatakanaToFullwidth(wchar_t ch) {
    switch (ch) {
    case L'｡': return L'。';
    case L'｢': return L'「';
    case L'｣': return L'」';
    case L'､': return L'、';
    case L'･': return L'・';
    case L'ｦ': return L'ヲ';
    case L'ｧ': return L'ァ';
    case L'ｨ': return L'ィ';
    case L'ｩ': return L'ゥ';
    case L'ｪ': return L'ェ';
    case L'ｫ': return L'ォ';
    case L'ｬ': return L'ャ';
    case L'ｭ': return L'ュ';
    case L'ｮ': return L'ョ';
    case L'ｯ': return L'ッ';
    case L'ｰ': return L'ー';
    case L'ｱ': return L'ア';
    case L'ｲ': return L'イ';
    case L'ｳ': return L'ウ';
    case L'ｴ': return L'エ';
    case L'ｵ': return L'オ';
    case L'ｶ': return L'カ';
    case L'ｷ': return L'キ';
    case L'ｸ': return L'ク';
    case L'ｹ': return L'ケ';
    case L'ｺ': return L'コ';
    case L'ｻ': return L'サ';
    case L'ｼ': return L'シ';
    case L'ｽ': return L'ス';
    case L'ｾ': return L'セ';
    case L'ｿ': return L'ソ';
    case L'ﾀ': return L'タ';
    case L'ﾁ': return L'チ';
    case L'ﾂ': return L'ツ';
    case L'ﾃ': return L'テ';
    case L'ﾄ': return L'ト';
    case L'ﾅ': return L'ナ';
    case L'ﾆ': return L'ニ';
    case L'ﾇ': return L'ヌ';
    case L'ﾈ': return L'ネ';
    case L'ﾉ': return L'ノ';
    case L'ﾊ': return L'ハ';
    case L'ﾋ': return L'ヒ';
    case L'ﾌ': return L'フ';
    case L'ﾍ': return L'ヘ';
    case L'ﾎ': return L'ホ';
    case L'ﾏ': return L'マ';
    case L'ﾐ': return L'ミ';
    case L'ﾑ': return L'ム';
    case L'ﾒ': return L'メ';
    case L'ﾓ': return L'モ';
    case L'ﾔ': return L'ヤ';
    case L'ﾕ': return L'ユ';
    case L'ﾖ': return L'ヨ';
    case L'ﾗ': return L'ラ';
    case L'ﾘ': return L'リ';
    case L'ﾙ': return L'ル';
    case L'ﾚ': return L'レ';
    case L'ﾛ': return L'ロ';
    case L'ﾜ': return L'ワ';
    case L'ﾝ': return L'ン';
    default: return ch;
    }
}

wchar_t ApplyKatakanaVoiceMark(wchar_t base, wchar_t mark) {
    if (mark == L'ﾞ') {
        switch (base) {
        case L'ウ': return L'ヴ';
        case L'カ': return L'ガ';
        case L'キ': return L'ギ';
        case L'ク': return L'グ';
        case L'ケ': return L'ゲ';
        case L'コ': return L'ゴ';
        case L'サ': return L'ザ';
        case L'シ': return L'ジ';
        case L'ス': return L'ズ';
        case L'セ': return L'ゼ';
        case L'ソ': return L'ゾ';
        case L'タ': return L'ダ';
        case L'チ': return L'ヂ';
        case L'ツ': return L'ヅ';
        case L'テ': return L'デ';
        case L'ト': return L'ド';
        case L'ハ': return L'バ';
        case L'ヒ': return L'ビ';
        case L'フ': return L'ブ';
        case L'ヘ': return L'ベ';
        case L'ホ': return L'ボ';
        case L'ヲ': return L'ヺ';
        default: return base;
        }
    }
    if (mark == L'ﾟ') {
        switch (base) {
        case L'ハ': return L'パ';
        case L'ヒ': return L'ピ';
        case L'フ': return L'プ';
        case L'ヘ': return L'ペ';
        case L'ホ': return L'ポ';
        default: return base;
        }
    }
    return base;
}

wchar_t NormalizeSearchWidthChar(wchar_t ch, SemanticSearchOptions options) {
    if (options.normalizeWidthKana) {
        if (ch == L'　') return L' ';
        if (ch >= L'！' && ch <= L'～') {
            ch = static_cast<wchar_t>(ch - L'！' + L'!');
        } else {
            ch = HalfwidthKatakanaToFullwidth(ch);
        }
    }
    if (options.ignoreCase) {
        ch = static_cast<wchar_t>(towlower(static_cast<wint_t>(ch)));
    }
    return ch;
}

struct NormalizedSemanticChar {
    wchar_t value = 0;
    size_t source_length = 1;
};

NormalizedSemanticChar NormalizeSemanticCharAt(std::wstring_view source, size_t index, SemanticSearchOptions options) {
    NormalizedSemanticChar out;
    if (index >= source.size()) return out;
    out.value = NormalizeSearchWidthChar(source[index], options);
    if (index + 1 < source.size() &&
        (source[index + 1] == L'ﾞ' || source[index + 1] == L'ﾟ')) {
        const wchar_t combined = ApplyKatakanaVoiceMark(out.value, source[index + 1]);
        if (combined != out.value) {
            out.value = combined;
            out.source_length = 2;
        }
    }
    return out;
}

Span ResolveLineSpan(const NoteTextModel& model, size_t offset) {
    if (model.line_starts.empty()) return Span{0, model.raw.size()};
    offset = std::min(offset, model.raw.size());
    auto it = std::upper_bound(model.line_starts.begin(), model.line_starts.end(), offset);
    const size_t lineIndex = (it == model.line_starts.begin())
        ? 0
        : static_cast<size_t>(std::distance(model.line_starts.begin(), it - 1));
    const size_t start = model.line_starts[std::min(lineIndex, model.line_starts.size() - 1)];
    size_t end = (lineIndex + 1 < model.line_starts.size())
        ? model.line_starts[lineIndex + 1]
        : model.raw.size();
    if (end > start && model.raw[end - 1] == L'\n') --end;
    if (end > start && model.raw[end - 1] == L'\r') --end;
    return Span{start, end};
}

void AppendNormalizedText(std::wstring_view source,
                          std::wstring* out,
                          bool* pendingSpaceState) {
    if (!out) return;
    bool pendingSpace = pendingSpaceState && *pendingSpaceState;
    for (wchar_t ch : source) {
        if (iswspace(ch) != 0) {
            pendingSpace = !out->empty();
            continue;
        }
        if (pendingSpace) {
            out->push_back(L' ');
            pendingSpace = false;
        }
        out->push_back(ch);
    }
    if (pendingSpaceState) *pendingSpaceState = pendingSpace;
}

std::wstring NormalizedSpanText(const NoteTextModel& model, Span span) {
    span = ClampSpan(span, model.raw.size());
    std::wstring out;
    bool pendingSpace = false;
    AppendNormalizedText(
        std::wstring_view(model.raw).substr(span.start.value, span.end.value - span.start.value),
        &out,
        &pendingSpace);
    return out;
}

std::wstring BuildHeadingText(const NoteTextModel& model,
                              const NoteDocument& document,
                              size_t blockIndex,
                              const BlockNode& block,
                              Span* outContentSpan) {
    std::vector<Span> textSpans;
    for (const InlineNode& node : document.inlines) {
        if (node.parent_block != blockIndex || node.kind != InlineKind::Text) continue;
        Span span = ClampSpan(node.span, model.raw.size());
        if (span.end > span.start) textSpans.push_back(span);
    }
    std::stable_sort(textSpans.begin(), textSpans.end(),
                     [](const Span& a, const Span& b) {
                         if (a.start != b.start) return a.start < b.start;
                         return a.end < b.end;
                     });

    std::wstring text;
    bool pendingSpace = false;
    Span content = ClampSpan(block.span, model.raw.size());
    if (!textSpans.empty()) {
        content.start = textSpans.front().start;
        content.end = textSpans.front().end;
        size_t consumedUntil = 0;
        bool hasConsumedSpan = false;
        for (const Span& span : textSpans) {
            const size_t start = hasConsumedSpan ? std::max(span.start.value, consumedUntil) : span.start.value;
            if (span.end.value <= start) continue;
            if (hasConsumedSpan && start > consumedUntil) {
                const std::wstring_view gap =
                    std::wstring_view(model.raw).substr(consumedUntil, start - consumedUntil);
                if (std::any_of(gap.begin(), gap.end(),
                                [](wchar_t ch) { return iswspace(ch) != 0; })) {
                    pendingSpace = !text.empty();
                }
            }
            AppendNormalizedText(
                std::wstring_view(model.raw).substr(start, span.end.value - start),
                &text,
                &pendingSpace);
            consumedUntil = std::max(consumedUntil, span.end.value);
            content.end = {std::max(content.end.value, span.end.value)};
            hasConsumedSpan = true;
        }
    }
    if (text.empty()) {
        text = NormalizedSpanText(model, content);
    }
    if (outContentSpan) *outContentSpan = content;
    return text;
}

} // namespace

std::wstring NormalizeSemanticSearchTerm(std::wstring_view term, SemanticSearchOptions options) {
    std::wstring normalized;
    normalized.reserve(term.size());
    for (size_t i = 0; i < term.size(); ++i) {
        const NormalizedSemanticChar ch = NormalizeSemanticCharAt(term, i, options);
        if (ch.source_length > 1) i += ch.source_length - 1;
        if (options.ignoreSeparators && IsIgnoredSearchSeparator(ch.value)) continue;
        normalized.push_back(ch.value);
    }
    return normalized;
}

SemanticNormalizedTextIndex BuildSemanticNormalizedTextIndex(std::wstring_view text, SemanticSearchOptions options) {
    SemanticNormalizedTextIndex index;
    index.text.reserve(text.size());
    index.source_start.reserve(text.size());
    index.source_end.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const size_t sourceStart = i;
        const NormalizedSemanticChar ch = NormalizeSemanticCharAt(text, i, options);
        const size_t sourceEnd = std::min(text.size(), sourceStart + ch.source_length);
        if (ch.source_length > 1) i += ch.source_length - 1;
        if (options.ignoreSeparators && IsIgnoredSearchSeparator(ch.value)) continue;
        index.text.push_back(ch.value);
        index.source_start.push_back(sourceStart);
        index.source_end.push_back(sourceEnd);
    }
    return index;
}

SemanticIndexSnapshot BuildSemanticIndexSnapshot(NoteId ownerNoteId,
                                                 const NoteTextModel& model,
    const NoteDocument& document) {
    SemanticIndexSnapshot index;
    index.source_identity = NoteDerivedSnapshotIdentity{ownerNoteId, model.revision};
    if (!index.source_identity.valid() ||
        !NoteDocumentMatchesTextModel(document, model) ||
        !NoteDocumentMatchesSourceIdentity(document, index.source_identity)) {
        return index;
    }
    index.valid = true;
    index.normalized_text = BuildSemanticNormalizedTextIndex(model.raw);

    for (size_t blockIndex = 0; blockIndex < document.blocks.size(); ++blockIndex) {
        const BlockNode& block = document.blocks[blockIndex];
        if (block.kind != BlockKind::Heading) continue;
        SemanticHeadingEntry heading;
        heading.level = std::clamp(block.level, 1, 6);
        heading.block_span = ClampSpan(block.span, model.raw.size());
        heading.line_span = ResolveLineSpan(model, heading.block_span.start.value);
        heading.loc = ResolveLineColumn(model, heading.block_span.start.value);
        heading.text = BuildHeadingText(model, document, blockIndex, block,
                                        &heading.content_span);
        if (!heading.text.empty()) index.headings.push_back(std::move(heading));
    }

    for (const InlineNode& node : document.inlines) {
        if (node.kind != InlineKind::Link || node.target.empty()) continue;
        SemanticLinkEntry link;
        link.kind = SemanticLinkKind::MarkdownTarget;
        link.span = ClampSpan(node.span, model.raw.size());
        link.target = node.target;
        link.text = NormalizedSpanText(model, link.span);
        index.links.push_back(std::move(link));
    }
    for (const StyleSpan& span : document.style_spans) {
        if (span.kind != StyleKind::LinkId || span.value.empty()) continue;
        SemanticLinkEntry link;
        link.kind = SemanticLinkKind::LinkId;
        link.span = ClampSpan(span.span, model.raw.size());
        link.target = span.value;
        link.text = NormalizedSpanText(model, link.span);
        index.links.push_back(std::move(link));
    }

    for (const MathSpan& span : document.math_spans) {
        SemanticMathEntry math;
        math.kind = span.kind;
        math.delimiter = span.delimiter;
        math.span = ClampSpan(span.span, model.raw.size());
        math.content_span = ClampSpan(span.content_span, model.raw.size());
        math.normalized_tex = span.normalized_tex;
        index.math.push_back(std::move(math));
    }

    std::stable_sort(index.headings.begin(), index.headings.end(),
                     [](const SemanticHeadingEntry& a, const SemanticHeadingEntry& b) {
                         return a.line_span.start < b.line_span.start;
                     });
    std::stable_sort(index.links.begin(), index.links.end(),
                     [](const SemanticLinkEntry& a, const SemanticLinkEntry& b) {
                         return a.span.start < b.span.start;
                     });
    std::stable_sort(index.math.begin(), index.math.end(),
                     [](const SemanticMathEntry& a, const SemanticMathEntry& b) {
                         return a.span.start < b.span.start;
                     });
    return index;
}

bool SemanticIndexMatchesTextModel(const SemanticIndexSnapshot& index,
                                   NoteId ownerNoteId,
    const NoteTextModel& model) {
    return index.valid && ownerNoteId.valid() &&
           index.source_identity ==
               NoteDerivedSnapshotIdentity{ownerNoteId, model.revision};
}

} // namespace note
