#include "note/note_parser.h"

#include "note/note_md4c_adapter.h"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string_view>

namespace note {
namespace {

constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

size_t AppendDiagnostic(NoteDocument* doc,
                        std::wstring code,
                        std::wstring message,
                        Span span,
                        DiagnosticSeverity severity);

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

bool EqualsAsciiNoCase(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        wchar_t l = lhs[i];
        wchar_t r = rhs[i];
        if (l >= L'A' && l <= L'Z') l = static_cast<wchar_t>(l - L'A' + L'a');
        if (r >= L'A' && r <= L'Z') r = static_cast<wchar_t>(r - L'A' + L'a');
        if (l != r) return false;
    }
    return true;
}

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

size_t TrimLineStart(std::wstring_view raw, size_t start, size_t end) {
    while (start < end && iswspace(raw[start])) {
        ++start;
    }
    return start;
}

size_t TrimLineEnd(std::wstring_view raw, size_t start, size_t end) {
    while (end > start && iswspace(raw[end - 1])) {
        --end;
    }
    return end;
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

bool IsBackslashEscaped(std::wstring_view text, size_t pos) {
    size_t count = 0;
    while (pos > 0 && text[pos - 1] == L'\\') {
        --pos;
        ++count;
    }
    return (count % 2) == 1;
}

bool TryParseFenceRun(std::wstring_view raw,
                     size_t lineStart,
                     size_t lineEnd,
                     wchar_t* outMarker,
                     size_t* outCount,
                     bool closingOnly) {
    if (!outMarker || !outCount) return false;
    size_t start = lineStart;
    while (start < lineEnd && (raw[start] == L' ' || raw[start] == L'\t')) {
        ++start;
    }
    if (start >= lineEnd) return false;
    const wchar_t marker = raw[start];
    if (marker != L'`' && marker != L'~') return false;

    size_t count = 0;
    while (start + count < lineEnd && raw[start + count] == marker) {
        ++count;
    }
    if (count < 3) return false;
    if (closingOnly) {
        size_t tail = start + count;
        while (tail < lineEnd) {
            if (!iswspace(raw[tail])) return false;
            ++tail;
        }
    }
    *outMarker = marker;
    *outCount = count;
    return true;
}

std::vector<Span> CollectRawMarkdownLiteralSpans(std::wstring_view raw) {
    std::vector<Span> spans;
    std::vector<Span> fencedSpans;
    bool inFence = false;
    wchar_t fenceMarker = 0;
    size_t fenceCount = 0;
    size_t fenceStart = 0;

    const std::vector<size_t> lineStarts = BuildLineStarts(raw);
    for (size_t lineIndex = 0; lineIndex < lineStarts.size(); ++lineIndex) {
        const size_t lineStart = lineStarts[lineIndex];
        size_t lineEnd = (lineIndex + 1 < lineStarts.size()) ? lineStarts[lineIndex + 1] : raw.size();
        size_t lineSpanEnd = lineEnd;
        if (lineSpanEnd > lineStart && raw[lineSpanEnd - 1] == L'\n') --lineSpanEnd;
        if (lineSpanEnd > lineStart && raw[lineSpanEnd - 1] == L'\r') --lineSpanEnd;

        wchar_t marker = 0;
        size_t count = 0;
        if (!inFence) {
            if (TryParseFenceRun(raw, lineStart, lineSpanEnd, &marker, &count, false)) {
                inFence = true;
                fenceMarker = marker;
                fenceCount = count;
                fenceStart = lineStart;
            }
            continue;
        }

        if (TryParseFenceRun(raw, lineStart, lineSpanEnd, &marker, &count, true) &&
            marker == fenceMarker &&
            count >= fenceCount) {
            fencedSpans.push_back(Span{fenceStart, lineEnd});
            inFence = false;
            fenceMarker = 0;
            fenceCount = 0;
        }
    }
    if (inFence) {
        fencedSpans.push_back(Span{fenceStart, raw.size()});
    }

    spans = fencedSpans;
    size_t fenceIndex = 0;
    size_t pos = 0;
    while (pos < raw.size()) {
        while (fenceIndex < fencedSpans.size() && pos >= fencedSpans[fenceIndex].end.value) {
            ++fenceIndex;
        }
        if (fenceIndex < fencedSpans.size() &&
            pos >= fencedSpans[fenceIndex].start.value &&
            pos < fencedSpans[fenceIndex].end.value) {
            pos = fencedSpans[fenceIndex].end.value;
            continue;
        }
        if (raw[pos] != L'`' || IsBackslashEscaped(raw, pos)) {
            ++pos;
            continue;
        }

        size_t runLen = 1;
        while (pos + runLen < raw.size() && raw[pos + runLen] == L'`') {
            ++runLen;
        }
        size_t search = pos + runLen;
        bool found = false;
        while (search < raw.size()) {
            while (fenceIndex < fencedSpans.size() && search >= fencedSpans[fenceIndex].end.value) {
                ++fenceIndex;
            }
            if (fenceIndex < fencedSpans.size() &&
                search >= fencedSpans[fenceIndex].start.value &&
                search < fencedSpans[fenceIndex].end.value) {
                search = fencedSpans[fenceIndex].end.value;
                continue;
            }
            if (raw[search] != L'`' || IsBackslashEscaped(raw, search)) {
                ++search;
                continue;
            }
            size_t closeRunLen = 1;
            while (search + closeRunLen < raw.size() && raw[search + closeRunLen] == L'`') {
                ++closeRunLen;
            }
            if (closeRunLen == runLen) {
                spans.push_back(Span{pos, search + closeRunLen});
                pos = search + closeRunLen;
                found = true;
                break;
            }
            search += closeRunLen;
        }
        if (!found) {
            pos += runLen;
        }
    }

    return spans;
}

std::vector<Span> CollectLiteralSpans(const NoteTextModel& model, const NoteDocument& doc) {
    std::vector<Span> spans = CollectRawMarkdownLiteralSpans(model.raw);
    spans.reserve(spans.size() + doc.blocks.size() + doc.inlines.size());
    for (const auto& block : doc.blocks) {
        if (block.kind == BlockKind::CodeBlock && block.span.end > block.span.start) {
            spans.push_back(block.span);
        }
    }
    for (const auto& inlineNode : doc.inlines) {
        if (inlineNode.kind == InlineKind::Code && inlineNode.span.end > inlineNode.span.start) {
            spans.push_back(inlineNode.span);
        }
    }
    std::sort(spans.begin(), spans.end(), [](const Span& lhs, const Span& rhs) {
        if (lhs.start != rhs.start) return lhs.start < rhs.start;
        return lhs.end < rhs.end;
    });
    std::vector<Span> merged;
    merged.reserve(spans.size());
    for (const auto& span : spans) {
        if (span.end <= span.start) continue;
        if (merged.empty() || span.start > merged.back().end) {
            merged.push_back(span);
            continue;
        }
        merged.back().end = std::max(merged.back().end, span.end);
    }
    return merged;
}

bool IsInsideAnySpan(const std::vector<Span>& spans, Span span) {
    for (const auto& candidate : spans) {
        if (span.start < candidate.end && span.end > candidate.start) {
            return true;
        }
    }
    return false;
}

bool ContainsLineBreak(std::wstring_view text, size_t start, size_t end) {
    end = std::min(end, text.size());
    for (size_t i = start; i < end; ++i) {
        if (text[i] == L'\n' || text[i] == L'\r') {
            return true;
        }
    }
    return false;
}

bool HasOnlyLineWhitespaceBefore(std::wstring_view text, size_t pos) {
    size_t cursor = pos;
    while (cursor > 0) {
        const wchar_t ch = text[cursor - 1];
        if (ch == L'\n' || ch == L'\r') break;
        if (ch != L' ' && ch != L'\t') return false;
        --cursor;
    }
    return true;
}

bool HasOnlyLineWhitespaceAfter(std::wstring_view text, size_t pos) {
    size_t cursor = pos;
    while (cursor < text.size()) {
        const wchar_t ch = text[cursor];
        if (ch == L'\n' || ch == L'\r') break;
        if (ch != L' ' && ch != L'\t') return false;
        ++cursor;
    }
    return true;
}

bool IsStandaloneMathRange(std::wstring_view text, size_t start, size_t end) {
    return HasOnlyLineWhitespaceBefore(text, start) &&
           HasOnlyLineWhitespaceAfter(text, end);
}

std::wstring NormalizeMathText(std::wstring_view text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') {
                continue;
            }
            normalized.push_back(L'\n');
            continue;
        }
        normalized.push_back(ch);
    }
    return normalized;
}

bool ConsumeMathBraceGroup(std::wstring_view text, size_t* pos, std::wstring* out) {
    if (!pos || !out) return false;
    size_t cursor = *pos;
    while (cursor < text.size() && iswspace(text[cursor])) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != L'{') return false;

    ++cursor;
    int depth = 1;
    out->clear();
    while (cursor < text.size() && depth > 0) {
        const wchar_t ch = text[cursor];
        if (ch == L'{') {
            ++depth;
            if (depth > 1) out->push_back(ch);
            ++cursor;
            continue;
        }
        if (ch == L'}') {
            --depth;
            ++cursor;
            if (depth > 0) out->push_back(ch);
            continue;
        }
        out->push_back(ch);
        ++cursor;
    }
    if (depth != 0) return false;
    *pos = cursor;
    return true;
}

bool IsSupportedMathEnvironment(std::wstring_view name) {
    return name == L"matrix" ||
           name == L"pmatrix" ||
           name == L"bmatrix" ||
           name == L"Bmatrix" ||
           name == L"vmatrix" ||
           name == L"Vmatrix" ||
           name == L"cases" ||
           name == L"aligned" ||
           name == L"align";
}

bool IsSupportedMathCommand(std::wstring_view name) {
    static constexpr std::wstring_view kCommands[] = {
        L"frac", L"dfrac", L"tfrac", L"sqrt", L"binom",
        L"begin", L"end",
        L"bar", L"overline", L"hat", L"widehat", L"vec", L"dot", L"ddot",
        L"operatorname", L"mathrm", L"mathbf", L"mathit", L"mathsf", L"mathtt",
        L"mathbb", L"mathcal", L"text", L"textrm",
        L"limits", L"nolimits", L"displaystyle", L"textstyle",
        L"int", L"oint", L"sum", L"prod", L"lim", L"limsup", L"liminf",
        L"infty", L"partial", L"forall", L"exists", L"nabla", L"angle", L"circ",
        L"cdot", L"times", L"div", L"pm", L"to", L"rightarrow", L"leftarrow",
        L"leftrightarrow", L"Rightarrow", L"Leftarrow", L"Leftrightarrow",
        L"leq", L"le", L"geq", L"ge", L"neq", L"approx", L"equiv",
        L"in", L"ni", L"notin", L"subset", L"subseteq", L"supset", L"supseteq",
        L"cap", L"cup", L"setminus", L"ldots", L"cdots",
        L"sin", L"cos", L"tan", L"log", L"ln", L"exp", L"sinh", L"cosh", L"tanh",
        L"alpha", L"beta", L"gamma", L"delta", L"epsilon", L"zeta", L"eta",
        L"theta", L"iota", L"kappa", L"lambda", L"mu", L"nu", L"xi", L"pi",
        L"rho", L"sigma", L"tau", L"upsilon", L"phi", L"chi", L"psi", L"omega",
        L"Gamma", L"Delta", L"Theta", L"Lambda", L"Xi", L"Pi", L"Sigma",
        L"Upsilon", L"Phi", L"Psi", L"Omega",
        L"langle", L"rangle", L"mid",
    };
    for (std::wstring_view command : kCommands) {
        if (name == command) return true;
    }
    return false;
}

void AttachMathWarning(NoteDocument* doc, size_t mathIndex, std::wstring code, std::wstring message, Span span) {
    if (!doc || mathIndex >= doc->math_spans.size()) return;
    const size_t diagnosticIndex = AppendDiagnostic(doc,
                                                    std::move(code),
                                                    std::move(message),
                                                    span,
                                                    DiagnosticSeverity::Warning);
    if (diagnosticIndex != kInvalidIndex) {
        doc->math_spans[mathIndex].diagnostic_ids.push_back(diagnosticIndex);
    }
}

void AddMathSupportDiagnostics(NoteDocument* doc, size_t mathIndex) {
    if (!doc || mathIndex >= doc->math_spans.size()) return;
    const MathSpan& math = doc->math_spans[mathIndex];
    std::wstring_view tex(math.normalized_tex);
    for (size_t pos = 0; pos < tex.size();) {
        if (tex[pos] != L'\\') {
            ++pos;
            continue;
        }
        if (pos + 1 >= tex.size()) break;
        const wchar_t next = tex[pos + 1];
        if (!iswalpha(next)) {
            pos += 2;
            continue;
        }

        size_t nameStart = pos + 1;
        size_t nameEnd = nameStart;
        while (nameEnd < tex.size() && iswalpha(tex[nameEnd])) {
            ++nameEnd;
        }
        std::wstring_view command = tex.substr(nameStart, nameEnd - nameStart);

        if (command == L"begin" || command == L"end") {
            size_t groupPos = nameEnd;
            std::wstring envName;
            if (!ConsumeMathBraceGroup(tex, &groupPos, &envName) || !IsSupportedMathEnvironment(envName)) {
                AttachMathWarning(doc,
                                  mathIndex,
                                  L"NOTE-W-MATH-UNSUPPORTED",
                                  L"Unsupported math environment: " + (envName.empty() ? std::wstring(L"(unknown)") : envName),
                                  math.span);
                return;
            }
            pos = groupPos;
            continue;
        }

        if (!IsSupportedMathCommand(command)) {
            AttachMathWarning(doc,
                              mathIndex,
                              L"NOTE-W-MATH-UNSUPPORTED",
                              L"Unsupported math command: \\" + std::wstring(command),
                              math.span);
            return;
        }

        pos = nameEnd;
    }
}

size_t AdvanceLiteralIndex(const std::vector<Span>& literalSpans, size_t literalIndex, size_t pos) {
    while (literalIndex < literalSpans.size() && pos >= literalSpans[literalIndex].end.value) {
        ++literalIndex;
    }
    return literalIndex;
}

bool SkipLiteralSpanAt(const std::vector<Span>& literalSpans, size_t* literalIndex, size_t* pos) {
    if (!literalIndex || !pos) return false;
    *literalIndex = AdvanceLiteralIndex(literalSpans, *literalIndex, *pos);
    if (*literalIndex < literalSpans.size() &&
        *pos >= literalSpans[*literalIndex].start.value &&
        *pos < literalSpans[*literalIndex].end.value) {
        *pos = literalSpans[*literalIndex].end.value;
        return true;
    }
    return false;
}

int ParseHeadingLevelToken(std::wstring_view token) {
    if (token.size() < 2) return 0;
    if (token[0] != L'h' && token[0] != L'H') return 0;

    int level = 0;
    for (size_t i = 1; i < token.size(); ++i) {
        const wchar_t ch = token[i];
        if (ch < L'0' || ch > L'9') return 0;
        level = level * 10 + static_cast<int>(ch - L'0');
    }
    if (level <= 0) return 0;
    return std::clamp(level, 1, 6);
}

int ParseHashHeadingCloseSelector(std::wstring_view selector) {
    if (selector.empty()) return 0;
    int level = 0;
    for (wchar_t ch : selector) {
        if (ch != L'#') return 0;
        if (level < 1000) ++level;
    }
    return level;
}

struct StyleSpec {
    StyleKind kind = StyleKind::TextColor;
    std::wstring value;
};

struct StyleFrame {
    std::vector<StyleSpec> specs;
    std::vector<std::wstring> close_aliases;
    size_t content_start = 0;
    int heading_level = 0;
};

void AddCloseAlias(std::vector<std::wstring>* aliases, std::wstring_view alias) {
    if (!aliases || alias.empty()) return;
    std::wstring lowered = ToLowerAscii(alias);
    if (std::find(aliases->begin(), aliases->end(), lowered) != aliases->end()) {
        return;
    }
    aliases->push_back(std::move(lowered));
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

bool ParseStyleOpenTag(std::wstring_view content, StyleFrame* outFrame) {
    if (!outFrame) return false;
    outFrame->specs.clear();
    outFrame->close_aliases.clear();
    outFrame->content_start = 0;

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
        auto pushSpec = [&](StyleKind kind, std::wstring specValue) {
            outFrame->specs.push_back(StyleSpec{kind, std::move(specValue)});
            AddCloseAlias(&outFrame->close_aliases, lowered);
            recognized = true;
        };

        if (!value) {
            if (lowered == L"u") {
                pushSpec(StyleKind::Underline, L"1");
            } else if (lowered == L"b" || lowered == L"bold" || lowered == L"strong") {
                AddCloseAlias(&outFrame->close_aliases, L"b");
                AddCloseAlias(&outFrame->close_aliases, L"bold");
                AddCloseAlias(&outFrame->close_aliases, L"strong");
                outFrame->specs.push_back(StyleSpec{StyleKind::Bold, L"1"});
                recognized = true;
            } else if (lowered == L"i" || lowered == L"italic" || lowered == L"em") {
                AddCloseAlias(&outFrame->close_aliases, L"i");
                AddCloseAlias(&outFrame->close_aliases, L"italic");
                AddCloseAlias(&outFrame->close_aliases, L"em");
                outFrame->specs.push_back(StyleSpec{StyleKind::Italic, L"1"});
                recognized = true;
            } else if (lowered == L"la" || lowered == L"linkaccent") {
                AddCloseAlias(&outFrame->close_aliases, L"la");
                AddCloseAlias(&outFrame->close_aliases, L"linkaccent");
                outFrame->specs.push_back(StyleSpec{StyleKind::LinkAccent, L"1"});
                recognized = true;
            } else if (lowered == L"lu" || lowered == L"linkunderline") {
                AddCloseAlias(&outFrame->close_aliases, L"lu");
                AddCloseAlias(&outFrame->close_aliases, L"linkunderline");
                outFrame->specs.push_back(StyleSpec{StyleKind::LinkUnderline, L"1"});
                recognized = true;
            } else if (lowered == L"m") {
                pushSpec(StyleKind::LineHeight, L"+0.5");
            }
            continue;
        }

        const std::wstring trimmedValue = TrimWhitespace(*value);
        if (trimmedValue.empty()) continue;

        if (lowered == L"char" || lowered == L"c") {
            AddCloseAlias(&outFrame->close_aliases, L"char");
            AddCloseAlias(&outFrame->close_aliases, L"c");
            outFrame->specs.push_back(StyleSpec{StyleKind::TextColor, trimmedValue});
            recognized = true;
        } else if (lowered == L"back") {
            pushSpec(StyleKind::BackgroundColor, trimmedValue);
        } else if (lowered == L"font" || lowered == L"f") {
            AddCloseAlias(&outFrame->close_aliases, L"font");
            AddCloseAlias(&outFrame->close_aliases, L"f");
            outFrame->specs.push_back(StyleSpec{StyleKind::FontFamily, trimmedValue});
            recognized = true;
        } else if (lowered == L"s") {
            pushSpec(StyleKind::FontSize, trimmedValue);
        } else if (lowered == L"m") {
            pushSpec(StyleKind::LineHeight, trimmedValue);
        } else if (lowered == L"d") {
            if (IsNumericValue(trimmedValue)) {
                pushSpec(StyleKind::Indent, trimmedValue);
            } else {
                pushSpec(StyleKind::Anchor, trimmedValue);
            }
        } else if (lowered == L"l" || lowered == L"link") {
            AddCloseAlias(&outFrame->close_aliases, L"l");
            AddCloseAlias(&outFrame->close_aliases, L"link");
            outFrame->specs.push_back(StyleSpec{StyleKind::LinkId, trimmedValue});
            recognized = true;
        } else if (lowered == L"la" || lowered == L"linkaccent") {
            if (ParseMarkupBool(trimmedValue)) {
                AddCloseAlias(&outFrame->close_aliases, L"la");
                AddCloseAlias(&outFrame->close_aliases, L"linkaccent");
                outFrame->specs.push_back(StyleSpec{StyleKind::LinkAccent, L"1"});
                recognized = true;
            }
        } else if (lowered == L"lu" || lowered == L"linkunderline") {
            if (ParseMarkupBool(trimmedValue)) {
                AddCloseAlias(&outFrame->close_aliases, L"lu");
                AddCloseAlias(&outFrame->close_aliases, L"linkunderline");
                outFrame->specs.push_back(StyleSpec{StyleKind::LinkUnderline, L"1"});
                recognized = true;
            }
        } else if (lowered == L"u") {
            if (ParseMarkupBool(trimmedValue)) {
                pushSpec(StyleKind::Underline, L"1");
            }
        } else if (lowered == L"b" || lowered == L"bold" || lowered == L"strong") {
            if (ParseMarkupBool(trimmedValue)) {
                AddCloseAlias(&outFrame->close_aliases, L"b");
                AddCloseAlias(&outFrame->close_aliases, L"bold");
                AddCloseAlias(&outFrame->close_aliases, L"strong");
                outFrame->specs.push_back(StyleSpec{StyleKind::Bold, L"1"});
                recognized = true;
            }
        } else if (lowered == L"i" || lowered == L"italic" || lowered == L"em") {
            if (ParseMarkupBool(trimmedValue)) {
                AddCloseAlias(&outFrame->close_aliases, L"i");
                AddCloseAlias(&outFrame->close_aliases, L"italic");
                AddCloseAlias(&outFrame->close_aliases, L"em");
                outFrame->specs.push_back(StyleSpec{StyleKind::Italic, L"1"});
                recognized = true;
            }
        }
    }

    return recognized && !outFrame->specs.empty();
}

void AppendStyleSpan(NoteDocument* doc, StyleKind kind, Span span, std::wstring value) {
    if (!doc) return;
    if (span.end <= span.start) return;
    StyleSpan styleSpan;
    styleSpan.kind = kind;
    styleSpan.span = span;
    styleSpan.value = std::move(value);
    doc->style_spans.push_back(std::move(styleSpan));
}

void CloseStyleFrame(NoteDocument* doc, StyleFrame* frame, size_t closePos) {
    if (!doc || !frame) return;
    if (closePos <= frame->content_start) return;
    const Span span{frame->content_start, closePos};
    for (const auto& spec : frame->specs) {
        AppendStyleSpan(doc, spec.kind, span, spec.value);
    }
}

void ApplyLegacyMarkupStyleSpans(const NoteTextModel& model, NoteDocument* doc) {
    if (!doc) return;

    struct HeadingInfo {
        size_t start;
        size_t content_start;
        int level;
    };
    std::vector<HeadingInfo> headings;
    for (const auto& block : doc->blocks) {
        if (block.kind == BlockKind::Heading) {
            size_t c_start = block.span.start.value;
            while (c_start < model.raw.size() && model.raw[c_start] != '\n') {
                c_start++;
            }
            if (c_start < model.raw.size()) c_start++;
            headings.push_back({block.span.start.value, c_start, block.level});
        }
    }
    std::sort(headings.begin(), headings.end(), [](const auto& a, const auto& b) {
        return a.start < b.start;
    });

    const std::vector<Span> literalSpans = CollectLiteralSpans(model, *doc);

    size_t literalIndex = 0;
    size_t headingIndex = 0;
    int current_heading_level = 0;
    std::vector<StyleFrame> stack;
    size_t pos = 0;
    while (pos < model.raw.size()) {
        while (headingIndex < headings.size() && pos >= headings[headingIndex].start) {
            const size_t hStart = headings[headingIndex].start;
            const int hLevel = headings[headingIndex].level;
            while (!stack.empty() && stack.back().heading_level >= hLevel) {
                StyleFrame frame = std::move(stack.back());
                stack.pop_back();
                CloseStyleFrame(doc, &frame, hStart);
            }
            
            StyleFrame implicitIndent;
            implicitIndent.content_start = headings[headingIndex].content_start;
            implicitIndent.heading_level = hLevel;
            implicitIndent.specs.push_back(StyleSpec{StyleKind::Indent, std::to_wstring(hLevel)});
            implicitIndent.close_aliases.push_back(L"__implicit_heading_indent__");
            stack.push_back(std::move(implicitIndent));
            
            current_heading_level = hLevel;
            ++headingIndex;
        }

        while (literalIndex < literalSpans.size() && pos >= literalSpans[literalIndex].end.value) {
            ++literalIndex;
        }
        if (literalIndex < literalSpans.size() &&
            pos >= literalSpans[literalIndex].start.value &&
            pos < literalSpans[literalIndex].end.value) {
            pos = literalSpans[literalIndex].end.value;
            continue;
        }

        if (model.raw[pos] != L'<') {
            ++pos;
            continue;
        }

        size_t tagEnd = 0;
        std::wstring content;
        if (!TryParseMarkupTag(model.raw, pos, &tagEnd, &content)) {
            ++pos;
            continue;
        }

        if (content == L"/") {
            for (size_t idx = stack.size(); idx-- > 0;) {
                const auto& aliases = stack[idx].close_aliases;
                if (std::find(aliases.begin(), aliases.end(), L"__implicit_heading_indent__") != aliases.end()) {
                    continue;
                }
                StyleFrame frame = std::move(stack[idx]);
                stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(idx));
                CloseStyleFrame(doc, &frame, pos);
                break;
            }
            pos = tagEnd;
            continue;
        }

        if (!content.empty() && content[0] == L'/') {
            const std::wstring selector = ToLowerAscii(TrimWhitespace(content.substr(1)));
            const int closeHeadingLevel = ParseHashHeadingCloseSelector(selector);
            if (closeHeadingLevel > 0) {
                for (size_t idx = stack.size(); idx-- > 0;) {
                    if (stack[idx].heading_level < closeHeadingLevel) {
                        continue;
                    }
                    StyleFrame frame = std::move(stack[idx]);
                    stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(idx));
                    CloseStyleFrame(doc, &frame, pos);
                }
                current_heading_level = 0;
                for (const StyleFrame& frame : stack) {
                    current_heading_level = std::max(current_heading_level, frame.heading_level);
                }
                pos = tagEnd;
                continue;
            }
            
            if (!selector.empty()) {
                for (size_t idx = stack.size(); idx-- > 0;) {
                    const auto& aliases = stack[idx].close_aliases;
                    if (std::find(aliases.begin(), aliases.end(), selector) == aliases.end()) {
                        continue;
                    }
                    StyleFrame frame = std::move(stack[idx]);
                    stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(idx));
                    CloseStyleFrame(doc, &frame, pos);
                    break;
                }
            }
            pos = tagEnd;
            continue;
        }

        StyleFrame frame;
        if (ParseStyleOpenTag(content, &frame)) {
            frame.content_start = tagEnd;
            frame.heading_level = current_heading_level;
            stack.push_back(std::move(frame));
            pos = tagEnd;
            continue;
        }

        pos = tagEnd;
    }

    while (!stack.empty()) {
        StyleFrame frame = std::move(stack.back());
        stack.pop_back();
        CloseStyleFrame(doc, &frame, model.raw.size());
    }
}

struct HeadingPromotion {
    Span block_span{};
    Span content_span{};
    int level = 0;
};

bool TryParseLegacyHeadingOpen(std::wstring_view raw,
                               size_t lineStart,
                               size_t lineEnd,
                               HeadingPromotion* outPromotion) {
    if (!outPromotion) return false;

    const size_t trimmedStart = TrimLineStart(raw, lineStart, lineEnd);
    const size_t trimmedEnd = TrimLineEnd(raw, trimmedStart, lineEnd);
    if (trimmedStart >= trimmedEnd) return false;
    if (trimmedEnd - trimmedStart < 4) return false;
    if (raw[trimmedStart] != L'<') return false;

    size_t tagCursor = trimmedStart + 1;
    while (tagCursor < trimmedEnd && raw[tagCursor] != L'>') {
        ++tagCursor;
    }
    if (tagCursor >= trimmedEnd || raw[tagCursor] != L'>') return false;

    const int level = ParseHeadingLevelToken(raw.substr(trimmedStart + 1, tagCursor - (trimmedStart + 1)));
    if (level <= 0) return false;

    const size_t contentStart = tagCursor + 1;
    size_t closeStart = trimmedEnd;

    if (trimmedEnd >= contentStart + 3 &&
        raw[trimmedEnd - 3] == L'<' &&
        raw[trimmedEnd - 2] == L'/' &&
        raw[trimmedEnd - 1] == L'>') {
        closeStart = trimmedEnd - 3;
    } else {
        size_t namedCloseStart = trimmedEnd;
        while (namedCloseStart > contentStart && raw[namedCloseStart - 1] != L'<') {
            --namedCloseStart;
        }
        if (namedCloseStart > contentStart && namedCloseStart < trimmedEnd) {
            const std::wstring_view closing = raw.substr(namedCloseStart, trimmedEnd - namedCloseStart);
            std::wstring expected = L"</h" + std::to_wstring(level) + L">";
            if (EqualsAsciiNoCase(closing, expected)) {
                closeStart = namedCloseStart;
            }
        }
    }

    outPromotion->block_span = Span{trimmedStart, trimmedEnd};
    outPromotion->content_span = Span{contentStart, closeStart};
    outPromotion->level = level;
    return true;
}

size_t AppendDiagnostic(NoteDocument* doc,
                        std::wstring code,
                        std::wstring message,
                        Span span,
                        DiagnosticSeverity severity) {
    if (!doc) return kInvalidIndex;
    Diagnostic diag;
    diag.code = std::move(code);
    diag.message = std::move(message);
    diag.span = span;
    diag.severity = severity;
    doc->diagnostics.push_back(std::move(diag));
    return doc->diagnostics.empty() ? kInvalidIndex : (doc->diagnostics.size() - 1);
}

size_t AppendMathSpan(NoteDocument* doc,
                      MathKind kind,
                      MathDelimiter delimiter,
                      Span span,
                      Span contentSpan,
                      std::wstring normalizedTex) {
    if (!doc) return kInvalidIndex;
    MathSpan math;
    math.kind = kind;
    math.delimiter = delimiter;
    math.span = span;
    math.content_span = contentSpan;
    math.normalized_tex = std::move(normalizedTex);
    doc->math_spans.push_back(std::move(math));
    return doc->math_spans.empty() ? kInvalidIndex : (doc->math_spans.size() - 1);
}

bool FindSingleDollarClose(std::wstring_view raw,
                           size_t start,
                           const std::vector<Span>& literalSpans,
                           size_t* outClosePos) {
    if (!outClosePos) return false;
    size_t literalIndex = 0;
    size_t pos = start;
    while (pos < raw.size()) {
        if (SkipLiteralSpanAt(literalSpans, &literalIndex, &pos)) {
            continue;
        }
        if (pos >= raw.size()) break;
        const wchar_t ch = raw[pos];
        if (ch == L'\n' || ch == L'\r') {
            return false;
        }
        if (ch == L'$' && !IsBackslashEscaped(raw, pos)) {
            if (pos + 1 < raw.size() && raw[pos + 1] == L'$') {
                pos += 2;
                continue;
            }
            *outClosePos = pos;
            return true;
        }
        ++pos;
    }
    return false;
}

bool FindDoubleDollarClose(std::wstring_view raw,
                           size_t start,
                           const std::vector<Span>& literalSpans,
                           size_t* outClosePos) {
    if (!outClosePos) return false;
    size_t literalIndex = 0;
    size_t pos = start;
    while (pos + 1 < raw.size()) {
        if (SkipLiteralSpanAt(literalSpans, &literalIndex, &pos)) {
            continue;
        }
        if (pos + 1 >= raw.size()) break;
        if (raw[pos] == L'$' &&
            raw[pos + 1] == L'$' &&
            !IsBackslashEscaped(raw, pos)) {
            *outClosePos = pos;
            return true;
        }
        ++pos;
    }
    return false;
}

bool FindBackslashMathClose(std::wstring_view raw,
                            size_t start,
                            wchar_t closer,
                            bool stopAtLineBreak,
                            const std::vector<Span>& literalSpans,
                            size_t* outClosePos) {
    if (!outClosePos) return false;
    size_t literalIndex = 0;
    size_t pos = start;
    while (pos + 1 < raw.size()) {
        if (SkipLiteralSpanAt(literalSpans, &literalIndex, &pos)) {
            continue;
        }
        if (pos + 1 >= raw.size()) break;
        const wchar_t ch = raw[pos];
        if (stopAtLineBreak && (ch == L'\n' || ch == L'\r')) {
            return false;
        }
        if (ch == L'\\' &&
            raw[pos + 1] == closer &&
            !IsBackslashEscaped(raw, pos)) {
            *outClosePos = pos;
            return true;
        }
        ++pos;
    }
    return false;
}

void ExtractTexMathSpans(const NoteTextModel& model, NoteDocument* doc) {
    if (!doc) return;
    const std::vector<Span> literalSpans = CollectLiteralSpans(model, *doc);
    size_t literalIndex = 0;
    size_t pos = 0;
    while (pos < model.raw.size()) {
        if (SkipLiteralSpanAt(literalSpans, &literalIndex, &pos)) {
            continue;
        }
        if (pos >= model.raw.size()) break;

        if (pos + 1 < model.raw.size() &&
            model.raw[pos] == L'$' &&
            model.raw[pos + 1] == L'$' &&
            !IsBackslashEscaped(model.raw, pos)) {
            if (!HasOnlyLineWhitespaceBefore(model.raw, pos)) {
                pos += 2;
                continue;
            }
            size_t closePos = 0;
            if (!FindDoubleDollarClose(model.raw, pos + 2, literalSpans, &closePos)) {
                pos += 2;
                continue;
            }
            const size_t endPos = closePos + 2;
            const bool standalone = IsStandaloneMathRange(model.raw, pos, endPos);
            const bool multiline = ContainsLineBreak(model.raw, pos, endPos);
            if (!standalone && !multiline) {
                pos += 2;
                continue;
            }
            const size_t mathIndex = AppendMathSpan(doc,
                                                    MathKind::Block,
                                                    MathDelimiter::DoubleDollar,
                                                    Span{pos, endPos},
                                                    Span{pos + 2, closePos},
                                                    NormalizeMathText(model.raw.substr(pos + 2, closePos - (pos + 2))));
            AddMathSupportDiagnostics(doc, mathIndex);
            pos = endPos;
            continue;
        }

        if (pos + 1 < model.raw.size() &&
            model.raw[pos] == L'\\' &&
            model.raw[pos + 1] == L'[' &&
            !IsBackslashEscaped(model.raw, pos)) {
            if (!HasOnlyLineWhitespaceBefore(model.raw, pos)) {
                pos += 2;
                continue;
            }
            size_t closePos = 0;
            if (!FindBackslashMathClose(model.raw, pos + 2, L']', false, literalSpans, &closePos)) {
                pos += 2;
                continue;
            }
            const size_t endPos = closePos + 2;
            const bool standalone = IsStandaloneMathRange(model.raw, pos, endPos);
            const bool multiline = ContainsLineBreak(model.raw, pos, endPos);
            if (!standalone && !multiline) {
                pos += 2;
                continue;
            }
            const size_t mathIndex = AppendMathSpan(doc,
                                                    MathKind::Block,
                                                    MathDelimiter::BackslashBracket,
                                                    Span{pos, endPos},
                                                    Span{pos + 2, closePos},
                                                    NormalizeMathText(model.raw.substr(pos + 2, closePos - (pos + 2))));
            AddMathSupportDiagnostics(doc, mathIndex);
            pos = endPos;
            continue;
        }

        if (pos + 1 < model.raw.size() &&
            model.raw[pos] == L'\\' &&
            model.raw[pos + 1] == L'(' &&
            !IsBackslashEscaped(model.raw, pos)) {
            size_t closePos = 0;
            if (!FindBackslashMathClose(model.raw, pos + 2, L')', true, literalSpans, &closePos)) {
                AppendDiagnostic(doc,
                                 L"NOTE-E-MATH-UNCLOSED-PAREN",
                                 L"Unclosed \\( ... \\) math span.",
                                 Span{pos, std::min(pos + 2, model.raw.size())},
                                 DiagnosticSeverity::Error);
                pos += 2;
                continue;
            }
            const size_t endPos = closePos + 2;
            const size_t mathIndex = AppendMathSpan(doc,
                                                    MathKind::Inline,
                                                    MathDelimiter::BackslashParen,
                                                    Span{pos, endPos},
                                                    Span{pos + 2, closePos},
                                                    NormalizeMathText(model.raw.substr(pos + 2, closePos - (pos + 2))));
            AddMathSupportDiagnostics(doc, mathIndex);
            pos = endPos;
            continue;
        }

        if (model.raw[pos] == L'$' &&
            !IsBackslashEscaped(model.raw, pos)) {
            if (pos + 1 < model.raw.size() && model.raw[pos + 1] == L'$') {
                pos += 2;
                continue;
            }
            size_t closePos = 0;
            if (!FindSingleDollarClose(model.raw, pos + 1, literalSpans, &closePos)) {
                ++pos;
                continue;
            }
            const size_t endPos = closePos + 1;
            const size_t mathIndex = AppendMathSpan(doc,
                                                    MathKind::Inline,
                                                    MathDelimiter::Dollar,
                                                    Span{pos, endPos},
                                                    Span{pos + 1, closePos},
                                                    NormalizeMathText(model.raw.substr(pos + 1, closePos - (pos + 1))));
            AddMathSupportDiagnostics(doc, mathIndex);
            pos = endPos;
            continue;
        }

        ++pos;
    }
}

void ApplyHeadingTagPromotion(const NoteTextModel& model, NoteDocument* doc) {
    if (!doc) return;
    const std::vector<Span> literalSpans = CollectLiteralSpans(model, *doc);
    for (size_t lineIndex = 0; lineIndex < model.line_starts.size(); ++lineIndex) {
        const size_t lineStart = model.line_starts[lineIndex];
        size_t lineEnd = (lineIndex + 1 < model.line_starts.size())
                             ? model.line_starts[lineIndex + 1]
                             : model.raw.size();
        if (lineEnd > lineStart && model.raw[lineEnd - 1] == L'\n') {
            --lineEnd;
        }
        if (lineEnd > lineStart && model.raw[lineEnd - 1] == L'\r') {
            --lineEnd;
        }

        HeadingPromotion promotion;
        if (!TryParseLegacyHeadingOpen(model.raw, lineStart, lineEnd, &promotion)) {
            continue;
        }
        if (IsInsideAnySpan(literalSpans, promotion.block_span)) {
            continue;
        }

        size_t trimmedContentStart = TrimLineStart(model.raw, promotion.content_span.start.value, promotion.content_span.end.value);
        if (trimmedContentStart >= promotion.content_span.end.value && lineIndex + 1 < model.line_starts.size()) {
            ++lineIndex;
            size_t nextLineStart = model.line_starts[lineIndex];
            size_t nextLineEnd = (lineIndex + 1 < model.line_starts.size())
                                     ? model.line_starts[lineIndex + 1]
                                     : model.raw.size();
            if (nextLineEnd > nextLineStart && model.raw[nextLineEnd - 1] == L'\n') --nextLineEnd;
            if (nextLineEnd > nextLineStart && model.raw[nextLineEnd - 1] == L'\r') --nextLineEnd;
            
            promotion.content_span = Span{{nextLineStart}, {nextLineEnd}};
            promotion.block_span.end = {nextLineEnd};
        }

        BlockNode block;
        block.kind = BlockKind::Heading;
        block.origin = BlockOrigin::LegacyHeadingTag;
        block.span = promotion.block_span;
        block.loc = ResolveLineColumn(model, promotion.block_span.start.value);
        block.level = promotion.level;
        block.first_inline = doc->inlines.size();
        block.inline_count = 1;
        block.parent = kInvalidIndex;

        InlineNode inlineNode;
        inlineNode.kind = InlineKind::Text;
        inlineNode.span = promotion.content_span;
        inlineNode.parent_block = doc->blocks.size();

        doc->blocks.push_back(std::move(block));
        doc->inlines.push_back(std::move(inlineNode));
    }
}

bool StartsLegacyMathTag(std::wstring_view raw, size_t pos, size_t* outEnd) {
    if (pos >= raw.size() || raw[pos] != L'<') return false;
    size_t cursor = pos + 1;
    if (cursor < raw.size() && raw[cursor] == L'/') {
        ++cursor;
    }
    if (cursor + 4 > raw.size()) return false;
    if (!EqualsAsciiNoCase(raw.substr(cursor, 4), L"math")) return false;
    cursor += 4;
    if (cursor >= raw.size()) return false;
    if (!(raw[cursor] == L'>' || iswspace(raw[cursor]) || raw[cursor] == L'/')) return false;
    while (cursor < raw.size() && raw[cursor] != L'>') {
        ++cursor;
    }
    if (cursor >= raw.size() || raw[cursor] != L'>') return false;
    if (outEnd) *outEnd = cursor + 1;
    return true;
}

void RejectLegacyMathTags(const NoteTextModel& model, NoteDocument* doc) {
    if (!doc) return;
    const std::vector<Span> literalSpans = CollectLiteralSpans(model, *doc);
    size_t pos = 0;
    while (pos < model.raw.size()) {
        size_t tagEnd = 0;
        if (!StartsLegacyMathTag(model.raw, pos, &tagEnd)) {
            ++pos;
            continue;
        }

        Span tagSpan{pos, tagEnd};
        if (!IsInsideAnySpan(literalSpans, tagSpan)) {
            AppendDiagnostic(doc,
                             L"NOTE-E-LEGACY-MATH",
                             L"Legacy <math> syntax is not accepted in the new Markdown/TeX path. Use $...$, $$...$$, \\(...\\), or \\[...\\].",
                             tagSpan,
                             DiagnosticSeverity::Error);
        }
        pos = std::max(tagEnd, pos + 1);
    }
}

void PostProcessNoteDocument(const NoteTextModel& model, NoteDocument* doc) {
    if (!doc) return;
    ApplyHeadingTagPromotion(model, doc);
    ApplyLegacyMarkupStyleSpans(model, doc);
    ExtractTexMathSpans(model, doc);
    RejectLegacyMathTags(model, doc);
}

} // namespace

NoteDocument ParseNoteDocument(const NoteTextModel& model) {
    NoteDocument doc = ParseNoteDocumentWithMd4c(model);
    PostProcessNoteDocument(model, &doc);
    return doc;
}

} // namespace note
