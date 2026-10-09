#include "office/work_paths.h"
#include "office/handoff_paths.h"
#include <cassert>
#include <fstream>
#include <iostream>

static std::wstring Env(const wchar_t* key) {
    const DWORD count = GetEnvironmentVariableW(key, nullptr, 0);
    if (!count) return {};
    std::wstring value(count, L'\0');
    const DWORD read = GetEnvironmentVariableW(key, value.data(), count);
    assert(read < count); value.resize(read); return value;
}
int wmain(int argc, wchar_t** argv) {
    const wchar_t* keys[] = {L"TEMP", L"TMP", L"APPDATA", L"LOCALAPPDATA", L"USERPROFILE", L"HOME", L"SystemRoot"};
    if (argc == 3 && std::wstring(argv[1]) == L"--child") {
        std::wofstream out(std::filesystem::path(argv[2]), std::ios::binary);
        assert(out);
        // ASCII result file; verify Unicode path values in this actual child.
        const auto local = std::filesystem::path(argv[2]).parent_path();
        assert(Env(L"TEMP") == (local / L"temp").wstring());
        assert(Env(L"TMP") == (local / L"temp").wstring());
        assert(Env(L"APPDATA") == (local / L"appdata").wstring());
        assert(Env(L"LOCALAPPDATA") == (local / L"localappdata").wstring());
        assert(Env(L"USERPROFILE") == local.wstring());
        assert(Env(L"HOME") == local.wstring());
        assert(!Env(L"SystemRoot").empty());
        out << L"PASS"; out.close(); assert(out); return 0;
    }
    assert(office::work_paths::Root({}).empty());
    assert(office::work_paths::Root(L"relative").empty());
    // Account for the second command line assembled by soffice.com, including
    // its escaped OOO_CWD value. Checking only the app's command can miss this.
    const std::filesystem::path launcherDirectory = L"C:\\lo\\program";
    const size_t overhead = office::handoff_paths::ConverterCommandLineLength(L"", launcherDirectory);
    assert(overhead == 50); // 18 added quotes + 17 fixed CWD units + 15 escaped path units.
    assert(office::handoff_paths::ConverterCommandLineLength(L"\U0001f600", launcherDirectory) == overhead + 2);
    const auto boundary = office::handoff_paths::kMaximumCommandLineLength - overhead;
    assert(office::handoff_paths::ConverterCommandLineLength(std::wstring(boundary, L'a'), launcherDirectory) == 32766);
    assert(office::handoff_paths::ConverterCommandLineLength(std::wstring(boundary + 1, L'a'), launcherDirectory) == 32767);
    assert(office::handoff_paths::ConverterCommandLineLength(L"", L"C:\\lo$\\") >
           office::handoff_paths::ConverterCommandLineLength(L"", L"C:\\lo\\"));
    std::vector<std::wstring> original;
    for (auto key : keys) original.push_back(Env(key));
    std::error_code ec;
    const auto working = std::filesystem::current_path(ec); assert(!ec && working.is_absolute());
    const auto root = working / (L"office_env_" + std::to_wstring(GetCurrentProcessId()));
    const auto local = root / L"local_環境 with spaces";
    assert(std::filesystem::create_directories(local, ec) && !ec);
    const auto canonical = office::work_paths::ExistingCanonicalPath(local, ec);
    assert(!ec && !canonical.empty());
    assert(office::work_paths::ExistingCanonicalPath(office::work_paths::IoPath(local), ec) == canonical && !ec);
    assert(office::work_paths::ExistingCanonicalPath(local / L"missing", ec).empty() && ec);
    assert(office::work_paths::ExistingCanonicalPath(L"C:relative", ec).empty() && ec);
    std::vector<std::filesystem::path> deepDirectories;
    auto deep = root / L"canonical_日本語 with spaces";
    while (deep.wstring().size() < 500) {
        deepDirectories.push_back(deep);
        assert(CreateDirectoryW(office::work_paths::IoPath(deep).c_str(), nullptr));
        deep /= std::wstring(40, L'x');
    }
    deepDirectories.push_back(deep);
    assert(CreateDirectoryW(office::work_paths::IoPath(deep).c_str(), nullptr));
    const auto deepCanonical = office::work_paths::ExistingCanonicalPath(deep, ec);
    assert(!ec && !deepCanonical.empty() && deepCanonical.wstring().size() >= 500);
    assert(office::work_paths::ExistingCanonicalPath(office::work_paths::IoPath(deep), ec) == deepCanonical && !ec);
    std::vector<office::work_paths::DirectoryEntry> entries;
    assert(office::work_paths::ReadDirectory(deep, entries, ec) && !ec && entries.empty());
    const auto deepFile = deep / L"owned_一覧 with spaces.txt";
    HANDLE deepHandle = CreateFileW(office::work_paths::IoPath(deepFile).c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(deepHandle != INVALID_HANDLE_VALUE); assert(CloseHandle(deepHandle));
    assert(office::work_paths::ReadDirectory(office::work_paths::IoPath(deep), entries, ec) && !ec);
    assert(entries.size() == 1 && entries.front().path == deepFile &&
           !(entries.front().attributes & FILE_ATTRIBUTE_DIRECTORY));
    assert(!office::work_paths::ReadDirectory(deep / L"missing", entries, ec) && ec && entries.empty());
    assert(!office::work_paths::ReadDirectory(L"relative", entries, ec) && ec && entries.empty());
    assert(DeleteFileW(office::work_paths::IoPath(deepFile).c_str()));
    for (auto it = deepDirectories.rbegin(); it != deepDirectories.rend(); ++it)
        assert(RemoveDirectoryW(office::work_paths::IoPath(*it).c_str()));
    assert(office::work_paths::Root(root) == root / L"__pdf_note_workspace__" / L"__tmp__" / L"lo");
    std::vector<wchar_t> block;
    assert(!office::work_paths::ChildEnvironment(L"relative", block));
    assert(office::work_paths::ChildEnvironment(local, block));
    assert(block.size() >= 2 && block.back() == L'\0' && block[block.size() - 2] == L'\0');
    for (size_t i = 0; i < std::size(keys); ++i) assert(Env(keys[i]) == original[i]);
    std::vector<wchar_t> exe(32768);
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    assert(length && length < exe.size());
    const auto report = local / L"result.txt";
    // Both arguments are Windows file names (no quotes) ending in file names,
    // so quoting them introduces no trailing-backslash escaping ambiguity.
    const auto command = L"\"" + std::wstring(exe.data(), length) + L"\" --child \"" + report.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    assert(CreateProcessW(exe.data(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, block.data(), local.c_str(), &startup, &process));
    const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    if (waited != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, ERROR_TIMEOUT); WaitForSingleObject(process.hProcess, 5000); }
    DWORD exitCode = 1; assert(GetExitCodeProcess(process.hProcess, &exitCode));
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    assert(waited == WAIT_OBJECT_0 && exitCode == 0);
    { std::ifstream in(report); std::string result; in >> result; assert(result == "PASS"); }
    for (size_t i = 0; i < std::size(keys); ++i) assert(Env(keys[i]) == original[i]);
    assert(DeleteFileW(report.c_str())); assert(RemoveDirectoryW(local.c_str())); assert(RemoveDirectoryW(root.c_str()));
    std::cout << "office workspace paths, actual child environment and parent isolation passed\n";
    return 0;
}
