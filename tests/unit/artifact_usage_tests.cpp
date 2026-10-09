#include "diagnostics/artifact_usage.h"
#include "core/atomic_write.h"
#include <sddl.h>
#include <cassert>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using artifact_usage::Area;
using artifact_usage::State;
static const artifact_usage::Totals& At(const artifact_usage::Result& result, Area area) {
    return result.areas[static_cast<size_t>(area)];
}
static void Seed(const fs::path& path, size_t bytes) {
    assert(atomic_write::EnsureDirectoryExists(path.parent_path()));
    const HANDLE file = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(file != INVALID_HANDLE_VALUE);
    const std::string data(bytes, 'x'); DWORD written = 0;
    assert(WriteFile(file, data.data(), static_cast<DWORD>(bytes), &written, nullptr) && written == bytes);
    assert(CloseHandle(file));
}
static void Junction(const fs::path& path, const fs::path& target, const fs::path& cwd) {
    std::wstring command = L"cmd.exe /c mklink /J \"" + path.wstring() + L"\" \"" + target.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    assert(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, cwd.c_str(), &startup, &process));
    CloseHandle(process.hThread);
    assert(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 99; assert(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    CloseHandle(process.hProcess);
}
class DenyRead {
    std::wstring path;
    std::vector<unsigned char> original;
public:
    explicit DenyRead(const fs::path& directory) : path(ToExtendedWin32PathIfAbsoluteLocal(directory)) {
        DWORD size = 0;
        GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size); assert(size);
        original.resize(size);
        assert(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, original.data(), size, &size));
        PSECURITY_DESCRIPTOR denied = nullptr;
        assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;OICI;GR;;;WD)(A;OICI;GA;;;WD)",
            SDDL_REVISION_1, &denied, nullptr));
        const bool applied = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, denied);
        LocalFree(denied); assert(applied);
    }
    ~DenyRead() { assert(SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, original.data())); }
};
int main() {
    const auto fixture = fs::current_path() / L"out/tests" /
        (L"artifact_usage_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    const auto root = fixture / L"workspace";
    assert(atomic_write::EnsureDirectoryExists(root));
    std::atomic_bool cancel{false};
    auto result = artifact_usage::Scan(root, cancel);
    assert(At(result, Area::All).state == State::Missing && result.checkedAt);
    assert(!PathExistsWin32(root / L"__pdf_note_workspace__"));
    for (const auto& invalid : {fs::path{}, fs::path(L"relative"), root / L"..", fixture / L"missing"})
        assert(At(artifact_usage::Scan(invalid, cancel), Area::All).state == State::Unavailable);
    const auto managed = root / L"__pdf_note_workspace__";
    Seed(root / L"original.pdf", 9000);
    result = artifact_usage::Scan(root / L"original.pdf", cancel);
    assert(At(result, Area::All).state == State::Unavailable && At(result, Area::All).error == ERROR_DIRECTORY);
    Seed(managed / L"__log__/crash.log", 11);
    Seed(managed / L"__tmp__/lo/a.bin", 20);
    Seed(managed / L"__escape__/stage.bin", 30);
    Seed(managed / L"__escape__/backup/a.pdf", 40);
    Seed(managed / L"__escape__/operation_transactions/a.bin", 50);
    Seed(managed / L"__memo__/backups/a.txt", 60);
    Seed(managed / L"other/settings.json", 70);
    // A locked document body remains countable: only directory metadata is read.
    const HANDLE locked = CreateFileW((managed / L"__escape__/backup/a.pdf").c_str(), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(SetFileAttributesW((managed / L"__log__/crash.log").c_str(), FILE_ATTRIBUTE_READONLY));
    result = artifact_usage::Scan(root, cancel);
    assert(At(result, Area::All).state == State::Complete && At(result, Area::All).bytes == 281 && At(result, Area::All).files == 7);
    assert(At(result, Area::Logs).bytes == 11 && At(result, Area::Temporary).bytes == 20);
    assert(At(result, Area::Recovery).bytes == 120 && At(result, Area::Recovery).files == 3);
    assert(At(result, Area::SaveBackups).bytes == 40 && At(result, Area::Operations).bytes == 50 && At(result, Area::MemoBackups).bytes == 60);
    CloseHandle(locked);
    std::string content;
    assert(ReadFileBytesWin32(root / L"original.pdf", content) && content == std::string(9000, 'x'));
    assert(ReadFileBytesWin32(managed / L"__log__/crash.log", content) && content == std::string(11, 'x'));
    assert(CreateHardLinkW((managed / L"__log__/hardlink.log").c_str(), (managed / L"__log__/crash.log").c_str(), nullptr));
    result = artifact_usage::Scan(root, cancel);
    assert(At(result, Area::Logs).bytes == 22 && At(result, Area::Logs).files == 2); // Per-name logical sizes.
    cancel = true; result = artifact_usage::Scan(root, cancel);
    for (const auto& area : result.areas) assert(area.state == State::Canceled && area.bytes == 0);
    cancel = false;
    for (const auto limits : {artifact_usage::Limits{1, 64, 5000}, artifact_usage::Limits{100000, 0, 5000},
                             artifact_usage::Limits{100000, 64, 0}}) {
        result = artifact_usage::Scan(root, cancel, limits);
        for (const auto& area : result.areas) assert(area.state == State::Limited && area.bytes <= 292);
    }
    {
        DenyRead denied(managed / L"__tmp__"); result = artifact_usage::Scan(root, cancel);
        assert(At(result, Area::All).state == State::Partial && At(result, Area::All).bytes == 272);
        assert(At(result, Area::Temporary).state == State::Partial && At(result, Area::Temporary).error == ERROR_ACCESS_DENIED);
        assert(At(result, Area::Logs).state == State::Complete && At(result, Area::Recovery).state == State::Complete);
    }
    const auto outside = fixture / L"outside"; Seed(outside / L"untouched.pdf", 7000);
    Junction(managed / L"__log__/redirect", outside, fixture);
    result = artifact_usage::Scan(root, cancel);
    assert(At(result, Area::All).state == State::Partial && At(result, Area::All).bytes == 292);
    assert(At(result, Area::Logs).state == State::Partial && At(result, Area::Logs).skipped == 1);
    assert(At(result, Area::Temporary).state == State::Complete);
    const auto redirected = fixture / L"redirected"; assert(atomic_write::EnsureDirectoryExists(redirected));
    Junction(redirected / L"__pdf_note_workspace__", outside, fixture);
    assert(At(artifact_usage::Scan(redirected, cancel), Area::All).state == State::Unavailable);
    Junction(fixture / L"ancestor", root, fixture);
    assert(At(artifact_usage::Scan(fixture / L"ancestor", cancel), Area::All).state == State::Unavailable);
    assert(ReadFileBytesWin32(outside / L"untouched.pdf", content) && content == std::string(7000, 'x'));
    auto deep = fixture / L"日本語 workspace";
    while (deep.wstring().size() < 500) deep /= L"long_component_012345678901234567890123456789";
    Seed(deep / L"__pdf_note_workspace__/__tmp__/a.bin", 123);
    result = artifact_usage::Scan(deep, cancel);
    assert(At(result, Area::All).state == State::Complete && At(result, Area::Temporary).bytes == 123);
    std::cout << "artifact usage: breakdown, read-only, limits, cancellation, ACL, junction and long-path checks passed\n";
}
