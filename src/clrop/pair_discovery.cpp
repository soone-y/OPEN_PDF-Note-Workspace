#include "pair_discovery.h"
#include "json.h"
#include "pdf_identity.h"
#include "core/document_source_protection.h"
#include "core/identity_path_scope.h"
#include "core/sha256.h"
#include "fpdfview.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>

extern std::recursive_mutex g_pdfiumMutex;
namespace clrop::discovery {
namespace {
namespace fs = std::filesystem;
std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}
struct Guard {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Guard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Work {
    const Request& request;
    const std::atomic_bool& cancel;
    Result result;
    ULONGLONG start = GetTickCount64();
    std::uint64_t resultBytes = 0;
    bool outputFull = false;
    bool Stop() {
        if (cancel.load()) { result.completion = Completion::Canceled; return true; }
        if (outputFull) { result.completion = Completion::Limited; return true; }
        if (GetTickCount64() - start >= request.limits.milliseconds) {
            result.completion = Completion::Limited; return true;
        }
        return false;
    }
    bool Add(Row row) {
        const std::uint64_t bytes = sizeof(Row) +
            (row.clrop.native().size() + row.pdf.native().size()) * sizeof(wchar_t) + row.expectedHash.size() + row.actualHash.size();
        if (result.rows.size() >= request.limits.rows || bytes > request.limits.resultBytes - resultBytes) {
            result.completion = Completion::Limited; outputFull = true; return false;
        }
        resultBytes += bytes;
        result.rows.push_back(std::move(row)); return true;
    }
};
bool Safe(const fs::path& path) {
    const auto local = identity_path_scope::LocalPath(path.wstring());
    return local && identity_path_scope::NoReparseAncestors(*local);
}
bool OpenRead(const fs::path& path, Guard& guard, std::uint64_t& size) {
    if (!Safe(path)) return false;
    guard.value = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (guard.value == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (!GetFileInformationByHandle(guard.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(guard.value, &length) || length.QuadPart < 0) return false;
    size = static_cast<std::uint64_t>(length.QuadPart); return true;
}
struct MetadataSink final : ItemLoadSink {
    Work& work;
    explicit MetadataSink(Work& w) : work(w) {}
    bool OnClropItem(int, Item&&, std::wstring&) override { return !work.Stop(); }
};
struct Sidecar { fs::path path; PdfId id; bool orphan = true; };
bool ReadSidecar(Work& work, const fs::path& path, Sidecar& out) {
    Guard guard;
    std::uint64_t size = 0;
    if (!OpenRead(path, guard, size)) return false;
    if (size > work.request.limits.clropBytes) { work.result.completion = Completion::Limited; return false; }
    MetadataSink sink(work);
    std::wstring error;
    if (!LoadClropFileToSink(path.wstring(), &out.id, sink, error) || work.Stop()) return false;
    // Stored paths are never discovery inputs. Discard them before retaining
    // metadata, and bound metadata that would otherwise be copied to many rows.
    std::wstring{}.swap(out.id.path);
    if ((out.id.pageCount > 0 && static_cast<size_t>(out.id.pageCount) > work.request.limits.pages) || out.id.pageSizesPt.size() > work.request.limits.pages) {
        work.result.completion = Completion::Limited; return false;
    }
    if (!out.id.sha256.empty() && (out.id.sha256.size() != 64 ||
        !std::all_of(out.id.sha256.begin(), out.id.sha256.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))) return false;
    out.path = path;
    auto adjacent = path; adjacent.replace_extension(L".pdf");
    const DWORD attrs = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(adjacent).c_str());
    out.orphan = attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    return true;
}
int PdfBlock(void* context, unsigned long position, unsigned char* data, unsigned long count) {
    LARGE_INTEGER offset{}; offset.QuadPart = position;
    const HANDLE handle = *static_cast<HANDLE*>(context);
    DWORD got = 0;
    return SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) &&
        ReadFile(handle, data, count, &got, nullptr) && got == count;
}
bool ReadPdf(Work& work, HANDLE handle, std::uint64_t size, PdfId& id) {
    id.size = size;
    if (!size || size > std::numeric_limits<unsigned long>::max()) return false;
    std::lock_guard<std::recursive_mutex> lock(g_pdfiumMutex);
    if (work.Stop()) return false;
    FPDF_FILEACCESS access{static_cast<unsigned long>(size), PdfBlock, &handle};
    FPDF_DOCUMENT document = FPDF_LoadCustomDocument(&access, nullptr);
    if (!document) return false;
    struct Close { FPDF_DOCUMENT value; ~Close() { FPDF_CloseDocument(value); } } close{document};
    id.pageCount = FPDF_GetPageCount(document);
    if (id.pageCount <= 0) return false;
    if (static_cast<size_t>(id.pageCount) > work.request.limits.pages) { work.result.completion = Completion::Limited; return false; }
    for (int page = 0; page < id.pageCount; ++page) {
        if (work.Stop()) return false;
        double width = 0, height = 0;
        if (!FPDF_GetPageSizeByIndex(document, page, &width, &height)) return false;
        id.pageSizesPt.push_back({width, height});
    }
    return true;
}
bool Hash(Work& work, HANDLE handle, std::uint64_t size, std::string& out) {
    if (size > work.request.limits.hashBytes - work.result.hashedBytes) {
        work.result.completion = Completion::Limited; return false;
    }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) return false;
    core_hash::Sha256 hash;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t left = size;
    while (left) {
        if (work.Stop()) return false;
        const DWORD count = static_cast<DWORD>(std::min<std::uint64_t>(left, buffer.size()));
        DWORD got = 0;
        if (!ReadFile(handle, buffer.data(), count, &got, nullptr) || got != count) return false;
        hash.Update(buffer.data(), got); left -= got; work.result.hashedBytes += got;
    }
    constexpr char hex[] = "0123456789abcdef";
    for (const auto b : hash.Finalize()) { out += hex[b >> 4]; out += hex[b & 15]; }
    return true;
}
Row Compare(const Sidecar& source, const fs::path& path, const PdfId& actual) {
    Row row;
    row.clrop = source.path; row.pdf = path; row.orphan = source.orphan; row.preview = true;
    row.sameName = Lower(path.stem().wstring()) == Lower(source.path.stem().wstring());
    const auto& expected = source.id;
    row.expectedSize = expected.size; row.actualSize = actual.size;
    row.expectedPages = expected.pageCount; row.actualPages = actual.pageCount;
    row.expectedHash = expected.sha256; row.actualHash = actual.sha256;
    row.size = !expected.size ? Evidence::Missing : expected.size == actual.size ? Evidence::Match : Evidence::Different;
    row.pages = expected.pageCount <= 0 ? Evidence::Missing : expected.pageCount == actual.pageCount ? Evidence::Match : Evidence::Different;
    row.dimensions = Evidence::Missing;
    if (PdfIdHasFastFingerprint(expected)) {
        row.dimensions = Evidence::Different;
        if (expected.pageCount == actual.pageCount) {
            bool equal = true;
            for (size_t i = 0; i < expected.pageSizesPt.size(); ++i)
                for (size_t axis = 0; axis < 2; ++axis)
                    equal = equal && std::abs(expected.pageSizesPt[i][axis] - actual.pageSizesPt[i][axis]) <= 0.01;
            if (equal) row.dimensions = Evidence::Match;
        }
    }
    row.hash = expected.sha256.empty() ? Evidence::Missing : actual.sha256.empty() ? Evidence::Unknown :
        expected.sha256 == actual.sha256 ? Evidence::Match : Evidence::Different;
    const bool structure = row.pages == Evidence::Match && row.dimensions == Evidence::Match;
    row.grade = row.hash == Evidence::Match ? Grade::Hash : structure && row.size == Evidence::Match ? Grade::StructureAndSize :
        structure ? Grade::Structure : row.size == Evidence::Match ? Grade::Size : row.pages == Evidence::Match ? Grade::Pages :
        row.sameName ? Grade::Name : Grade::NoCandidate;
    return row;
}
}
Result Scan(const Request& request, const std::atomic_bool& cancel) {
    Work work{request, cancel, {}};
    try {
        if (work.Stop()) return std::move(work.result);
        if (!Safe(request.directory) || (request.mode == Mode::ForClrop &&
            (!Safe(request.clrop) || Lower(request.clrop.extension().wstring()) != L".clrop"))) {
            work.result.completion = Completion::Failed; return std::move(work.result);
        }
        std::vector<fs::path> pdfs, clrops;
        if (request.mode == Mode::ForClrop) clrops.push_back(request.clrop);
        std::error_code error;
        fs::directory_iterator iterator(fs::path(ToExtendedWin32PathIfAbsoluteLocal(request.directory)), error), end;
        if (error) { work.result.completion = Completion::Failed; return std::move(work.result); }
        for (; iterator != end; iterator.increment(error)) {
            if (error) { work.result.completion = Completion::Failed; break; }
            if (work.Stop()) break;
            if (++work.result.entries > request.limits.entries) { work.result.completion = Completion::Limited; break; }
            const auto path = request.directory / iterator->path().filename();
            const DWORD attrs = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) { ++work.result.skipped; continue; }
            if (attrs & FILE_ATTRIBUTE_DIRECTORY) continue;
            const auto ext = Lower(path.extension().wstring());
            if (ext == L".pdf") {
                if (pdfs.size() >= request.limits.pdfs) { work.result.completion = Completion::Limited; break; }
                pdfs.push_back(path);
            } else if (ext == L".clrop" && request.mode == Mode::Directory) {
                if (clrops.size() >= request.limits.clrops) { work.result.completion = Completion::Limited; break; }
                clrops.push_back(path);
            }
        }
        if (error) work.result.completion = Completion::Failed;
        std::sort(pdfs.begin(), pdfs.end()); std::sort(clrops.begin(), clrops.end());
        std::vector<Sidecar> sources;
        for (const auto& path : clrops) {
            if (work.Stop()) break;
            ++work.result.clrops;
            Sidecar source;
            if (ReadSidecar(work, path, source)) sources.push_back(std::move(source));
            else { Row row; row.clrop = path; if (!work.Add(std::move(row))) break; }
        }
        std::vector<bool> found(sources.size(), false);
        if (sources.empty()) return std::move(work.result);
        for (const auto& path : pdfs) {
            if (work.Stop()) break;
            ++work.result.pdfs;
            Guard guard;
            PdfId actual;
            if (!OpenRead(path, guard, actual.size) || !ReadPdf(work, guard.value, actual.size, actual)) {
                Row row; row.pdf = path; if (!work.Add(std::move(row))) break;
                continue;
            }
            bool needHash = false;
            for (const auto& source : sources)
                needHash = needHash || (!source.id.sha256.empty() && (source.id.size == actual.size || !source.id.size));
            if (needHash) (void)Hash(work, guard.value, actual.size, actual.sha256);
            for (size_t i = 0; i < sources.size(); ++i) {
                if (work.Stop()) break;
                auto row = Compare(sources[i], path, actual);
                if (row.grade != Grade::NoCandidate) {
                    found[i] = true;
                    if (!work.Add(std::move(row))) break;
                }
            }
            if (work.result.rows.size() >= request.limits.rows) break;
        }
        for (size_t i = 0; i < sources.size() && !work.Stop(); ++i) if (!found[i]) {
            Row row; row.clrop = sources[i].path; row.orphan = sources[i].orphan; row.grade = Grade::NoCandidate;
            if (!work.Add(std::move(row))) break;
        }
        std::stable_sort(work.result.rows.begin(), work.result.rows.end(), [](const Row& a, const Row& b) { return a.grade < b.grade; });
    } catch (...) { work.result.completion = Completion::Failed; }
    return std::move(work.result);
}
} // namespace clrop::discovery
