#include "clrop/pair_discovery.h"
#include "clrop/json.h"
#include "core/file_sha256.h"
#include "fpdfview.h"
#include "fpdf_edit.h"
#include "fpdf_save.h"
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

std::recursive_mutex g_pdfiumMutex;
namespace fs = std::filesystem;
using namespace clrop::discovery;
int checks = 0;
void Check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size()); Check(bool(output), "write owned fixture");
}
std::string Pdf(double width = 600, double height = 800) {
    FPDF_DOCUMENT document = FPDF_CreateNewDocument(); Check(document != nullptr, "create PDF");
    for (int i = 0; i < 2; ++i) { FPDF_PAGE page = FPDFPage_New(document, i, width, height); Check(page != nullptr, "create page"); FPDF_ClosePage(page); }
    struct Writer : FPDF_FILEWRITE {
        std::string bytes;
        Writer() { version = 1; WriteBlock = [](FPDF_FILEWRITE* base, const void* bytes, unsigned long size) -> int {
            try { static_cast<Writer*>(base)->bytes.append(static_cast<const char*>(bytes), size); return 1; } catch (...) { return 0; }
        }; }
    } writer;
    const bool saved = FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    FPDF_CloseDocument(document); Check(saved, "serialize PDF"); return writer.bytes + "\n% FIRST\n";
}
std::string Digest(const fs::path& path) {
    std::string hash; Check(core_hash::FileSha256(path, &hash), "hash fixture"); return hash;
}
void Sidecar(const fs::path& path, const clrop::PdfId& id) {
    std::ostringstream json;
    json << "{\"version\":2,\"pdf_id\":{\"path\":\"" << fs::path(id.path).generic_string() <<
        "\",\"size\":" << id.size << ",\"page_count\":" << id.pageCount << ",\"sha256\":\"" << id.sha256 << "\"";
    if (!id.pageSizesPt.empty()) {
        json << ",\"page_sizes_pt\":[";
        for (size_t i=0;i<id.pageSizesPt.size();++i) {
            if (i) json << ',';
            json << '[' << id.pageSizesPt[i][0] << ',' << id.pageSizesPt[i][1] << ']';
        }
        json << ']';
    }
    json << "},\"pages\":[]}"; Write(path, json.str());
}
const Row* Find(const Result& result, const std::wstring& clrop, const std::wstring& pdf) {
    for (const auto& row : result.rows) if (row.clrop.filename() == clrop && row.pdf.filename() == pdf) return &row;
    return nullptr;
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "owned fixture directory supplied");
        const fs::path root(argv[1]), search = root / L"search", external = root / L"source";
        fs::create_directories(search / L"nested"); fs::create_directories(external);
        FPDF_InitLibrary();
        struct Cleanup { ~Cleanup() { FPDF_DestroyLibrary(); } } cleanup;
        const auto original = Pdf();
        Write(search / L"renamed.pdf", original); Write(search / L"copy.pdf", original);
        Write(search / L"\u53c2\u7167 \u5143.PDF", original);
        auto different = original; different.replace(different.size() - 6, 5, "OTHER");
        Check(different.size() == original.size(), "same-size substitution fixture");
        Write(search / L"same-size-other.pdf", different);
        Write(search / L"edited.pdf", original + "\n% annotation-like incremental change\n");
        Write(search / L"same_name.pdf", Pdf(610, 700));
        Write(search / L"nested" / L"excluded.pdf", original);
        Write(external / L"outside.pdf", original);
        Write(search / L"broken.pdf", "not a PDF");
        clrop::PdfId id; id.path = (external / L"outside.pdf").wstring();
        id.size = original.size(); id.pageCount = 2; id.pageSizesPt = {{600, 800}, {600, 800}};
        id.sha256 = Digest(search / L"renamed.pdf");
        Sidecar(search / L"orphan.clrop", id); Sidecar(external / L"chosen.CLROP", id);
        auto legacy = id; legacy.sha256.clear(); legacy.pageSizesPt.clear(); Sidecar(search / L"legacy.clrop", legacy);
        auto wrong = id; wrong.size = 123456; wrong.pageCount = 10; wrong.pageSizesPt.clear(); wrong.sha256.clear();
        Sidecar(search / L"same_name.clrop", wrong);
        Write(search / L"broken.clrop", "{");
        std::vector<std::pair<fs::path, std::string>> before;
        for (const auto& entry : fs::recursive_directory_iterator(root)) if (entry.is_regular_file()) before.emplace_back(entry.path(), Digest(entry.path()));
        std::atomic_bool cancel{false}; Request request; request.directory = search;
        auto result = Scan(request, cancel);
        Check(result.completion == Completion::Complete, "directory scan completes");
        Check(result.pdfs == 7 && result.clrops == 4, "only immediate PDF and CLROP files are examined");
        const auto* row = Find(result, L"orphan.clrop", L"renamed.pdf");
        Check(row && row->grade == Grade::Hash && row->orphan && row->preview, "renamed orphan has a previewable hash candidate");
        row = Find(result, L"orphan.clrop", L"copy.pdf");
        Check(row && row->grade == Grade::Hash, "identical copies remain distinct candidates");
        row = Find(result, L"orphan.clrop", L"\u53c2\u7167 \u5143.PDF");
        Check(row && row->grade == Grade::Hash, "Unicode names, spaces and uppercase PDF extensions preserve matching");
        row = Find(result, L"orphan.clrop", L"same-size-other.pdf");
        Check(row && row->hash == Evidence::Different && row->grade == Grade::StructureAndSize, "same-size hash mismatch is a weaker candidate");
        row = Find(result, L"orphan.clrop", L"edited.pdf");
        Check(row && row->grade == Grade::Structure && row->size == Evidence::Different, "changed-size PDF remains a structure candidate");
        row = Find(result, L"same_name.clrop", L"same_name.pdf");
        Check(row && row->sameName && !row->orphan && row->grade == Grade::Name, "same name never implies identity equality");
        row = Find(result, L"legacy.clrop", L"renamed.pdf");
        Check(row && row->hash == Evidence::Missing && row->dimensions == Evidence::Missing && row->grade == Grade::Size, "legacy evidence is not promoted to hash or structure equality");
        Check(Find(result, L"broken.clrop", L"") && Find(result, L"", L"broken.pdf"), "invalid inputs remain visible and unchanged");
        for (const auto& item : result.rows) Check(item.pdf.filename() != L"excluded.pdf" && item.pdf.filename() != L"outside.pdf", "no recursion or stored-path expansion");
        request.mode = Mode::ForClrop; request.clrop = external / L"chosen.CLROP";
        result = Scan(request, cancel);
        Check(result.clrops == 1 && Find(result, L"chosen.CLROP", L"renamed.pdf"), "CLROP-focused search accepts a separate source directory");
        for (const auto& item : result.rows) Check(item.clrop.empty() || item.clrop == request.clrop, "focused mode does not load neighboring CLROP files");
        auto limited = request; limited.limits.hashBytes = 0;
        result = Scan(limited, cancel);
        Check(result.completion == Completion::Limited && result.hashedBytes == 0, "hash budget reports unchecked work");
        row = Find(result, L"chosen.CLROP", L"renamed.pdf");
        Check(row && row->hash == Evidence::Unknown, "unchecked hash is not a mismatch or a match");
        limited = request; limited.limits.clropBytes = 1; result = Scan(limited, cancel);
        Check(result.completion == Completion::Limited && result.rows.size() == 1 && !result.rows[0].preview, "sidecar byte limit leaves an unchecked row");
        limited = request; limited.limits.pages = 1; result = Scan(limited, cancel);
        Check(result.completion == Completion::Limited, "page limit is reported rather than accepted as full comparison");
        limited = request; limited.limits.rows = 1; result = Scan(limited, cancel);
        Check(result.completion == Completion::Limited && result.rows.size() <= 1, "result row limit is explicit");
        limited = request; limited.limits.resultBytes = 1; result = Scan(limited, cancel);
        Check(result.completion == Completion::Limited && result.rows.empty(), "result payload budget is explicit");
        limited = request; limited.limits.entries = 1; result = Scan(limited, cancel);
        Check(result.completion == Completion::Limited, "directory entry limit is explicit");
        limited = request; limited.limits.milliseconds = 0; result = Scan(limited, cancel);
        Check(result.completion == Completion::Limited, "time budget is explicit");
        cancel.store(true); result = Scan(request, cancel);
        Check(result.completion == Completion::Canceled && result.rows.empty(), "canceled search does not publish success"); cancel.store(false);
        const auto held = search / L"renamed.pdf";
        HANDLE writer = CreateFileW(held.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(writer != INVALID_HANDLE_VALUE, "hold external-style PDF writer");
        result = Scan(request, cancel); CloseHandle(writer);
        row = Find(result, L"", L"renamed.pdf");
        Check(row && row->grade == Grade::Unreadable && !row->preview, "unprotectable PDF is not accepted as evidence");
        auto bad = request; bad.directory = L"relative";
        Check(Scan(bad, cancel).completion == Completion::Failed, "relative directory is rejected");
        bad.directory = L"\\\\server\\share";
        Check(Scan(bad, cancel).completion == Completion::Failed, "UNC is rejected before enumeration");
        auto malformed = id; malformed.sha256 = std::string(1000000, 'a');
        const auto invalidId = external / L"invalid-id.clrop";
        Sidecar(invalidId, malformed);
        auto invalidRequest = request; invalidRequest.clrop = invalidId;
        result = Scan(invalidRequest, cancel);
        Check(result.rows.size() == 1 && result.rows[0].grade == Grade::Unreadable && !result.rows[0].preview,
            "oversized SHA metadata is rejected before candidate multiplication");
        malformed = id; malformed.pageCount = 10001; malformed.pageSizesPt.clear();
        Sidecar(invalidId, malformed); result = Scan(invalidRequest, cancel);
        Check(result.completion == Completion::Limited && result.rows.size() == 1 && !result.rows[0].preview,
            "stored page metadata is bounded before retention");
        for (const auto& saved : before) Check(Digest(saved.first) == saved.second, "all originals remain byte-identical");
        std::cout << "PASS: PDF pair discovery / " << checks << " checks\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
