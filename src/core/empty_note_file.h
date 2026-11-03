#pragma once

#include "core/document_source_protection.h"

// Physical deletion boundary for an explicitly selected, saved empty CLRO.
// The UI owns the pending-edit/save check. Revalidate through the same locked
// handle used for deletion; preserve nonempty bytes and other processes' locks.
namespace empty_note_file {
enum class Result { Deleted, NotEmpty, Failed };

[[nodiscard]] inline bool EmptyBody(HANDLE file, bool* empty, DWORD* error) {
    *empty = false;
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) {
        *error = GetLastError(); return false;
    }
    if ((basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return true;
    if (standard.EndOfFile.QuadPart == 0) { *empty = true; return true; }
    if (standard.EndOfFile.QuadPart != 2 && standard.EndOfFile.QuadPart != 3) return true;
    unsigned char bytes[3]{};
    DWORD read = 0;
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) ||
        !ReadFile(file, bytes, sizeof(bytes), &read, nullptr)) {
        *error = GetLastError(); return false;
    }
    *empty = (read == 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) ||
             (read == 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                            (bytes[0] == 0xFE && bytes[1] == 0xFF)));
    return true;
}

[[nodiscard]] inline bool IsSavedEmpty(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute() || IsUncPath(path)) return false;
    const auto native = ToExtendedWin32PathIfAbsoluteLocal(path);
    HANDLE file = CreateFileW(native.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool empty = false; DWORD error = 0;
    const bool read = EmptyBody(file, &empty, &error);
    CloseHandle(file);
    return read && empty;
}

[[nodiscard]] inline Result Remove(const std::filesystem::path& path, DWORD* error) {
    *error = ERROR_SUCCESS;
    if (path.empty() || !path.is_absolute() || IsUncPath(path)) {
        *error = ERROR_BAD_PATHNAME; return Result::Failed;
    }
    document_source_protection::Mutation protection(path, path);
    if (!protection.ready()) { *error = protection.error(); return Result::Failed; }
    const auto native = ToExtendedWin32PathIfAbsoluteLocal(path);
    HANDLE file = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { *error = GetLastError(); return Result::Failed; }
    Result result = Result::Failed;
    OVERLAPPED lock{};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &lock)) {
        *error = GetLastError();
    } else {
        bool empty = false;
        if (EmptyBody(file, &empty, error)) {
            if (!empty) result = Result::NotEmpty;
            else {
                FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
                if (SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)))
                    result = Result::Deleted;
                else *error = GetLastError();
            }
        }
    }
    CloseHandle(file);
    DWORD protectionError = 0;
    if (!protection.FinishRemoval(result == Result::Deleted, &protectionError) && *error == ERROR_SUCCESS)
        *error = protectionError;
    return result;
}
} // namespace empty_note_file
