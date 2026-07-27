#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/app_core.h"

namespace {

namespace fs = std::filesystem;

int g_failed = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return;
    }
    std::cout << "[FAIL] " << message << "\n";
    ++g_failed;
}

fs::path MakeTestRoot() {
    fs::path root = fs::temp_directory_path() / L"pdf_note_path_safety_tests";
    root /= std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

bool WriteRaw(const fs::path& path, const std::string& contents) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(ofs);
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    const fs::path root = MakeTestRoot();

    Expect(IsUncPathString(L"\\\\server\\share\\doc.pdf"),
           "plain UNC path is classified as UNC");
    Expect(IsUncPathString(L"\\\\?\\UNC\\server\\share\\doc.pdf"),
           "extended UNC path is classified as UNC");
    Expect(!IsUncPathString(L"\\\\?\\C:\\local\\doc.pdf"),
           "extended local drive path is not classified as UNC");
    Expect(!IsUncPathString(L"\\\\.\\C:\\local\\doc.pdf"),
           "device-style local path is not classified as UNC");

    const fs::path localFile = root / L"日本語 path & space.txt";
    Expect(WriteRaw(localFile, "ok"), "special-character local file fixture is written");
    Expect(PathExistsWin32(localFile), "PathExistsWin32 handles special-character local paths");
    Expect(RegularFileExistsWin32(localFile), "RegularFileExistsWin32 recognizes local file");
    Expect(!DirectoryExistsWin32(localFile), "DirectoryExistsWin32 rejects local file");

    const std::wstring extended = ToExtendedWin32PathIfAbsoluteLocal(localFile);
    Expect(extended.rfind(L"\\\\?\\", 0) == 0,
           "absolute local path is converted to extended Win32 path");
    Expect(PathExistsWin32(fs::path(extended)),
           "extended Win32 path still resolves to the local file");

    bool isReparse = true;
    Expect(TryIsReparsePointNoFollow(localFile, isReparse) && !isReparse,
           "regular local file is not reported as a reparse point");

    std::uint64_t size = 0;
    std::int64_t mtimeMs = 0;
    Expect(TryGetFileMetaWin32(localFile, &size, &mtimeMs) && size == 2,
           "TryGetFileMetaWin32 reads local file metadata without following network paths");

    std::string bytes;
    Expect(ReadFileBytesWin32(localFile, bytes) && bytes == "ok",
           "ReadFileBytesWin32 reads special-character local paths");

    const fs::path missing = root / L"missing.txt";
    Expect(!PathExistsWin32(missing), "missing local path is reported absent");
    Expect(!RegularFileExistsWin32(missing), "missing local file is not reported regular");

    std::error_code ec;
    fs::remove_all(root, ec);

    std::cout << "Summary: failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
