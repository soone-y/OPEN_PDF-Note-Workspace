// file: core/path_safety.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#include <algorithm>

// Path safety helpers (no-network, no reparse traversal)

inline bool IsUncPathString(const std::wstring& path) {
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0 || path.rfind(L"\\\\?\\unc\\", 0) == 0) return true;
        if (path.rfind(L"\\\\?\\", 0) == 0) return false;
        if (path.rfind(L"\\\\.\\", 0) == 0) return false;
        return true;
    }
    return false;
}

inline bool IsUncPath(const std::filesystem::path& p) {
    return IsUncPathString(p.wstring());
}

inline bool TryIsReparsePointNoFollow(const std::filesystem::path& p, bool& outIsReparse) {
    outIsReparse = false;
    if (p.empty()) return false;
    std::wstring s = p.wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.rfind(L"\\\\?\\", 0) != 0) {
        if (s.size() >= 2 && s[1] == L':') {
            s = L"\\\\?\\" + s;
        } else if (s.rfind(L"\\\\", 0) == 0) {
            s = L"\\\\?\\UNC\\" + s.substr(2);
        }
    }
    DWORD attrs = GetFileAttributesW(s.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    outIsReparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

inline std::wstring ToExtendedWin32PathIfAbsoluteLocal(const std::filesystem::path& p) {
    std::wstring s = p.wstring();
    if (s.empty()) return s;
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.rfind(L"\\\\?\\", 0) == 0) return s;
    if (s.size() >= 2 && s[1] == L':') {
        return L"\\\\?\\" + s;
    }
    if (s.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    return s;
}

inline std::int64_t FileTimeToUnixEpochMsWin32(const FILETIME& ft) {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    constexpr ULONGLONG kUnixEpochOffset100ns = 116444736000000000ULL;
    if (value.QuadPart < kUnixEpochOffset100ns) return 0;
    return static_cast<std::int64_t>((value.QuadPart - kUnixEpochOffset100ns) / 10000ULL);
}

inline bool TryGetPathAttributesWin32(const std::filesystem::path& p, DWORD* outAttrs = nullptr) {
    if (outAttrs) *outAttrs = INVALID_FILE_ATTRIBUTES;
    if (p.empty()) return false;
    std::wstring openPath = ToExtendedWin32PathIfAbsoluteLocal(p);
    DWORD attrs = GetFileAttributesW(openPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (outAttrs) *outAttrs = attrs;
    return true;
}

inline bool PathExistsWin32(const std::filesystem::path& p) {
    return TryGetPathAttributesWin32(p, nullptr);
}

inline bool DirectoryExistsWin32(const std::filesystem::path& p) {
    DWORD attrs = INVALID_FILE_ATTRIBUTES;
    return TryGetPathAttributesWin32(p, &attrs) && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool RegularFileExistsWin32(const std::filesystem::path& p) {
    DWORD attrs = INVALID_FILE_ATTRIBUTES;
    return TryGetPathAttributesWin32(p, &attrs) && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool TryGetFileMetaWin32(const std::filesystem::path& path,
                                std::uint64_t* outSize,
                                std::int64_t* outMtimeMs) {
    if (outSize) *outSize = 0;
    if (outMtimeMs) *outMtimeMs = 0;
    if (path.empty()) return false;

    std::wstring openPath = ToExtendedWin32PathIfAbsoluteLocal(path);
    HANDLE h = CreateFileW(openPath.c_str(),
                           FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    const bool ok = GetFileInformationByHandle(h, &info) && GetFileSizeEx(h, &size) && size.QuadPart >= 0;
    CloseHandle(h);
    if (!ok) return false;

    if (outSize) *outSize = static_cast<std::uint64_t>(size.QuadPart);
    if (outMtimeMs) *outMtimeMs = FileTimeToUnixEpochMsWin32(info.ftLastWriteTime);
    return true;
}

inline bool ReadFileBytesWin32(const std::filesystem::path& path, std::string& out) {
    out.clear();
    if (path.empty()) return false;

    std::wstring openPath = ToExtendedWin32PathIfAbsoluteLocal(path);
    HANDLE h = CreateFileW(openPath.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    const auto maxReadableSize = static_cast<unsigned long long>(std::numeric_limits<size_t>::max());
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maxReadableSize) {
        CloseHandle(h);
        return false;
    }

    std::vector<char> buffer(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < buffer.size()) {
        const size_t remaining = buffer.size() - total;
        const DWORD chunkSize =
            static_cast<DWORD>(std::min<size_t>(remaining, static_cast<size_t>(1 << 20)));
        DWORD read = 0;
        if (!ReadFile(h, buffer.data() + total, chunkSize, &read, nullptr) || read == 0) {
            out.clear();
            CloseHandle(h);
            return false;
        }
        total += static_cast<size_t>(read);
    }

    CloseHandle(h);
    out.assign(buffer.begin(), buffer.end());
    return true;
}

inline std::wstring NormalizePathKey(const std::wstring& path) {
    if (path.empty()) return L"";
    std::filesystem::path p(path);
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(p, ec);
    std::wstring key = ec ? p.wstring() : canon.wstring();
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    return key;
}
