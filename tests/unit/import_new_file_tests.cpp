#include "workspace/import_new_file.h"
#include <cassert>
#include <fstream>
#include <iostream>

static void Write(const std::filesystem::path& path, const char* bytes) {
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    file.close();
    assert(file);
}
static std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main() {
    std::error_code ec;
    const auto root = std::filesystem::current_path(ec) /
        (L"import_fixture_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    assert(!ec && std::filesystem::create_directory(root, ec) && !ec);
    DWORD error = ERROR_SUCCESS;
    const auto requested = root / L"資料.PDF";
    std::filesystem::path chosen;
    assert(workspace_import::ChooseNewDestination(requested, &chosen, &error) && chosen == requested);
    Write(requested, "existing original");
    Write(root / L"資料 (2).PDF", "existing second");
    assert(std::filesystem::create_directory(root / L"資料 (3).PDF", ec) && !ec);
    assert(workspace_import::ChooseNewDestination(requested, &chosen, &error));
    assert(chosen.filename() == L"資料 (4).PDF" && chosen.extension() == requested.extension());
    const auto temporary = root / L"completed.tmp";
    Write(temporary, "copied input");
    // Reproduce a check/use race: the chosen name appears before final commit.
    Write(chosen, "concurrent writer");
    assert(!workspace_import::CommitNewFile(temporary, chosen, &error));
    assert(Read(chosen) == "concurrent writer" && Read(temporary) == "copied input");
    assert(Read(requested) == "existing original" && Read(root / L"資料 (2).PDF") == "existing second");
    assert(workspace_import::ChooseNewDestination(requested, &chosen, &error));
    assert(chosen.filename() == L"資料 (5).PDF");
    assert(workspace_import::CommitNewFile(temporary, chosen, &error) && error == ERROR_SUCCESS);
    assert(Read(chosen) == "copied input" && !std::filesystem::exists(temporary, ec) && !ec);
    Write(temporary, "recoverable copy");
    const auto other = root / L"other";
    assert(std::filesystem::create_directory(other, ec) && !ec);
    assert(!workspace_import::CommitNewFile(temporary, other / L"different.docx", &error));
    assert(error == ERROR_INVALID_NAME && Read(temporary) == "recoverable copy");
    assert(!workspace_import::ChooseNewDestination(L"relative.docx", &chosen, &error));
    assert(!workspace_import::ChooseNewDestination(L"\\\\server\\share\\remote.docx", &chosen, &error));
    assert(!workspace_import::ChooseNewDestination(root / (std::wstring(255, L'a') + L".docx"), &chosen, &error));
    assert(error == ERROR_FILENAME_EXCED_RANGE);
    Write(root / L"companion.clrop", "existing annotations");
    assert(workspace_import::ChooseNewDestination(root / L"companion.pdf", &chosen, &error, true));
    assert(chosen.filename() == L"companion (2).pdf");
    Write(root / L"standalone.pdf", "existing PDF");
    assert(workspace_import::ChooseNewDestination(root / L"standalone.clrop", &chosen, &error, true));
    assert(chosen.filename() == L"standalone (2).clrop");
    std::cout << "Import new-file tests passed: extension, directory collision, race protection, retained recovery, invalid inputs.\n";
}
