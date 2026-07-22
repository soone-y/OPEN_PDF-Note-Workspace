#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "clrop/json.h"

namespace {

namespace fs = std::filesystem;

int g_failed = 0;

fs::path MakeTestRoot() {
    fs::path root = fs::temp_directory_path() / L"pdf_note_clrop_file_safety_tests";
    root /= std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

bool SetReadOnlyFlag(const fs::path& path, bool readOnly) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (readOnly) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    } else {
        attrs &= ~FILE_ATTRIBUTE_READONLY;
    }
    return SetFileAttributesW(path.c_str(), attrs) != 0;
}

bool HasRegularFileUnder(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (entry.is_regular_file(ec) && !ec) return true;
        ec.clear();
    }
    return false;
}

clrop::Document MakeDoc(const std::wstring& pdfPath, std::uint64_t size, const std::wstring& itemId) {
    clrop::Document doc;
    doc.version = 1;
    doc.pdfId.path = pdfPath;
    doc.pdfId.size = size;
    doc.pdfId.pageCount = 1;
    doc.pdfId.pageSizesPt = {std::array<double, 2>{595.0, 842.0}};
    doc.pdfId.sha256 = "abc123";

    clrop::Page page;
    page.page = 0;
    clrop::Item item;
    item.kind = clrop::Item::Kind::Text;
    item.id = itemId;
    item.created = L"2026-05-12T00:00:00Z";
    item.updated = L"2026-05-12T00:00:01Z";
    item.content = L"safe save payload";
    item.bbox = std::array<double, 4>{1.0, 2.0, 3.0, 4.0};
    page.items.push_back(std::move(item));
    doc.pages.push_back(std::move(page));
    return doc;
}

bool LoadHasTextItemId(const fs::path& path, const std::wstring& itemId) {
    clrop::Document loaded;
    std::wstring err;
    clrop::LoadFileFailureKind failureKind = clrop::LoadFileFailureKind::None;
    if (!clrop::LoadClropFile(path.wstring(), loaded, err, &failureKind)) return false;
    return failureKind == clrop::LoadFileFailureKind::None &&
           loaded.pages.size() == 1 &&
           loaded.pages[0].items.size() == 1 &&
           loaded.pages[0].items[0].id == itemId;
}

bool TestSaveCreatesNestedFileAndLoads(const fs::path& root) {
    const fs::path dest = root / L"nested" / L"sample.clrop";
    const clrop::Document doc = MakeDoc(L"nested.pdf", 100, L"created-id");
    std::wstring err;
    const bool saved = clrop::SaveClropFile(dest.wstring(), doc, err, root / L"tmp", root / L"escape");
    return saved && LoadHasTextItemId(dest, L"created-id");
}

bool TestSaveReplaceExistingValidFile(const fs::path& root) {
    const fs::path dest = root / L"replace.clrop";
    std::wstring err;
    if (!clrop::SaveClropFile(dest.wstring(), MakeDoc(L"old.pdf", 100, L"old-id"), err,
                              root / L"tmp_replace", root / L"escape_replace")) {
        return false;
    }
    const std::string before = ReadAll(dest);
    if (before.empty()) return false;
    if (!clrop::SaveClropFile(dest.wstring(), MakeDoc(L"new.pdf", 200, L"new-id"), err,
                              root / L"tmp_replace", root / L"escape_replace")) {
        return false;
    }
    const std::string after = ReadAll(dest);
    return after != before && LoadHasTextItemId(dest, L"new-id");
}

bool TestSaveLockedDestinationFailsAndPreservesOriginal(const fs::path& root) {
    const fs::path dest = root / L"locked.clrop";
    std::wstring err;
    if (!clrop::SaveClropFile(dest.wstring(), MakeDoc(L"locked.pdf", 100, L"original-id"), err,
                              root / L"tmp_locked", root / L"escape_locked")) {
        return false;
    }
    const std::string original = ReadAll(dest);
    if (original.empty()) return false;

    HANDLE lock = CreateFileW(dest.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (lock == INVALID_HANDLE_VALUE) return false;

    err.clear();
    const bool saved = clrop::SaveClropFile(dest.wstring(), MakeDoc(L"locked.pdf", 200, L"new-id"), err,
                                            root / L"tmp_locked", root / L"escape_locked");
    CloseHandle(lock);

    return !saved &&
           ReadAll(dest) == original &&
           LoadHasTextItemId(dest, L"original-id") &&
           !err.empty() &&
           HasRegularFileUnder(root / L"escape_locked");
}

bool TestSaveReadOnlyDestinationFailsAndPreservesOriginal(const fs::path& root) {
    const fs::path dest = root / L"readonly.clrop";
    std::wstring err;
    if (!clrop::SaveClropFile(dest.wstring(), MakeDoc(L"readonly.pdf", 100, L"original-id"), err,
                              root / L"tmp_readonly", root / L"escape_readonly")) {
        return false;
    }
    const std::string original = ReadAll(dest);
    if (original.empty()) return false;
    if (!SetReadOnlyFlag(dest, true)) return false;

    err.clear();
    const bool saved = clrop::SaveClropFile(dest.wstring(), MakeDoc(L"readonly.pdf", 200, L"new-id"), err,
                                            root / L"tmp_readonly", root / L"escape_readonly");
    DWORD attrsAfter = GetFileAttributesW(dest.c_str());
    const bool stillReadOnly =
        attrsAfter != INVALID_FILE_ATTRIBUTES && ((attrsAfter & FILE_ATTRIBUTE_READONLY) != 0);
    SetReadOnlyFlag(dest, false);

    return !saved &&
           stillReadOnly &&
           ReadAll(dest) == original &&
           LoadHasTextItemId(dest, L"original-id") &&
           !err.empty();
}

bool TestSaveEmptyDestinationFailsWithoutCreatingTmp(const fs::path& root) {
    std::wstring err;
    const bool saved = clrop::SaveClropFile(L"", MakeDoc(L"empty.pdf", 100, L"id"), err,
                                            root / L"tmp_empty", root / L"escape_empty");
    std::error_code ec;
    const bool tmpExists = fs::exists(root / L"tmp_empty", ec) && !ec;
    ec.clear();
    const bool escapeExists = fs::exists(root / L"escape_empty", ec) && !ec;
    return !saved && !err.empty() && !tmpExists && !escapeExists;
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    const fs::path root = MakeTestRoot();

    auto run = [&](const char* name, bool (*fn)(const fs::path&)) {
        std::cout << "Testing: " << name << "... " << std::flush;
        const bool ok = fn(root);
        std::cout << (ok ? "OK" : "FAILED") << std::endl;
        if (!ok) ++g_failed;
    };

    run("SaveClropFile creates nested file and loads", &TestSaveCreatesNestedFileAndLoads);
    run("SaveClropFile replaces existing valid file", &TestSaveReplaceExistingValidFile);
    run("SaveClropFile locked destination fails and preserves original", &TestSaveLockedDestinationFailsAndPreservesOriginal);
    run("SaveClropFile read-only destination fails and preserves original", &TestSaveReadOnlyDestinationFailsAndPreservesOriginal);
    run("SaveClropFile empty destination fails without temp artifacts", &TestSaveEmptyDestinationFailsWithoutCreatingTmp);

    std::error_code ec;
    fs::remove_all(root, ec);

    std::cout << "Summary: failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
