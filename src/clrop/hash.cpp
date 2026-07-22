#include "hash.h"

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

// Minimal SHA-256 implementation (public domain style)
struct Sha256Ctx {
    std::array<std::uint32_t, 8> h{};
    std::array<std::uint8_t, 64> buf{};
    std::uint64_t bits = 0;
    size_t idx = 0;
};

constexpr std::array<std::uint32_t, 64> kShaK = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

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

inline std::uint32_t RotR(std::uint32_t v, int b) { return (v >> b) | (v << (32 - b)); }
inline std::uint32_t Ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (~x & z); }
inline std::uint32_t Maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

void ShaInit(Sha256Ctx& c) {
    c.h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    c.idx = 0;
    c.bits = 0;
}

void ShaProcess(Sha256Ctx& c, const std::uint8_t* data) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
               (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(data[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t A = c.h[0], B = c.h[1], C = c.h[2], D = c.h[3];
    std::uint32_t E = c.h[4], F = c.h[5], G = c.h[6], H = c.h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
        std::uint32_t T1 = H + S1 + Ch(E, F, G) + kShaK[static_cast<size_t>(i)] + w[i];
        std::uint32_t S0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
        std::uint32_t T2 = S0 + Maj(A, B, C);
        H = G;
        G = F;
        F = E;
        E = D + T1;
        D = C;
        C = B;
        B = A;
        A = T1 + T2;
    }
    c.h[0] += A; c.h[1] += B; c.h[2] += C; c.h[3] += D;
    c.h[4] += E; c.h[5] += F; c.h[6] += G; c.h[7] += H;
}

void ShaUpdate(Sha256Ctx& c, const std::uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        c.buf[c.idx++] = data[i];
        c.bits += 8;
        if (c.idx == 64) {
            ShaProcess(c, c.buf.data());
            c.idx = 0;
        }
    }
}

void ShaFinal(Sha256Ctx& c, std::array<std::uint8_t, 32>& out) {
    c.buf[c.idx++] = 0x80;
    if (c.idx > 56) {
        while (c.idx < 64) c.buf[c.idx++] = 0;
        ShaProcess(c, c.buf.data());
        c.idx = 0;
    }
    while (c.idx < 56) c.buf[c.idx++] = 0;
    std::uint64_t bitsBE = c.bits;
    for (int i = 7; i >= 0; --i) {
        c.buf[c.idx++] = static_cast<std::uint8_t>((bitsBE >> (i * 8)) & 0xFF);
    }
    ShaProcess(c, c.buf.data());
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((c.h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((c.h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((c.h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(c.h[i] & 0xFF);
    }
}

std::string ToHex(const std::array<std::uint8_t, 32>& data) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : data) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

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

bool FillSha256(const std::wstring& pdfPath, clrop::PdfId* out) {
    if (!out) return false;
    out->sha256.clear();
    if (pdfPath.empty()) return false;

    const std::wstring openPath = ToExtendedPathIfAbsolute(pdfPath);
    HANDLE hFile = CreateFileW(openPath.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                               nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        out->sha256.clear();
        return false;
    }

    Sha256Ctx ctx;
    ShaInit(ctx);
    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            CloseHandle(hFile);
            return false;
        }
        if (read == 0) break;
        ShaUpdate(ctx, buffer.data(), static_cast<size_t>(read));
    }
    CloseHandle(hFile);

    std::array<std::uint8_t, 32> digest{};
    ShaFinal(ctx, digest);
    out->sha256 = ToHex(digest);
    return true;
}

bool NearlyEqualPageSize(double a, double b) {
    constexpr double kPageSizeEpsilon = 0.01;
    return std::abs(a - b) <= kPageSizeEpsilon;
}

} // namespace

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
    (void)FillSha256(pdfPath, &id);

    if (haveMeta) {
        StoreCachedPdfId(NormalizePathKey(pdfPath), size, mtimeMs, id, /*hasStrongHash=*/!id.sha256.empty());
    }
    return id;
}

bool PdfIdHasFastFingerprint(const clrop::PdfId& id) {
    return id.pageCount > 0 && static_cast<size_t>(id.pageCount) == id.pageSizesPt.size();
}

bool PdfIdFastMatches(const clrop::PdfId& lhs, const clrop::PdfId& rhs) {
    if (lhs.size != 0 && rhs.size != 0 && lhs.size != rhs.size) return false;

    const bool lhsHasFast = PdfIdHasFastFingerprint(lhs);
    const bool rhsHasFast = PdfIdHasFastFingerprint(rhs);
    if (lhsHasFast && rhsHasFast) {
        if (lhs.pageCount != rhs.pageCount) return false;
        for (size_t i = 0; i < lhs.pageSizesPt.size(); ++i) {
            if (!NearlyEqualPageSize(lhs.pageSizesPt[i][0], rhs.pageSizesPt[i][0]) ||
                !NearlyEqualPageSize(lhs.pageSizesPt[i][1], rhs.pageSizesPt[i][1])) {
                return false;
            }
        }
        return true;
    }

    if (lhs.pageCount > 0 && rhs.pageCount > 0 && lhs.pageCount != rhs.pageCount) return false;
    return lhs.size != 0 && rhs.size != 0 && lhs.size == rhs.size;
}

bool PdfIdStrongMatches(const clrop::PdfId& lhs, const clrop::PdfId& rhs) {
    if (!PdfIdFastMatches(lhs, rhs)) return false;
    if (lhs.sha256.empty() || rhs.sha256.empty()) return false;
    return lhs.sha256 == rhs.sha256;
}

} // namespace clrop
