#include "reader_document.h"
#include "core/localization.h"
#include "readonly_viewer/mermaid_subset_parser.h"
#include "note/note_syntax_lexical.h"
#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <sstream>

std::wstring_view TrimMarkdownLine(std::wstring_view line) {
    while (!line.empty() && iswspace(line.front())) line.remove_prefix(1);
    while (!line.empty() && iswspace(line.back())) line.remove_suffix(1);
    return line;
}

void AppendStyledText(RenderedDocument& document, std::wstring_view text, unsigned int style, int headingLevel) {
    if (text.empty()) return;
    const size_t start = document.text.size();
    document.text.append(text);
    const size_t end = document.text.size();
    if (style == TextStyleNone && headingLevel == 0) return;
    if (!document.styles.empty()) {
        StyleRun& previous = document.styles.back();
        if (previous.end == start && previous.style == style && previous.headingLevel == headingLevel) {
            previous.end = end;
            return;
        }
    }
    document.styles.push_back({start, end, style, headingLevel});
}

void RenderInlineMarkdown(
    std::wstring_view source,
    size_t start,
    size_t end,
    RenderedDocument& document,
    unsigned int style,
    int headingLevel, unsigned int depth = 0) {
    if (depth >= 32) { AppendStyledText(document, source.substr(start, end - start), style, headingLevel); return; }
    size_t pos = start;
    while (pos < end) {
        if (source[pos] == L'\\' && pos + 1 < end) {
            AppendStyledText(document, source.substr(pos + 1, 1), style, headingLevel);
            pos += 2;
            continue;
        }

        if (source[pos] == L'`') {
            size_t markerEnd = pos;
            while (markerEnd < end && source[markerEnd] == L'`') ++markerEnd;
            size_t close = markerEnd, closeEnd = markerEnd;
            bool matched = false;
            while (close < end) {
                close = source.find(L'`', close);
                if (close == std::wstring_view::npos || close >= end) break;
                closeEnd = close;
                while (closeEnd < end && source[closeEnd] == L'`') ++closeEnd;
                if (closeEnd - close == markerEnd - pos) { matched = true; break; }
                close = closeEnd;
            }
            if (matched) {
                auto content = source.substr(markerEnd, close - markerEnd);
                if (content.size() >= 2 && content.front() == L' ' && content.back() == L' ' &&
                    content.find_first_not_of(L' ') != std::wstring_view::npos) {
                    content.remove_prefix(1); content.remove_suffix(1);
                }
                AppendStyledText(document, content, style | TextStyleCode, headingLevel);
                pos = closeEnd;
                continue;
            }
            AppendStyledText(document, source.substr(pos, markerEnd - pos), style, headingLevel);
            pos = markerEnd;
            continue;
        }

        const bool strongMarker = pos + 1 < end &&
            ((source[pos] == L'*' && source[pos + 1] == L'*') ||
             (source[pos] == L'_' && source[pos + 1] == L'_'));
        if (strongMarker) {
            const std::wstring_view marker = source.substr(pos, 2);
            const size_t close = source.find(marker, pos + 2);
            if (close != std::wstring_view::npos && close < end) {
                RenderInlineMarkdown(source, pos + 2, close, document, style | TextStyleBold, headingLevel, depth + 1);
                pos = close + 2;
                continue;
            }
        }

        if (pos + 1 < end && source[pos] == L'~' && source[pos + 1] == L'~') {
            const size_t close = source.find(L"~~", pos + 2);
            if (close != std::wstring_view::npos && close < end) {
                RenderInlineMarkdown(source, pos + 2, close, document, style | TextStyleStrike, headingLevel, depth + 1);
                pos = close + 2;
                continue;
            }
        }

        if (source[pos] == L'*' || source[pos] == L'_') {
            const wchar_t marker = source[pos];
            const size_t close = source.find(marker, pos + 1);
            if (close != std::wstring_view::npos && close < end && close > pos + 1) {
                RenderInlineMarkdown(source, pos + 1, close, document, style | TextStyleItalic, headingLevel, depth + 1);
                pos = close + 1;
                continue;
            }
        }

        const bool image = source[pos] == L'!' && pos + 1 < end && source[pos + 1] == L'[';
        const bool link = source[pos] == L'[';
        if (image || link) {
            const size_t labelStart = pos + (image ? 2 : 1);
            const size_t labelEnd = source.find(L']', labelStart);
            if (labelEnd != std::wstring_view::npos && labelEnd + 1 < end && source[labelEnd + 1] == L'(') {
                const size_t targetEnd = source.find(L')', labelEnd + 2);
                if (targetEnd != std::wstring_view::npos && targetEnd < end) {
                    if (image) AppendStyledText(document, localization::Text(L"readonly.render.image_prefix"), style, headingLevel);
                    RenderInlineMarkdown(source, labelStart, labelEnd, document, style | TextStyleLink, headingLevel, depth + 1);
                    pos = targetEnd + 1;
                    continue;
                }
            }
        }

        AppendStyledText(document, source.substr(pos, 1), style, headingLevel);
        ++pos;
    }
}

[[nodiscard]] bool IsHorizontalRule(std::wstring_view line) {
    wchar_t marker = 0;
    int count = 0;
    for (wchar_t c : line) {
        if (iswspace(c)) continue;
        if (c != L'-' && c != L'*' && c != L'_') return false;
        if (marker == 0) marker = c;
        if (c != marker) return false;
        ++count;
    }
    return count >= 3;
}

[[nodiscard]] bool IsReaderEscapedAt(std::wstring_view line, size_t position) {
    size_t slashes = 0;
    while (position > 0 && line[--position] == L'\\') ++slashes;
    return (slashes % 2) != 0;
}

[[nodiscard]] bool ParseMarkdownTableRow(std::wstring_view line, std::vector<std::wstring>* cells) {
    cells->clear();
    while (!line.empty() && iswspace(line.front())) line.remove_prefix(1);
    while (!line.empty() && iswspace(line.back())) line.remove_suffix(1);
    if (line.empty() || line.find(L'|') == std::wstring_view::npos) return false;
    if (line.front() == L'|') line.remove_prefix(1);
    if (!line.empty() && line.back() == L'|' && !IsReaderEscapedAt(line, line.size() - 1)) line.remove_suffix(1);
    size_t start = 0;
    while (start <= line.size()) {
        size_t end = std::wstring_view::npos;
        for (size_t i = start; i < line.size(); ++i) {
            if (line[i] == L'\\' && i + 1 < line.size()) { ++i; continue; }
            // Match the main app's cell boundary contract: an unmatched code
            // marker cannot consume later cells; literal pipes must be escaped.
            if (line[i] == L'|') { end = i; break; }
        }
        std::wstring_view cell = line.substr(start, end == std::wstring_view::npos ? line.size() - start : end - start);
        while (!cell.empty() && iswspace(cell.front())) cell.remove_prefix(1);
        while (!cell.empty() && iswspace(cell.back())) cell.remove_suffix(1);
        if (cell.size() > 65536 || cells->size() >= 16) return false;
        cells->emplace_back(cell);
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return !cells->empty();
}

[[nodiscard]] bool IsMarkdownTableSeparator(const std::vector<std::wstring>& cells) {
    if (cells.empty()) return false;
    for (const std::wstring& value : cells) {
        size_t first = 0, last = value.size();
        if (first < last && value[first] == L':') ++first;
        if (first < last && value[last - 1] == L':') --last;
        if (last - first < 3) return false;
        for (size_t index = first; index < last; ++index) if (value[index] != L'-') return false;
    }
    return true;
}

void AppendMarkdownTable(RenderedDocument& document, const std::vector<std::vector<std::wstring>>& rows, const std::vector<std::wstring>& separators) {
    if (rows.empty()) return;
    ReaderTable table; table.start = document.text.size();
    for (const auto& separator : separators) {
        const bool left = separator.front() == L':', right = separator.back() == L':';
        table.alignment.push_back(right ? (left ? 1 : 2) : 0);
    }
    for (size_t row = 0; row < rows.size(); ++row) {
        std::vector<std::wstring> displayed;
        for (size_t col = 0; col < rows[row].size(); ++col) {
            const size_t cellStart = document.text.size();
            RenderInlineMarkdown(rows[row][col], 0, rows[row][col].size(), document,
                                 row == 0 ? TextStyleBold : TextStyleNone, 0);
            displayed.push_back(document.text.substr(cellStart));
            document.text.push_back(col + 1 < rows[row].size() ? L'\t' : L'\n');
        }
        table.rows.push_back(std::move(displayed));
    }
    table.end = document.text.size();
    document.tables.push_back(std::move(table));
}

RenderedDocument RenderMarkdown(const std::wstring& source, std::vector<InlineDiagram>* inline_diagrams) {
    RenderedDocument document;
    bool inCodeBlock = false;
    bool inMermaidBlock = false;
    note::NoteCodeFenceRun openFence{};
    std::wstring mermaid_source;
    size_t diagram_number = 0;
    size_t lineStart = 0;
    while (lineStart <= source.size()) {
        size_t lineEnd = source.find(L'\n', lineStart);
        if (lineEnd == std::wstring::npos) lineEnd = source.size();
        size_t contentEnd = lineEnd;
        if (contentEnd > lineStart && source[contentEnd - 1] == L'\r') --contentEnd;
        std::wstring_view line(source.data() + lineStart, contentEnd - lineStart);

        size_t first = 0;
        while (first < line.size() && first < 3 && line[first] == L' ') ++first;
        const std::wstring_view trimmed = TrimMarkdownLine(line);
        note::NoteCodeFenceRun fence{};
        const bool hasFence = note::TryParseNoteCodeFenceRun(line, {0}, {line.size()},
            inMermaidBlock || inCodeBlock, &fence);
        const bool closesFence = hasFence && fence.marker == openFence.marker && fence.marker_count >= openFence.marker_count;
        if (!inMermaidBlock && !inCodeBlock && !hasFence) {
            std::vector<std::wstring> header;
            const size_t separator_start = lineEnd == source.size() ? source.size() : lineEnd + 1;
            const size_t separator_end = source.find(L'\n', separator_start);
            const size_t separator_limit = separator_end == std::wstring::npos ? source.size() : separator_end;
            std::wstring_view separator(source.data() + separator_start, separator_limit - separator_start);
            if (!separator.empty() && separator.back() == L'\r') separator.remove_suffix(1);
            std::vector<std::wstring> separator_cells;
            const auto setext = TrimMarkdownLine(separator);
            if (!trimmed.empty() && !setext.empty() && (setext.front() == L'=' || setext.front() == L'-') &&
                setext.find_first_not_of(setext.front()) == std::wstring_view::npos) {
                const int level = setext.front() == L'=' ? 1 : 2;
                const size_t offset = document.text.size();
                RenderInlineMarkdown(trimmed, 0, trimmed.size(), document, TextStyleBold, level);
                document.headings.push_back({level, offset, document.text.substr(offset), lineStart});
                document.text.push_back(L'\n');
                lineStart = separator_end == std::wstring::npos ? source.size() : separator_end + 1;
                continue;
            }
            if (ParseMarkdownTableRow(line, &header) && ParseMarkdownTableRow(separator, &separator_cells) &&
                header.size() == separator_cells.size() && IsMarkdownTableSeparator(separator_cells)) {
                std::vector<std::vector<std::wstring>> rows;
                rows.push_back(std::move(header));
                size_t next_start = separator_end == std::wstring::npos ? source.size() : separator_end + 1;
                while (next_start < source.size() && rows.size() < 512) {
                    const size_t next_end = source.find(L'\n', next_start);
                    const size_t next_limit = next_end == std::wstring::npos ? source.size() : next_end;
                    std::wstring_view candidate(source.data() + next_start, next_limit - next_start);
                    if (!candidate.empty() && candidate.back() == L'\r') candidate.remove_suffix(1);
                    std::vector<std::wstring> cells;
                    if (!ParseMarkdownTableRow(candidate, &cells) || cells.size() != rows.front().size()) break;
                    rows.push_back(std::move(cells));
                    next_start = next_end == std::wstring::npos ? source.size() : next_end + 1;
                }
                AppendMarkdownTable(document, rows, separator_cells);
                lineStart = next_start;
                continue;
            }
        }
        if (inMermaidBlock) {
            if (closesFence) {
                const textviewer::mermaid::ParseResult result = textviewer::mermaid::ParseFlowchart(mermaid_source);
                if (result.can_render() && inline_diagrams && inline_diagrams->size() < 16) {
                    ++diagram_number;
                    const size_t text_offset = document.text.size();
                    AppendStyledText(document, localization::Format(L"readonly.render.diagram_label", {{L"NUMBER", std::to_wstring(diagram_number)}}) + L"\n", TextStyleItalic, 0);
                    // Reserve vertical space in the RichEdit document for the child preview.
                    for (int blank_line = 0; blank_line < 15; ++blank_line) document.text.push_back(L'\n');
                    inline_diagrams->push_back({text_offset, document.text.size() - text_offset, result.model});
                } else {
                    AppendStyledText(document, mermaid_source, TextStyleCode, 0);
                    if (!mermaid_source.empty() && mermaid_source.back() != L'\n') document.text.push_back(L'\n');
                }
                mermaid_source.clear();
                inMermaidBlock = false;
            } else {
                mermaid_source.append(line);
                mermaid_source.push_back(L'\n');
            }
        } else if (inCodeBlock) {
            if (closesFence) inCodeBlock = false;
            else {
                AppendStyledText(document, line, TextStyleCode, 0);
                document.text.push_back(L'\n');
            }
        } else if (hasFence) {
            openFence = fence;
            inMermaidBlock = TrimMarkdownLine(trimmed.substr(fence.marker_count)) == L"mermaid";
            inCodeBlock = !inMermaidBlock;
            mermaid_source.clear();
        } else {
            int headingLevel = 0;
            size_t contentStart = first;
            while (contentStart + headingLevel < line.size() && headingLevel < 6 &&
                   line[contentStart + headingLevel] == L'#') {
                ++headingLevel;
            }
            if (headingLevel > 0 && contentStart + headingLevel < line.size() &&
                iswspace(line[contentStart + headingLevel])) {
                contentStart += headingLevel;
                while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                size_t headingEnd = line.size();
                while (headingEnd > contentStart && iswspace(line[headingEnd - 1])) --headingEnd;
                while (headingEnd > contentStart && line[headingEnd - 1] == L'#') --headingEnd;
                while (headingEnd > contentStart && iswspace(line[headingEnd - 1])) --headingEnd;
                const size_t headingOffset = document.text.size();
                RenderInlineMarkdown(line, contentStart, headingEnd, document, TextStyleBold, headingLevel);
                document.headings.push_back({headingLevel, headingOffset, document.text.substr(headingOffset), lineStart});
                document.text.push_back(L'\n');
            } else if (IsHorizontalRule(line)) {
                AppendStyledText(document, L"────────────────────────", TextStyleNone, 0);
                document.text.push_back(L'\n');
            } else {
                contentStart = 0;
                while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                unsigned int baseStyle = TextStyleNone;
                if (contentStart < line.size() && line[contentStart] == L'>') {
                    AppendStyledText(document, L"│ ", TextStyleItalic, 0);
                    ++contentStart;
                    if (contentStart < line.size() && line[contentStart] == L' ') ++contentStart;
                    baseStyle |= TextStyleItalic;
                } else if (contentStart + 1 < line.size() &&
                           (line[contentStart] == L'-' || line[contentStart] == L'*' || line[contentStart] == L'+') &&
                           iswspace(line[contentStart + 1])) {
                    AppendStyledText(document, L"• ", TextStyleBold, 0);
                    contentStart += 2;
                    while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                    if (contentStart + 2 < line.size() && line[contentStart] == L'[' && line[contentStart + 2] == L']') {
                        const wchar_t checked = static_cast<wchar_t>(towlower(line[contentStart + 1]));
                        AppendStyledText(document, checked == L'x' ? L"☑ " : L"☐ ", TextStyleNone, 0);
                        contentStart += 3;
                        while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                    }
                }
                RenderInlineMarkdown(line, contentStart, line.size(), document, baseStyle, 0);
                document.text.push_back(L'\n');
            }
        }

        if (lineEnd == source.size()) break;
        lineStart = lineEnd + 1;
    }
    // A malformed/unclosed diagram remains readable; never drop its body or
    // publish a partially parsed model. The source itself is never modified.
    if (inMermaidBlock) AppendStyledText(document, mermaid_source, TextStyleCode, 0);
    return document;
}

namespace {
void RtfText(std::string& out, std::wstring_view value, bool previousNewline) {
    for (wchar_t c : value) {
        if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
        else if (c == L'\n') out += previousNewline ? "\\fs8\\par\\fs25\n" : "\\par\n";
        else if (c == L'\r') continue;
        else if (c == L'\t') out += "\\tab ";
        else if (c >= 32 && c < 127) out += static_cast<char>(c);
        else out += "\\u" + std::to_string(static_cast<int16_t>(c)) + "?";
        previousNewline = c == L'\n';
    }
}
void StyledRange(std::string& out, const RenderedDocument& doc, size_t start, size_t end, size_t& runIndex) {
    while (start < end) {
        while (runIndex < doc.styles.size() && doc.styles[runIndex].end <= start) ++runIndex;
        const StyleRun* run = runIndex < doc.styles.size() && doc.styles[runIndex].start <= start ? &doc.styles[runIndex] : nullptr;
        const size_t limit = std::min(end, run ? run->end : (runIndex < doc.styles.size() ? doc.styles[runIndex].start : end));
        out += "{\\f0\\fs25\\cf1 ";
        if (run) {
            if (run->style & TextStyleBold) out += "\\b ";
            if (run->style & TextStyleItalic) out += "\\i ";
            if (run->style & TextStyleStrike) out += "\\strike ";
            if (run->style & TextStyleLink) out += "\\cf3 ";
            if (run->headingLevel) {
                out += "\\cf3\\fs" + std::to_string(ReaderHeadingFontHalfPoints(run->headingLevel)) + " ";
            }
            if (run->style & TextStyleCode) out += "\\f1\\highlight2 ";
        }
        RtfText(out, std::wstring_view(doc.text).substr(start, limit - start), start > 0 && doc.text[start - 1] == L'\n');
        out += '}'; start = limit;
    }
}
}

int ReaderHeadingFontHalfPoints(int level) {
    static const int sizes[] = {25, 42, 35, 30, 26, 24, 22};
    return sizes[std::clamp(level, 0, 6)];
}

std::string BuildReaderRtf(const RenderedDocument& doc, int widthTwips) {
    // Only this generator creates RTF controls; every document character is escaped.
    // Native tables keep full cell content, wrap at pixel-based column boundaries,
    // and are ordinary selectable/searchable text. No OLE/fields or URL loading.
    std::string out = "{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0\\fnil\\fcharset128 Yu Gothic UI;}{\\f1\\fmodern Consolas;}}"
        "{\\colortbl;\\red32\\green44\\blue56;\\red235\\green239\\blue243;\\red24\\green69\\blue91;"
        "\\red209\\green219\\blue228;\\red229\\green237\\blue243;\\red245\\green247\\blue249;}"
        "\\viewkind4\\pard\\f0\\fs25\\cf1\\sl240\\slmult1\\sa60 ";
    size_t position = 0, runIndex = 0;
    for (const auto& table : doc.tables) {
        StyledRange(out, doc, position, table.start, runIndex);
        const size_t columns = table.rows.front().size();
        std::vector<int> weights(columns, 8);
        for (const auto& row : table.rows) for (size_t c = 0; c < columns; ++c) {
            int weight = 0;
            for (wchar_t ch : row[c]) weight += ch >= 0x2e80 ? 2 : 1;
            weights[c] = std::max(weights[c], std::min(weight, 60));
        }
        int sum = 0; for (int weight : weights) sum += weight;
        size_t cellStart = table.start;
        for (size_t row = 0; row < table.rows.size(); ++row) {
            out += "\\trowd\\trgaph100\\trleft0\\trftsWidth3\\trwWidth" + std::to_string(widthTwips);
            int edge = 0;
            for (size_t col = 0; col < columns; ++col) {
                edge += widthTwips * weights[col] / sum;
                if (col + 1 == columns) edge = widthTwips;
                out += "\\clvertalt\\clbrdrt\\brdrs\\brdrw10\\brdrcf4\\clbrdrb\\brdrs\\brdrw10\\brdrcf4"
                       "\\clbrdrl\\brdrs\\brdrw10\\brdrcf4\\clbrdrr\\brdrs\\brdrw10\\brdrcf4"
                       "\\clcbpat" + std::to_string(row == 0 ? 5 : (row % 2 ? 0 : 6)) + "\\cellx" + std::to_string(edge);
            }
            for (size_t col = 0; col < columns; ++col) {
                out += "\\pard\\intbl\\sl240\\slmult1\\sa100\\sb100 ";
                out += table.alignment[col] == 1 ? "\\qc " : (table.alignment[col] == 2 ? "\\qr " : "\\ql ");
                const size_t cellEnd = cellStart + table.rows[row][col].size();
                StyledRange(out, doc, cellStart, cellEnd, runIndex);
                out += "\\cell "; cellStart = cellEnd + 1;
            }
            out += "\\row\n";
        }
        out += "\\pard\\sl240\\slmult1\\sa60 ";
        position = table.end;
    }
    StyledRange(out, doc, position, doc.text.size(), runIndex);
    out += '}';
    return out;
}
