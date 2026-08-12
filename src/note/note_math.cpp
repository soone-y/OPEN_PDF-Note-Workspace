#include "note/note_math.h"

#include <algorithm>
#include <cwctype>

namespace note {
namespace {

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

bool IsBackslashEscaped(std::wstring_view text, size_t pos) {
    size_t count = 0;
    while (pos > 0 && text[pos - 1] == L'\\') {
        --pos;
        ++count;
    }
    return (count % 2) == 1;
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

void PushError(MathInputAnalysis* out,
               std::wstring code,
               std::wstring message,
               Span span) {
    if (!out) return;
    Diagnostic diag;
    diag.code = std::move(code);
    diag.message = std::move(message);
    diag.span = span;
    diag.severity = DiagnosticSeverity::Error;
    out->diagnostics.push_back(std::move(diag));
}

bool StartsLegacyMathOpenTag(std::wstring_view text, size_t* outTagEnd) {
    if (!outTagEnd) return false;
    if (text.empty() || text[0] != L'<') return false;
    size_t cursor = 1;
    if (cursor + 4 > text.size()) return false;
    if (!EqualsAsciiNoCase(text.substr(cursor, 4), L"math")) return false;
    cursor += 4;
    if (cursor >= text.size()) return false;
    if (!(text[cursor] == L'>' || iswspace(text[cursor]) || text[cursor] == L'/')) return false;
    while (cursor < text.size() && text[cursor] != L'>') {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != L'>') return false;
    *outTagEnd = cursor + 1;
    return true;
}

size_t FindLegacyMathClose(std::wstring_view text,
                           size_t start,
                           size_t* outCloseLen) {
    if (outCloseLen) *outCloseLen = 0;
    for (size_t pos = start; pos < text.size(); ++pos) {
        if (text[pos] != L'<') continue;
        if (pos + 3 <= text.size() && text[pos + 1] == L'/' && text[pos + 2] == L'>') {
            if (outCloseLen) *outCloseLen = 3;
            return pos;
        }
        if (pos + 7 <= text.size() && EqualsAsciiNoCase(text.substr(pos, 7), L"</math>")) {
            if (outCloseLen) *outCloseLen = 7;
            return pos;
        }
    }
    return std::wstring_view::npos;
}

bool FindSingleDollarClose(std::wstring_view raw,
                           size_t start,
                           size_t* outClosePos) {
    if (!outClosePos) return false;
    size_t pos = start;
    while (pos < raw.size()) {
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
                           size_t* outClosePos) {
    if (!outClosePos) return false;
    size_t pos = start;
    while (pos + 1 < raw.size()) {
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
                            size_t* outClosePos) {
    if (!outClosePos) return false;
    size_t pos = start;
    while (pos + 1 < raw.size()) {
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

} // namespace

MathInputAnalysis AnalyzeMathBoxInput(std::wstring_view rawText) {
    MathInputAnalysis out;
    out.trimmed_text = TrimWhitespace(rawText);
    if (out.trimmed_text.empty()) {
        return out;
    }

    out.has_input = true;
    out.kind = ContainsLineBreak(out.trimmed_text, 0, out.trimmed_text.size())
        ? MathKind::Block
        : MathKind::Inline;
    out.content_text = NormalizeMathText(out.trimmed_text);

    size_t openTagEnd = 0;
    if (StartsLegacyMathOpenTag(out.trimmed_text, &openTagEnd)) {
        out.has_wrapping = true;
        out.flavor = MathInputFlavor::Markup;
        out.delimiter = MathDelimiter::LegacyMathTag;
        size_t closeLen = 0;
        size_t closePos = FindLegacyMathClose(out.trimmed_text, openTagEnd, &closeLen);
        if (closePos == std::wstring::npos) {
            PushError(&out,
                      L"NOTE-E-MATHBOX-UNCLOSED-LEGACY-MATH",
                      L"Unclosed <math> wrapper.",
                      Span{0, std::min(openTagEnd, out.trimmed_text.size())});
            return out;
        }
        out.content_text = NormalizeMathText(out.trimmed_text.substr(openTagEnd, closePos - openTagEnd));
        out.kind = ContainsLineBreak(out.trimmed_text, 0, closePos + closeLen)
            ? MathKind::Block
            : MathKind::Inline;
        return out;
    }

    if (out.trimmed_text.size() >= 2 &&
        out.trimmed_text[0] == L'$' &&
        out.trimmed_text[1] == L'$') {
        out.has_wrapping = true;
        out.flavor = MathInputFlavor::Latex;
        out.kind = MathKind::Block;
        out.delimiter = MathDelimiter::DoubleDollar;
        size_t closePos = 0;
        if (!FindDoubleDollarClose(out.trimmed_text, 2, &closePos)) {
            PushError(&out,
                      L"NOTE-E-MATHBOX-UNCLOSED-DOUBLE-DOLLAR",
                      L"Unclosed $$ math block.",
                      Span{0, std::min<size_t>(2, out.trimmed_text.size())});
            return out;
        }
        out.content_text = NormalizeMathText(out.trimmed_text.substr(2, closePos - 2));
        return out;
    }

    if (out.trimmed_text.size() >= 2 &&
        out.trimmed_text[0] == L'\\' &&
        out.trimmed_text[1] == L'[') {
        out.has_wrapping = true;
        out.flavor = MathInputFlavor::Latex;
        out.kind = MathKind::Block;
        out.delimiter = MathDelimiter::BackslashBracket;
        size_t closePos = 0;
        if (!FindBackslashMathClose(out.trimmed_text, 2, L']', false, &closePos)) {
            PushError(&out,
                      L"NOTE-E-MATHBOX-UNCLOSED-BRACKET",
                      L"Unclosed \\[ ... \\] math block.",
                      Span{0, std::min<size_t>(2, out.trimmed_text.size())});
            return out;
        }
        out.content_text = NormalizeMathText(out.trimmed_text.substr(2, closePos - 2));
        return out;
    }

    if (out.trimmed_text.size() >= 2 &&
        out.trimmed_text[0] == L'\\' &&
        out.trimmed_text[1] == L'(') {
        out.has_wrapping = true;
        out.flavor = MathInputFlavor::Latex;
        out.kind = MathKind::Inline;
        out.delimiter = MathDelimiter::BackslashParen;
        size_t closePos = 0;
        if (!FindBackslashMathClose(out.trimmed_text, 2, L')', true, &closePos)) {
            PushError(&out,
                      L"NOTE-E-MATHBOX-UNCLOSED-PAREN",
                      L"Unclosed \\( ... \\) math span.",
                      Span{0, std::min<size_t>(2, out.trimmed_text.size())});
            return out;
        }
        out.content_text = NormalizeMathText(out.trimmed_text.substr(2, closePos - 2));
        return out;
    }

    if (!out.trimmed_text.empty() && out.trimmed_text[0] == L'$') {
        out.has_wrapping = true;
        out.flavor = MathInputFlavor::Latex;
        out.kind = MathKind::Inline;
        out.delimiter = MathDelimiter::Dollar;
        size_t closePos = 0;
        if (!FindSingleDollarClose(out.trimmed_text, 1, &closePos)) {
            PushError(&out,
                      L"NOTE-E-MATHBOX-UNCLOSED-DOLLAR",
                      L"Unclosed $...$ math span.",
                      Span{0, std::min<size_t>(1, out.trimmed_text.size())});
            return out;
        }
        out.content_text = NormalizeMathText(out.trimmed_text.substr(1, closePos - 1));
        return out;
    }

    return out;
}

std::wstring MathDelimiterLabel(MathDelimiter delimiter) {
    switch (delimiter) {
    case MathDelimiter::Dollar:
        return L"$...$";
    case MathDelimiter::DoubleDollar:
        return L"$$...$$";
    case MathDelimiter::BackslashParen:
        return L"\\(...\\)";
    case MathDelimiter::BackslashBracket:
        return L"\\[...\\]";
    case MathDelimiter::LegacyMathTag:
        return L"<math>...</>";
    default:
        break;
    }
    return L"?";
}

} // namespace note
