#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace atomic_write {

inline std::filesystem::path MakeUniqueDestInDir(const std::filesystem::path& dir,
                                                 const std::filesystem::path& baseName) {
    std::filesystem::path base = baseName.filename();
    if (base.empty()) base = L"file";
    std::filesystem::path cand = dir / base;
    std::error_code ec;
    if (!std::filesystem::exists(cand, ec)) return cand;
    DWORD pid = GetCurrentProcessId();
    ULONGLONG tick = GetTickCount64();
    std::wstring dirStr = dir.wstring();
    for (int i = 0; i < 64; ++i) {
        std::wstring suffix = L".dup." + std::to_wstring(pid) + L"." +
                              std::to_wstring(static_cast<unsigned long long>(tick)) + L"." + std::to_wstring(i);
        std::wstring baseStr = base.wstring();
        size_t maxAllowedBaseLen = (dirStr.length() + suffix.length() + 1 < 250)
                                 ? 250 - (dirStr.length() + suffix.length() + 1) : 0;
        if (baseStr.length() > maxAllowedBaseLen && maxAllowedBaseLen > 0) {
            baseStr = baseStr.substr(0, maxAllowedBaseLen);
        }
        cand = dir / (baseStr + suffix);
        ec.clear();
        if (!std::filesystem::exists(cand, ec)) return cand;
    }
    std::wstring suffix = L".dup." + std::to_wstring(static_cast<unsigned long long>(tick));
    std::wstring baseStr = base.wstring();
    size_t maxAllowedBaseLen = (dirStr.length() + suffix.length() + 1 < 250)
                             ? 250 - (dirStr.length() + suffix.length() + 1) : 0;
    if (baseStr.length() > maxAllowedBaseLen && maxAllowedBaseLen > 0) {
        baseStr = baseStr.substr(0, maxAllowedBaseLen);
    }
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
    std::wstring w = p.wstring();
    if (w.empty()) return L"";
    wchar_t root[MAX_PATH + 2]{};
    if (!GetVolumePathNameW(w.c_str(), root, static_cast<DWORD>(sizeof(root) / sizeof(root[0])))) {
        return L"";
    }
    return std::wstring(root);
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
    std::error_code ec;
    std::filesystem::create_directories(quarantineDir, ec);
    if (ec) return false;

    std::filesystem::path dest = MakeUniqueDestInDir(quarantineDir, src.filename());
    ec.clear();
    std::filesystem::rename(src, dest, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
        ec.clear();
        std::filesystem::remove(src, ec);
        // even if remove fails, the copy exists; still treat as moved for recovery.
    }
    if (outMovedPath) *outMovedPath = dest;
    return true;
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

    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    if (ec) {
        // Fallback to destination directory.
        ec.clear();
        tempDir = dest.parent_path();
        if (!tempDir.empty()) {
            std::filesystem::create_directories(tempDir, ec);
        }
    }
    if (ec) {
        if (err) *err = L"Failed to create temp directory: " + tempDir.wstring();
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    ULONGLONG tick = GetTickCount64();
    std::filesystem::path baseName = dest.filename();
    if (baseName.empty()) baseName = L"file";

    DWORD lastErr = 0;
    std::filesystem::path tmp;
    std::wstring dirStr = tempDir.wstring();
    for (int i = 0; i < 64; ++i) {
        std::wstring suffix = L".__atomic__." + std::to_wstring(pid) + L"." +
                              std::to_wstring(static_cast<unsigned long long>(tick)) + L"." + std::to_wstring(i) + L".tmp";
        std::wstring baseStr = baseName.wstring();
        size_t maxAllowedBaseLen = (dirStr.length() + suffix.length() + 1 < 250)
                                 ? 250 - (dirStr.length() + suffix.length() + 1) : 0;
        if (baseStr.length() > maxAllowedBaseLen && maxAllowedBaseLen > 0) {
            baseStr = baseStr.substr(0, maxAllowedBaseLen);
        }
        
        tmp = tempDir / (baseStr + suffix);
        std::wstring w = tmp.wstring();
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
    std::wstring wDest = dest.wstring();
    std::wstring wTmp = tmp.wstring();

    // Safety: never mutate destination attributes (e.g. clearing read-only).
    // If the destination exists and is read-only, fail and keep/quarantine the temp file.
    DWORD destAttrs = GetFileAttributesW(wDest.c_str());
    if (destAttrs != INVALID_FILE_ATTRIBUTES && (destAttrs & FILE_ATTRIBUTE_READONLY)) {
        if (err) *err = L"Destination file is read-only: " + wDest;
        std::filesystem::path moved;
        if (!QuarantineFileBestEffort(tmp, quarantineDir, &moved)) {
            std::error_code rmec;
            std::filesystem::remove(tmp, rmec);
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
            lastErr = GetLastError();
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
            std::error_code rmec;
            std::filesystem::remove(tmp, rmec);
        } else if (err && !moved.empty()) {
            *err += L"\nQuarantined temp file: " + moved.wstring();
        }
        return false;
    }
    if (err) *err = L"Failed to replace destination file: " + wDest + L" (unknown error)";
    std::filesystem::path moved;
    if (!QuarantineFileBestEffort(tmp, quarantineDir, &moved)) {
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
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
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
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
            std::error_code rmec;
            std::filesystem::remove(tmp, rmec);
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
