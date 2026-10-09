#pragma once
#include "core/path_safety.h"
#include <windows.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <cwchar>

namespace office::work_paths {
inline constexpr wchar_t kProfileDirectoryName[] = L"profile";
// Keep the selected/display path unchanged; only filesystem calls receive the
// extended local form, independently of the system LongPathsEnabled setting.
[[nodiscard]] inline std::filesystem::path IoPath(const std::filesystem::path& path) {
    return std::filesystem::path(ToExtendedWin32PathIfAbsoluteLocal(path));
}
[[nodiscard]] inline std::filesystem::path LocalPath(const std::filesystem::path& path) {
    const auto text = path.wstring();
    if (text.size() >= 7 && text.rfind(L"\\\\?\\", 0) == 0 && text[5] == L':')
        return std::filesystem::path(text.substr(4));
    return path;
}
// Existing Office-owned paths only. MinGW weakly_canonical cannot resolve the
// extended namespace reliably; use a read-only handle, preserving reparse checks
// at the caller, and fail closed instead of substituting a lexical path.
[[nodiscard]] inline std::filesystem::path ExistingCanonicalPath(const std::filesystem::path& path,
                                                               std::error_code& error) {
    error.clear();
    const auto local = LocalPath(path);
    if (!local.is_absolute() || IsUncPath(local)) {
        error = std::error_code(ERROR_INVALID_NAME, std::system_category()); return {};
    }
    HANDLE handle = CreateFileW(IoPath(local).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = std::error_code(GetLastError(), std::system_category()); return {};
    }
    struct CloseHandleGuard { HANDLE value; ~CloseHandleGuard() { CloseHandle(value); } } guard{handle};
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!required || required > 32768) {
        error = std::error_code(required ? ERROR_FILENAME_EXCED_RANGE : GetLastError(), std::system_category()); return {};
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= buffer.size()) {
        error = std::error_code(length ? ERROR_INSUFFICIENT_BUFFER : GetLastError(), std::system_category()); return {};
    }
    return LocalPath(std::filesystem::path(std::wstring(buffer.data(), length)));
}
struct DirectoryEntry {
    std::filesystem::path path;
    DWORD attributes = 0;
};
// Enumerate explicitly with Unicode Win32. MinGW's extended-path directory
// iterator can enumerate the process CWD instead, so it is unsuitable here.
[[nodiscard]] inline bool ReadDirectory(const std::filesystem::path& directory,
                                      std::vector<DirectoryEntry>& entries, std::error_code& error) {
    entries.clear(); error.clear();
    const auto local = LocalPath(directory);
    if (!local.is_absolute() || IsUncPath(local)) {
        error = std::error_code(ERROR_INVALID_NAME, std::system_category()); return false;
    }
    const DWORD attributes = GetFileAttributesW(IoPath(local).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = std::error_code(attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY,
                                std::system_category()); return false;
    }
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(IoPath(local / L"*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD cause = GetLastError();
        if (cause == ERROR_FILE_NOT_FOUND) return true; // confirmed existing empty directory
        error = std::error_code(cause, std::system_category()); return false;
    }
    struct FindCloseGuard { HANDLE value; ~FindCloseGuard() { FindClose(value); } } guard{search};
    do {
        if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L".."))
            entries.push_back({local / data.cFileName, data.dwFileAttributes});
    } while (FindNextFileW(search, &data));
    const DWORD cause = GetLastError();
    if (cause != ERROR_NO_MORE_FILES) {
        entries.clear(); error = std::error_code(cause, std::system_category()); return false;
    }
    return true;
}
// The UI supplies its selected workspace once per operation. Workers never
// resolve a mutable current workspace or fall back to a host temporary path.
[[nodiscard]] inline std::filesystem::path Root(const std::filesystem::path& workspace) {
    if (workspace.empty() || !workspace.is_absolute()) return {};
    return (workspace / L"__pdf_note_workspace__" / L"__tmp__" / L"lo").lexically_normal();
}
struct IgnoreCase {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};
// Explicit Unicode environment for this child only. Preserve system/toolchain
// variables, but keep its writable home, application data and temporary paths
// inside the operation's owned local directory. The parent is never changed.
[[nodiscard]] inline bool ChildEnvironment(const std::filesystem::path& local, std::vector<wchar_t>& block) {
    block.clear();
    if (local.empty() || !local.is_absolute()) { SetLastError(ERROR_INVALID_NAME); return false; }
    LPWCH raw = GetEnvironmentStringsW();
    if (!raw) return false;
    struct FreeEnvironment { LPWCH value; ~FreeEnvironment() { FreeEnvironmentStringsW(value); } } freeEnvironment{raw};
    std::map<std::wstring, std::wstring, IgnoreCase> values;
    size_t total = 0;
    for (const wchar_t* row = raw; *row; row += wcslen(row) + 1) {
        const std::wstring entry(row);
        total += entry.size() + 1;
        if (total > 1024 * 1024) { SetLastError(ERROR_BAD_ENVIRONMENT); return false; }
        const auto separator = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
        if (separator != std::wstring::npos) values[entry.substr(0, separator)] = entry.substr(separator + 1);
    }
    values[L"USERPROFILE"] = local.wstring();
    values[L"HOME"] = local.wstring();
    values[L"APPDATA"] = (local / L"appdata").wstring();
    values[L"LOCALAPPDATA"] = (local / L"localappdata").wstring();
    values[L"TEMP"] = (local / L"temp").wstring();
    values[L"TMP"] = (local / L"temp").wstring();
    values[L"PYTHONDONTWRITEBYTECODE"] = L"1";
    values[L"PYTHONPYCACHEPREFIX"] = (local / L"pycache").wstring();
    for (const auto& [key, value] : values) {
        const auto row = key + L"=" + value;
        block.insert(block.end(), row.begin(), row.end()); block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return true;
}
}
