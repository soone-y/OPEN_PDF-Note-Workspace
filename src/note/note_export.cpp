#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "note/note_export.h"
#include "note/note_clro_markdown.h"
#include "note/note_markup_color.h"

#include "note/note_markdown_escape.h"
#include "note/note_parser.h"
#include "note/note_syntax_lexical.h"
#include "note/note_render_final_style.h"

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

// One immutable parser-proven token index per export, shared by all slices.
// Unknown/orphan tags and literal code are never reinterpreted or discarded.
struct ExportSource {
    const NoteTextModel& model;
    std::vector<Span> style_syntax;
    ExportSource(const NoteTextModel& text, const NoteDocument& doc) : model(text) {
        style_syntax.reserve(doc.style_spans.size() * 2);
        for (const auto& style : doc.style_spans) {
            Span token;
            if (FindNoteMarkupTagEndingAt(model.raw, style.span.start, &token, doc.uses_clro_fullwidth_markdown)) style_syntax.push_back(token);
            if (FindNoteMarkupTagStartingAt(model.raw, style.span.end, &token, doc.uses_clro_fullwidth_markdown)) style_syntax.push_back(token);
        }
        std::sort(style_syntax.begin(), style_syntax.end(), [](Span a, Span b) {
            return a.start != b.start ? a.start < b.start : a.end < b.end;
        });
        style_syntax.erase(std::unique(style_syntax.begin(), style_syntax.end(), [](Span a, Span b) {
            return a.start == b.start && a.end == b.end;
        }), style_syntax.end());
    }
    [[nodiscard]] std::wstring Slice(Span span, bool literal = false) const {
        if (span.end <= span.start || span.start.value >= model.raw.size()) return {};
        const size_t end = std::min(span.end.value, model.raw.size());
        size_t cursor = span.start.value;
        if (literal) return model.raw.substr(cursor, end - cursor);
        std::wstring out;
        const auto first = std::lower_bound(style_syntax.begin(), style_syntax.end(), span.start,
            [](Span token, Utf16CodeUnitOffset start) { return token.end <= start; });
        for (auto it = first; it != style_syntax.end() && it->start.value < end; ++it) {
            if (it->start.value > cursor) out.append(model.raw, cursor, it->start.value - cursor);
            cursor = std::max(cursor, std::min(end, it->end.value));
        }
        out.append(model.raw, cursor, end - cursor);
        return out;
    }
};

std::wstring SliceRenderedMarkdownText(const ExportSource& source, Span span) {
    return DecodeMarkdownBackslashEscapes(source.Slice(span));
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
                                          const BlockNode& block,
                                          size_t blockIndex) {
    (void)model;
    std::vector<Segment> text;
    std::vector<Segment> overlays;
    for (size_t i = 0; i < block.inline_count; ++i) {
        const size_t index = block.first_inline + i;
        if (index >= doc.inlines.size()) break;
        const auto& node = doc.inlines[index];
        if (node.parent_block != blockIndex) continue;
        if (node.kind == InlineKind::Text) text.push_back({Segment::Kind::Text, node.span, nullptr});
        if (node.kind == InlineKind::Code) overlays.push_back({Segment::Kind::Code, node.span, nullptr});
    }
    for (const auto& math : doc.math_spans) {
        if (math.span.start < block.span.end && math.span.end > block.span.start)
            overlays.push_back({Segment::Kind::Math, math.span, &math});
    }
    const auto order = [](const Segment& lhs, const Segment& rhs) {
        if (lhs.span.start != rhs.span.start) return lhs.span.start < rhs.span.start;
        return lhs.span.end < rhs.span.end;
    };
    std::sort(overlays.begin(), overlays.end(), order);
    // Code and math are parser-owned, disjoint literal regions. Text callbacks
    // also cover their contents; subtract those regions rather than outputting
    // the callback text and overlay twice.
    std::vector<Segment> out;
    for (const auto& segment : text) {
        auto overlay = std::lower_bound(overlays.begin(), overlays.end(), segment.span.start,
            [](const Segment& candidate, Utf16CodeUnitOffset start) { return candidate.span.end <= start; });
        size_t cursor = segment.span.start.value;
        for (; overlay != overlays.end() && overlay->span.start < segment.span.end; ++overlay) {
            if (cursor < overlay->span.start.value)
                out.push_back({Segment::Kind::Text, {{cursor}, overlay->span.start}, nullptr});
            out.push_back(*overlay);
            cursor = std::max(cursor, overlay->span.end.value);
        }
        if (cursor < segment.span.end.value)
            out.push_back({Segment::Kind::Text, {{cursor}, segment.span.end}, nullptr});
    }
    std::sort(out.begin(), out.end(), order);
    out.erase(std::unique(out.begin(), out.end(), [](const Segment& lhs, const Segment& rhs) {
        return lhs.kind == rhs.kind && lhs.span.start == rhs.span.start && lhs.span.end == rhs.span.end;
    }), out.end());
    return out;
}
std::wstring RenderPlainSegment(const ExportSource& source,
                                const Segment& segment,
                                const TextExportConfig& config) {
    const auto& model = source.model;
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
        return SliceRenderedMarkdownText(source, segment.span);
    }
}

std::wstring RenderPlainBlockInlineContent(const ExportSource& source,
                                           const NoteDocument& doc,
                                           const BlockNode& block,
                                           size_t blockIndex,
                                           const TextExportConfig& config) {
    const auto& model = source.model;
    std::wstring out;
    const std::vector<Segment> segments = BuildVisibleSegments(model, doc, block, blockIndex);
    for (const auto& segment : segments) {
        out += RenderPlainSegment(source, segment, config);
    }
    if (!out.empty() && out.back() != L'\n') out.push_back(L'\n');
    return out;
}

std::wstring BuildMarkdownHeadingPrefix(int level) {
    level = std::clamp(level, 1, 6);
    return std::wstring(static_cast<size_t>(level), L'#') + L' ';
}

std::string DefaultHtmlCss() {
    return R"CSS(
:root{--fg:#1c1d22;--muted:#5f6472;--bg:#fbfaf7;--paper:#ffffff;--line:#e7e0d6;}
body{font-family:"Segoe UI Variable Text","Yu Gothic UI","Hiragino Sans",sans-serif;font-size:16px;line-height:1.65;margin:28px;color:var(--fg);background:
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

[[nodiscard]] std::string CssColor(std::wstring_view value, bool background) {
    const auto parsed = ResolveNoteMarkupColor(value);
    if (parsed.kind == NoteMarkupColorKind::HighContrast)
        return background ? "Canvas" : "CanvasText";
    if (parsed.kind != NoteMarkupColorKind::Explicit) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string css(7, '#');
    for (int i = 0; i < 6; ++i) css[1 + i] = digits[(parsed.rgb >> (20 - 4 * i)) & 0xf];
    return css;
}

[[nodiscard]] std::string HtmlStyleCss(const std::vector<const StyleSpan*>& styles) {
    NoteRenderSourceRun run;
    bool underline = false, strike = false, accent = false;
    std::string colour, background;
    for (const auto* style : styles) {
        run.styles.push_back({style->kind, style->value});
        if (style->kind == StyleKind::TextColor) {
            const auto parsed = CssColor(style->value, false);
            if (!parsed.empty()) colour = parsed;
        } else if (style->kind == StyleKind::BackgroundColor) {
            const auto parsed = CssColor(style->value, true);
            if (!parsed.empty()) background = parsed;
        }
        underline = underline || style->kind == StyleKind::Underline || style->kind == StyleKind::LinkUnderline;
        strike = strike || style->kind == StyleKind::Strike;
        accent = accent || style->kind == StyleKind::LinkAccent;
    }
    // The HTML body has a fixed logical 16px base. Resolve the same bounded,
    // outer-to-inner arithmetic as GDI; never concatenate user CSS values.
    NoteRenderFinalRunFontStyle resolved;
    if (!ResolveNoteRenderFinalRunFontStyle({}, run, 16, &resolved)) return {};
    std::string css;
    if (resolved.bold) css += "font-weight:bold;";
    if (resolved.italic) css += "font-style:italic;";
    if (underline || strike) css += std::string("text-decoration:") +
        (underline ? "underline" : "") + (underline && strike ? " " : "") + (strike ? "line-through" : "") + ";";
    if (accent) colour = "#0b57d0";
    if (!colour.empty()) css += "color:" + colour + ";";
    if (!background.empty()) css += "background-color:" + background + ";";
    if (!resolved.font_family.empty()) {
        std::string face;
        for (const auto ch : WideToUtf8(resolved.font_family)) {
            if (ch == '\\' || ch == '"') face.push_back('\\');
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) face.push_back(' ');
            else face.push_back(ch);
        }
        css += "font-family:\"" + face + "\";";
    }
    if (resolved.font_size_specified) css += "font-size:" + std::to_string(resolved.font_height_px) + "px;";
    if (resolved.line_height_permille != 1000) {
        const uint64_t units = uint64_t{165} * resolved.line_height_permille;
        css += "line-height:" + std::to_string(units / 100000) + "." +
            std::to_string(100000 + units % 100000).substr(1) + ";";
    }
    return css;
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

// Both export and the immutable source plan use the lexical title rule.
// MD4C's nonempty code span starts at the body, so its opener is one row above.
[[nodiscard]] std::wstring FenceTitle(const NoteTextModel& model, const BlockNode& block) {
    const bool container = block.kind == BlockKind::FencedContainer;
    if ((!container && block.kind != BlockKind::CodeBlock) || block.span.end <= block.span.start ||
        block.loc.line <= (container ? 0 : 1)) return {};
    const size_t line = static_cast<size_t>(block.loc.line - (container ? 1 : 2));
    if (line >= model.line_starts.size()) return {};
    const size_t start = model.line_starts[line];
    const size_t foundEnd = model.raw.find_first_of(L"\r\n", start);
    const size_t end = foundEnd == std::wstring::npos ? model.raw.size() : foundEnd;
    const std::wstring_view raw(model.raw.data() + start, end - start);
    std::wstring syntax(raw);
    if (IsClroMarkdownFile(model.meta.file_name)) {
        std::transform(syntax.begin(), syntax.end(), syntax.begin(), ClroMarkdownCharacter);
    }
    const Span title = NoteFenceTitleSpan(syntax, container);
    return std::wstring(raw.substr(title.start.value, title.end - title.start));
}

std::wstring RenderPlainBlock(const ExportSource& source,
                              const NoteDocument& doc,
                              const std::vector<std::vector<size_t>>& children,
                              size_t blockIndex,
                              const TextExportConfig& config) {
    const auto& model = source.model;
    if (blockIndex >= doc.blocks.size()) return L"";
    const BlockNode& block = doc.blocks[blockIndex];
    std::wstring out;

    switch (block.kind) {
    case BlockKind::HorizontalRule:
        return L"\n";
    case BlockKind::CodeBlock:
        out = FenceTitle(model, block);
        if (!out.empty()) out.push_back(L'\n');
        out += source.Slice(block.span, true);
        if (!out.empty() && out.back() != L'\n') out.push_back(L'\n');
        return out;
    case BlockKind::Paragraph:
    case BlockKind::Heading:
        return RenderPlainBlockInlineContent(source, doc, block, blockIndex, config);
    case BlockKind::List:
    case BlockKind::ListItem:
    case BlockKind::Quote: {
        // MD4C may attach text directly to a list item or quote. Preserve it
        // before walking child blocks so text export removes only syntax.
        out += RenderPlainBlockInlineContent(source, doc, block, blockIndex, config);
        const size_t bucket = (blockIndex < children.size()) ? blockIndex : children.size() - 1;
        for (size_t child : children[bucket]) {
            out += RenderPlainBlock(source, doc, children, child, config);
        }
        return out;
    }
    case BlockKind::FencedContainer: {
        out = FenceTitle(model, block);
        if (!out.empty()) out.push_back(L'\n');
        for (size_t child : children[blockIndex]) {
            out += RenderPlainBlock(source, doc, children, child, config);
        }
        return out;
    }
    case BlockKind::Table:
    case BlockKind::TableHead:
    case BlockKind::TableBody:
    case BlockKind::TableRow:
    case BlockKind::TableHeaderCell:
    case BlockKind::TableCell: {
        out += RenderPlainBlockInlineContent(source, doc, block, blockIndex, config);
        const size_t bucket = (blockIndex < children.size()) ? blockIndex : children.size() - 1;
        for (size_t child : children[bucket]) {
            out += RenderPlainBlock(source, doc, children, child, config);
        }
        return out;
    }
    case BlockKind::Blank:
        return L"\n";
    }

    return out;
}

std::optional<std::wstring> RewriteMarkdownHeadings(std::wstring text,
                                                     const std::wstring& title,
                                                     bool includeTitleHeading,
                                                     bool shiftHeadingLevels) {
    NoteMetadata meta;
    meta.file_name = L"export.md";
    NoteTextModel model = MakeNoteTextModel(std::move(meta), text, 1);
    NoteDocument doc = ParseNoteDocument(model);
    if (NoteDocumentHasFatalParseFailure(doc)) return std::nullopt;

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

std::string RenderHtmlInlineRange(const ExportSource& source,
                                  const NoteDocument& doc,
                                  size_t blockIndex,
                                  const MarkupExportConfig& config) {
    const auto& model = source.model;
    if (blockIndex >= doc.blocks.size()) return {};
    const BlockNode& block = doc.blocks[blockIndex];
    std::vector<Segment> segments = BuildVisibleSegments(model, doc, block, blockIndex);
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
    std::stable_sort(styles.begin(), styles.end(), [](const StyleSpan& a, const StyleSpan& b) {
        return a.span.start != b.span.start ? a.span.start < b.span.start : a.span.end > b.span.end;
    });

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
            std::wstring text = SliceRenderedMarkdownText(source, seg.span);
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

        std::vector<const StyleSpan*> activeStyles;
        for (const auto& style : styles) {
            if (seg.span.start < style.span.start || seg.span.end > style.span.end) continue;
            activeStyles.push_back(&style);
        }
        const auto css = HtmlStyleCss(activeStyles);
        if (!css.empty()) {
            inner = "<span style=\"" + EscapeHtml(css) + "\">" + inner + "</span>";
        }

        out += inner;
    }
    return out;
}

std::string RenderHtmlBlock(const ExportSource& source,
                            const NoteDocument& doc,
                            const std::vector<std::vector<size_t>>& children,
                            size_t blockIndex,
                            const MarkupExportConfig& config);

std::string RenderHtmlChildren(const ExportSource& source,
                               const NoteDocument& doc,
                               const std::vector<std::vector<size_t>>& children,
                               size_t parentIndex,
                               const MarkupExportConfig& config) {
    const size_t bucket = (parentIndex == kInvalidIndex) ? doc.blocks.size() : parentIndex;
    std::string out;
    if (bucket >= children.size()) return out;
    for (size_t child : children[bucket]) {
        out += RenderHtmlBlock(source, doc, children, child, config);
    }
    return out;
}

std::string RenderHtmlBlock(const ExportSource& source,
                            const NoteDocument& doc,
                            const std::vector<std::vector<size_t>>& children,
                            size_t blockIndex,
                            const MarkupExportConfig& config) {
    const auto& model = source.model;
    if (blockIndex >= doc.blocks.size()) return {};
    const BlockNode& block = doc.blocks[blockIndex];

    switch (block.kind) {
    case BlockKind::HorizontalRule:
        return "<hr>\n";
    case BlockKind::Paragraph: {
        const std::vector<Segment> segments = BuildVisibleSegments(model, doc, block, blockIndex);
        if (segments.size() == 1 && segments[0].kind == Segment::Kind::Math &&
            segments[0].math && segments[0].math->kind == MathKind::Block) {
            if (config.mathMode == ExportMarkupMathMode::Placeholder) {
                return "<div>" + EscapeHtml(config.mathPlaceholder) + "</div>\n";
            }
            return "<div class=\"math block\">" + EscapeHtml(WideToUtf8(segments[0].math->normalized_tex)) + "</div>\n";
        }
        return "<p>" + RenderHtmlInlineRange(source, doc, blockIndex, config) + "</p>\n";
    }
    case BlockKind::Heading: {
        const bool shiftHeadings = config.includeTitleHeading && config.shiftHeadingLevels;
        const int level = std::clamp(shiftHeadings ? block.level + 1 : block.level, 1, 6);
        return "<h" + std::to_string(level) + ">" +
               RenderHtmlInlineRange(source, doc, blockIndex, config) +
               "</h" + std::to_string(level) + ">\n";
    }
    case BlockKind::CodeBlock: {
        const std::wstring title = FenceTitle(model, block);
        std::string out = title.empty() ? "" :
            "<div class=\"note-block-title\"><strong>" + EscapeHtml(WideToUtf8(title)) + "</strong></div>\n";
        out += "<pre><code";
        if (!block.info_string.empty()) {
            out += " class=\"language-" + EscapeHtml(WideToUtf8(block.info_string)) + "\"";
        }
        out += ">" + EscapeHtml(WideToUtf8(source.Slice(block.span, true))) + "</code></pre>\n";
        return out;
    }
    case BlockKind::List: {
        const char* tag = block.ordered ? "ol" : "ul";
        std::string out = std::string("<") + tag;
        if (block.ordered && block.start_number > 1) {
            out += " start=\"" + std::to_string(block.start_number) + "\"";
        }
        out += ">\n";
        out += RenderHtmlChildren(source, doc, children, blockIndex, config);
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
        out += RenderHtmlChildren(source, doc, children, blockIndex, config);
        while (!out.empty() && out.back() == '\n') out.pop_back();
        out += "</li>\n";
        return out;
    }
    case BlockKind::Quote:
        return "<blockquote>\n" + RenderHtmlChildren(source, doc, children, blockIndex, config) + "</blockquote>\n";
    case BlockKind::FencedContainer: {
        const std::wstring title = FenceTitle(model, block);
        const std::string header = title.empty() ? "" :
            "<div class=\"note-block-title\"><strong>" + EscapeHtml(WideToUtf8(title)) + "</strong></div>\n";
        return "<div class=\"note-container\" style=\"background:#f7f6f2;border:1px solid #b0aea6;padding:0.5em;\">\n" +
               header + RenderHtmlChildren(source, doc, children, blockIndex, config) + "</div>\n";
    }
    case BlockKind::Table:
        return "<table>\n" + RenderHtmlChildren(source, doc, children, blockIndex, config) + "</table>\n";
    case BlockKind::TableHead:
        return "<thead>\n" + RenderHtmlChildren(source, doc, children, blockIndex, config) + "</thead>\n";
    case BlockKind::TableBody:
        return "<tbody>\n" + RenderHtmlChildren(source, doc, children, blockIndex, config) + "</tbody>\n";
    case BlockKind::TableRow:
        return "<tr>" + RenderHtmlChildren(source, doc, children, blockIndex, config) + "</tr>\n";
    case BlockKind::TableHeaderCell: {
        const std::string css = TableCellAlignCss(block.table_cell_align);
        return std::string("<th") + (css.empty() ? "" : (" style=\"" + css + "\"")) + ">" +
               RenderHtmlInlineRange(source, doc, blockIndex, config) + "</th>";
    }
    case BlockKind::TableCell: {
        const std::string css = TableCellAlignCss(block.table_cell_align);
        return std::string("<td") + (css.empty() ? "" : (" style=\"" + css + "\"")) + ">" +
               RenderHtmlInlineRange(source, doc, blockIndex, config) + "</td>";
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

    const ExportSource source(model, doc);
    const std::vector<std::vector<size_t>> children = BuildChildBlockLists(doc);
    std::wstring out;
    for (size_t rootChild : children[doc.blocks.size()]) {
        out += RenderPlainBlock(source, doc, children, rootChild, config);
    }
    return WideToUtf8(out);
}

std::optional<std::string> ExportMarkdown(const NoteTextModel& model,
                                          const NoteDocument& doc,
                                          const MarkupExportConfig& config) {
    if (NoteDocumentHasFatalParseFailure(doc)) return std::nullopt;
    std::wstring raw = model.raw;
    const bool normalized = config.normalizeClroSyntax && IsClroMarkdownFile(model.meta.file_name) &&
                            doc.uses_clro_fullwidth_markdown;
    if (normalized) {
        auto converted = BuildClroMarkdownExportSource(model);
        if (!converted) return std::nullopt;
        raw = std::move(*converted);
    }
    if (config.mathMode == ExportMarkupMathMode::Placeholder) {
        if (normalized) {
            const auto outputModel = MakeNoteTextModel({L"export.md", L""}, raw, model.revision);
            const auto outputDoc = ParseNoteDocument(outputModel);
            if (NoteDocumentHasFatalParseFailure(outputDoc)) return std::nullopt;
            raw = ReplaceMathSpansWithPlaceholder(outputModel, outputDoc, std::move(raw), Utf8ToWide(config.mathPlaceholder));
        } else {
            raw = ReplaceMathSpansWithPlaceholder(model, doc, std::move(raw), Utf8ToWide(config.mathPlaceholder));
        }
    }
    if (config.includeTitleHeading) {
        auto rewritten = RewriteMarkdownHeadings(std::move(raw),
                                                 config.title,
                                                 config.includeTitleHeading,
                                                 config.includeTitleHeading && config.shiftHeadingLevels);
        if (!rewritten) return std::nullopt;
        raw = std::move(*rewritten);
    }
    return WideToUtf8(raw);
}

std::string ExportHtml(const NoteTextModel& model,
                       const NoteDocument& doc,
                       const MarkupExportConfig& config) {
    const ExportSource source(model, doc);
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
    out += RenderHtmlChildren(source, doc, children, kInvalidIndex, config);
    out += "</body>\n</html>\n";
    return out;
}

bool WorkspaceNoteIndexCanExport(const WorkspaceNoteIndexSnapshot& index) {
    if (!index.valid || !index.snapshot_identity.valid() ||
        NoteDocumentHasFatalParseFailure(index.document)) return false;
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
    auto bytes = ExportMarkdown(index.text_model, index.document, config);
    if (!bytes) return {};
    return WorkspaceNoteExportResult{
        true,
        index.snapshot_identity,
        std::move(*bytes),
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
