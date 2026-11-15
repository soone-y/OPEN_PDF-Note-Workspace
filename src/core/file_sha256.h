#pragma once

#include "path_safety.h"
#include "sha256.h"
#include <array>
#include <string>

namespace core_hash {
// Uncached whole-file digest. The caller owns any required lifetime lock; this
// read helper never creates files and rejects directories and reparse points.
[[nodiscard]] inline bool FileSha256(const std::filesystem::path& path, std::string* out) {
    if (out) out->clear();
    if (path.empty()) return false;
    HANDLE file = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    Sha256 hash;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) return false;
        if (!count) break;
        hash.Update(buffer.data(), count);
    }
    const auto digest = hash.Finalize();
    if (!out) return true;
    constexpr char hex[] = "0123456789abcdef";
    out->reserve(64);
    for (const auto byte : digest) {
        out->push_back(hex[byte >> 4]);
        out->push_back(hex[byte & 15]);
    }
    return true;
}
} // namespace core_hash
