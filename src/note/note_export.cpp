#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "note/note_export.h"

#include "note/note_markdown_escape.h"
#include "note/note_parser.h"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace note {
namespace {

constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

struct Segment {
    enum class Kind {
        Text,
        Code,
        Math,
    };

    Kind kind = Kind::Text;
    Span span{};
    const MathSpan* math = nullptr;
};

std::wstring TrimWhitespace(std::wstring_view text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && iswspace(text[start])) {
        ++start;
    }
    while (end > start && iswspace(text[end - 1])) {
        --end;
    }
    return std::wstring(text.substr(start, end - start));
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0,
                                  text.data(), static_cast<int>(text.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
                        text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0,
                                  text.data(), static_cast<int>(text.size()),
                                  nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        text.data(), static_cast<int>(text.size()),
                        out.data(), len);
    return out;
}

std::string EscapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring ToLowerAscii(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch >= L'A' && ch <= L'Z') {
            out.push_back(static_cast<wchar_t>(ch - L'A' + L'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

bool ParseMarkupBool(std::wstring_view value) {
    const std::wstring lowered = ToLowerAscii(value);
    return lowered.empty() ||
           lowered == L"1" ||
           lowered == L"true" ||
           lowered == L"on" ||
           lowered == L"yes";
}

bool TryParseMarkupTag(std::wstring_view raw,
                       size_t openPos,
                       size_t* outTagEnd,
                       std::wstring* outContent) {
    if (!outTagEnd || !outContent) return false;
    if (openPos >= raw.size() || raw[openPos] != L'<') return false;

    size_t cursor = openPos + 1;
    while (cursor < raw.size() && raw[cursor] != L'>') {
        ++cursor;
    }
    if (cursor >= raw.size() || raw[cursor] != L'>') return false;

    *outTagEnd = cursor + 1;
    *outContent = TrimWhitespace(raw.substr(openPos + 1, cursor - (openPos + 1)));
    return !outContent->empty();
}

bool IsNumericValue(std::wstring_view value) {
    if (value.empty()) return false;
    bool hasDigit = false;
    for (size_t i = 0; i < value.size(); ++i) {
        const wchar_t ch = value[i];
        if (ch >= L'0' && ch <= L'9') {
            hasDigit = true;
            continue;
        }
        if (ch == L'+' || ch == L'-') {
            if (i != 0) return false;
            continue;
        }
        if (ch == L'.') continue;
        return false;
    }
    return hasDigit;
}

bool IsRecognizedStyleOpenTag(std::wstring_view content) {
    if (content.empty() || content[0] == L'/') return false;

    size_t i = 0;
    bool recognized = false;
    while (i < content.size()) {
        while (i < content.size() && (iswspace(content[i]) || content[i] == L',')) ++i;
        if (i >= content.size()) break;

        size_t keyStart = i;
        while (i < content.size() && !iswspace(content[i]) && content[i] != L'=' && content[i] != L',') ++i;
        std::wstring key = std::wstring(content.substr(keyStart, i - keyStart));
        if (key.empty()) {
            ++i;
            continue;
        }

        std::optional<std::wstring> value;
        if (i < content.size() && content[i] == L'=') {
            ++i;
            while (i < content.size() && iswspace(content[i])) ++i;
            if (i < content.size() && (content[i] == L'"' || content[i] == L'\'')) {
                const wchar_t quote = content[i++];
                const size_t valueStart = i;
                while (i < content.size() && content[i] != quote) ++i;
                value = std::wstring(content.substr(valueStart, i - valueStart));
                if (i < content.size() && content[i] == quote) ++i;
            } else {
                const size_t valueStart = i;
                while (i < content.size() && !iswspace(content[i]) && content[i] != L',') ++i;
                value = std::wstring(content.substr(valueStart, i - valueStart));
            }
        }

        if (!value && key.size() >= 2 && (key[1] == L'+' || key[1] == L'-')) {
            const wchar_t head = towlower(key[0]);
            if (head == L'm' || head == L's') {
                value = key.substr(1);
                key = key.substr(0, 1);
            }
        }

        const std::wstring lowered = ToLowerAscii(key);
        if (!value) {
            if (lowered == L"u" || lowered == L"la" || lowered == L"linkaccent" ||
                lowered == L"lu" || lowered == L"linkunderline" || lowered == L"m") {
                recognized = true;
            }
            continue;
        }

        const std::wstring trimmedValue = TrimWhitespace(*value);
        if (trimmedValue.empty()) continue;
        if (lowered == L"char" || lowered == L"c" ||
            lowered == L"back" ||
            lowered == L"font" || lowered == L"f" ||
            lowered == L"s" ||
            lowered == L"m" ||
            lowered == L"l" || lowered == L"link" ||
            lowered == L"la" || lowered == L"linkaccent" ||
            lowered == L"lu" || lowered == L"linkunderline") {
            recognized = true;
        } else if (lowered == L"d") {
            recognized = IsNumericValue(trimmedValue) || !trimmedValue.empty();
        } else if (lowered == L"u") {
            recognized = ParseMarkupBool(trimmedValue);
        }
    }

    return recognized;
}

bool IsLegacyStyleTag(std::wstring_view raw, size_t pos, size_t* outTagEnd) {
    size_t tagEnd = 0;
    std::wstring content;
    if (!TryParseMarkupTag(raw, pos, &tagEnd, &content)) {
        return false;
    }
    if (content == L"/" || (!content.empty() && content[0] == L'/') || IsRecognizedStyleOpenTag(content)) {
        if (outTagEnd) *outTagEnd = tagEnd;
        return true;
    }
    return false;
}

std::wstring StripLegacyStyleTags(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t tagEnd = 0;
        if (raw[pos] == L'<' && IsLegacyStyleTag(raw, pos, &tagEnd)) {
            pos = tagEnd;
            continue;
        }
        out.push_back(raw[pos]);
        ++pos;
    }
    return out;
}

std::wstring SliceWithoutLegacyStyleTags(const NoteTextModel& model, Span span) {
    if (span.end.value <= span.start.value || span.start.value >= model.raw.size()) return L"";
    const size_t end = std::min(span.end.value, model.raw.size());
    return StripLegacyStyleTags(std::wstring_view(model.raw).substr(span.start.value, end - span.start.value));
}

std::wstring SliceRenderedMarkdownText(const NoteTextModel& model, Span span) {
    return DecodeMarkdownBackslashEscapes(SliceWithoutLegacyStyleTags(model, span));
}

std::wstring MathRawText(const NoteTextModel& model, const MathSpan& math) {
    if (math.span.end.value <= math.span.start.value || math.span.start.value >= model.raw.size()) return L"";
    const size_t end = std::min(math.span.end.value, model.raw.size());
    return std::wstring(std::wstring_view(model.raw).substr(math.span.start.value, end - math.span.start.value));
}

std::wstring ReplaceMathSpansWithPlaceholder(const NoteTextModel& model,
                                             const NoteDocument& doc,
                                             std::wstring text,
                                             std::wstring_view placeholder) {
    std::vector<const MathSpan*> spans;
    spans.reserve(doc.math_spans.size());
    for (const auto& math : doc.math_spans) {
        spans.push_back(&math);
    }
    std::sort(spans.begin(), spans.end(), [](const MathSpan* lhs, const MathSpan* rhs) {
        if (lhs->span.start != rhs->span.start) return lhs->span.start > rhs->span.start;
        return lhs->span.end > rhs->span.end;
    });
    for (const MathSpan* math : spans) {
        if (!math) continue;
        if (math->span.end.value <= math->span.start.value || math->span.end.value > text.size()) continue;
        text.replace(math->span.start.value, math->span.end.value - math->span.start.value, placeholder);
    }
    return text;
}

std::vector<std::vector<size_t>> BuildChildBlockLists(const NoteDocument& doc) {
    std::vector<std::vector<size_t>> children(doc.blocks.size() + 1);
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const size_t parent = doc.blocks[i].parent;
        const size_t bucket = (parent == kInvalidIndex || parent >= doc.blocks.size())
            ? doc.blocks.size()
            : parent;
        children[bucket].push_back(i);
    }
    return children;
}

std::vector<Segment> BuildVisibleSegments(const NoteTextModel& model,
                                          const NoteDocument& doc,
                                          const BlockNode& block) {
    (void)model;
    std::vector<Segment> base;
    for (size_t i = 0; i < block.inline_count; ++i) {
        const size_t inlineIndex = block.first_inline + i;
        if (inlineIndex >= doc.inlines.size()) break;
        const InlineNode& node = doc.inlines[inlineIndex];
        if (node.kind == InlineKind::Text || node.kind == InlineKind::Code) {
            Segment seg;
            seg.kind = (node.kind == InlineKind::Code) ? Segment::Kind::Code : Segment::Kind::Text;
            seg.span = node.span;
            base.push_back(seg);
        }
    }
    std::sort(base.begin(), base.end(), [](const Segment& lhs, const Segment& rhs) {
        if (lhs.span.start != rhs.span.start) return lhs.span.start < rhs.span.start;
        return lhs.span.end < rhs.span.end;
    });

    std::vector<const MathSpan*> mathSpans;
    for (const auto& math : doc.math_spans) {
        if (math.span.start < block.span.end && math.span.end > block.span.start) {
            mathSpans.push_back(&math);
        }
    }
    std::sort(mathSpans.begin(), mathSpans.end(), [](const MathSpan* lhs, const MathSpan* rhs) {
        if (lhs->span.start != rhs->span.start) return lhs->span.start < rhs->span.start;
        return lhs->span.end < rhs->span.end;
    });

    std::vector<Segment> out;
    for (const auto& seg : base) {
        if (seg.kind != Segment::Kind::Text) {
            out.push_back(seg);
            continue;
        }
        size_t cursor = seg.span.start.value;
        for (const MathSpan* math : mathSpans) {
            if (!math) continue;
            if (math->span.end.value <= seg.span.start.value) continue;
            if (math->span.start.value >= seg.span.end.value) break;
            if (cursor < math->span.start.value) {
                out.push_back(Segment{Segment::Kind::Text, Span{{cursor}, math->span.start}, nullptr});
            }
            out.push_back(Segment{Segment::Kind::Math, math->span, math});
            cursor = std::max(cursor, math->span.end.value);
        }
        if (cursor < seg.span.end.value) {
            out.push_back(Segment{Segment::Kind::Text, Span{{cursor}, seg.span.end}, nullptr});
        }
    }

    std::sort(out.begin(), out.end(), [](const Segment& lhs, const Segment& rhs) {
        if (lhs.span.start != rhs.span.start) return lhs.span.start < rhs.span.start;
        return lhs.span.end < rhs.span.end;
    });
    return out;
}

std::wstring RenderPlainSegment(const NoteTextModel& model,
                                const Segment& segment,
                                const TextExportConfig& config) {
    switch (segment.kind) {
    case Segment::Kind::Code:
        if (segment.span.end.value <= segment.span.start.value || segment.span.start.value >= model.raw.size()) return L"";
        return std::wstring(std::wstring_view(model.raw).substr(
            segment.span.start.value, std::min(segment.span.end.value, model.raw.size()) - segment.span.start.value));
    case Segment::Kind::Math:
        if (!segment.math) return L"";
        switch (config.mathMode) {
        case ExportTextMathMode::Placeholder:
            return Utf8ToWide(config.mathPlaceholder);
        case ExportTextMathMode::Simplified:
            return segment.math->normalized_tex;
        case ExportTextMathMode::Raw:
        default:
            return MathRawText(model, *segment.math);
        }
    case Segment::Kind::Text:
    default:
        return SliceRenderedMarkdownText(model, segment.span);
    }
}

std::wstring BuildMarkdownHeadingPrefix(int level) {
    level = std::clamp(level, 1, 6);
    return std::wstring(static_cast<size_t>(level), L'#') + L' ';
}

std::string DefaultHtmlCss() {
    return R"CSS(
:root{--fg:#1c1d22;--muted:#5f6472;--bg:#fbfaf7;--paper:#ffffff;--line:#e7e0d6;}
body{font-family:"Segoe UI Variable Text","Yu Gothic UI","Hiragino Sans",sans-serif;line-height:1.65;margin:28px;color:var(--fg);background:
radial-gradient(circle at top left,rgba(31,107,91,.08),transparent 28rem),
linear-gradient(180deg,#fcfbf8,#f6f1e8 55%,#fbfaf7);}
pre{background:#f6f8fa;padding:12px;overflow:auto;border-radius:10px;border:1px solid #e4e7eb;}
code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:0.95em;}
h1,h2,h3,h4,h5,h6{margin:1.2em 0 0.4em;letter-spacing:.01em;}
p{margin:0.6em 0;}
ul,ol{margin:0.6em 0 0.6em 1.4em;}
blockquote{margin:1em 0;padding:0.2em 1rem;border-left:4px solid #7f8ea3;background:rgba(245,247,250,.88);}
table{border-collapse:collapse;margin:.8em 0;}
th,td{border:1px solid var(--line);padding:.35em .6em;vertical-align:top;}
th{background:#f3f5f7;font-weight:600;}
.image-ref{color:#0b57d0;text-decoration:underline;text-decoration-style:dotted;}
.math.inline{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;background:#fff4cc;padding:0 .28em;border-radius:4px;}
.math.block{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;background:#fff8de;padding:10px;border-radius:8px;white-space:pre-wrap;margin:.8em 0;border:1px solid #f0df9d;}
)CSS";
}

std::string StyleSpanToCss(StyleKind kind, std::wstring_view value) {
    const std::string v = EscapeHtml(WideToUtf8(value));
    switch (kind) {
    case StyleKind::Underline:
        return "text-decoration:underline;";
    case StyleKind::TextColor:
        return "color:" + v + ";";
    case StyleKind::BackgroundColor:
        return "background:" + v + ";";
    case StyleKind::FontFamily:
        return "font-family:" + v + ";";
    case StyleKind::FontSize:
        return "font-size:" + v + ";";
    case StyleKind::LinkAccent:
        return "color:#0b57d0;";
    case StyleKind::LinkUnderline:
        return "text-decoration:underline;";
    default:
        break;
    }
    return {};
}

std::string TableCellAlignCss(TableCellAlign align) {
    switch (align) {
    case TableCellAlign::Left:
        return "text-align:left;";
    case TableCellAlign::Center:
        return "text-align:center;";
    case TableCellAlign::Right:
        return "text-align:right;";
    case TableCellAlign::Default:
    default:
        return {};
    }
}

std::wstring RenderPlainBlock(const NoteTextModel& model,
                              const NoteDocument& doc,
                              const std::vector<std::vector<size_t>>& children,
                              size_t blockIndex,
                              const TextExportConfig& config) {
    if (blockIndex >= doc.blocks.size()) return L"";
    const BlockNode& block = doc.blocks[blockIndex];
    std::wstring out;

    switch (block.kind) {
    case BlockKind::HorizontalRule:
        return L"-----\n";
    case BlockKind::CodeBlock:
        out += SliceWithoutLegacyStyleTags(model, block.span);
        if (!out.empty() && out.back() != L'\n') out.push_back(L'\n');
        return out;
    case BlockKind::Paragraph:
    case BlockKind::Heading: {
        const std::vector<Segment> segments = BuildVisibleSegments(model, doc, block);
        for (const auto& segment : segments) {
            out += RenderPlainSegment(model, segment, config);
        }
        if (!out.empty() && out.back() != L'\n') out.push_back(L'\n');
        return out;
    }
    case BlockKind::List:
    case BlockKind::ListItem:
    case BlockKind::Quote: {
        const size_t bucket = (blockIndex < children.size()) ? blockIndex : children.size() - 1;
        for (size_t child : children[bucket]) {
            out += RenderPlainBlock(model, doc, children, child, config);
        }
        return out;
    }
    case BlockKind::Table:
    case BlockKind::TableHead:
    case BlockKind::TableBody:
    case BlockKind::TableRow:
    case BlockKind::TableHeaderCell:
    case BlockKind::TableCell: {
        const size_t bucket = (blockIndex < children.size()) ? blockIndex : children.size() - 1;
        for (size_t child : children[bucket]) {
            out += RenderPlainBlock(model, doc, children, child, config);
        }
        return out;
    }
    case BlockKind::Blank:
        return L"\n";
    }

    return out;
}

std::wstring RewriteMarkdownHeadings(std::wstring text,
                                     const std::wstring& title,
                                     bool includeTitleHeading,
                                     bool shiftHeadingLevels) {
    NoteMetadata meta;
    meta.file_name = L"export.md";
    NoteTextModel model = MakeNoteTextModel(std::move(meta), text, 1);
    NoteDocument doc = ParseNoteDocument(model);

    std::unordered_map<size_t, const BlockNode*> headingByLine;
    for (const auto& block : doc.blocks) {
        if (block.kind != BlockKind::Heading) continue;
        if (block.loc.line <= 0) continue;
        headingByLine.emplace(static_cast<size_t>(block.loc.line - 1), &block);
    }

    std::wstring out;
    if (includeTitleHeading && !title.empty()) {
        out += L"# " + title + L"\n\n";
    }

    for (size_t lineIndex = 0; lineIndex < model.line_starts.size(); ++lineIndex) {
        const size_t lineStart = model.line_starts[lineIndex];
        const size_t nextLineStart = (lineIndex + 1 < model.line_starts.size())
            ? model.line_starts[lineIndex + 1]
            : model.raw.size();
        size_t contentEnd = nextLineStart;
        std::wstring lineBreak;
        if (contentEnd > lineStart && model.raw[contentEnd - 1] == L'\n') {
            lineBreak.insert(lineBreak.begin(), L'\n');
            --contentEnd;
        }
        if (contentEnd > lineStart && model.raw[contentEnd - 1] == L'\r') {
            lineBreak.insert(lineBreak.begin(), L'\r');
            --contentEnd;
        }

        auto it = headingByLine.find(lineIndex);
        if (it == headingByLine.end()) {
            out += model.raw.substr(lineStart, nextLineStart - lineStart);
            continue;
        }

        const BlockNode* heading = it->second;
        if (!heading || heading->span.end <= heading->span.start) {
            out += model.raw.substr(lineStart, nextLineStart - lineStart);
            continue;
        }

        const int level = shiftHeadingLevels ? std::clamp(heading->level + 1, 1, 6) : std::clamp(heading->level, 1, 6);
        out += BuildMarkdownHeadingPrefix(level);
        out += model.raw.substr(heading->span.start.value, heading->span.end.value - heading->span.start.value);
        out += lineBreak;
    }

    return out;
}

std::string RenderHtmlInlineRange(const NoteTextModel& model,
                                  const NoteDocument& doc,
                                  size_t blockIndex,
                                  const MarkupExportConfig& config) {
    if (blockIndex >= doc.blocks.size()) return {};
    const BlockNode& block = doc.blocks[blockIndex];
    std::vector<Segment> segments = BuildVisibleSegments(model, doc, block);
    std::vector<InlineNode> overlays;
    overlays.reserve(block.inline_count);
    for (size_t i = 0; i < block.inline_count; ++i) {
        const size_t inlineIndex = block.first_inline + i;
        if (inlineIndex >= doc.inlines.size()) break;
        const InlineNode& node = doc.inlines[inlineIndex];
        if (node.kind == InlineKind::Emphasis ||
            node.kind == InlineKind::Strong ||
            node.kind == InlineKind::Strike ||
            node.kind == InlineKind::Link ||
            node.kind == InlineKind::Image) {
            overlays.push_back(node);
        }
    }

    std::vector<StyleSpan> styles;
    for (const auto& style : doc.style_spans) {
        if (style.span.start < block.span.end && style.span.end > block.span.start) {
            styles.push_back(style);
        }
    }

    std::vector<Segment> finalSegments;
    for (const auto& seg : segments) {
        if (seg.kind != Segment::Kind::Text) {
            finalSegments.push_back(seg);
            continue;
        }

        std::vector<size_t> cuts{seg.span.start.value, seg.span.end.value};
        for (const auto& overlay : overlays) {
            if (overlay.span.start.value > seg.span.start.value && overlay.span.start.value < seg.span.end.value) cuts.push_back(overlay.span.start.value);
            if (overlay.span.end.value > seg.span.start.value && overlay.span.end.value < seg.span.end.value) cuts.push_back(overlay.span.end.value);
        }
        for (const auto& style : styles) {
            if (style.span.start.value > seg.span.start.value && style.span.start.value < seg.span.end.value) cuts.push_back(style.span.start.value);
            if (style.span.end.value > seg.span.start.value && style.span.end.value < seg.span.end.value) cuts.push_back(style.span.end.value);
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            if (cuts[i + 1] <= cuts[i]) continue;
            finalSegments.push_back(Segment{Segment::Kind::Text, Span{cuts[i], cuts[i + 1]}, nullptr});
        }
    }

    std::sort(finalSegments.begin(), finalSegments.end(), [](const Segment& lhs, const Segment& rhs) {
        if (lhs.span.start != rhs.span.start) return lhs.span.start < rhs.span.start;
        return lhs.span.end < rhs.span.end;
    });

    std::string out;
    for (const auto& seg : finalSegments) {
        std::string inner;
        if (seg.kind == Segment::Kind::Code) {
            inner = "<code>" + EscapeHtml(WideToUtf8(std::wstring(
                std::wstring_view(model.raw).substr(seg.span.start.value, seg.span.end.value - seg.span.start.value)))) + "</code>";
        } else if (seg.kind == Segment::Kind::Math) {
            const MathSpan* math = seg.math;
            if (!math) continue;
            if (config.mathMode == ExportMarkupMathMode::Placeholder) {
                inner = EscapeHtml(config.mathPlaceholder);
            } else if (math->kind == MathKind::Block) {
                inner = "<div class=\"math block\">" + EscapeHtml(WideToUtf8(math->normalized_tex)) + "</div>";
            } else {
                inner = "<span class=\"math inline\">" + EscapeHtml(WideToUtf8(math->normalized_tex)) + "</span>";
            }
        } else {
            std::wstring text = (seg.kind == Segment::Kind::Code)
                ? SliceWithoutLegacyStyleTags(model, seg.span)
                : SliceRenderedMarkdownText(model, seg.span);
            if (text.empty()) continue;
            inner = EscapeHtml(WideToUtf8(text));
        }

        for (const auto& overlay : overlays) {
            if (seg.span.start < overlay.span.start || seg.span.end > overlay.span.end) continue;
            switch (overlay.kind) {
            case InlineKind::Emphasis:
                inner = "<em>" + inner + "</em>";
                break;
            case InlineKind::Strong:
                inner = "<strong>" + inner + "</strong>";
                break;
            case InlineKind::Strike:
                inner = "<del>" + inner + "</del>";
                break;
            case InlineKind::Link:
                inner = "<a href=\"" + EscapeHtml(WideToUtf8(overlay.target)) + "\">" + inner + "</a>";
                break;
            case InlineKind::Image:
                inner = "<span class=\"image-ref\" data-src=\"" + EscapeHtml(WideToUtf8(overlay.target)) +
                        "\">" + inner + "</span>";
                break;
            default:
                break;
            }
        }

        std::string css;
        for (const auto& style : styles) {
            if (seg.span.start < style.span.start || seg.span.end > style.span.end) continue;
            css += StyleSpanToCss(style.kind, style.value);
        }
        if (!css.empty()) {
            inner = "<span style=\"" + css + "\">" + inner + "</span>";
        }

        out += inner;
    }
    return out;
}

std::string RenderHtmlBlock(const NoteTextModel& model,
                            const NoteDocument& doc,
                            const std::vector<std::vector<size_t>>& children,
                            size_t blockIndex,
                            const MarkupExportConfig& config);

std::string RenderHtmlChildren(const NoteTextModel& model,
                               const NoteDocument& doc,
                               const std::vector<std::vector<size_t>>& children,
                               size_t parentIndex,
                               const MarkupExportConfig& config) {
    const size_t bucket = (parentIndex == kInvalidIndex) ? doc.blocks.size() : parentIndex;
    std::string out;
    if (bucket >= children.size()) return out;
    for (size_t child : children[bucket]) {
        out += RenderHtmlBlock(model, doc, children, child, config);
    }
    return out;
}

std::string RenderHtmlBlock(const NoteTextModel& model,
                            const NoteDocument& doc,
                            const std::vector<std::vector<size_t>>& children,
                            size_t blockIndex,
                            const MarkupExportConfig& config) {
    if (blockIndex >= doc.blocks.size()) return {};
    const BlockNode& block = doc.blocks[blockIndex];

    switch (block.kind) {
    case BlockKind::HorizontalRule:
        return "<hr>\n";
    case BlockKind::Paragraph: {
        const std::vector<Segment> segments = BuildVisibleSegments(model, doc, block);
        if (segments.size() == 1 && segments[0].kind == Segment::Kind::Math &&
            segments[0].math && segments[0].math->kind == MathKind::Block) {
            if (config.mathMode == ExportMarkupMathMode::Placeholder) {
                return "<div>" + EscapeHtml(config.mathPlaceholder) + "</div>\n";
            }
            return "<div class=\"math block\">" + EscapeHtml(WideToUtf8(segments[0].math->normalized_tex)) + "</div>\n";
        }
        return "<p>" + RenderHtmlInlineRange(model, doc, blockIndex, config) + "</p>\n";
    }
    case BlockKind::Heading: {
        const bool shiftHeadings = config.includeTitleHeading && config.shiftHeadingLevels;
        const int level = std::clamp(shiftHeadings ? block.level + 1 : block.level, 1, 6);
        return "<h" + std::to_string(level) + ">" +
               RenderHtmlInlineRange(model, doc, blockIndex, config) +
               "</h" + std::to_string(level) + ">\n";
    }
    case BlockKind::CodeBlock: {
        std::string out = "<pre><code";
        if (!block.info_string.empty()) {
            out += " class=\"language-" + EscapeHtml(WideToUtf8(block.info_string)) + "\"";
        }
        out += ">" + EscapeHtml(WideToUtf8(SliceWithoutLegacyStyleTags(model, block.span))) + "</code></pre>\n";
        return out;
    }
    case BlockKind::List: {
        const char* tag = block.ordered ? "ol" : "ul";
        std::string out = std::string("<") + tag;
        if (block.ordered && block.start_number > 1) {
            out += " start=\"" + std::to_string(block.start_number) + "\"";
        }
        out += ">\n";
        out += RenderHtmlChildren(model, doc, children, blockIndex, config);
        out += std::string("</") + tag + ">\n";
        return out;
    }
    case BlockKind::ListItem: {
        std::string out = "<li";
        if (block.task_item) {
            out += " class=\"task-list-item\"";
            out += block.task_checked ? " data-checked=\"true\"" : " data-checked=\"false\"";
        }
        out += ">";
        if (block.task_item) {
            out += block.task_checked ? "[x] " : "[ ] ";
        }
        out += RenderHtmlChildren(model, doc, children, blockIndex, config);
        while (!out.empty() && out.back() == '\n') out.pop_back();
        out += "</li>\n";
        return out;
    }
    case BlockKind::Quote:
        return "<blockquote>\n" + RenderHtmlChildren(model, doc, children, blockIndex, config) + "</blockquote>\n";
    case BlockKind::Table:
        return "<table>\n" + RenderHtmlChildren(model, doc, children, blockIndex, config) + "</table>\n";
    case BlockKind::TableHead:
        return "<thead>\n" + RenderHtmlChildren(model, doc, children, blockIndex, config) + "</thead>\n";
    case BlockKind::TableBody:
        return "<tbody>\n" + RenderHtmlChildren(model, doc, children, blockIndex, config) + "</tbody>\n";
    case BlockKind::TableRow:
        return "<tr>" + RenderHtmlChildren(model, doc, children, blockIndex, config) + "</tr>\n";
    case BlockKind::TableHeaderCell: {
        const std::string css = TableCellAlignCss(block.table_cell_align);
        return std::string("<th") + (css.empty() ? "" : (" style=\"" + css + "\"")) + ">" +
               RenderHtmlInlineRange(model, doc, blockIndex, config) + "</th>";
    }
    case BlockKind::TableCell: {
        const std::string css = TableCellAlignCss(block.table_cell_align);
        return std::string("<td") + (css.empty() ? "" : (" style=\"" + css + "\"")) + ">" +
               RenderHtmlInlineRange(model, doc, blockIndex, config) + "</td>";
    }
    case BlockKind::Blank:
        return "\n";
    }
    return {};
}

} // namespace

std::string ExportPlainText(const NoteTextModel& model,
                            const NoteDocument& doc,
                            const TextExportConfig& config) {
    if (config.markupMode == ExportTextMarkupMode::Raw) {
        std::wstring raw = model.raw;
        if (config.mathMode == ExportTextMathMode::Placeholder ||
            config.mathMode == ExportTextMathMode::Simplified) {
            raw = ReplaceMathSpansWithPlaceholder(model, doc, std::move(raw), Utf8ToWide(config.mathPlaceholder));
        }
        return WideToUtf8(raw);
    }

    const std::vector<std::vector<size_t>> children = BuildChildBlockLists(doc);
    std::wstring out;
    for (size_t rootChild : children[doc.blocks.size()]) {
        out += RenderPlainBlock(model, doc, children, rootChild, config);
    }
    return WideToUtf8(out);
}

std::string ExportMarkdown(const NoteTextModel& model,
                           const NoteDocument& doc,
                           const MarkupExportConfig& config) {
    std::wstring raw = model.raw;
    if (config.mathMode == ExportMarkupMathMode::Placeholder) {
        raw = ReplaceMathSpansWithPlaceholder(model, doc, std::move(raw), Utf8ToWide(config.mathPlaceholder));
    }
    if (config.includeTitleHeading) {
        raw = RewriteMarkdownHeadings(std::move(raw),
                                      config.title,
                                      config.includeTitleHeading,
                                      config.includeTitleHeading && config.shiftHeadingLevels);
    }
    return WideToUtf8(raw);
}

std::string ExportHtml(const NoteTextModel& model,
                       const NoteDocument& doc,
                       const MarkupExportConfig& config) {
    const std::vector<std::vector<size_t>> children = BuildChildBlockLists(doc);
    const std::string title = EscapeHtml(WideToUtf8(config.title));
    std::string out;
    out.reserve(4096);
    out += "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<title>" + title + "</title>\n";
    out += "<style>\n" + DefaultHtmlCss() + "\n</style>\n</head>\n<body>\n";
    if (config.includeTitleHeading && !config.title.empty()) {
        out += "<h1>" + title + "</h1>\n";
    }
    out += RenderHtmlChildren(model, doc, children, kInvalidIndex, config);
    out += "</body>\n</html>\n";
    return out;
}

bool WorkspaceNoteIndexCanExport(const WorkspaceNoteIndexSnapshot& index) {
    if (!index.valid || !index.snapshot_identity.valid()) return false;
    if (index.snapshot_identity.content_revision != 0 &&
        index.text_model.revision != index.snapshot_identity.content_revision) {
        return false;
    }
    const NoteDerivedSnapshotIdentity expectedSource{
        index.snapshot_identity.note_id, index.text_model.revision};
    if (!NoteDocumentMatchesSourceIdentity(index.document, expectedSource) ||
        !NoteDocumentMatchesTextModel(index.document, index.text_model)) {
        return false;
    }
    return index.snapshot_identity.content_fingerprint ==
        FingerprintSnapshotBytes(WideToUtf8(index.text_model.raw));
}

WorkspaceNoteExportResult ExportWorkspacePlainText(
    const WorkspaceNoteIndexSnapshot& index,
    const TextExportConfig& config) {
    if (!WorkspaceNoteIndexCanExport(index)) return {};
    return WorkspaceNoteExportResult{
        true,
        index.snapshot_identity,
        ExportPlainText(index.text_model, index.document, config),
    };
}

WorkspaceNoteExportResult ExportWorkspaceMarkdown(
    const WorkspaceNoteIndexSnapshot& index,
    const MarkupExportConfig& config) {
    if (!WorkspaceNoteIndexCanExport(index)) return {};
    return WorkspaceNoteExportResult{
        true,
        index.snapshot_identity,
        ExportMarkdown(index.text_model, index.document, config),
    };
}

WorkspaceNoteExportResult ExportWorkspaceHtml(
    const WorkspaceNoteIndexSnapshot& index,
    const MarkupExportConfig& config) {
    if (!WorkspaceNoteIndexCanExport(index)) return {};
    return WorkspaceNoteExportResult{
        true,
        index.snapshot_identity,
        ExportHtml(index.text_model, index.document, config),
    };
}

} // namespace note
