#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "path_safety.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace atomic_write {

inline bool EnsureDirectoryExists(const std::filesystem::path& dir) {
    if (dir.empty()) return true;

    std::vector<std::filesystem::path> missing;
    std::filesystem::path current = dir;
    for (;;) {
        const std::wstring openPath = ToExtendedWin32PathIfAbsoluteLocal(current);
        const DWORD attrs = GetFileAttributesW(openPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        missing.push_back(current);
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) return false;
        current = parent;
    }

    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        const std::wstring openPath = ToExtendedWin32PathIfAbsoluteLocal(*it);
        if (CreateDirectoryW(openPath.c_str(), nullptr)) continue;
        const DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS && DirectoryExistsWin32(*it)) continue;
        return false;
    }
    return true;
}

inline std::filesystem::path MakeUniqueDestInDir(const std::filesystem::path& dir,
                                                 const std::filesystem::path& baseName) {
    std::filesystem::path base = baseName.filename();
    if (base.empty()) base = L"file";
    std::filesystem::path cand = dir / base;
    if (!PathExistsWin32(cand)) return cand;
    DWORD pid = GetCurrentProcessId();
    ULONGLONG tick = GetTickCount64();
    for (int i = 0; i < 64; ++i) {
        std::wstring suffix = L".dup." + std::to_wstring(pid) + L"." +
                              std::to_wstring(static_cast<unsigned long long>(tick)) + L"." + std::to_wstring(i);
        std::wstring baseStr = base.wstring();
        // Limit only the file-name component.  Limiting the whole path here
        // used to make a valid deep destination impossible to use.
        constexpr size_t kMaxFileNameChars = 255;
        const size_t maxAllowedBaseLen = suffix.length() < kMaxFileNameChars
                                       ? kMaxFileNameChars - suffix.length() : 1;
        if (baseStr.length() > maxAllowedBaseLen) baseStr.resize(maxAllowedBaseLen);
        cand = dir / (baseStr + suffix);
        if (!PathExistsWin32(cand)) return cand;
    }
    std::wstring suffix = L".dup." + std::to_wstring(static_cast<unsigned long long>(tick));
    std::wstring baseStr = base.wstring();
    constexpr size_t kMaxFileNameChars = 255;
    const size_t maxAllowedBaseLen = suffix.length() < kMaxFileNameChars
                                   ? kMaxFileNameChars - suffix.length() : 1;
    if (baseStr.length() > maxAllowedBaseLen) baseStr.resize(maxAllowedBaseLen);
    return dir / (baseStr + suffix);
}

inline std::wstring Win32ErrorMessage(DWORD code) {
    if (code == 0) return L"";
    wchar_t* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    DWORD n = FormatMessageW(flags, nullptr, code, lang, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring out;
    if (n && buf) {
        out.assign(buf, buf + n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    }
    if (buf) LocalFree(buf);
    return out;
}

inline std::wstring VolumeRootForPath(const std::filesystem::path& p) {
    const std::wstring w = ToExtendedWin32PathIfAbsoluteLocal(p);
    if (w.empty()) return L"";
    std::vector<wchar_t> root(512, L'\0');
    for (;;) {
        if (GetVolumePathNameW(w.c_str(), root.data(), static_cast<DWORD>(root.size()))) {
            return std::wstring(root.data());
        }
        const DWORD error = GetLastError();
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) return L"";
        if (root.size() >= 32768) return L"";
        root.resize(std::min<size_t>(root.size() * 2, 32768), L'\0');
    }
}

inline bool SameVolume(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto va = VolumeRootForPath(a);
    auto vb = VolumeRootForPath(b);
    if (va.empty() || vb.empty()) return false;
    // Case-insensitive compare (drive letters).
    if (va.size() != vb.size()) return false;
    for (size_t i = 0; i < va.size(); ++i) {
        wchar_t ca = va[i];
        wchar_t cb = vb[i];
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) return false;
    }
    return true;
}

inline bool IsTransientReplaceError(DWORD e) {
    return e == ERROR_ACCESS_DENIED ||
           e == ERROR_SHARING_VIOLATION ||
           e == ERROR_LOCK_VIOLATION;
}

inline std::filesystem::path PickTempDirForTarget(const std::filesystem::path& target,
                                                  const std::filesystem::path& preferred) {
    if (!preferred.empty() && SameVolume(preferred, target)) return preferred;
    auto parent = target.parent_path();
    if (!parent.empty()) return parent;
    return preferred;
}

inline bool QuarantineFileBestEffort(const std::filesystem::path& src,
                                     const std::filesystem::path& quarantineDir,
                                     std::filesystem::path* outMovedPath) {
    if (src.empty() || quarantineDir.empty()) return false;
    if (!EnsureDirectoryExists(quarantineDir)) return false;

    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::filesystem::path dest = MakeUniqueDestInDir(quarantineDir, src.filename());
        const std::wstring sourcePath = ToExtendedWin32PathIfAbsoluteLocal(src);
        const std::wstring destinationPath = ToExtendedWin32PathIfAbsoluteLocal(dest);
        if (sourcePath.empty() || destinationPath.empty()) return false;

        // Never replace a path that appeared after MakeUniqueDestInDir checked it.
        // MoveFileExW without MOVEFILE_REPLACE_EXISTING and CopyFileW with
        // bFailIfExists=TRUE close that check/use race in both move and fallback paths.
        if (MoveFileExW(sourcePath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (outMovedPath) *outMovedPath = dest;
            return true;
        }
        DWORD moveError = GetLastError();
        if (moveError == ERROR_FILE_EXISTS || moveError == ERROR_ALREADY_EXISTS) continue;

        if (CopyFileW(sourcePath.c_str(), destinationPath.c_str(), TRUE)) {
            DeleteFileW(sourcePath.c_str());
            // Even if source cleanup fails, the recovery copy is complete.
            if (outMovedPath) *outMovedPath = dest;
            return true;
        }
        DWORD copyError = GetLastError();
        if (copyError == ERROR_FILE_EXISTS || copyError == ERROR_ALREADY_EXISTS) continue;
        return false;
    }
    return false;
}

inline bool CreateUniqueTempFile(const std::filesystem::path& dest,
                                 const std::filesystem::path& preferredTempDir,
                                 std::filesystem::path* outTmp,
                                 HANDLE* outHandle,
                                 std::wstring* err) {
    if (!outTmp) return false;
    if (!outHandle) return false;
    outTmp->clear();
    *outHandle = INVALID_HANDLE_VALUE;
    if (dest.empty()) {
        if (err) *err = L"Invalid destination path.";
        return false;
    }

    std::filesystem::path tempDir = PickTempDirForTarget(dest, preferredTempDir);
    if (tempDir.empty()) {
        if (err) *err = L"Failed to determine a temp directory.";
        return false;
    }

    if (!EnsureDirectoryExists(tempDir)) {
        // Fallback to destination directory.
        tempDir = dest.parent_path();
        if (!tempDir.empty()) {
            (void)EnsureDirectoryExists(tempDir);
        }
    }
    if (!DirectoryExistsWin32(tempDir)) {
        if (err) *err = L"Failed to create temp directory: " + tempDir.wstring();
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    ULONGLONG tick = GetTickCount64();
    std::filesystem::path baseName = dest.filename();
    if (baseName.empty()) baseName = L"file";

    DWORD lastErr = 0;
    std::filesystem::path tmp;
    for (int i = 0; i < 64; ++i) {
        std::wstring suffix = L".__atomic__." + std::to_wstring(pid) + L"." +
                              std::to_wstring(static_cast<unsigned long long>(tick)) + L"." + std::to_wstring(i) + L".tmp";
        std::wstring baseStr = baseName.wstring();
        constexpr size_t kMaxFileNameChars = 255;
        const size_t maxAllowedBaseLen = suffix.length() < kMaxFileNameChars
                                       ? kMaxFileNameChars - suffix.length() : 1;
        if (baseStr.length() > maxAllowedBaseLen) baseStr.resize(maxAllowedBaseLen);
        
        tmp = tempDir / (baseStr + suffix);
        std::wstring w = ToExtendedWin32PathIfAbsoluteLocal(tmp);
        if (w.empty()) continue;
        HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            *outTmp = tmp;
            *outHandle = h;
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) continue;
        lastErr = e;
        break; // Stop retrying for unrecoverable errors like ERROR_PATH_NOT_FOUND
    }
    if (err) {
        *err = L"Failed to create a unique temp file in: " + tempDir.wstring();
        if (lastErr) {
            *err += L" (" + std::to_wstring(lastErr) + L") " + Win32ErrorMessage(lastErr);
        }
    }
    return false;
}

inline bool AtomicReplaceFile(const std::filesystem::path& dest,
                              const std::filesystem::path& tmp,
                              const std::filesystem::path& quarantineDir,
                              std::wstring* err) {
    if (dest.empty() || tmp.empty()) {
        if (err) *err = L"Invalid path.";
        return false;
    }
    std::wstring wDest = ToExtendedWin32PathIfAbsoluteLocal(dest);
    std::wstring wTmp = ToExtendedWin32PathIfAbsoluteLocal(tmp);

    // Safety: never mutate destination attributes (e.g. clearing read-only).
    // If the destination exists and is read-only, fail and keep/quarantine the temp file.
    DWORD destAttrs = GetFileAttributesW(wDest.c_str());
    if (destAttrs != INVALID_FILE_ATTRIBUTES && (destAttrs & FILE_ATTRIBUTE_READONLY)) {
        if (err) *err = L"Destination file is read-only: " + wDest;
        std::filesystem::path moved;
        if (!QuarantineFileBestEffort(tmp, quarantineDir, &moved)) {
            DeleteFileW(ToExtendedWin32PathIfAbsoluteLocal(tmp).c_str());
        } else if (err && !moved.empty()) {
            *err += L"\nQuarantined temp file: " + moved.wstring();
        }
        return false;
    }

    DWORD lastErr = 0;
    constexpr int kMaxReplaceAttempts = 8;
    for (int attempt = 0; attempt < kMaxReplaceAttempts; ++attempt) {
        destAttrs = GetFileAttributesW(wDest.c_str());
        const bool destExists = (destAttrs != INVALID_FILE_ATTRIBUTES);

        if (destExists) {
            if (ReplaceFileW(wDest.c_str(), wTmp.c_str(),
                             /*lpBackupFileName=*/nullptr,
                             REPLACEFILE_WRITE_THROUGH,
                             /*lpExclude=*/nullptr,
                             /*lpReserved=*/nullptr)) {
                return true;
            }
            if (MoveFileExW(wTmp.c_str(), wDest.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return true;
            }
            lastErr = GetLastError();
        } else {
            if (MoveFileExW(wTmp.c_str(), wDest.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return true;
            }
            lastErr = GetLastError();
        }

        if (!IsTransientReplaceError(lastErr)) break;
        if (attempt + 1 < kMaxReplaceAttempts) {
            Sleep(static_cast<DWORD>((attempt + 1) * 20));
        }
    }
    if (lastErr != 0) {
        if (err) {
            *err = L"Failed to replace destination file: " + wDest + L" (" + std::to_wstring(lastErr) + L") " +
                   Win32ErrorMessage(lastErr);
        }
        std::filesystem::path moved;
        if (!QuarantineFileBestEffort(tmp, quarantineDir, &moved)) {
            DeleteFileW(ToExtendedWin32PathIfAbsoluteLocal(tmp).c_str());
        } else if (err && !moved.empty()) {
            *err += L"\nQuarantined temp file: " + moved.wstring();
        }
        return false;
    }
    if (err) *err = L"Failed to replace destination file: " + wDest + L" (unknown error)";
    std::filesystem::path moved;
    if (!QuarantineFileBestEffort(tmp, quarantineDir, &moved)) {
        DeleteFileW(ToExtendedWin32PathIfAbsoluteLocal(tmp).c_str());
    } else if (err && !moved.empty()) {
        *err += L"\nQuarantined temp file: " + moved.wstring();
    }
    return false;
}

inline bool WriteAllBytesWin32(HANDLE h,
                               const std::filesystem::path& path,
                               const void* data,
                               size_t size,
                               std::wstring* err) {
    std::wstring w = path.wstring();
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"Invalid file handle.";
        return false;
    }
    if (w.empty()) {
        if (err) *err = L"Invalid path.";
        CloseHandle(h);
        return false;
    }
    if (size > 0 && data == nullptr) {
        if (err) *err = L"Invalid write buffer.";
        CloseHandle(h);
        return false;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    size_t left = size;
    while (left > 0) {
        DWORD chunk = left > (1u << 20) ? (1u << 20) : static_cast<DWORD>(left);
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written != chunk) {
            if (err) {
                DWORD e = GetLastError();
                *err = L"Failed to write temp file: " + w + L" (" + std::to_wstring(e) + L") " + Win32ErrorMessage(e);
            }
            CloseHandle(h);
            return false;
        }
        p += written;
        left -= written;
    }
    // Require flush success before replacement to reduce power-loss data risk.
    if (!FlushFileBuffers(h)) {
        if (err) {
            DWORD e = GetLastError();
            *err = L"Failed to flush temp file: " + w + L" (" + std::to_wstring(e) + L") " + Win32ErrorMessage(e);
        }
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    return true;
}

inline bool AtomicWriteBytes(const std::filesystem::path& dest,
                             const void* data,
                             size_t size,
                             const std::filesystem::path& preferredTempDir,
                             const std::filesystem::path& quarantineDir,
                             std::wstring* err) {
    if (dest.empty()) {
        if (err) *err = L"Invalid destination path.";
        return false;
    }
    if (size > 0 && data == nullptr) {
        if (err) *err = L"Invalid write buffer.";
        return false;
    }
    if (!dest.parent_path().empty()) {
        if (!EnsureDirectoryExists(dest.parent_path())) {
            if (err) *err = L"Failed to create destination directory: " + dest.parent_path().wstring();
            return false;
        }
    }

    std::filesystem::path tmp;
    HANDLE tmpHandle = INVALID_HANDLE_VALUE;
    if (!CreateUniqueTempFile(dest, preferredTempDir, &tmp, &tmpHandle, err)) {
        return false;
    }

    if (!WriteAllBytesWin32(tmpHandle, tmp, data, size, err)) {
        std::filesystem::path moved;
        if (!QuarantineFileBestEffort(tmp, quarantineDir, &moved)) {
            DeleteFileW(ToExtendedWin32PathIfAbsoluteLocal(tmp).c_str());
        } else if (err && !moved.empty()) {
            *err += L"\nQuarantined temp file: " + moved.wstring();
        }
        return false;
    }

    return AtomicReplaceFile(dest, tmp, quarantineDir, err);
}

inline bool AtomicWriteBytes(const std::filesystem::path& dest,
                             const void* data,
                             size_t size,
                             const std::filesystem::path& preferredTempDir,
                             std::wstring* err) {
    return AtomicWriteBytes(dest, data, size, preferredTempDir, /*quarantineDir=*/{}, err);
}

inline bool AtomicWriteUtf8(const std::filesystem::path& dest,
                            std::string_view utf8,
                            const std::filesystem::path& preferredTempDir,
                            std::wstring* err) {
    return AtomicWriteBytes(dest, utf8.data(), utf8.size(), preferredTempDir, err);
}

inline bool AtomicWriteUtf8(const std::filesystem::path& dest,
                            std::string_view utf8,
                            const std::filesystem::path& preferredTempDir,
                            const std::filesystem::path& quarantineDir,
                            std::wstring* err) {
    return AtomicWriteBytes(dest, utf8.data(), utf8.size(), preferredTempDir, quarantineDir, err);
}

} // namespace atomic_write
