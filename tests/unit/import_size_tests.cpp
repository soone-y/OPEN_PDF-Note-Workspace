#include "workspace/import_size.h"
#include <array>
#include <cassert>
#include <iostream>

int main() {
    using namespace workspace_import;
    using Level = ImportSizeLevel;
    const std::array<std::pair<std::uint64_t, Level>, 10> cases{{
        {0, Level::Small}, {99999999, Level::Small}, {100000000, Level::Over100MB},
        {100000001, Level::Over100MB}, {499999999, Level::Over100MB}, {500000000, Level::Over500MB},
        {500000001, Level::Over500MB}, {999999999, Level::Over500MB}, {1000000000, Level::Over1GB},
        {1000000001, Level::Over1GB}
    }};
    for (const auto& test : cases) assert(ImportLevel(test.first) == test.second);
    ImportSize combined;
    assert(combined.Add({60000000, 1}) && combined.Add({60000000, 1}));
    assert(combined.bytes == 120000000 && combined.files == 2 && ImportLevel(combined.bytes) == Level::Over100MB);
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    ImportSize overflow{maximum - 1, 2};
    assert(!overflow.Add({2, 1}) && overflow.bytes == maximum - 1 && overflow.files == 2);
    overflow = {1, maximum};
    assert(!overflow.Add({1, 1}) && overflow.bytes == 1 && overflow.files == maximum);
    assert(ImportSizeNumber(100000000) == L"100.00");
    assert(ImportSizeNumber(500999999) == L"500.99");
    assert(ImportSizeNumber(1000000000) == L"1.00");
    assert(ImportSizeNumber(1500000000) == L"1.50");

    std::error_code error;
    const auto cwd = std::filesystem::current_path(error); assert(!error);
    const auto root = cwd / (L"size_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    assert(std::filesystem::create_directory(root, error) && !error);
    const auto file = root / L"サイズ.txt";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(handle != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    assert(WriteFile(handle, "size", 4, &written, nullptr) && written == 4);
    assert(CloseHandle(handle));
    ImportSize measured;
    assert(MeasureImportFiles({file.wstring()}, measured) && measured.bytes == 4 && measured.files == 1);
    assert(!MeasureImportFiles({file.wstring(), (root / L"missing").wstring()}, measured));
    assert(measured.bytes == 4 && measured.files == 1); // Failure cannot publish a partial sum.
    assert(!measured.AddFile(root) && measured.bytes == 4 && measured.files == 1);
    assert(!measured.AddFile(L"relative.txt"));
    std::cout << "Import size policy PASS: 10 boundary cases, aggregation, overflow, display and metadata failure preservation.\n";
}
