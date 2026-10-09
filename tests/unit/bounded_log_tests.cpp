#include "diagnostics/bounded_log.h"
#include "core/path_safety.h"
#include "core/atomic_write.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>

using diagnostic_log::Result;
namespace {
void Seed(const std::filesystem::path& path, size_t bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec); assert(!ec);
    std::ofstream out(path, std::ios::binary); out << std::string(bytes, 'x');
    out.close(); assert(out);
}
std::filesystem::path Logs(const std::filesystem::path& root) {
    return root / L"__pdf_note_workspace__" / L"__log__";
}
HANDLE Child(const std::filesystem::path& exe, const wchar_t* mode, const std::filesystem::path& root) {
    std::wstring command = L"\"" + exe.wstring() + L"\" " + mode + L" \"" + root.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    assert(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, root.c_str(), &startup, &process));
    CloseHandle(process.hThread); return process.hProcess;
}
void Wait(HANDLE child) {
    assert(WaitForSingleObject(child, 30000) == WAIT_OBJECT_0);
    DWORD code = 99; assert(GetExitCodeProcess(child, &code) && code == 0); CloseHandle(child);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3) {
        const std::filesystem::path root(argv[2]);
        if (std::wstring(argv[1]) == L"stopped") {
            assert(diagnostic_log::Inspect(root).stopped);
            assert(diagnostic_log::Append(root, L"office_conversion.log", "child\n") == Result::LimitReached);
        } else {
            for (int i = 0; i < 250; ++i) {
                const auto result = diagnostic_log::Append(root, L"office_conversion.log", std::string(32768, 'o'));
                assert(result == Result::Written || result == Result::LimitReached || result == Result::Unavailable);
            }
        }
        return 0;
    }
    std::error_code ec;
    const auto current = std::filesystem::current_path(ec); assert(!ec);
    const auto fixture = current / L"out" / L"tests" /
        (L"bounded_log_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(fixture, ec); assert(!ec);
    const auto root = fixture / L"workspace"; assert(std::filesystem::create_directory(root, ec) && !ec);
    auto status = diagnostic_log::Inspect(root);
    assert(status.available && status.bytes == 0 && !status.stopped);
    assert(!PathExistsWin32(Logs(root)));
    assert(diagnostic_log::Append({}, L"crash.log", "x") == Result::Unavailable);
    assert(diagnostic_log::Append(L"relative", L"crash.log", "x") == Result::Unavailable);
    assert(diagnostic_log::Append(root / L".." / L"outside", L"crash.log", "x") == Result::Unavailable);
    assert(diagnostic_log::Append(root, L"../user.pdf", "x") == Result::Unavailable);
    assert(diagnostic_log::Append(fixture / L"missing", L"crash.log", "x") == Result::Unavailable);
    assert(!PathExistsWin32(Logs(root)));
    assert(diagnostic_log::Append(root, L"crash.log", "crash\n") == Result::Written);
    assert(diagnostic_log::Append(root, L"office_conversion.log", "office\n") == Result::Written);
    assert(diagnostic_log::Inspect(root).bytes == 13);
    assert(SetFileAttributesW((Logs(root) / L"crash.log").c_str(), FILE_ATTRIBUTE_READONLY));
    assert(diagnostic_log::Append(root, L"crash.log", "readonly") == Result::Unavailable);
    assert(SetFileAttributesW((Logs(root) / L"crash.log").c_str(), FILE_ATTRIBUTE_NORMAL));
    assert(diagnostic_log::Inspect(root).bytes == 13);
    // A diagnostic read/write contention must neither append nor delete bytes.
    HANDLE busy = CreateFileW((Logs(root) / L"crash.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(busy != INVALID_HANDLE_VALUE);
    assert(diagnostic_log::Append(root, L"office_conversion.log", "blocked\n") == Result::Unavailable);
    assert(!diagnostic_log::Inspect(root).available); CloseHandle(busy);
    assert(diagnostic_log::Inspect(root).bytes == 13);
    // Even a diagnostic hardlink must not alter an unrelated original.
    const auto original = fixture / L"original.txt"; Seed(original, 7);
    assert(CreateHardLinkW((Logs(root) / L"preview_trace.log").c_str(), original.c_str(), nullptr));
    assert(diagnostic_log::Append(root, L"preview_trace.log", "bad") == Result::Unavailable);
    assert(std::filesystem::file_size(original, ec) == 7 && !ec);
    assert(DeleteFileW((Logs(root) / L"preview_trace.log").c_str()));
    // The aggregate cap includes Office logs, disabled log kinds, and UTF-8 bytes.
    Seed(Logs(root) / L"crash.log", diagnostic_log::kMaxBytes - 512 - 7);
    assert(diagnostic_log::Append(root, L"preview_trace.log", std::string(400, 'p')) == Result::Written);
    assert(diagnostic_log::Append(root, L"switch_timing.log", std::string(512, 's')) == Result::LimitReached);
    status = diagnostic_log::Inspect(root); assert(status.available && status.stopped && status.bytes <= diagnostic_log::kMaxBytes);
    const auto stoppedBytes = status.bytes;
    assert(diagnostic_log::Append(root, L"startup_watchdog.log", "next\n") == Result::LimitReached);
    assert(diagnostic_log::Inspect(root).bytes == stoppedBytes);
    const auto independent = fixture / L"independent";
    assert(std::filesystem::create_directory(independent, ec) && !ec);
    assert(diagnostic_log::Append(independent, L"crash.log", "independent\n") == Result::Written);
    std::vector<wchar_t> exe(32768); assert(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    Wait(Child(exe.data(), L"stopped", root)); // Stop survives a new process.
    // Explicitly deleting the diagnostic files permits resume; no automatic deletion.
    for (const auto* name : diagnostic_log::kFileNames) {
        if (PathExistsWin32(Logs(root) / name)) assert(DeleteFileW((Logs(root) / name).c_str()));
    }
    assert(diagnostic_log::Append(root, L"crash.log", "resume\n") == Result::Written);
    Seed(Logs(root) / L"crash.log", diagnostic_log::kMaxBytes + 1);
    assert(diagnostic_log::Append(root, L"crash.log", "oversize") == Result::LimitReached);
    assert(std::filesystem::file_size(Logs(root) / L"crash.log", ec) == diagnostic_log::kMaxBytes + 1 && !ec);
    const auto concurrent = fixture / L"concurrent"; assert(std::filesystem::create_directory(concurrent, ec) && !ec);
    Seed(Logs(concurrent) / L"crash.log", diagnostic_log::kMaxBytes - 1024 * 1024);
    HANDLE first = Child(exe.data(), L"writer", concurrent);
    HANDLE second = Child(exe.data(), L"writer", concurrent);
    for (int i = 0; i < 250; ++i) {
        const auto result = diagnostic_log::Append(concurrent, L"preview_trace.log", std::string(32768, 'p'));
        assert(result == Result::Written || result == Result::LimitReached || result == Result::Unavailable);
    }
    Wait(first); Wait(second);
    status = diagnostic_log::Inspect(concurrent);
    assert(status.available && status.bytes <= diagnostic_log::kMaxBytes &&
        status.bytes > diagnostic_log::kMaxBytes - 1024 * 1024);
    // Non-blocking diagnostics can drop contended attempts. After all writers
    // finish, deterministically exhaust the remaining allowance as well.
    assert(diagnostic_log::Append(concurrent, L"office_conversion.log", std::string(1024 * 1024, 'o')) == Result::LimitReached);
    status = diagnostic_log::Inspect(concurrent);
    assert(status.available && status.stopped && status.bytes <= diagnostic_log::kMaxBytes);
    // Fail closed on a local directory junction (no symlink privileges required).
    const auto redirected = fixture / L"redirected"; const auto outside = fixture / L"outside";
    assert(std::filesystem::create_directory(redirected, ec) && !ec);
    assert(std::filesystem::create_directory(outside, ec) && !ec);
    std::wstring junction = L"cmd.exe /c mklink /J \"" + (redirected / L"__pdf_note_workspace__").wstring() +
        L"\" \"" + outside.wstring() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    assert(CreateProcessW(nullptr, junction.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        fixture.c_str(), &si, &pi)); CloseHandle(pi.hThread); Wait(pi.hProcess);
    assert(diagnostic_log::Append(redirected, L"crash.log", "bad") == Result::Unavailable);
    assert(std::filesystem::is_empty(outside, ec) && !ec);
    // Every file API uses extended paths; Unicode/500-character workspaces
    // must not silently fall back to a shorter destination.
    auto deep = fixture / L"日本語 workspace";
    while (deep.wstring().size() < 500) deep /= L"long_component_012345678901234567890123456789";
    assert(atomic_write::EnsureDirectoryExists(deep));
    assert(diagnostic_log::Append(deep, L"crash.log", "unicode: \xE6\x97\xA5\n") == Result::Written);
    status = diagnostic_log::Inspect(deep);
    assert(status.available && status.bytes == 13 && !status.stopped);
    // Fixtures remain under ignored out/tests for inspection.
    std::cout << "bounded diagnostic logs: limits, restart, concurrent processes, I/O and path safety passed\n";
    return 0;
}
