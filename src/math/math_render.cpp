#include "math/math_render.h"
#include <algorithm>
#include <cwctype>
#include <initializer_list>

namespace mathrender {

namespace {

int& SupSubGapSupPercentStorage() {
    static int percent = 0; // 0 = auto
    return percent;
}

int ResolveSupSubGapPreferredSupPercent() {
    int percent = SupSubGapSupPercentStorage();
    if (percent <= 0) {
        // Default bias: keep superscripts a bit farther from the baseline than subscripts (2:1).
        return 67;
    }
    return std::clamp(percent, 5, 95);
}

struct MeasureResult {
    Layout box;
};

std::wstring_view TrimMathTextView(const std::wstring& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && iswspace(text[start])) ++start;
    while (end > start && iswspace(text[end - 1])) --end;
    return std::wstring_view(text.data() + start, end - start);
}

bool IsLargeDisplayOperatorText(std::wstring_view t) {
    return t == L"\u03a3" || t == L"\u220f" || t == L"\u222b";
}

int ResolveTextFontPx(const Node& n, int fontPx, RenderStyle style) {
    if (n.type != Node::Type::Text) return fontPx;
    if (style != RenderStyle::Display) return fontPx;
    std::wstring_view trimmed = TrimMathTextView(n.text);
    if (!IsLargeDisplayOperatorText(trimmed)) return fontPx;
    return std::max(fontPx + 6, (fontPx * 3) / 2);
}

int ResolveScriptFontPx(int fontPx) {
    fontPx = std::max(1, fontPx);
    // Smaller scripts reduce super/sub collisions and improve readability in inline sentences.
    return std::max(6, (fontPx * 2) / 3);
}

void EnsureMinSupSubGap(int fontPx, const Layout& base, const Layout& sup, const Layout& sub, int* supRaise, int* subDrop) {
    if (!supRaise || !subDrop) return;

    int minGap = std::max(1, fontPx / 12);
    int supDescent = std::max(0, sup.height - sup.baseline);
    int subAscent = std::max(0, sub.baseline);
    int need = supDescent + subAscent + minGap;
    int have = std::max(0, *supRaise) + std::max(0, *subDrop);
    if (have >= need) return;

    // Add just enough total margin to satisfy the minimum gap, but try to allocate the additional
    // pixels to the side that doesn't increase the overall layout height. This reduces unnecessary
    // line-height expansion for tall bases (e.g. display operators), while still preventing
    // super/sub collisions.
    int add = need - have;

    int baseAscent = std::max(0, base.baseline);
    int baseDescent = std::max(0, base.height - base.baseline);
    int supAscent = std::max(0, sup.baseline);
    int subDescent = std::max(0, sub.height - sub.baseline);

    int curSupExtent = std::max(0, *supRaise) + supAscent;
    int curSubExtent = std::max(0, *subDrop) + subDescent;
    int freeSup = std::max(0, baseAscent - curSupExtent);
    int freeSub = std::max(0, baseDescent - curSubExtent);

    // Preferred bias: either user-specified or the default (2:1), while still auto-adjusting
    // to avoid increasing the overall height when a non-expanding allocation exists.
    const int supPercent = ResolveSupSubGapPreferredSupPercent();
    int preferredAddSup = (add * supPercent + 50) / 100;
    preferredAddSup = std::clamp(preferredAddSup, 0, add);

    int lowNoExpand = std::max(0, add - freeSub);
    int highNoExpand = std::min(add, freeSup);

    int addSup = 0;
    if (lowNoExpand <= highNoExpand) {
        addSup = std::clamp(preferredAddSup, lowNoExpand, highNoExpand);
    } else {
        // Height expansion is unavoidable; any split yields the same total extra height.
        addSup = preferredAddSup;
    }
    int addSub = add - addSup;
    *supRaise += addSup;
    *subDrop += addSub;
}

HFONT CreateDerivedFont(HDC hdc, int pixelHeight, bool italic = false, int pixelWidth = 0) {
    if (pixelHeight <= 0) pixelHeight = 12;
    LOGFONTW lf{};
    bool haveCurrent = false;
    HGDIOBJ current = GetCurrentObject(hdc, OBJ_FONT);
    if (current) {
        haveCurrent = (GetObjectW(current, sizeof(lf), &lf) == sizeof(lf));
    }
    if (!haveCurrent) {
        lf.lfHeight = -pixelHeight;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH;
    }
    lf.lfHeight = -pixelHeight;
    lf.lfWidth = pixelWidth;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfItalic = italic ? TRUE : FALSE;
    return CreateFontIndirectW(&lf);
}

std::unique_ptr<Node> ParseExpr(std::wstring_view, size_t& pos);

bool ConsumeGroupedPlainText(std::wstring_view expr, size_t& pos, std::wstring& out);

std::unique_ptr<Node> ParsePrimary(std::wstring_view expr, size_t& pos);

std::unique_ptr<Node> ParseGroup(std::wstring_view expr, size_t& pos);

Layout MeasureNode(const Node& n, HDC hdc, int fontPx, RenderStyle style);

bool MatchLatexCommandAt(std::wstring_view expr, size_t pos, std::wstring_view name, size_t* outEnd = nullptr) {
    if (pos >= expr.size() || expr[pos] != L'\\') return false;
    const size_t start = pos + 1;
    const size_t end = start + name.size();
    if (end > expr.size()) return false;
    if (expr.substr(start, name.size()) != name) return false;
    if (end < expr.size() && iswalpha(expr[end])) return false;
    if (outEnd) *outEnd = end;
    return true;
}

bool MatchAnyLatexCommandAt(std::wstring_view expr,
                            size_t pos,
                            std::initializer_list<std::wstring_view> names) {
    for (std::wstring_view name : names) {
        if (MatchLatexCommandAt(expr, pos, name)) return true;
    }
    return false;
}

std::unique_ptr<Node> MakeTextNode(std::wstring text) {
    auto n = std::make_unique<Node>();
    n->type = Node::Type::Text;
    n->text = std::move(text);
    return n;
}

bool IsAccentCommand(std::wstring_view name) {
    return name == L"bar" ||
           name == L"overline" ||
           name == L"hat" ||
           name == L"widehat" ||
           name == L"vec" ||
           name == L"dot" ||
           name == L"ddot";
}

std::unique_ptr<Node> ParseAccentNode(std::wstring_view expr, size_t& pos) {
    if (pos >= expr.size() || expr[pos] != L'\\') return nullptr;
    size_t cmdStart = pos + 1;
    size_t cmdEnd = cmdStart;
    while (cmdEnd < expr.size() && iswalpha(expr[cmdEnd])) ++cmdEnd;
    std::wstring cmd(expr.substr(cmdStart, cmdEnd - cmdStart));
    if (!IsAccentCommand(cmd)) return nullptr;

    pos = cmdEnd;
    auto n = std::make_unique<Node>();
    n->type = Node::Type::Accent;
    n->text = std::move(cmd);
    n->a = ParseGroup(expr, pos);
    if (!n->a) return MakeTextNode(L"\\" + n->text);
    return n;
}

bool ConsumeRawBraceGroup(std::wstring_view expr, size_t& pos, std::wstring* out) {
    if (!out) return false;
    while (pos < expr.size() && iswspace(expr[pos])) ++pos;
    if (pos >= expr.size() || expr[pos] != L'{') return false;
    ++pos;
    int depth = 1;
    out->clear();
    while (pos < expr.size() && depth > 0) {
        wchar_t ch = expr[pos];
        if (ch == L'{') {
            ++depth;
            if (depth > 1) out->push_back(ch);
            ++pos;
            continue;
        }
        if (ch == L'}') {
            --depth;
            ++pos;
            if (depth > 0) out->push_back(ch);
            continue;
        }
        out->push_back(ch);
        ++pos;
    }
    return depth == 0;
}

bool SplitEnvironmentBody(std::wstring_view body, std::vector<std::vector<std::wstring>>* rows) {
    if (!rows) return false;
    rows->clear();
    rows->push_back({});
    rows->back().push_back(L"");
    int braceDepth = 0;
    for (size_t i = 0; i < body.size();) {
        if (body[i] == L'{') {
            ++braceDepth;
            rows->back().back().push_back(body[i++]);
            continue;
        }
        if (body[i] == L'}') {
            braceDepth = std::max(0, braceDepth - 1);
            rows->back().back().push_back(body[i++]);
            continue;
        }
        if (braceDepth == 0 && body[i] == L'\\' && i + 1 < body.size() && body[i + 1] == L'\\') {
            rows->push_back({});
            rows->back().push_back(L"");
            i += 2;
            while (i < body.size() && iswspace(body[i])) ++i;
            continue;
        }
        if (braceDepth == 0 && body[i] == L'&') {
            rows->back().push_back(L"");
            ++i;
            while (i < body.size() && iswspace(body[i])) ++i;
            continue;
        }
        rows->back().back().push_back(body[i++]);
    }
    return true;
}

bool IsSupportedEnvironmentName(std::wstring_view envName) {
    return envName == L"matrix" ||
           envName == L"pmatrix" ||
           envName == L"bmatrix" ||
           envName == L"Bmatrix" ||
           envName == L"vmatrix" ||
           envName == L"Vmatrix" ||
           envName == L"cases" ||
           envName == L"aligned" ||
           envName == L"align";
}

std::unique_ptr<Node> ParseEnvironmentNode(std::wstring_view expr, size_t& pos) {
    if (expr.compare(pos, 6, L"\\begin") != 0) return nullptr;

    size_t cur = pos + 6;
    std::wstring envName;
    if (!ConsumeRawBraceGroup(expr, cur, &envName) || !IsSupportedEnvironmentName(envName)) {
        return nullptr;
    }

    const size_t bodyStart = cur;
    int depth = 1;
    while (cur < expr.size()) {
        if (expr.compare(cur, 6, L"\\begin") == 0) {
            size_t next = cur + 6;
            std::wstring nestedName;
            if (ConsumeRawBraceGroup(expr, next, &nestedName) && nestedName == envName) {
                ++depth;
                cur = next;
                continue;
            }
        }
        if (expr.compare(cur, 4, L"\\end") == 0) {
            size_t next = cur + 4;
            std::wstring closeName;
            if (ConsumeRawBraceGroup(expr, next, &closeName) && closeName == envName) {
                --depth;
                if (depth == 0) {
                    std::vector<std::vector<std::wstring>> cells;
                    SplitEnvironmentBody(expr.substr(bodyStart, cur - bodyStart), &cells);
                    auto n = std::make_unique<Node>();
                    n->type = Node::Type::Matrix;
                    n->alignAtAmpersand = (envName == L"aligned" || envName == L"align");
                    n->centerColumns = !n->alignAtAmpersand && envName != L"cases";
                    if (envName == L"pmatrix") {
                        n->leftDelimiter = L"(";
                        n->rightDelimiter = L")";
                    } else if (envName == L"bmatrix") {
                        n->leftDelimiter = L"[";
                        n->rightDelimiter = L"]";
                    } else if (envName == L"Bmatrix") {
                        n->leftDelimiter = L"{";
                        n->rightDelimiter = L"}";
                    } else if (envName == L"vmatrix") {
                        n->leftDelimiter = L"|";
                        n->rightDelimiter = L"|";
                    } else if (envName == L"Vmatrix") {
                        n->leftDelimiter = L"||";
                        n->rightDelimiter = L"||";
                    } else if (envName == L"cases") {
                        n->leftDelimiter = L"{";
                    }
                    n->rows.reserve(cells.size());
                    for (const auto& row : cells) {
                        std::vector<std::unique_ptr<Node>> parsedRow;
                        parsedRow.reserve(row.size());
                        for (const auto& cell : row) {
                            size_t cellPos = 0;
                            auto cellNode = ParseExpr(cell, cellPos);
                            if (!cellNode) cellNode = MakeTextNode(L"");
                            parsedRow.push_back(std::move(cellNode));
                        }
                        n->rows.push_back(std::move(parsedRow));
                    }
                    pos = next;
                    return n;
                }
                cur = next;
                continue;
            }
        }
        ++cur;
    }
    return nullptr;
}

bool ConsumeScriptLayoutDirective(std::wstring_view expr, size_t& pos, Node::ScriptLayoutPreference& pref) {
    size_t cur = pos;
    while (cur < expr.size() && iswspace(expr[cur])) ++cur;
    if (expr.compare(cur, 7, L"\\limits") == 0) {
        pref = Node::ScriptLayoutPreference::Centered;
        pos = cur + 7;
        return true;
    }
    if (expr.compare(cur, 9, L"\\nolimits") == 0) {
        pref = Node::ScriptLayoutPreference::RightScripts;
        pos = cur + 9;
        return true;
    }
    return false;
}

std::wstring NormalizeMathText(std::wstring_view src) {
    std::wstring out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        wchar_t ch = src[i];
        if (ch == L'*') ch = L'\u00d7';
        else if (ch == L'/') ch = L'\u00f7';

        if (iswspace(ch)) {
            if (!out.empty() && out.back() != L' ') out.push_back(L' ');
            continue;
        }

        if (ch == L'=') {
            if (!out.empty() && out.back() != L' ') out.push_back(L' ');
            out.push_back(L'=');
            if (i + 1 < src.size() && !iswspace(src[i + 1])) out.push_back(L' ');
            continue;
        }

        out.push_back(ch);
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

const wchar_t* MapLatexCommand(std::wstring_view name) {
    if (name == L"int") return L"\u222b";
    if (name == L"oint") return L"\u222e";
    if (name == L"sum") return L"\u03a3";
    if (name == L"prod") return L"\u220f";
    if (name == L"lim") return L"lim";
    if (name == L"limsup") return L"limsup";
    if (name == L"liminf") return L"liminf";
    if (name == L"infty") return L"\u221e";
    if (name == L"partial") return L"\u2202";
    if (name == L"forall") return L"\u2200";
    if (name == L"exists") return L"\u2203";
    if (name == L"nabla") return L"\u2207";
    if (name == L"angle") return L"\u2220";
    if (name == L"circ") return L"\u2218";
    if (name == L"cdot") return L"\u00b7";
    if (name == L"times") return L"\u00d7";
    if (name == L"div") return L"\u00f7";
    if (name == L"pm") return L"\u00b1";
    if (name == L"to" || name == L"rightarrow") return L"\u2192";
    if (name == L"leftarrow") return L"\u2190";
    if (name == L"leftrightarrow") return L"\u2194";
    if (name == L"Rightarrow") return L"\u21d2";
    if (name == L"Leftarrow") return L"\u21d0";
    if (name == L"Leftrightarrow") return L"\u21d4";
    if (name == L"leq" || name == L"le") return L"\u2264";
    if (name == L"geq" || name == L"ge") return L"\u2265";
    if (name == L"neq") return L"\u2260";
    if (name == L"approx") return L"\u2248";
    if (name == L"equiv") return L"\u2261";
    if (name == L"in") return L"\u2208";
    if (name == L"ni") return L"\u220b";
    if (name == L"notin") return L"\u2209";
    if (name == L"subset") return L"\u2282";
    if (name == L"subseteq") return L"\u2286";
    if (name == L"supset") return L"\u2283";
    if (name == L"supseteq") return L"\u2287";
    if (name == L"cap") return L"\u2229";
    if (name == L"cup") return L"\u222a";
    if (name == L"setminus") return L"\u2216";
    if (name == L"ldots" || name == L"cdots") return L"\u2026";
    if (name == L"sin") return L"sin";
    if (name == L"cos") return L"cos";
    if (name == L"tan") return L"tan";
    if (name == L"log") return L"log";
    if (name == L"ln") return L"ln";
    if (name == L"exp") return L"exp";
    if (name == L"sinh") return L"sinh";
    if (name == L"cosh") return L"cosh";
    if (name == L"tanh") return L"tanh";
    if (name == L"alpha") return L"\u03b1";
    if (name == L"beta") return L"\u03b2";
    if (name == L"gamma") return L"\u03b3";
    if (name == L"delta") return L"\u03b4";
    if (name == L"epsilon") return L"\u03b5";
    if (name == L"zeta") return L"\u03b6";
    if (name == L"eta") return L"\u03b7";
    if (name == L"theta") return L"\u03b8";
    if (name == L"iota") return L"\u03b9";
    if (name == L"kappa") return L"\u03ba";
    if (name == L"lambda") return L"\u03bb";
    if (name == L"mu") return L"\u03bc";
    if (name == L"nu") return L"\u03bd";
    if (name == L"xi") return L"\u03be";
    if (name == L"pi") return L"\u03c0";
    if (name == L"rho") return L"\u03c1";
    if (name == L"sigma") return L"\u03c3";
    if (name == L"tau") return L"\u03c4";
    if (name == L"upsilon") return L"\u03c5";
    if (name == L"phi") return L"\u03c6";
    if (name == L"chi") return L"\u03c7";
    if (name == L"psi") return L"\u03c8";
    if (name == L"omega") return L"\u03c9";
    if (name == L"Gamma") return L"\u0393";
    if (name == L"Delta") return L"\u0394";
    if (name == L"Theta") return L"\u0398";
    if (name == L"Lambda") return L"\u039b";
    if (name == L"Xi") return L"\u039e";
    if (name == L"Pi") return L"\u03a0";
    if (name == L"Sigma") return L"\u03a3";
    if (name == L"Upsilon") return L"\u03a5";
    if (name == L"Phi") return L"\u03a6";
    if (name == L"Psi") return L"\u03a8";
    if (name == L"Omega") return L"\u03a9";
    if (name == L"langle") return L"\u27e8";
    if (name == L"rangle") return L"\u27e9";
    if (name == L"mid") return L"|";
    return nullptr;
}

bool ConsumeLatexCommand(std::wstring_view expr, size_t& pos, std::wstring& out) {
    if (pos >= expr.size() || expr[pos] != L'\\' || pos + 1 >= expr.size()) return false;
    size_t cmdStart = pos + 1;
    if (!iswalpha(expr[cmdStart])) {
        const wchar_t symbol = expr[cmdStart];
        if (symbol == L',' || symbol == L';' || symbol == L':') {
            out.push_back(L' ');
            pos = cmdStart + 1;
            return true;
        }
        if (symbol == L'!') {
            pos = cmdStart + 1;
            return true;
        }
        if (expr[cmdStart] == L'\\') {
            out.push_back(L' ');
            pos = cmdStart + 1;
            return true;
        }
        out.push_back(symbol);
        pos = cmdStart + 1;
        return true;
    }
    size_t cmdEnd = cmdStart;
    while (cmdEnd < expr.size() && iswalpha(expr[cmdEnd])) ++cmdEnd;
    std::wstring_view cmd = expr.substr(cmdStart, cmdEnd - cmdStart);

    if (cmd == L"," || cmd == L";" || cmd == L":" || cmd == L"quad" || cmd == L"qquad") {
        out.push_back(L' ');
        pos = cmdEnd;
        return true;
    }
    if (cmd == L"operatorname" ||
        cmd == L"mathrm" ||
        cmd == L"mathbf" ||
        cmd == L"mathit" ||
        cmd == L"mathsf" ||
        cmd == L"mathtt" ||
        cmd == L"mathbb" ||
        cmd == L"mathcal" ||
        cmd == L"text" ||
        cmd == L"textrm") {
        pos = cmdEnd;
        ConsumeGroupedPlainText(expr, pos, out);
        return true;
    }
    if (cmd == L"limits" || cmd == L"nolimits" || cmd == L"displaystyle" || cmd == L"textstyle") {
        pos = cmdEnd;
        return true;
    }
    if (const wchar_t* mapped = MapLatexCommand(cmd)) {
        out.append(mapped);
    } else {
        out.append(cmd);
    }
    pos = cmdEnd;
    return true;
}

bool ConsumeGroupedPlainText(std::wstring_view expr, size_t& pos, std::wstring& out) {
    while (pos < expr.size() && iswspace(expr[pos])) ++pos;
    if (pos >= expr.size() || expr[pos] != L'{') return false;
    ++pos;
    int depth = 1;
    while (pos < expr.size() && depth > 0) {
        wchar_t ch = expr[pos];
        if (ch == L'{') {
            ++depth;
            if (depth > 1) out.push_back(ch);
            ++pos;
            continue;
        }
        if (ch == L'}') {
            --depth;
            ++pos;
            if (depth > 0) out.push_back(ch);
            continue;
        }
        if (ch == L'\\') {
            std::wstring tmp;
            size_t inner = pos;
            if (ConsumeLatexCommand(expr, inner, tmp)) {
                out.append(tmp);
                pos = inner;
                continue;
            }
        }
        if (ch == L'&') {
            out.push_back(L' ');
            ++pos;
            continue;
        }
        out.push_back(ch);
        ++pos;
    }
    return true;
}

std::unique_ptr<Node> ParseGroup(std::wstring_view expr, size_t& pos) {
    if (pos < expr.size() && expr[pos] == L'{') {
        ++pos;
        auto n = ParseExpr(expr, pos);
        if (pos < expr.size() && expr[pos] == L'}') ++pos;
        return n;
    }
    // Single-token fallback: TeX allows scripts like x_\alpha without braces.
    // Also skip whitespace (ignored in math mode) so "x_ t" behaves like "x_t".
    while (pos < expr.size() && iswspace(expr[pos])) ++pos;
    if (pos >= expr.size()) return nullptr;

    if (expr[pos] == L'\\') {
        // Structural commands should be parsed as nodes (e.g. x_\frac12).
        if (MatchAnyLatexCommandAt(expr, pos, {L"frac", L"dfrac", L"tfrac", L"sqrt", L"binom", L"begin",
                                               L"bar", L"overline", L"hat", L"widehat", L"vec", L"dot", L"ddot"})) {
            return ParsePrimary(expr, pos);
        }
        std::wstring tmp;
        size_t inner = pos;
        if (ConsumeLatexCommand(expr, inner, tmp) && !tmp.empty()) {
            pos = inner;
            auto n = std::make_unique<Node>();
            n->type = Node::Type::Text;
            n->text = std::move(tmp);
            return n;
        }
    }

    auto n = std::make_unique<Node>();
    n->type = Node::Type::Text;
    n->text.push_back(expr[pos++]);
    return n;
}

std::unique_ptr<Node> ParsePrimary(std::wstring_view expr, size_t& pos) {
    size_t commandEnd = 0;
    if (MatchLatexCommandAt(expr, pos, L"frac", &commandEnd) ||
        MatchLatexCommandAt(expr, pos, L"dfrac", &commandEnd) ||
        MatchLatexCommandAt(expr, pos, L"tfrac", &commandEnd)) {
        pos = commandEnd;
        auto n = std::make_unique<Node>();
        n->type = Node::Type::Fraction;
        n->a = ParseGroup(expr, pos);
        n->b = ParseGroup(expr, pos);
        if (!n->a || !n->b) {
            auto t = std::make_unique<Node>();
            t->type = Node::Type::Text;
            t->text = L"\\frac";
            return t;
        }
        return n;
    }
    if (MatchLatexCommandAt(expr, pos, L"sqrt", &commandEnd)) {
        pos = commandEnd;
        auto n = std::make_unique<Node>();
        n->type = Node::Type::Sqrt;
        n->a = ParseGroup(expr, pos);
        if (!n->a) {
            auto t = std::make_unique<Node>();
            t->type = Node::Type::Text;
            t->text = L"\\sqrt";
            return t;
        }
        return n;
    }
    if (MatchLatexCommandAt(expr, pos, L"binom", &commandEnd)) {
        pos = commandEnd;
        auto n = std::make_unique<Node>();
        n->type = Node::Type::Binom;
        n->a = ParseGroup(expr, pos);
        n->b = ParseGroup(expr, pos);
        if (!n->a || !n->b) {
            return MakeTextNode(L"\\binom");
        }
        return n;
    }
    if (MatchLatexCommandAt(expr, pos, L"left", &commandEnd)) {
        pos = commandEnd;
        auto n = std::make_unique<Node>();
        n->type = Node::Type::Bracket;
        while (pos < expr.size() && iswspace(expr[pos])) ++pos;
        if (pos < expr.size()) {
            if (expr[pos] == L'.') {
                ++pos;
            } else if (expr[pos] == L'\\') {
                size_t inner = pos;
                std::wstring tmp;
                if (ConsumeLatexCommand(expr, inner, tmp)) {
                    n->leftDelimiter = tmp;
                    pos = inner;
                }
            } else {
                n->leftDelimiter = std::wstring(1, expr[pos]);
                ++pos;
            }
        }
        n->a = ParseExpr(expr, pos);
        if (MatchLatexCommandAt(expr, pos, L"right", &commandEnd)) {
            pos = commandEnd;
            while (pos < expr.size() && iswspace(expr[pos])) ++pos;
            if (pos < expr.size()) {
                if (expr[pos] == L'.') {
                    ++pos;
                } else if (expr[pos] == L'\\') {
                    size_t inner = pos;
                    std::wstring tmp;
                    if (ConsumeLatexCommand(expr, inner, tmp)) {
                        n->rightDelimiter = tmp;
                        pos = inner;
                    }
                } else {
                    n->rightDelimiter = std::wstring(1, expr[pos]);
                    ++pos;
                }
            }
        }
        if (!n->a) n->a = MakeTextNode(L"");
        return n;
    }
    if (MatchLatexCommandAt(expr, pos, L"begin")) {
        if (auto envNode = ParseEnvironmentNode(expr, pos)) {
            return envNode;
        }
        size_t cur = pos + 6;
        std::wstring envName;
        if (ConsumeRawBraceGroup(expr, cur, &envName)) {
            pos = cur;
            return MakeTextNode(L"\\begin{" + envName + L"}");
        }
        pos += 6;
        return MakeTextNode(L"\\begin");
    }
    if (auto accent = ParseAccentNode(expr, pos)) {
        return accent;
    }
    if (expr[pos] == L'{') {
        return ParseGroup(expr, pos);
    }
    auto n = std::make_unique<Node>();
    n->type = Node::Type::Text;
    while (pos < expr.size()) {
        wchar_t c = expr[pos];
        if (c == L'{' || c == L'}' || c == L'^' || c == L'_' || c == L'/') break;
        if (c == L'&') {
            n->text.push_back(L' ');
            ++pos;
            continue;
        }
        // Suppress the common "∂ x" / "∂ L" artifact caused by spaces in input like "\partial x".
        // (In TeX, normal spaces in math mode are ignored.)
        if (iswspace(c)) {
            if (!n->text.empty() && n->text.back() == L'\u2202') { // ∂
                ++pos;
                continue;
            }
        }
        if (c == L'\\') {
            // Structural commands should start a new node even mid-expression (e.g. "x = \\frac{a}{b}").
            // Otherwise they get consumed as plain text by ConsumeLatexCommand and fractions/roots never form.
            if (MatchAnyLatexCommandAt(expr, pos, {L"frac", L"dfrac", L"tfrac", L"sqrt", L"binom", L"begin",
                                                   L"bar", L"overline", L"hat", L"widehat", L"vec", L"dot", L"ddot"})) {
                break;
            }
            if (ConsumeLatexCommand(expr, pos, n->text)) continue;
        }
        n->text.push_back(c);
        ++pos;
    }
    return n;
}

std::unique_ptr<Node> ParseExpr(std::wstring_view expr, size_t& pos) {
    auto parsePrimaryWithScripts = [&](size_t& p) -> std::unique_ptr<Node> {
        auto prim = ParsePrimary(expr, p);
        if (!prim) return nullptr;
        Node::ScriptLayoutPreference scriptLayout = Node::ScriptLayoutPreference::Auto;
        while (ConsumeScriptLayoutDirective(expr, p, scriptLayout)) {
        }
        if (p < expr.size() && (expr[p] == L'^' || expr[p] == L'_')) {
            auto wrapper = std::make_unique<Node>();
            wrapper->type = Node::Type::SuperSub;
            wrapper->scriptLayout = scriptLayout;
            wrapper->a = std::move(prim);
            bool hasSuper = false, hasSub = false;
            while (p < expr.size() && (expr[p] == L'^' || expr[p] == L'_')) {
                wchar_t op = expr[p++];
                auto grp = ParseGroup(expr, p);
                if (op == L'^' && !hasSuper) { wrapper->super = std::move(grp); hasSuper = true; }
                else if (op == L'_' && !hasSub) { wrapper->sub = std::move(grp); hasSub = true; }
                while (ConsumeScriptLayoutDirective(expr, p, wrapper->scriptLayout)) {
                }
            }
            prim = std::move(wrapper);
        }
        return prim;
    };

    std::vector<std::unique_ptr<Node>> parts;
    while (pos < expr.size() && expr[pos] != L'}') {
        if (expr.compare(pos, 6, L"\\right") == 0) {
            if (pos + 6 >= expr.size() || !iswalpha(expr[pos + 6])) break;
        }
        if (expr[pos] == L'/') {
            auto t = std::make_unique<Node>();
            t->type = Node::Type::Text;
            t->text.push_back(L'/');
            ++pos;
            parts.push_back(std::move(t));
            continue;
        }
        auto prim = parsePrimaryWithScripts(pos);
        if (!prim) break;
        if (pos < expr.size() && expr[pos] == L'/' &&
            (pos > 0 && !iswspace(expr[pos - 1])) &&
            (pos + 1 < expr.size() && !iswspace(expr[pos + 1]))) {
            size_t slashPos = pos;
            ++pos;
            auto denom = parsePrimaryWithScripts(pos);
            if (denom) {
                auto frac = std::make_unique<Node>();
                frac->type = Node::Type::Fraction;
                frac->a = std::move(prim);
                frac->b = std::move(denom);
                parts.push_back(std::move(frac));
                continue;
            }
            pos = slashPos;
        }
        parts.push_back(std::move(prim));
    }
    if (parts.empty()) return nullptr;
    if (parts.size() == 1) return std::move(parts.front());
    auto grp = std::make_unique<Node>();
    grp->type = Node::Type::Group;
    grp->a = std::move(parts.front());
    Node* cur = grp.get();
    for (size_t i = 1; i < parts.size(); ++i) {
        auto next = std::make_unique<Node>();
        next->type = Node::Type::Group;
        next->a = std::move(parts[i]);
        cur->b = std::move(next);
        cur = cur->b.get();
    }
    return grp;
}

SIZE MeasureText(HDC hdc, const std::wstring& txt) {
    std::wstring normalized = NormalizeMathText(txt);
    SIZE sz{0,0};
    if (!normalized.empty()) {
        GetTextExtentPoint32W(hdc, normalized.c_str(), static_cast<int>(normalized.size()), &sz);
    }
    return sz;
}

bool IsLimitOperator(const Node& n) {
    if (n.type != Node::Type::Text) return false;
    std::wstring_view t = TrimMathTextView(n.text);
    if (t.empty()) return false;
    if (t == L"\u03a3" || t == L"\u222b" || t == L"\u220f") return true;
    if (t == L"lim" || t == L"limsup" || t == L"liminf") return true;
    return false;
}

bool IsIntegralOperator(const Node& n) {
    if (n.type != Node::Type::Text) return false;
    return TrimMathTextView(n.text) == L"\u222b";
}

bool PrefersCenteredLimitsByDefault(const Node& n) {
    if (!IsLimitOperator(n)) return false;
    if (IsIntegralOperator(n)) return false;
    return true;
}

enum class LimitLayoutMode {
    None,
    Centered,
    RightScripts
};

LimitLayoutMode ResolveLimitLayoutMode(const Node& superSub, RenderStyle style) {
    if (superSub.scriptLayout == Node::ScriptLayoutPreference::Centered) {
        return LimitLayoutMode::Centered;
    }
    if (superSub.scriptLayout == Node::ScriptLayoutPreference::RightScripts) {
        return LimitLayoutMode::RightScripts;
    }
    const Node* base = superSub.a.get();
    if (!base) return LimitLayoutMode::None;
    if (style != RenderStyle::Display) return LimitLayoutMode::None;
    if (IsIntegralOperator(*base)) return LimitLayoutMode::RightScripts;
    if (PrefersCenteredLimitsByDefault(*base)) return LimitLayoutMode::Centered;
    return LimitLayoutMode::None;
}

struct MatrixMetrics {
    std::vector<int> colWidths;
    std::vector<int> rowHeights;
    std::vector<int> rowBaselines;
    int leftWidth = 0;
    int rightWidth = 0;
    int colGap = 0;
    int rowGap = 0;
    int innerPad = 0;
    int contentWidth = 0;
    int contentHeight = 0;
};

int MeasureDelimiterWidth(HDC hdc, const std::wstring& delim) {
    if (delim.empty()) return 0;
    return MeasureText(hdc, delim).cx;
}

MatrixMetrics ComputeMatrixMetrics(const Node& n, HDC hdc, int fontPx) {
    MatrixMetrics m;
    size_t cols = 0;
    for (const auto& row : n.rows) cols = std::max(cols, row.size());
    m.colWidths.assign(cols, 0);
    m.rowHeights.assign(n.rows.size(), 0);
    m.rowBaselines.assign(n.rows.size(), 0);
    m.colGap = std::max(8, fontPx / 2);
    m.rowGap = std::max(4, fontPx / 3);
    m.innerPad = std::max(4, fontPx / 4);
    m.leftWidth = MeasureDelimiterWidth(hdc, n.leftDelimiter);
    m.rightWidth = MeasureDelimiterWidth(hdc, n.rightDelimiter);

    for (size_t r = 0; r < n.rows.size(); ++r) {
        int rowBaseline = 0;
        int rowBottom = 0;
        for (size_t c = 0; c < n.rows[r].size(); ++c) {
            Layout cell = MeasureNode(*n.rows[r][c], hdc, fontPx, RenderStyle::Inline);
            m.colWidths[c] = std::max(m.colWidths[c], cell.width);
            rowBaseline = std::max(rowBaseline, cell.baseline);
        }
        for (size_t c = 0; c < n.rows[r].size(); ++c) {
            Layout cell = MeasureNode(*n.rows[r][c], hdc, fontPx, RenderStyle::Inline);
            rowBottom = std::max(rowBottom, (rowBaseline - cell.baseline) + cell.height);
        }
        m.rowBaselines[r] = rowBaseline;
        m.rowHeights[r] = rowBottom;
    }

    for (size_t c = 0; c < m.colWidths.size(); ++c) {
        if (c > 0) m.contentWidth += m.colGap;
        m.contentWidth += m.colWidths[c];
    }
    for (size_t r = 0; r < m.rowHeights.size(); ++r) {
        if (r > 0) m.contentHeight += m.rowGap;
        m.contentHeight += m.rowHeights[r];
    }
    return m;
}

Layout MeasureNode(const Node& n, HDC hdc, int fontPx, RenderStyle style) {
    Layout box{};
    if (n.type == Node::Type::Text) {
        const int textPx = ResolveTextFontPx(n, fontPx, style);
        HFONT hFont = (textPx != fontPx) ? CreateDerivedFont(hdc, textPx) : nullptr;
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        SIZE sz = MeasureText(hdc, n.text);
        box.width = sz.cx;
        box.height = sz.cy;
        TEXTMETRICW tm{}; GetTextMetricsW(hdc, &tm);
        box.baseline = tm.tmAscent;
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);
        return box;
    }
    if (n.type == Node::Type::Group) {
        Layout left{};
        if (n.a) {
            left = MeasureNode(*n.a, hdc, fontPx, style);
            box = left;
        }
        if (n.b) {
            Layout right = MeasureNode(*n.b, hdc, fontPx, style);
            box.width = box.width + right.width;
            const int mergedBaseline = std::max(box.baseline, right.baseline);
            const int leftBottom = (mergedBaseline - box.baseline) + box.height;
            const int rightBottom = (mergedBaseline - right.baseline) + right.height;
            box.baseline = mergedBaseline;
            box.height = std::max(leftBottom, rightBottom);
        }
        return box;
    }
    if (n.type == Node::Type::Bracket) {
        Layout inner{};
        if (n.a) inner = MeasureNode(*n.a, hdc, fontPx, style);
        int parenW = std::max(6, static_cast<int>(MeasureText(hdc, L"(").cx));
        int leftW = n.leftDelimiter.empty() ? 0 : parenW;
        int rightW = n.rightDelimiter.empty() ? 0 : parenW;
        box.width = leftW + inner.width + rightW;
        box.height = std::max(fontPx, inner.height);
        box.baseline = inner.baseline + (box.height - inner.height) / 2; // Center inner inside box
        return box;
    }
    if (n.type == Node::Type::Fraction) {
        if (!n.a || !n.b) return box;
        int small = std::max(8, fontPx - 4);
        HFONT hFont = CreateDerivedFont(hdc, small);
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        Layout num = MeasureNode(*n.a, hdc, small, RenderStyle::Inline);
        Layout den = MeasureNode(*n.b, hdc, small, RenderStyle::Inline);
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);
        int pad = std::max(4, (small / 3) + 2);
        int halfPad = pad / 2;
        box.width = std::max(num.width, den.width) + pad * 2;
        box.height = num.height + den.height + pad + halfPad;
        // Place the baseline slightly below the fraction bar (math-axis-like), so "q =" doesn't look
        // a hair too high next to the fraction in inline formulas.
        const int axisDrop = std::clamp(fontPx / 12, 1, 4);
        box.baseline = std::min(box.height, num.height + halfPad + axisDrop);
        return box;
    }
    if (n.type == Node::Type::Binom) {
        if (!n.a || !n.b) return box;
        int small = std::max(8, fontPx - 4);
        HFONT hFont = CreateDerivedFont(hdc, small);
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        Layout top = MeasureNode(*n.a, hdc, small, RenderStyle::Inline);
        Layout bottom = MeasureNode(*n.b, hdc, small, RenderStyle::Inline);
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);
        const int parenW = std::max(6, static_cast<int>(MeasureText(hdc, L"(").cx));
        const int sidePad = std::max(3, fontPx / 6);
        const int rowGap = std::max(2, fontPx / 8);
        box.width = std::max(top.width, bottom.width) + (parenW + sidePad) * 2;
        box.height = top.height + bottom.height + rowGap;
        box.baseline = std::min(box.height, top.height + rowGap + bottom.baseline);
        return box;
    }
    if (n.type == Node::Type::Sqrt) {
        Layout inner{};
        if (n.a) inner = MeasureNode(*n.a, hdc, fontPx, style);
        int radW = std::max(8, fontPx / 2);
        int topPad = std::max(1, fontPx / 8);
        int rightPad = std::max(1, fontPx / 10);
        box.width = radW + inner.width + rightPad;
        box.baseline = inner.baseline + topPad;
        int descent = std::max(1, inner.height - inner.baseline);
        box.height = std::max(box.baseline + descent, topPad + inner.height);
        return box;
    }
    if (n.type == Node::Type::Matrix) {
        MatrixMetrics m = ComputeMatrixMetrics(n, hdc, fontPx);
        box.width = m.leftWidth + m.rightWidth + m.contentWidth;
        if (!n.leftDelimiter.empty()) box.width += m.innerPad;
        if (!n.rightDelimiter.empty()) box.width += m.innerPad;
        box.height = std::max(m.contentHeight, std::max(fontPx + 2, 1));
        box.baseline = !m.rowBaselines.empty() ? m.rowBaselines.front() : std::max(1, fontPx);
        return box;
    }
    if (n.type == Node::Type::Accent) {
        Layout inner{};
        if (n.a) inner = MeasureNode(*n.a, hdc, fontPx, style);
        const int accentHeight = std::max(2, fontPx / 5);
        const int topGap = std::max(1, fontPx / 10);
        box.width = std::max(inner.width, std::max(3, fontPx / 3));
        box.height = inner.height + accentHeight + topGap;
        box.baseline = inner.baseline + accentHeight + topGap;
        return box;
    }
    if (n.type == Node::Type::SuperSub) {
        Layout base{};
        if (n.a) base = MeasureNode(*n.a, hdc, fontPx, style);
        int small = ResolveScriptFontPx(fontPx);
        HFONT hFont = CreateDerivedFont(hdc, small);
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        Layout sup{}, sub{};
        if (n.super) sup = MeasureNode(*n.super, hdc, small, RenderStyle::Inline);
        if (n.sub) sub = MeasureNode(*n.sub, hdc, small, RenderStyle::Inline);
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);
        LimitLayoutMode limitMode = ResolveLimitLayoutMode(n, style);
        if (limitMode == LimitLayoutMode::Centered) {
            int topGap = std::max(1, fontPx / 8);
            int bottomGap = std::max(2, fontPx / 6);
            box.width = std::max({ base.width, sup.width, sub.width });
            box.baseline = (n.super ? (sup.height + topGap) : 0) + base.baseline;
            int below = base.height - base.baseline;
            if (n.sub) below += bottomGap + sub.height;
            box.height = box.baseline + below;
            return box;
        }
        if (limitMode == LimitLayoutMode::RightScripts) {
            int sideGap = std::max(2, fontPx / 8);
            int supRaise = std::max(2, fontPx / 2);
            int subDrop = std::max(2, fontPx / 4);
            if (n.super && n.sub) EnsureMinSupSubGap(fontPx, base, sup, sub, &supRaise, &subDrop);

            int top = 0;
            int bottom = base.height;
            if (n.super) {
                int supTop = base.baseline - supRaise - sup.baseline;
                top = std::min(top, supTop);
            }
            if (n.sub) {
                int subTop = base.baseline + subDrop - sub.baseline;
                bottom = std::max(bottom, subTop + sub.height);
            }

            box.width = base.width + (n.super || n.sub ? (sideGap + std::max(sup.width, sub.width)) : 0);
            box.baseline = base.baseline - top;
            box.height = bottom - top;
            return box;
        }
        int sideGap = std::max(1, fontPx / 12);
        int supRaise = std::max(2, fontPx / 2);
        int subDrop = std::max(2, fontPx / 4);
        if (n.super && n.sub) EnsureMinSupSubGap(fontPx, base, sup, sub, &supRaise, &subDrop);
        int top = 0;
        int bottom = base.height;
        if (n.super) {
            int supTop = base.baseline - supRaise - sup.baseline;
            top = std::min(top, supTop);
        }
        if (n.sub) {
            int subTop = base.baseline + subDrop - sub.baseline;
            bottom = std::max(bottom, subTop + sub.height);
        }
        box.width = base.width + (n.super || n.sub ? (sideGap + std::max(sup.width, sub.width)) : 0);
        box.baseline = base.baseline - top;
        box.height = bottom - top;
        return box;
    }
    return box;
}

void DrawNode(const Node& n, const Layout& box, HDC hdc, int x, int y, int fontPx, COLORREF color, RenderStyle style) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    if (n.type == Node::Type::Text) {
        const int textPx = ResolveTextFontPx(n, fontPx, style);
        bool italic = (n.text.size() == 1 && iswalpha(n.text[0]));
        HFONT hFont = (textPx != fontPx || italic) ? CreateDerivedFont(hdc, textPx, italic) : nullptr;
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        TEXTMETRICW tm{}; GetTextMetricsW(hdc, &tm);
        int drawY = y + box.baseline - tm.tmAscent;
        std::wstring normalized = NormalizeMathText(n.text);
        TextOutW(hdc, x, drawY, normalized.c_str(), static_cast<int>(normalized.size()));
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);
        return;
    }
    if (n.type == Node::Type::Group) {
        Layout left{};
        if (n.a) {
            left = MeasureNode(*n.a, hdc, fontPx, style);
            DrawNode(*n.a, left, hdc, x, y + (box.baseline - left.baseline), fontPx, color, style);
        }
        if (n.b) {
            Layout right = MeasureNode(*n.b, hdc, fontPx, style);
            DrawNode(*n.b, right, hdc, x + left.width, y + (box.baseline - right.baseline), fontPx, color, style);
        }
        return;
    }
    if (n.type == Node::Type::Fraction) {
        if (!n.a || !n.b) return;
        int small = std::max(8, fontPx - 4);
        HFONT hFont = CreateDerivedFont(hdc, small);
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        Layout num = MeasureNode(*n.a, hdc, small, RenderStyle::Inline);
        Layout den = MeasureNode(*n.b, hdc, small, RenderStyle::Inline);
        int pad = std::max(4, (small / 3) + 2);
        int halfPad = pad / 2;
        int numX = x + (box.width - num.width) / 2;
        int numY = y;
        DrawNode(*n.a, num, hdc, numX, numY, small, color, RenderStyle::Inline);
        int barY = y + num.height + halfPad;
        HPEN hPen = CreatePen(PS_SOLID, 1, color);
        HPEN oldPen = nullptr;
        if (hPen) {
            oldPen = static_cast<HPEN>(SelectObject(hdc, hPen));
        }
        MoveToEx(hdc, x + 1, barY, nullptr);
        LineTo(hdc, x + box.width - 1, barY);
        if (oldPen) {
            SelectObject(hdc, oldPen);
        }
        if (hPen) {
            DeleteObject(hPen);
        }
        int denX = x + (box.width - den.width) / 2;
        int denY = barY + pad;
        DrawNode(*n.b, den, hdc, denX, denY, small, color, RenderStyle::Inline);
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);
        return;
    }
    if (n.type == Node::Type::Bracket) {
        Layout inner{};
        if (n.a) inner = MeasureNode(*n.a, hdc, fontPx, style);
        
        int parenW = std::max(6, static_cast<int>(MeasureText(hdc, L"(").cx));
        int leftW = n.leftDelimiter.empty() ? 0 : parenW;
        
        SetTextColor(hdc, color);
        SetBkMode(hdc, TRANSPARENT);
        
        HFONT hFont = CreateDerivedFont(hdc, box.height, false, parenW);
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        
        // TextOutW draws from the top of the cell by default
        if (!n.leftDelimiter.empty()) {
            TextOutW(hdc, x, y, n.leftDelimiter.c_str(), static_cast<int>(n.leftDelimiter.size()));
        }
        if (!n.rightDelimiter.empty()) {
            TextOutW(hdc, x + leftW + inner.width, y, n.rightDelimiter.c_str(), static_cast<int>(n.rightDelimiter.size()));
        }
        
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);
        
        if (n.a) {
            DrawNode(*n.a, inner, hdc, x + leftW, y + (box.baseline - inner.baseline), fontPx, color, style);
        }
        return;
    }
    if (n.type == Node::Type::Binom) {
        if (!n.a || !n.b) return;
        int small = std::max(8, fontPx - 4);
        HFONT hFont = CreateDerivedFont(hdc, small);
        HFONT old = nullptr;
        if (hFont) old = static_cast<HFONT>(SelectObject(hdc, hFont));
        Layout top = MeasureNode(*n.a, hdc, small, RenderStyle::Inline);
        Layout bottom = MeasureNode(*n.b, hdc, small, RenderStyle::Inline);
        const int parenW = std::max(6, static_cast<int>(MeasureText(hdc, L"(").cx));
        const int sidePad = std::max(3, fontPx / 6);
        const int rowGap = std::max(2, fontPx / 8);
        const int innerWidth = std::max(top.width, bottom.width);
        const int topX = x + parenW + sidePad + (innerWidth - top.width) / 2;
        const int bottomX = x + parenW + sidePad + (innerWidth - bottom.width) / 2;
        DrawNode(*n.a, top, hdc, topX, y, small, color, RenderStyle::Inline);
        DrawNode(*n.b, bottom, hdc, bottomX, y + top.height + rowGap, small, color, RenderStyle::Inline);
        if (old) SelectObject(hdc, old);
        if (hFont) DeleteObject(hFont);

        SetTextColor(hdc, color);
        SetBkMode(hdc, TRANSPARENT);
        SIZE leftSz = MeasureText(hdc, L"(");
        SIZE rightSz = MeasureText(hdc, L")");
        TextOutW(hdc, x, y + (box.height - leftSz.cy) / 2, L"(", 1);
        TextOutW(hdc, x + box.width - rightSz.cx, y + (box.height - rightSz.cy) / 2, L")", 1);
        return;
    }
    if (n.type == Node::Type::Sqrt) {
        Layout inner{};
        if (n.a) inner = MeasureNode(*n.a, hdc, fontPx, style);
        int radW = std::max(8, fontPx / 2);
        int topPad = std::max(1, fontPx / 8);
        int rightPad = std::max(1, fontPx / 10);
        int innerX = x + radW + rightPad;
        int innerY = y + (box.baseline - inner.baseline);
        int barY = std::clamp(innerY - topPad, y, y + box.height - 1);
        int bottomY = std::clamp(innerY + inner.height - 1, y, y + box.height - 1);
        int midY = std::clamp(barY + std::max(1, (bottomY - barY) / 2), y, bottomY);
        int kneeX = x + std::max(1, radW / 4);
        int riseX = x + std::max(2, radW / 2);
        int stemX = x + radW;
        HPEN hPen = CreatePen(PS_SOLID, 1, color);
        HPEN oldPen = nullptr;
        if (hPen) {
            oldPen = static_cast<HPEN>(SelectObject(hdc, hPen));
        }
        MoveToEx(hdc, x, midY, nullptr);
        LineTo(hdc, kneeX, bottomY);
        LineTo(hdc, riseX, std::max(barY + 1, midY - 1));
        LineTo(hdc, stemX, barY);
        LineTo(hdc, x + box.width, barY);
        if (oldPen) {
            SelectObject(hdc, oldPen);
        }
        if (hPen) {
            DeleteObject(hPen);
        }
        DrawNode(*n.a, inner, hdc, innerX, innerY, fontPx, color, style);
        return;
    }
    if (n.type == Node::Type::Matrix) {
        MatrixMetrics m = ComputeMatrixMetrics(n, hdc, fontPx);
        SetTextColor(hdc, color);
        SetBkMode(hdc, TRANSPARENT);

        if (!n.leftDelimiter.empty()) {
            SIZE sz = MeasureText(hdc, n.leftDelimiter);
            TextOutW(hdc, x, y + (box.height - sz.cy) / 2, n.leftDelimiter.c_str(),
                     static_cast<int>(n.leftDelimiter.size()));
        }
        if (!n.rightDelimiter.empty()) {
            SIZE sz = MeasureText(hdc, n.rightDelimiter);
            TextOutW(hdc, x + box.width - sz.cx, y + (box.height - sz.cy) / 2, n.rightDelimiter.c_str(),
                     static_cast<int>(n.rightDelimiter.size()));
        }

        int contentX = x + m.leftWidth + (n.leftDelimiter.empty() ? 0 : m.innerPad);
        int rowY = y;
        for (size_t r = 0; r < n.rows.size(); ++r) {
            int colX = contentX;
            for (size_t c = 0; c < n.rows[r].size(); ++c) {
                Layout cell = MeasureNode(*n.rows[r][c], hdc, fontPx, RenderStyle::Inline);
                int drawX = colX;
                if (n.alignAtAmpersand && c == 0 && n.rows[r].size() > 1) {
                    drawX += m.colWidths[c] - cell.width;
                } else if (n.centerColumns) {
                    drawX += (m.colWidths[c] - cell.width) / 2;
                }
                DrawNode(*n.rows[r][c], cell, hdc, drawX, rowY + (m.rowBaselines[r] - cell.baseline),
                         fontPx, color, RenderStyle::Inline);
                colX += m.colWidths[c] + m.colGap;
            }
            rowY += m.rowHeights[r] + m.rowGap;
        }
        return;
    }
    if (n.type == Node::Type::Accent) {
        Layout inner{};
        if (n.a) inner = MeasureNode(*n.a, hdc, fontPx, style);
        const int accentHeight = std::max(2, fontPx / 5);
        const int topGap = std::max(1, fontPx / 10);
        const int innerX = x + (box.width - inner.width) / 2;
        const int innerY = y + accentHeight + topGap;

        HPEN hPen = CreatePen(PS_SOLID, 1, color);
        HPEN oldPen = hPen ? static_cast<HPEN>(SelectObject(hdc, hPen)) : nullptr;
        const int left = x;
        const int right = x + std::max(1, box.width - 1);
        const int mid = x + box.width / 2;
        const int accentTop = y;
        const int accentBottom = y + accentHeight;
        if (n.text == L"bar" || n.text == L"overline") {
            MoveToEx(hdc, left, accentBottom / 2 + accentTop / 2, nullptr);
            LineTo(hdc, right, accentBottom / 2 + accentTop / 2);
        } else if (n.text == L"hat" || n.text == L"widehat") {
            MoveToEx(hdc, left, accentBottom, nullptr);
            LineTo(hdc, mid, accentTop);
            LineTo(hdc, right, accentBottom);
        } else if (n.text == L"vec") {
            const int lineY = accentBottom / 2 + accentTop / 2;
            MoveToEx(hdc, left, lineY, nullptr);
            LineTo(hdc, right, lineY);
            LineTo(hdc, std::max(left, right - accentHeight), std::max(accentTop, lineY - accentHeight / 2));
            MoveToEx(hdc, right, lineY, nullptr);
            LineTo(hdc, std::max(left, right - accentHeight), std::min(accentBottom, lineY + accentHeight / 2));
        } else if (n.text == L"dot" || n.text == L"ddot") {
            const int dot = std::max(2, accentHeight);
            const int cy = accentTop;
            HBRUSH brush = CreateSolidBrush(color);
            HBRUSH oldBrush = brush ? static_cast<HBRUSH>(SelectObject(hdc, brush)) : nullptr;
            if (n.text == L"ddot") {
                const int gap = std::max(1, dot / 2);
                const int lx = mid - dot - gap / 2;
                const int rx = mid + gap / 2;
                Ellipse(hdc, lx, cy, lx + dot, cy + dot);
                Ellipse(hdc, rx, cy, rx + dot, cy + dot);
            } else {
                Ellipse(hdc, mid - dot / 2, cy, mid + dot / 2 + 1, cy + dot);
            }
            if (oldBrush) SelectObject(hdc, oldBrush);
            if (brush) DeleteObject(brush);
        }
        if (oldPen) SelectObject(hdc, oldPen);
        if (hPen) DeleteObject(hPen);

        if (n.a) DrawNode(*n.a, inner, hdc, innerX, innerY, fontPx, color, style);
        return;
    }
    if (n.type == Node::Type::SuperSub) {
        Layout base{};
        if (n.a) base = MeasureNode(*n.a, hdc, fontPx, style);
        int small = ResolveScriptFontPx(fontPx);
        HFONT scriptFont = CreateDerivedFont(hdc, small);
        HFONT baseFont = nullptr;
        if (scriptFont) baseFont = static_cast<HFONT>(SelectObject(hdc, scriptFont));
        Layout sup{}, sub{};
        if (n.super) sup = MeasureNode(*n.super, hdc, small, RenderStyle::Inline);
        if (n.sub) sub = MeasureNode(*n.sub, hdc, small, RenderStyle::Inline);
        if (baseFont) SelectObject(hdc, baseFont);
        LimitLayoutMode limitMode = ResolveLimitLayoutMode(n, style);
        if (limitMode == LimitLayoutMode::Centered) {
            int topGap = std::max(1, fontPx / 8);
            int bottomGap = std::max(2, fontPx / 6);
            int width = std::max({ base.width, sup.width, sub.width });
            int baseX = x + (width - base.width) / 2;
            int baseY = y + (n.super ? (sup.height + topGap) : 0);
            DrawNode(*n.a, base, hdc, baseX, baseY, fontPx, color, style);
            int scriptBlockX = x;
            if (n.super && n.sub) {
                int scriptW = std::max(sup.width, sub.width);
                scriptBlockX = x + (width - scriptW) / 2;
            }
            HFONT oldScript = nullptr;
            if (scriptFont) oldScript = static_cast<HFONT>(SelectObject(hdc, scriptFont));
            if (n.super) {
                int supX = (n.sub ? scriptBlockX : (x + (width - sup.width) / 2));
                DrawNode(*n.super, sup, hdc, supX, y, small, color, RenderStyle::Inline);
            }
            if (n.sub) {
                int subX = (n.super ? scriptBlockX : (x + (width - sub.width) / 2));
                int subY = baseY + base.height + bottomGap;
                DrawNode(*n.sub, sub, hdc, subX, subY, small, color, RenderStyle::Inline);
            }
            if (oldScript) SelectObject(hdc, oldScript);
            if (scriptFont) DeleteObject(scriptFont);
            return;
        }
        if (limitMode == LimitLayoutMode::RightScripts) {
            int sideGap = std::max(2, fontPx / 8);
            int supRaise = std::max(2, fontPx / 2);
            int subDrop = std::max(2, fontPx / 4);
            if (n.super && n.sub) EnsureMinSupSubGap(fontPx, base, sup, sub, &supRaise, &subDrop);
            int baseY = y + (box.baseline - base.baseline);
            DrawNode(*n.a, base, hdc, x, baseY, fontPx, color, style);
            int scriptX = x + base.width + sideGap;
            int baselineY = baseY + base.baseline;
            HFONT oldScript = nullptr;
            if (scriptFont) oldScript = static_cast<HFONT>(SelectObject(hdc, scriptFont));
            if (n.super) {
                int supY = baselineY - supRaise - sup.baseline;
                int supX = scriptX;
                DrawNode(*n.super, sup, hdc, supX, supY, small, color, RenderStyle::Inline);
            }
            if (n.sub) {
                int subY = baselineY + subDrop - sub.baseline;
                int subX = scriptX;
                DrawNode(*n.sub, sub, hdc, subX, subY, small, color, RenderStyle::Inline);
            }
            if (oldScript) SelectObject(hdc, oldScript);
            if (scriptFont) DeleteObject(scriptFont);
            return;
        }
        int baseY = y + (box.baseline - base.baseline);
        DrawNode(*n.a, base, hdc, x, baseY, fontPx, color, style);
        int sideGap = std::max(1, fontPx / 12);
        int supRaise = std::max(2, fontPx / 2);
        int subDrop = std::max(2, fontPx / 4);
        if (n.super && n.sub) EnsureMinSupSubGap(fontPx, base, sup, sub, &supRaise, &subDrop);
        int curX = x + base.width + sideGap;
        int baselineY = baseY + base.baseline;
        HFONT oldScript = nullptr;
        if (scriptFont) oldScript = static_cast<HFONT>(SelectObject(hdc, scriptFont));
        if (n.super) {
            int supY = baselineY - supRaise - sup.baseline;
            int supX = curX;
            DrawNode(*n.super, sup, hdc, supX, supY, small, color, RenderStyle::Inline);
        }
        if (n.sub) {
            int subY = baselineY + subDrop - sub.baseline;
            int subX = curX;
            DrawNode(*n.sub, sub, hdc, subX, subY, small, color, RenderStyle::Inline);
        }
        if (oldScript) SelectObject(hdc, oldScript);
        if (scriptFont) DeleteObject(scriptFont);
        return;
    }
}

} // namespace

std::unique_ptr<Node> Parse(std::wstring_view expr) {
    size_t pos = 0;
    auto n = ParseExpr(expr, pos);
    if (!n) {
        auto t = std::make_unique<Node>();
        t->type = Node::Type::Text;
        t->text = std::wstring(expr);
        return t;
    }
    return n;
}

Layout Measure(const Node& n, HDC hdc, int fontPx, RenderStyle style) {
    return MeasureNode(n, hdc, fontPx, style);
}

void Draw(const Node& n, const Layout& box, HDC hdc, int x, int y, int fontPx, COLORREF color, RenderStyle style) {
    DrawNode(n, box, hdc, x, y, fontPx, color, style);
}

void SetSupSubGapSupPercent(int percent) {
    if (percent <= 0) {
        SupSubGapSupPercentStorage() = 0;
        return;
    }
    SupSubGapSupPercentStorage() = std::clamp(percent, 5, 95);
}

int GetSupSubGapSupPercent() {
    return SupSubGapSupPercentStorage();
}

} // namespace mathrender
