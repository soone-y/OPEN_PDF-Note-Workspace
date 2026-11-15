#include "hash.h"
#include "core/file_sha256.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

#include "core/app_core.h"
#include "fpdfview.h"

namespace clrop {

namespace {

struct PdfIdCacheEntry {
    std::wstring key;
    std::uint64_t size = 0;
    std::int64_t mtimeMs = 0;
    clrop::PdfId id;
    bool hasStrongHash = false;
};

std::mutex g_pdfIdCacheMutex;
std::vector<PdfIdCacheEntry> g_pdfIdCache;
constexpr size_t kPdfIdCacheLimit = 64;

std::wstring NormalizePathKey(const std::wstring& path) {
    if (path.empty()) return {};
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    std::wstring s = (ec || abs.empty()) ? p.wstring() : abs.wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return s;
}

bool TryGetFileMeta(const std::wstring& path, std::uint64_t* outSize, std::int64_t* outMtimeMs) {
    return TryGetFileMetaWin32(std::filesystem::path(path), outSize, outMtimeMs);
}

bool TryGetCachedPdfId(const std::wstring& key,
                       std::uint64_t size,
                       std::int64_t mtimeMs,
                       bool requireStrongHash,
                       clrop::PdfId* out) {
    std::lock_guard<std::mutex> lock(g_pdfIdCacheMutex);
    for (size_t i = 0; i < g_pdfIdCache.size(); ++i) {
        const auto& entry = g_pdfIdCache[i];
        if (entry.key != key || entry.size != size || entry.mtimeMs != mtimeMs) continue;
        if (requireStrongHash && !entry.hasStrongHash) return false;
        if (out) *out = entry.id;
        if (i + 1 < g_pdfIdCache.size()) {
            PdfIdCacheEntry hot = entry;
            g_pdfIdCache.erase(g_pdfIdCache.begin() + static_cast<std::ptrdiff_t>(i));
            g_pdfIdCache.push_back(std::move(hot));
        }
        return true;
    }
    return false;
}

void StoreCachedPdfId(const std::wstring& key,
                      std::uint64_t size,
                      std::int64_t mtimeMs,
                      const clrop::PdfId& id,
                      bool hasStrongHash) {
    std::lock_guard<std::mutex> lock(g_pdfIdCacheMutex);

    for (size_t i = 0; i < g_pdfIdCache.size(); ++i) {
        auto& entry = g_pdfIdCache[i];
        if (entry.key != key || entry.size != size || entry.mtimeMs != mtimeMs) continue;
        PdfId merged = id;
        if (!hasStrongHash && entry.hasStrongHash && merged.sha256.empty()) {
            merged.sha256 = entry.id.sha256;
        }
        entry.id = std::move(merged);
        entry.hasStrongHash = entry.hasStrongHash || hasStrongHash;
        if (i + 1 < g_pdfIdCache.size()) {
            PdfIdCacheEntry hot = std::move(entry);
            g_pdfIdCache.erase(g_pdfIdCache.begin() + static_cast<std::ptrdiff_t>(i));
            g_pdfIdCache.push_back(std::move(hot));
        }
        return;
    }

    PdfIdCacheEntry entry;
    entry.key = key;
    entry.size = size;
    entry.mtimeMs = mtimeMs;
    entry.id = id;
    entry.hasStrongHash = hasStrongHash;
    g_pdfIdCache.push_back(std::move(entry));
    if (g_pdfIdCache.size() > kPdfIdCacheLimit) {
        g_pdfIdCache.erase(g_pdfIdCache.begin());
    }
}

std::wstring ToExtendedPathIfAbsolute(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return std::wstring(L"\\\\?\\UNC\\") + path.substr(2);
    }
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return std::wstring(L"\\\\?\\") + path;
    }
    return path;
}

int GetPdfBlockFromHandle(void* param,
                          unsigned long position,
                          unsigned char* outBuffer,
                          unsigned long size) {
    if (!param || !outBuffer || size == 0) return 0;
    HANDLE h = reinterpret_cast<HANDLE>(param);
    if (!h || h == INVALID_HANDLE_VALUE) return 0;

    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(position);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(position) >> 32);
    DWORD read = 0;
    if (ReadFile(h, outBuffer, size, &read, &ov)) {
        return (read == size) ? 1 : 0;
    }
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) {
        if (GetOverlappedResult(h, &ov, &read, TRUE)) {
            return (read == size) ? 1 : 0;
        }
    }
    return 0;
}

bool TryPopulatePdfFastFingerprint(const std::wstring& pdfPath, clrop::PdfId* out) {
    if (!out || pdfPath.empty()) return false;
    out->path = pdfPath;

    std::wstring openPath = ToExtendedPathIfAbsolute(pdfPath);
    HANDLE hFile = CreateFileW(openPath.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
                               nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (GetFileSizeEx(hFile, &size) && size.QuadPart >= 0) {
        out->size = static_cast<std::uint64_t>(size.QuadPart);
    }
    if (size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<unsigned long>::max())) {
        CloseHandle(hFile);
        return false;
    }

    FPDF_FILEACCESS access{};
    access.m_FileLen = static_cast<unsigned long>(size.QuadPart);
    access.m_GetBlock = GetPdfBlockFromHandle;
    access.m_Param = reinterpret_cast<void*>(hFile);

    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&access, nullptr);
    if (!doc) {
        CloseHandle(hFile);
        return false;
    }

    const int pageCount = FPDF_GetPageCount(doc);
    out->pageCount = pageCount;
    out->pageSizesPt.clear();
    if (pageCount > 0) {
        out->pageSizesPt.reserve(static_cast<size_t>(pageCount));
        for (int i = 0; i < pageCount; ++i) {
            double widthPt = 0.0;
            double heightPt = 0.0;
            if (!FPDF_GetPageSizeByIndex(doc, i, &widthPt, &heightPt)) {
                out->pageSizesPt.clear();
                break;
            }
            out->pageSizesPt.push_back({widthPt, heightPt});
        }
    }

    FPDF_CloseDocument(doc);
    CloseHandle(hFile);
    return true;
}

} // namespace

bool ComputeFileSha256(const std::filesystem::path& path, std::string* outSha256) {
    return core_hash::FileSha256(path, outSha256);
}

void InvalidatePdfIdCache(const std::wstring& pdfPath) {
    const auto key = NormalizePathKey(pdfPath);
    std::lock_guard<std::mutex> lock(g_pdfIdCacheMutex);
    g_pdfIdCache.erase(std::remove_if(g_pdfIdCache.begin(), g_pdfIdCache.end(),
        [&](const PdfIdCacheEntry& entry) { return entry.key == key; }), g_pdfIdCache.end());
}

clrop::PdfId ComputePdfFastId(const std::wstring& pdfPath) {
    clrop::PdfId id;
    id.path = pdfPath;

    std::uint64_t size = 0;
    std::int64_t mtimeMs = 0;
    const bool haveMeta = TryGetFileMeta(pdfPath, &size, &mtimeMs);
    if (haveMeta) {
        id.size = size;
        const std::wstring key = NormalizePathKey(pdfPath);
        clrop::PdfId cached;
        if (!key.empty() && TryGetCachedPdfId(key, size, mtimeMs, /*requireStrongHash=*/false, &cached)) {
            cached.path = pdfPath;
            return cached;
        }
    }

    (void)TryPopulatePdfFastFingerprint(pdfPath, &id);

    if (haveMeta) {
        StoreCachedPdfId(NormalizePathKey(pdfPath), size, mtimeMs, id, /*hasStrongHash=*/false);
    }
    return id;
}

clrop::PdfId ComputePdfId(const std::wstring& pdfPath) {
    clrop::PdfId id;
    id.path = pdfPath;

    std::uint64_t size = 0;
    std::int64_t mtimeMs = 0;
    const bool haveMeta = TryGetFileMeta(pdfPath, &size, &mtimeMs);
    if (haveMeta) {
        id.size = size;
        const std::wstring key = NormalizePathKey(pdfPath);
        clrop::PdfId cached;
        if (!key.empty() && TryGetCachedPdfId(key, size, mtimeMs, /*requireStrongHash=*/true, &cached)) {
            cached.path = pdfPath;
            return cached;
        }
    }

    id = ComputePdfFastId(pdfPath);
    id.path = pdfPath;
    (void)ComputeFileSha256(std::filesystem::path(pdfPath), &id.sha256);

    if (haveMeta) {
        StoreCachedPdfId(NormalizePathKey(pdfPath), size, mtimeMs, id, /*hasStrongHash=*/!id.sha256.empty());
    }
    return id;
}

} // namespace clrop
