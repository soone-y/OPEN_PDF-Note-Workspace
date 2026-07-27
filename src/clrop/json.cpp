#include "json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <new>
#include <sstream>
#include <string_view>
#include <cctype>
#include <windows.h>
#include <filesystem>

#include "core/app_core.h"
#ifndef CLROP_READ_ONLY_BUILD
#include "core/atomic_write.h"
#endif

namespace clrop {

namespace {

constexpr std::uint64_t kMaxClropFileBytes = 256ull * 1024ull * 1024ull;

// ---------------- UTF-8 helpers ----------------
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                  static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

// ---------------- file helpers ----------------
std::string_view SkipUtf8Bom(std::string_view input) {
    if (input.size() >= 3 &&
        static_cast<unsigned char>(input[0]) == 0xEF &&
        static_cast<unsigned char>(input[1]) == 0xBB &&
        static_cast<unsigned char>(input[2]) == 0xBF) {
        return input.substr(3);
    }
    return input;
}

std::wstring TrimTrailingMessageWhitespace(std::wstring msg) {
    while (!msg.empty()) {
        const wchar_t tail = msg.back();
        if (tail != L'\r' && tail != L'\n' && tail != L' ' && tail != L'\t' && tail != L'.') {
            break;
        }
        msg.pop_back();
    }
    return msg;
}

std::wstring DescribeWin32Error(DWORD error) {
    if (error == ERROR_SUCCESS) return {};
    LPWSTR raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags,
                                     nullptr,
                                     error,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPWSTR>(&raw),
                                     0,
                                     nullptr);
    std::wstring msg;
    if (len != 0 && raw) {
        msg.assign(raw, raw + len);
        LocalFree(raw);
    }
    msg = TrimTrailingMessageWhitespace(std::move(msg));
    if (msg.empty()) {
        msg = L"error=" + std::to_wstring(error);
    } else {
        msg += L" (error=" + std::to_wstring(error) + L")";
    }
    return msg;
}

bool IsRetryableReadError(DWORD error) {
    switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_DELETE_PENDING:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_LOCK_VIOLATION:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_SHARING_VIOLATION:
        return true;
    default:
        return false;
    }
}

bool TryReadFileToStringOnce(const std::wstring& path, std::string& out, DWORD* outError) {
    out.clear();
    if (outError) *outError = ERROR_SUCCESS;
    if (path.empty()) {
        if (outError) *outError = ERROR_PATH_NOT_FOUND;
        return false;
    }

    const std::wstring openPath = ToExtendedWin32PathIfAbsoluteLocal(std::filesystem::path(path));
    HANDLE h = CreateFileW(openPath.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outError) *outError = GetLastError();
        return false;
    }

    LARGE_INTEGER size{};
    const auto maxReadableSize = std::min<unsigned long long>(
        static_cast<unsigned long long>(std::numeric_limits<size_t>::max()),
        kMaxClropFileBytes);
    if (!GetFileSizeEx(h, &size)) {
        if (outError) *outError = GetLastError();
        CloseHandle(h);
        return false;
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > maxReadableSize) {
        if (outError) *outError = ERROR_FILE_TOO_LARGE;
        CloseHandle(h);
        return false;
    }

    std::vector<char> buffer;
    try {
        buffer.resize(static_cast<size_t>(size.QuadPart));
    } catch (const std::bad_alloc&) {
        if (outError) *outError = ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(h);
        return false;
    }
    size_t total = 0;
    bool ok = true;
    DWORD readError = ERROR_SUCCESS;
    while (total < buffer.size()) {
        const size_t remaining = buffer.size() - total;
        const DWORD chunkSize =
            static_cast<DWORD>(std::min<size_t>(remaining, static_cast<size_t>(1 << 20)));
        DWORD read = 0;
        if (!ReadFile(h, buffer.data() + total, chunkSize, &read, nullptr)) {
            ok = false;
            readError = GetLastError();
            break;
        }
        if (read == 0) {
            ok = false;
            readError = ERROR_HANDLE_EOF;
            break;
        }
        total += static_cast<size_t>(read);
    }
    CloseHandle(h);

    if (!ok) {
        out.clear();
        if (outError) *outError = readError;
        return false;
    }

    try {
        out.assign(buffer.begin(), buffer.end());
    } catch (const std::bad_alloc&) {
        out.clear();
        if (outError) *outError = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    return true;
}

bool ReadFileToString(const std::wstring& path, std::string& out, std::wstring& err) {
    constexpr int kMaxAttempts = 4;
    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (TryReadFileToStringOnce(path, out, &lastError)) {
            err.clear();
            return true;
        }
        if (attempt + 1 >= kMaxAttempts || !IsRetryableReadError(lastError)) break;
        Sleep(20);
    }

    err = L"failed to read clrop file";
    const std::wstring detail = DescribeWin32Error(lastError);
    if (!detail.empty()) {
        err += L"\n";
        err += detail;
    }
    if (!path.empty()) {
        err += L"\n";
        err += path;
    }
    return false;
}

constexpr double kPageSizeEpsilon = 0.01;

bool NearlyEqualPageSizeComponent(double a, double b) {
    return std::abs(a - b) <= kPageSizeEpsilon;
}

MAYBE_UNUSED bool NearlyEqualPageSizePair(const std::array<double, 2>& lhs, const std::array<double, 2>& rhs) {
    return NearlyEqualPageSizeComponent(lhs[0], rhs[0]) &&
           NearlyEqualPageSizeComponent(lhs[1], rhs[1]);
}

struct PageSizeRuleException {
    int page = -1;
    std::array<double, 2> sizePt{ 0.0, 0.0 };
};

struct PageSizeRuleDefinition {
    bool hasDefault = false;
    std::array<double, 2> defaultSizePt{ 0.0, 0.0 };
    std::vector<PageSizeRuleException> exceptions;
};

bool ExpandPageSizeRule(int pageCount,
                        const PageSizeRuleDefinition& rule,
                        std::vector<std::array<double, 2>>& out) {
    if (pageCount <= 0 || !rule.hasDefault) return false;
    out.assign(static_cast<size_t>(pageCount), rule.defaultSizePt);
    for (const auto& exception : rule.exceptions) {
        if (exception.page < 0 || exception.page >= pageCount) continue;
        out[static_cast<size_t>(exception.page)] = exception.sizePt;
    }
    return true;
}

#include "json_direct_parser.inc"

#ifndef CLROP_READ_ONLY_BUILD
// ---------------- serialization helpers ----------------
void AppendEscaped(std::ostringstream& oss, const std::string& utf8) {
    oss << '"';
    for (unsigned char c : utf8) {
        switch (c) {
        case '"': oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\b': oss << "\\b"; break;
        case '\f': oss << "\\f"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
            if (c < 0x20) {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
                    << std::dec << std::setfill('0');
            } else {
                oss << c;
            }
            break;
        }
    }
    oss << '"';
}

void AppendDouble(std::ostringstream& oss, double v) {
    oss << std::setprecision(12) << std::noshowpoint << std::defaultfloat << v;
}

void AppendPageSizePair(std::ostringstream& oss, const std::array<double, 2>& sizePt) {
    oss << "[";
    AppendDouble(oss, sizePt[0]);
    oss << ",";
    AppendDouble(oss, sizePt[1]);
    oss << "]";
}

void AppendPageSizesField(std::ostringstream& oss, const std::vector<std::array<double, 2>>& pageSizesPt) {
    oss << "\"page_sizes_pt\":[";
    for (size_t i = 0; i < pageSizesPt.size(); ++i) {
        if (i) oss << ",";
        AppendPageSizePair(oss, pageSizesPt[i]);
    }
    oss << "]";
}

void AppendPageSizeRuleField(std::ostringstream& oss, const PageSizeRuleDefinition& rule) {
    oss << "\"page_size_rule\":{";
    oss << "\"default_pt\":";
    AppendPageSizePair(oss, rule.defaultSizePt);
    oss << ",\"exceptions\":[";
    for (size_t i = 0; i < rule.exceptions.size(); ++i) {
        if (i) oss << ",";
        oss << "[";
        oss << rule.exceptions[i].page << ",";
        AppendDouble(oss, rule.exceptions[i].sizePt[0]);
        oss << ",";
        AppendDouble(oss, rule.exceptions[i].sizePt[1]);
        oss << "]";
    }
    oss << "]}";
}

bool TryBuildCompactPageSizeRule(const clrop::PdfId& pdfId, PageSizeRuleDefinition* out) {
    if (!out) return false;
    if (pdfId.pageCount <= 0) return false;
    if (pdfId.pageSizesPt.size() != static_cast<size_t>(pdfId.pageCount)) return false;
    if (pdfId.pageSizesPt.size() < 2) return false;

    struct Group {
        std::array<double, 2> sizePt{ 0.0, 0.0 };
        size_t count = 0;
    };

    std::vector<Group> groups;
    std::vector<size_t> membership(pdfId.pageSizesPt.size(), 0);
    for (size_t i = 0; i < pdfId.pageSizesPt.size(); ++i) {
        const auto& sizePt = pdfId.pageSizesPt[i];
        bool matched = false;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            if (!NearlyEqualPageSizePair(groups[gi].sizePt, sizePt)) continue;
            ++groups[gi].count;
            membership[i] = gi;
            matched = true;
            break;
        }
        if (!matched) {
            Group group;
            group.sizePt = sizePt;
            group.count = 1;
            membership[i] = groups.size();
            groups.push_back(group);
        }
    }
    if (groups.empty()) return false;

    size_t bestGroup = 0;
    for (size_t gi = 1; gi < groups.size(); ++gi) {
        if (groups[gi].count > groups[bestGroup].count) {
            bestGroup = gi;
        }
    }

    PageSizeRuleDefinition rule;
    rule.hasDefault = true;
    rule.defaultSizePt = groups[bestGroup].sizePt;
    for (size_t i = 0; i < membership.size(); ++i) {
        if (membership[i] == bestGroup) continue;
        PageSizeRuleException exception;
        exception.page = static_cast<int>(i);
        exception.sizePt = pdfId.pageSizesPt[i];
        rule.exceptions.push_back(exception);
    }

    *out = std::move(rule);
    return true;
}

std::string SerializePreferredPageSizeField(const clrop::PdfId& pdfId) {
    if (pdfId.pageSizesPt.empty()) return {};

    std::ostringstream raw;
    AppendPageSizesField(raw, pdfId.pageSizesPt);
    const std::string rawField = raw.str();

    PageSizeRuleDefinition rule;
    if (!TryBuildCompactPageSizeRule(pdfId, &rule)) {
        return rawField;
    }

    std::ostringstream compact;
    AppendPageSizeRuleField(compact, rule);
    const std::string compactField = compact.str();
    if (compactField.size() <= rawField.size()) {
        return compactField;
    }
    return rawField;
}

void AppendDoubleArray(std::ostringstream& oss, const std::vector<double>& vals, int group = 0) {
    oss << "[";
    if (group <= 0) {
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i) oss << ",";
            AppendDouble(oss, vals[i]);
        }
    } else {
        size_t idx = 0;
        size_t groupIdx = 0;
        while (idx < vals.size()) {
            if (groupIdx) oss << ",";
            oss << "[";
            for (int j = 0; j < group; ++j) {
                if (j) oss << ",";
                if (idx < vals.size()) {
                    AppendDouble(oss, vals[idx]);
                    ++idx;
                }
            }
            oss << "]";
            ++groupIdx;
        }
    }
    oss << "]";
}
#endif

} // namespace

bool ParseClropFromJson(const std::string& json, clrop::Document& out, std::wstring& err) {
    DirectDocumentParser parser(SkipUtf8Bom(json));
    return parser.Parse(out, err);
}

bool ParseClropFromJsonToSink(const std::string& json,
                              clrop::PdfId* outPdfId,
                              clrop::ItemLoadSink& sink,
                              std::wstring& err) {
    DirectDocumentParser parser(SkipUtf8Bom(json));
    return parser.ParseToSink(outPdfId, sink, err);
}

bool LoadClropFile(const std::wstring& path,
                   clrop::Document& out,
                   std::wstring& err,
                   LoadFileFailureKind* failureKind) {
    if (failureKind) *failureKind = LoadFileFailureKind::None;
    std::string json;
    if (!ReadFileToString(path, json, err)) {
        if (failureKind) *failureKind = LoadFileFailureKind::Read;
        return false;
    }
    const bool ok = ParseClropFromJson(json, out, err);
    if (!ok && failureKind) *failureKind = LoadFileFailureKind::Parse;
    return ok;
}

bool LoadClropFileToSink(const std::wstring& path,
                         clrop::PdfId* outPdfId,
                         clrop::ItemLoadSink& sink,
                         std::wstring& err,
                         LoadFileFailureKind* failureKind) {
    if (failureKind) *failureKind = LoadFileFailureKind::None;
    std::string json;
    if (!ReadFileToString(path, json, err)) {
        if (failureKind) *failureKind = LoadFileFailureKind::Read;
        return false;
    }
    const bool ok = ParseClropFromJsonToSink(json, outPdfId, sink, err);
    if (!ok && failureKind) *failureKind = LoadFileFailureKind::Parse;
    return ok;
}

#ifndef CLROP_READ_ONLY_BUILD
bool SerializeClrop(const clrop::Document& doc, std::string& out, std::wstring& err) {
    (void)err;
    std::ostringstream oss;
    oss << "{";
    oss << "\"version\":1,";
    oss << "\"pdf_id\":{";
    oss << "\"path\":";
    AppendEscaped(oss, WideToUtf8(doc.pdfId.path));
    oss << ",\"size\":" << doc.pdfId.size << ",";
    if (doc.pdfId.pageCount > 0) {
        oss << "\"page_count\":" << doc.pdfId.pageCount << ",";
    }
    const std::string pageSizeField = SerializePreferredPageSizeField(doc.pdfId);
    if (!pageSizeField.empty()) {
        oss << pageSizeField << ",";
    }
    oss << "\"sha256\":";
    AppendEscaped(oss, doc.pdfId.sha256);
    oss << "},";
    oss << "\"pages\":[";
    for (size_t pi = 0; pi < doc.pages.size(); ++pi) {
        if (pi) oss << ",";
        const auto& p = doc.pages[pi];
        oss << "{";
        oss << "\"page\":" << p.page << ",";
        oss << "\"items\":[";
        for (size_t ii = 0; ii < p.items.size(); ++ii) {
            if (ii) oss << ",";
            const auto& it = p.items[ii];
            oss << "{";
            oss << "\"type\":";
            switch (it.kind) {
            case clrop::Item::Kind::Text: AppendEscaped(oss, "text"); break;
            case clrop::Item::Kind::Math: AppendEscaped(oss, "math"); break;
            case clrop::Item::Kind::MarkerText: AppendEscaped(oss, "marker-text"); break;
            case clrop::Item::Kind::TextColor: AppendEscaped(oss, "text-color"); break;
            case clrop::Item::Kind::MarkerFree: AppendEscaped(oss, "marker-free"); break;
            case clrop::Item::Kind::Line: AppendEscaped(oss, "line"); break;
            case clrop::Item::Kind::Arrow: AppendEscaped(oss, "arrow"); break;
            case clrop::Item::Kind::Wave: AppendEscaped(oss, "wave"); break;
            case clrop::Item::Kind::Freehand: AppendEscaped(oss, "freehand"); break;
            case clrop::Item::Kind::LinkMarker: AppendEscaped(oss, "link-marker"); break;
            case clrop::Item::Kind::Shape: AppendEscaped(oss, "shape"); break;
            }
            if (!it.id.empty())      { oss << ",\"id\":";      AppendEscaped(oss, WideToUtf8(it.id)); }
            if (!it.created.empty()) { oss << ",\"created\":"; AppendEscaped(oss, WideToUtf8(it.created)); }
            if (!it.updated.empty()) { oss << ",\"updated\":"; AppendEscaped(oss, WideToUtf8(it.updated)); }
            if (!it.color.empty())   { oss << ",\"color\":";   AppendEscaped(oss, it.color); }
            if (it.alpha != 0.0)     { oss << ",\"alpha\":";   AppendDouble(oss, it.alpha); }
            if (it.width != 0.0)     { oss << ",\"width\":";   AppendDouble(oss, it.width); }
            if (it.pt != 0.0)        { oss << ",\"pt\":";      AppendDouble(oss, it.pt); }
            if (!it.font.empty())    { oss << ",\"font\":";    AppendEscaped(oss, WideToUtf8(it.font)); }
            if (!it.writingMode.empty()) { oss << ",\"writing_mode\":"; AppendEscaped(oss, it.writingMode); }
            if (!it.border.empty())  { oss << ",\"border\":";  AppendEscaped(oss, it.border); }
            if (!it.content.empty()) { oss << ",\"content\":"; AppendEscaped(oss, WideToUtf8(it.content)); }
            if (!it.linkId.empty())  { oss << ",\"link_id\":"; AppendEscaped(oss, WideToUtf8(it.linkId)); }
            if (!it.notePath.empty()) { oss << ",\"note_path\":"; AppendEscaped(oss, WideToUtf8(it.notePath)); }
            if (!it.lines.empty()) {
                oss << ",\"lines\":[";
                for (size_t li = 0; li < it.lines.size(); ++li) {
                    if (li) oss << ",";
                    AppendEscaped(oss, WideToUtf8(it.lines[li]));
                }
                oss << "]";
            }
            if (!it.mathKind.empty()) {
                oss << ",\"math_kind\":";
                AppendEscaped(oss, it.mathKind);
            }
            if (!it.shapeKind.empty()) {
                oss << ",\"shape_kind\":";
                AppendEscaped(oss, it.shapeKind);
            }
            if (!it.shapeDrawMode.empty()) {
                oss << ",\"shape_draw_mode\":";
                AppendEscaped(oss, it.shapeDrawMode);
            }
            if (!it.arrowHead.empty()) {
                oss << ",\"arrow_head\":";
                AppendEscaped(oss, it.arrowHead);
            }
            if (it.shapeRotation && std::isfinite(*it.shapeRotation)) {
                oss << ",\"shape_rotation\":" << *it.shapeRotation;
            }
            if (it.bbox) {
                oss << ",\"bbox\":";
                std::vector<double> v{(*it.bbox)[0], (*it.bbox)[1], (*it.bbox)[2], (*it.bbox)[3]};
                AppendDoubleArray(oss, v);
            }
            if (it.p1) {
                oss << ",\"p1\":";
                std::vector<double> v{(*it.p1)[0], (*it.p1)[1]};
                AppendDoubleArray(oss, v);
            }
            if (it.p2) {
                oss << ",\"p2\":";
                std::vector<double> v{(*it.p2)[0], (*it.p2)[1]};
                AppendDoubleArray(oss, v);
            }
            if (!it.dash.empty()) {
                oss << ",\"dash\":";
                AppendDoubleArray(oss, it.dash);
            }
            if (!it.path.empty()) {
                oss << ",\"path\":";
                AppendDoubleArray(oss, it.path, 2);
            }
            if (!it.quads.empty()) {
                oss << ",\"quads\":";
                AppendDoubleArray(oss, it.quads, 8);
            }
            oss << "}";
        }
        oss << "]";
        oss << "}";
    }
    oss << "]";
    oss << "}";
    out = oss.str();
    return true;
}

bool SaveClropFile(const std::wstring& path, const clrop::Document& doc, std::wstring& err) {
    return SaveClropFile(path, doc, err, /*preferredTempDir=*/{}, /*quarantineDir=*/{});
}

bool SaveClropFile(const std::wstring& path,
                   const clrop::Document& doc,
                   std::wstring& err,
                   const std::filesystem::path& preferredTempDir,
                   const std::filesystem::path& quarantineDir) {
    std::string json;
    if (!SerializeClrop(doc, json, err)) return false;
    std::filesystem::path p(path);
    if (!p.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::wstring writeErr;
    if (!atomic_write::AtomicWriteUtf8(p, json, preferredTempDir, quarantineDir, &writeErr)) {
        err = writeErr.empty() ? L"failed to write clrop file" : writeErr;
        return false;
    }
    return true;
}
#endif

} // namespace clrop

