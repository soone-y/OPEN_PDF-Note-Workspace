bool TryParseItemKind(const std::string& typeStr, clrop::Item::Kind* out) {
    if (!out) return false;
    if (typeStr == "text") *out = clrop::Item::Kind::Text;
    else if (typeStr == "marker-text") *out = clrop::Item::Kind::MarkerText;
    else if (typeStr == "text-color") *out = clrop::Item::Kind::TextColor;
    else if (typeStr == "marker-free") *out = clrop::Item::Kind::MarkerFree;
    else if (typeStr == "line") *out = clrop::Item::Kind::Line;
    else if (typeStr == "arrow") *out = clrop::Item::Kind::Arrow;
    else if (typeStr == "wave") *out = clrop::Item::Kind::Wave;
    else if (typeStr == "math") *out = clrop::Item::Kind::Math;
    else if (typeStr == "freehand") *out = clrop::Item::Kind::Freehand;
    else if (typeStr == "link-marker") *out = clrop::Item::Kind::LinkMarker;
    else if (typeStr == "shape") *out = clrop::Item::Kind::Shape;
    else return false;
    return true;
}

class DirectDocumentParser {
public:
    explicit DirectDocumentParser(std::string_view input) : s(input) {}

    bool Parse(clrop::Document& out, std::wstring& err) {
        clrop::Document parsed;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '{') {
            if (!SkipValue(err)) return false;
            SkipWs();
            if (pos != s.size()) {
                err = L"Trailing characters after JSON value";
                return false;
            }
            err = L"Root must be object";
            return false;
        }
        if (!ParseRootObject(parsed, err)) return false;
        SkipWs();
        if (pos != s.size()) {
            err = L"Trailing characters after JSON value";
            return false;
        }
        parsed.dirty = false;
        out = std::move(parsed);
        return true;
    }

    bool ParseToSink(clrop::PdfId* outPdfId, clrop::ItemLoadSink& sink, std::wstring& err) {
        clrop::PdfId parsedPdfId;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '{') {
            if (!SkipValue(err)) return false;
            SkipWs();
            if (pos != s.size()) {
                err = L"Trailing characters after JSON value";
                return false;
            }
            err = L"Root must be object";
            return false;
        }
        if (!ParseRootToSink(parsedPdfId, sink, err)) return false;
        SkipWs();
        if (pos != s.size()) {
            err = L"Trailing characters after JSON value";
            return false;
        }
        if (outPdfId) *outPdfId = std::move(parsedPdfId);
        return true;
    }

private:
    static constexpr size_t kMaxJsonNestingDepth = 64;
    std::string_view s;
    size_t pos = 0;

    static bool IsHex(char c) {
        return (c >= '0' && c <= '9') ||
               (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    }

    static int HexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return 0;
    }

    static bool IsNumberStart(char c) {
        return c == '-' || (c >= '0' && c <= '9');
    }

    void SkipWs() {
        while (pos < s.size()) {
            char c = s[pos];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool Match(std::string_view lit) {
        if (s.substr(pos, lit.size()) == lit) {
            pos += lit.size();
            return true;
        }
        return false;
    }

    bool ParseNull(std::wstring& err) {
        if (!Match("null")) {
            err = L"Invalid literal (null)";
            return false;
        }
        return true;
    }

    bool ParseBool(bool& out, std::wstring& err) {
        if (Match("true")) {
            out = true;
            return true;
        }
        if (Match("false")) {
            out = false;
            return true;
        }
        err = L"Invalid literal (bool)";
        return false;
    }

    bool ParseNumber(double& out, std::wstring& err) {
        size_t start = pos;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        }
        std::string_view numStr = s.substr(start, pos - start);
        try {
            out = std::stod(std::string(numStr));
        } catch (...) {
            err = L"Invalid number";
            return false;
        }
        return true;
    }

    bool ParseString(std::string& out, std::wstring& err) {
        if (s[pos] != '"') {
            err = L"Expected '\"' for string";
            return false;
        }
        ++pos;
        std::string buf;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') {
                out = std::move(buf);
                return true;
            }
            if (c == '\\') {
                if (pos >= s.size()) {
                    err = L"Invalid escape";
                    return false;
                }
                char esc = s[pos++];
                switch (esc) {
                case '"': buf.push_back('"'); break;
                case '\\': buf.push_back('\\'); break;
                case '/': buf.push_back('/'); break;
                case 'b': buf.push_back('\b'); break;
                case 'f': buf.push_back('\f'); break;
                case 'n': buf.push_back('\n'); break;
                case 'r': buf.push_back('\r'); break;
                case 't': buf.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > s.size()) {
                        err = L"Incomplete unicode escape";
                        return false;
                    }
                    int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (!IsHex(s[pos + i])) {
                            err = L"Invalid hex in unicode escape";
                            return false;
                        }
                        code = (code << 4) | HexVal(s[pos + i]);
                    }
                    pos += 4;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                            int code2 = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = s[pos + 2 + i];
                                if (!IsHex(h)) {
                                    err = L"Invalid hex in unicode escape";
                                    return false;
                                }
                                code2 = (code2 << 4) | HexVal(h);
                            }
                            if (code2 >= 0xDC00 && code2 <= 0xDFFF) {
                                pos += 6;
                                int cp = (((code - 0xD800) << 10) | (code2 - 0xDC00)) + 0x10000;
                                if (cp <= 0x10FFFF) {
                                    buf.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
                                    buf.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                                    buf.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                    buf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                                    break;
                                }
                            }
                        }
                    }
                    wchar_t wch = static_cast<wchar_t>(code);
                    std::wstring tmp(1, wch);
                    std::string utf8 = WideToUtf8(tmp);
                    buf.append(utf8);
                    break;
                }
                default:
                    err = L"Unknown escape";
                    return false;
                }
            } else {
                buf.push_back(c);
            }
        }
        err = L"Unterminated string";
        return false;
    }

    bool ParseWideString(std::wstring& out, std::wstring& err) {
        std::string utf8;
        if (!ParseString(utf8, err)) return false;
        out = Utf8ToWide(utf8);
        return true;
    }

    bool SkipArray(std::wstring& err, size_t depth) {
        if (s[pos] != '[') {
            err = L"Expected '['";
            return false;
        }
        ++pos;
        SkipWs();
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            return true;
        }
        while (true) {
            if (!SkipValue(err, depth + 1)) return false;
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        return true;
    }

    bool SkipObject(std::wstring& err, size_t depth) {
        if (s[pos] != '{') {
            err = L"Expected '{'";
            return false;
        }
        ++pos;
        SkipWs();
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            std::string key;
            if (!ParseString(key, err)) return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != ':') {
                err = L"Expected ':' after key";
                return false;
            }
            ++pos;
            if (!SkipValue(err, depth + 1)) return false;
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated object";
                return false;
            }
            char c = s[pos++];
            if (c == '}') break;
            if (c != ',') {
                err = L"Expected ',' in object";
                return false;
            }
            SkipWs();
        }
        return true;
    }

    bool SkipValue(std::wstring& err, size_t depth = 0) {
        if (depth > kMaxJsonNestingDepth) {
            err = L"JSON nesting depth limit exceeded";
            return false;
        }
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        char c = s[pos];
        if (c == 'n') return ParseNull(err);
        if (c == 't' || c == 'f') {
            bool dummy = false;
            return ParseBool(dummy, err);
        }
        if (c == '"') {
            std::string dummy;
            return ParseString(dummy, err);
        }
        if (IsNumberStart(c)) {
            double dummy = 0.0;
            return ParseNumber(dummy, err);
        }
        if (c == '[') return SkipArray(err, depth);
        if (c == '{') return SkipObject(err, depth);
        err = L"Unexpected character in JSON";
        return false;
    }

    bool ParseMaybeWideString(std::wstring& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '"') return SkipValue(err);
        if (!ParseWideString(out, err)) return false;
        assigned = true;
        return true;
    }

    bool ParseMaybeUtf8String(std::string& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '"') return SkipValue(err);
        if (!ParseString(out, err)) return false;
        assigned = true;
        return true;
    }

    bool ParseMaybeInt(int& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (!IsNumberStart(s[pos])) return SkipValue(err);
        double value = 0.0;
        if (!ParseNumber(value, err)) return false;
        out = static_cast<int>(value);
        assigned = true;
        return true;
    }

    bool ParseMaybeUint64(std::uint64_t& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (!IsNumberStart(s[pos])) return SkipValue(err);
        double value = 0.0;
        if (!ParseNumber(value, err)) return false;
        out = static_cast<std::uint64_t>(value);
        assigned = true;
        return true;
    }

    bool ParseMaybeDouble(double& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (!IsNumberStart(s[pos])) return SkipValue(err);
        if (!ParseNumber(out, err)) return false;
        assigned = true;
        return true;
    }

    bool ParseMaybeDoubleArray(std::vector<double>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') {
            err = L"Expected numeric array";
            return false;
        }
        ++pos;
        SkipWs();
        std::vector<double> values;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            if (IsNumberStart(s[pos])) {
                double value = 0.0;
                if (!ParseNumber(value, err)) return false;
                values.push_back(value);
            } else {
                err = L"Expected number in numeric array";
                return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(values);
        assigned = true;
        return true;
    }

    bool ParseMaybeStringArray(std::vector<std::wstring>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') {
            err = L"Expected string array";
            return false;
        }
        ++pos;
        SkipWs();
        std::vector<std::wstring> values;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            if (s[pos] == '"') {
                std::wstring value;
                if (!ParseWideString(value, err)) return false;
                values.push_back(std::move(value));
            } else {
                err = L"Expected string in string array";
                return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(values);
        assigned = true;
        return true;
    }

    bool ParseMaybePointPairs(std::vector<double>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') {
            err = L"Expected point-pair array";
            return false;
        }
        ++pos;
        SkipWs();
        std::vector<double> values;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            std::vector<double> pair;
            bool gotPair = false;
            if (!ParseMaybeDoubleArray(pair, gotPair, err)) return false;
            if (!gotPair || pair.size() != 2) {
                err = L"point pair must contain two numbers";
                return false;
            }
            values.push_back(pair[0]);
            values.push_back(pair[1]);
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(values);
        assigned = true;
        return true;
    }

    bool ParseMaybeQuads(std::vector<double>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') {
            err = L"Expected quad array";
            return false;
        }
        ++pos;
        SkipWs();
        std::vector<double> values;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            std::vector<double> quad;
            bool gotQuad = false;
            if (!ParseMaybeDoubleArray(quad, gotQuad, err)) return false;
            if (!gotQuad || quad.size() != 8) {
                err = L"quad must contain eight numbers";
                return false;
            }
            values.insert(values.end(), quad.begin(), quad.end());
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(values);
        assigned = true;
        return true;
    }

    bool ParseMaybePageSizes(std::vector<std::array<double, 2>>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') {
            err = L"Expected page-size array";
            return false;
        }
        ++pos;
        SkipWs();
        std::vector<std::array<double, 2>> values;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            std::vector<double> pair;
            bool gotPair = false;
            if (!ParseMaybeDoubleArray(pair, gotPair, err)) return false;
            if (!gotPair || pair.size() != 2) {
                err = L"page size must contain two numbers";
                return false;
            }
            values.push_back({ pair[0], pair[1] });
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(values);
        assigned = true;
        return true;
    }

    bool ParseMaybePageSizePair(std::array<double, 2>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        std::vector<double> value;
        bool got = false;
        if (!ParseMaybeDoubleArray(value, got, err)) return false;
        if (!got || value.size() != 2) {
            err = L"page size pair must contain two numbers";
            return false;
        }
        out = { value[0], value[1] };
        assigned = true;
        return true;
    }

    bool ParseMaybePageSizeRuleExceptions(std::vector<PageSizeRuleException>& out,
                                          bool& assigned,
                                          std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') {
            err = L"Expected page-size exception array";
            return false;
        }
        ++pos;
        SkipWs();
        std::vector<PageSizeRuleException> values;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            std::vector<double> entry;
            bool gotEntry = false;
            if (!ParseMaybeDoubleArray(entry, gotEntry, err)) return false;
            if (!gotEntry || entry.size() != 3) {
                err = L"page size exception must contain page,width,height";
                return false;
            }
            const int page = static_cast<int>(entry[0]);
            if (page < 0) {
                err = L"page size exception page is negative";
                return false;
            }
            values.push_back(PageSizeRuleException{ page, { entry[1], entry[2] } });
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(values);
        assigned = true;
        return true;
    }

    bool ParseMaybePageSizeRule(PageSizeRuleDefinition& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '{') return SkipValue(err);
        ++pos;
        SkipWs();
        PageSizeRuleDefinition rule;
        bool haveDefault = false;
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            std::string key;
            if (!ParseString(key, err)) return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != ':') {
                err = L"Expected ':' after key";
                return false;
            }
            ++pos;
            if (key == "default_pt") {
                std::array<double, 2> value{ 0.0, 0.0 };
                bool got = false;
                if (!ParseMaybePageSizePair(value, got, err)) return false;
                if (got) {
                    rule.hasDefault = true;
                    rule.defaultSizePt = value;
                    haveDefault = true;
                }
            } else if (key == "exceptions") {
                std::vector<PageSizeRuleException> value;
                bool got = false;
                if (!ParseMaybePageSizeRuleExceptions(value, got, err)) return false;
                if (got) rule.exceptions = std::move(value);
            } else {
                if (!SkipValue(err)) return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated object";
                return false;
            }
            char c = s[pos++];
            if (c == '}') break;
            if (c != ',') {
                err = L"Expected ',' in object";
                return false;
            }
            SkipWs();
        }
        if (haveDefault) {
            out = std::move(rule);
            assigned = true;
        }
        return true;
    }

    bool ParseMaybePdfId(clrop::PdfId& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '{') return SkipValue(err);
        ++pos;
        SkipWs();
        clrop::PdfId pdfId;
        PageSizeRuleDefinition pageSizeRule;
        bool havePageSizeRule = false;
        bool haveSize = false;
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            err = L"pdf_id.size missing";
            return false;
        }
        while (true) {
            std::string key;
            if (!ParseString(key, err)) return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != ':') {
                err = L"Expected ':' after key";
                return false;
            }
            ++pos;
            if (key == "path") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) pdfId.path = std::move(value);
            } else if (key == "size") {
                std::uint64_t value = 0;
                bool got = false;
                if (!ParseMaybeUint64(value, got, err)) return false;
                if (got) {
                    pdfId.size = value;
                    haveSize = true;
                }
            } else if (key == "page_count") {
                int value = 0;
                bool got = false;
                if (!ParseMaybeInt(value, got, err)) return false;
                if (got && value > 0) pdfId.pageCount = value;
            } else if (key == "page_sizes_pt") {
                std::vector<std::array<double, 2>> value;
                bool got = false;
                if (!ParseMaybePageSizes(value, got, err)) return false;
                if (got) pdfId.pageSizesPt = std::move(value);
            } else if (key == "page_size_rule") {
                PageSizeRuleDefinition value;
                bool got = false;
                if (!ParseMaybePageSizeRule(value, got, err)) return false;
                if (got) {
                    pageSizeRule = std::move(value);
                    havePageSizeRule = true;
                }
            } else if (key == "sha256") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) pdfId.sha256 = std::move(value);
            } else {
                if (!SkipValue(err)) return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated object";
                return false;
            }
            char c = s[pos++];
            if (c == '}') break;
            if (c != ',') {
                err = L"Expected ',' in object";
                return false;
            }
            SkipWs();
        }
        if (!haveSize) {
            err = L"pdf_id.size missing";
            return false;
        }
        if (pdfId.pageCount <= 0 && !pdfId.pageSizesPt.empty()) {
            pdfId.pageCount = static_cast<int>(pdfId.pageSizesPt.size());
        }
        if (pdfId.pageSizesPt.empty() && havePageSizeRule && pdfId.pageCount > 0) {
            (void)ExpandPageSizeRule(pdfId.pageCount, pageSizeRule, pdfId.pageSizesPt);
        }
        out = std::move(pdfId);
        assigned = true;
        return true;
    }

    bool ValidateItemObject(const clrop::Item& item, bool haveType, std::wstring& err) {
        if (!haveType) {
            err = L"item.type missing or unsupported";
            return false;
        }
        if (item.id.empty()) {
            err = L"item.id missing";
            return false;
        }
        if (item.created.empty()) {
            err = L"item.created missing";
            return false;
        }
        if (item.updated.empty()) {
            err = L"item.updated missing";
            return false;
        }
        switch (item.kind) {
        case clrop::Item::Kind::Line:
        case clrop::Item::Kind::Arrow:
        case clrop::Item::Kind::Wave:
            if (!item.p1 || !item.p2) {
                err = L"line item point missing";
                return false;
            }
            break;
        case clrop::Item::Kind::MarkerText:
        case clrop::Item::Kind::TextColor:
            if (item.quads.empty()) {
                err = (item.kind == clrop::Item::Kind::MarkerText)
                    ? L"marker-text item quads missing"
                    : L"text-color item quads missing";
                return false;
            }
            break;
        case clrop::Item::Kind::MarkerFree:
        case clrop::Item::Kind::Freehand:
            if (item.path.empty()) {
                err = L"path item points missing";
                return false;
            }
            break;
        case clrop::Item::Kind::Text:
        case clrop::Item::Kind::Math:
        case clrop::Item::Kind::Shape:
            if (!item.bbox) {
                err = L"box item bbox missing";
                return false;
            }
            break;
        case clrop::Item::Kind::LinkMarker:
            if (!item.p1) {
                err = L"link-marker item point missing";
                return false;
            }
            break;
        }
        return true;
    }

    bool ParseItemObject(clrop::Item& out, std::wstring& err) {
        if (s[pos] != '{') {
            err = L"Expected '{'";
            return false;
        }
        ++pos;
        SkipWs();
        clrop::Item item;
        bool haveType = false;
        if (pos < s.size() && s[pos] == '}') {
            err = L"empty item object";
            return false;
        }
        while (true) {
            std::string key;
            if (!ParseString(key, err)) return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != ':') {
                err = L"Expected ':' after key";
                return false;
            }
            ++pos;
            if (key == "type") {
                std::string typeStr;
                bool got = false;
                if (!ParseMaybeUtf8String(typeStr, got, err)) return false;
                if (got) haveType = TryParseItemKind(typeStr, &item.kind);
            } else if (key == "id") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) item.id = std::move(value);
            } else if (key == "created") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) item.created = std::move(value);
            } else if (key == "updated") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) item.updated = std::move(value);
            } else if (key == "color") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) item.color = std::move(value);
            } else if (key == "alpha") {
                double value = 0.0;
                bool got = false;
                if (!ParseMaybeDouble(value, got, err)) return false;
                if (got) item.alpha = value;
            } else if (key == "width") {
                double value = 0.0;
                bool got = false;
                if (!ParseMaybeDouble(value, got, err)) return false;
                if (got) item.width = value;
            } else if (key == "pt") {
                double value = 0.0;
                bool got = false;
                if (!ParseMaybeDouble(value, got, err)) return false;
                if (got) item.pt = value;
            } else if (key == "font") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) item.font = std::move(value);
            } else if (key == "border") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) item.border = std::move(value);
            } else if (key == "writing_mode") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) item.writingMode = std::move(value);
            } else if (key == "content") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) item.content = std::move(value);
            } else if (key == "math_kind") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) item.mathKind = std::move(value);
            } else if (key == "shape_kind") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) item.shapeKind = std::move(value);
            } else if (key == "shape_draw_mode") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) item.shapeDrawMode = std::move(value);
            } else if (key == "arrow_head") {
                std::string value;
                bool got = false;
                if (!ParseMaybeUtf8String(value, got, err)) return false;
                if (got) item.arrowHead = std::move(value);
            } else if (key == "shape_rotation") {
                double value = 0.0;
                bool got = false;
                if (!ParseMaybeDouble(value, got, err)) return false;
                if (got) item.shapeRotation = value;
            } else if (key == "lines") {
                std::vector<std::wstring> value;
                bool got = false;
                if (!ParseMaybeStringArray(value, got, err)) return false;
                if (got) item.lines = std::move(value);
            } else if (key == "link_id") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) item.linkId = std::move(value);
            } else if (key == "note_path") {
                std::wstring value;
                bool got = false;
                if (!ParseMaybeWideString(value, got, err)) return false;
                if (got) item.notePath = std::move(value);
            } else if (key == "bbox") {
                std::vector<double> value;
                bool got = false;
                if (!ParseMaybeDoubleArray(value, got, err)) return false;
                if (got && value.size() == 4) {
                    item.bbox = std::array<double, 4>{ value[0], value[1], value[2], value[3] };
                }
            } else if (key == "p1") {
                std::vector<double> value;
                bool got = false;
                if (!ParseMaybeDoubleArray(value, got, err)) return false;
                if (got && value.size() == 2) {
                    item.p1 = std::array<double, 2>{ value[0], value[1] };
                }
            } else if (key == "p2") {
                std::vector<double> value;
                bool got = false;
                if (!ParseMaybeDoubleArray(value, got, err)) return false;
                if (got && value.size() == 2) {
                    item.p2 = std::array<double, 2>{ value[0], value[1] };
                }
            } else if (key == "dash") {
                std::vector<double> value;
                bool got = false;
                if (!ParseMaybeDoubleArray(value, got, err)) return false;
                if (got) item.dash = std::move(value);
            } else if (key == "path") {
                std::vector<double> value;
                bool got = false;
                if (!ParseMaybePointPairs(value, got, err)) return false;
                if (got) item.path = std::move(value);
            } else if (key == "quads") {
                std::vector<double> value;
                bool got = false;
                if (!ParseMaybeQuads(value, got, err)) return false;
                if (got) item.quads = std::move(value);
            } else {
                if (!SkipValue(err)) return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated object";
                return false;
            }
            char c = s[pos++];
            if (c == '}') break;
            if (c != ',') {
                err = L"Expected ',' in object";
                return false;
            }
            SkipWs();
        }
        if (!ValidateItemObject(item, haveType, err)) return false;
        out = std::move(item);
        return true;
    }

    bool ParseMaybeItems(std::vector<clrop::Item>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') return SkipValue(err);
        ++pos;
        SkipWs();
        std::vector<clrop::Item> items;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            if (s[pos] == '{') {
                clrop::Item item;
                size_t startPos = pos;
                std::wstring itemErr;
                if (!ParseItemObject(item, itemErr)) {
                    // Skip invalid item but continue parsing other items
                    pos = startPos;
                    if (!SkipValue(itemErr)) return false; // Catastrophic JSON error
                } else {
                    items.push_back(std::move(item));
                }
            } else {
                // Expected object but found something else? Skip it.
                if (!SkipValue(err)) return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(items);
        assigned = true;
        return true;
    }

    bool ParsePageObject(clrop::Page& out, std::wstring& err) {
        if (s[pos] != '{') {
            err = L"Expected '{'";
            return false;
        }
        ++pos;
        SkipWs();
        clrop::Page page;
        bool havePageNumber = false;
        bool haveItems = false;
        if (pos < s.size() && s[pos] == '}') {
            err = L"empty page object";
            return false;
        }
        while (true) {
            std::string key;
            if (!ParseString(key, err)) return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != ':') {
                err = L"Expected ':' after key";
                return false;
            }
            ++pos;
            if (key == "page") {
                int value = 0;
                bool got = false;
                if (!ParseMaybeInt(value, got, err)) return false;
                if (got) {
                    page.page = value;
                    havePageNumber = true;
                }
            } else if (key == "items") {
                std::vector<clrop::Item> items;
                bool got = false;
                if (!ParseMaybeItems(items, got, err)) return false;
                if (got) {
                    page.items = std::move(items);
                    haveItems = true;
                }
            } else {
                if (!SkipValue(err)) return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated object";
                return false;
            }
            char c = s[pos++];
            if (c == '}') break;
            if (c != ',') {
                err = L"Expected ',' in object";
                return false;
            }
            SkipWs();
        }
        if (!havePageNumber) {
            err = L"page.page missing";
            return false;
        }
        if (!haveItems) {
            err = L"page.items missing";
            return false;
        }
        out = std::move(page);
        return true;
    }

    bool ParseMaybePages(std::vector<clrop::Page>& out, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') return SkipValue(err);
        ++pos;
        SkipWs();
        std::vector<clrop::Page> pages;
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            out.clear();
            assigned = true;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            if (s[pos] == '{') {
                clrop::Page page;
                if (!ParsePageObject(page, err)) return false;
                pages.push_back(std::move(page));
            } else {
                err = L"Expected page object";
                return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        out = std::move(pages);
        assigned = true;
        return true;
    }

    bool ParseMaybePagesToSink(clrop::ItemLoadSink& sink, bool& assigned, std::wstring& err) {
        assigned = false;
        SkipWs();
        if (pos >= s.size()) {
            err = L"Unexpected end of input";
            return false;
        }
        if (s[pos] != '[') return SkipValue(err);
        ++pos;
        SkipWs();
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            assigned = true;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            if (s[pos] == '{') {
                clrop::Page page;
                if (!ParsePageObject(page, err)) return false;
                for (auto& item : page.items) {
                    if (!sink.OnClropItem(page.page, std::move(item), err)) return false;
                }
            } else {
                err = L"Expected page object";
                return false;
            }
            SkipWs();
            if (pos >= s.size()) {
                err = L"Unterminated array";
                return false;
            }
            char c = s[pos++];
            if (c == ']') break;
            if (c != ',') {
                err = L"Expected ',' in array";
                return false;
            }
            SkipWs();
        }
        assigned = true;
        return true;
    }

    bool ParseRootObject(clrop::Document& out, std::wstring& err) {
        if (s[pos] != '{') {
            err = L"Expected '{'";
            return false;
        }
        ++pos;
        SkipWs();
        bool haveVersion = false;
        bool havePdfId = false;
        bool havePages = false;
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
        } else {
            while (true) {
                std::string key;
                if (!ParseString(key, err)) return false;
                SkipWs();
                if (pos >= s.size() || s[pos] != ':') {
                    err = L"Expected ':' after key";
                    return false;
                }
                ++pos;
                if (key == "version") {
                    int value = 0;
                    bool got = false;
                    if (!ParseMaybeInt(value, got, err)) return false;
                    if (got) {
                        out.version = value;
                        haveVersion = true;
                        if (value != 1) {
                            err = L"unsupported version";
                            return false;
                        }
                    }
                } else if (key == "pdf_id") {
                    clrop::PdfId pdfId;
                    bool got = false;
                    if (!ParseMaybePdfId(pdfId, got, err)) return false;
                    if (got) {
                        out.pdfId = std::move(pdfId);
                        havePdfId = true;
                    }
                } else if (key == "pages") {
                    std::vector<clrop::Page> pages;
                    bool got = false;
                    if (!ParseMaybePages(pages, got, err)) return false;
                    if (got) {
                        out.pages = std::move(pages);
                        havePages = true;
                    }
                } else {
                    if (!SkipValue(err)) return false;
                }
                SkipWs();
                if (pos >= s.size()) {
                    err = L"Unterminated object";
                    return false;
                }
                char c = s[pos++];
                if (c == '}') break;
                if (c != ',') {
                    err = L"Expected ',' in object";
                    return false;
                }
                SkipWs();
            }
        }
        if (!haveVersion) {
            err = L"version missing";
            return false;
        }
        if (!havePdfId) {
            err = L"pdf_id missing";
            return false;
        }
        if (!havePages) {
            err = L"pages missing";
            return false;
        }
        return true;
    }

    bool ParseRootToSink(clrop::PdfId& outPdfId, clrop::ItemLoadSink& sink, std::wstring& err) {
        if (s[pos] != '{') {
            err = L"Expected '{'";
            return false;
        }
        ++pos;
        SkipWs();
        bool haveVersion = false;
        bool havePdfId = false;
        bool havePages = false;
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
        } else {
            while (true) {
                std::string key;
                if (!ParseString(key, err)) return false;
                SkipWs();
                if (pos >= s.size() || s[pos] != ':') {
                    err = L"Expected ':' after key";
                    return false;
                }
                ++pos;
                if (key == "version") {
                    int value = 0;
                    bool got = false;
                    if (!ParseMaybeInt(value, got, err)) return false;
                    if (got) {
                        haveVersion = true;
                        if (value != 1) {
                            err = L"unsupported version";
                            return false;
                        }
                    }
                } else if (key == "pdf_id") {
                    clrop::PdfId pdfId;
                    bool got = false;
                    if (!ParseMaybePdfId(pdfId, got, err)) return false;
                    if (got) {
                        outPdfId = std::move(pdfId);
                        havePdfId = true;
                    }
                } else if (key == "pages") {
                    bool got = false;
                    if (!ParseMaybePagesToSink(sink, got, err)) return false;
                    if (got) havePages = true;
                } else {
                    if (!SkipValue(err)) return false;
                }
                SkipWs();
                if (pos >= s.size()) {
                    err = L"Unterminated object";
                    return false;
                }
                char c = s[pos++];
                if (c == '}') break;
                if (c != ',') {
                    err = L"Expected ',' in object";
                    return false;
                }
                SkipWs();
            }
        }
        if (!haveVersion) {
            err = L"version missing";
            return false;
        }
        if (!havePdfId) {
            err = L"pdf_id missing";
            return false;
        }
        if (!havePages) {
            err = L"pages missing";
            return false;
        }
        return true;
    }
};
