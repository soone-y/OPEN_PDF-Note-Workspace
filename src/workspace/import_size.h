#pragma once

#include "workspace/drop_import_policy.h"
#include <cstdint>
#include <limits>

namespace workspace_import {

// Decimal MB / GB, matching the units shown to the user. The UI thread measures
// one complete import before copying; this policy never writes source or target.
inline constexpr std::uint64_t kImportMegabyte = 1000000;
inline constexpr std::uint64_t kImportGigabyte = 1000 * kImportMegabyte;
enum class ImportSizeLevel { Small, Over100MB, Over500MB, Over1GB };

[[nodiscard]] inline constexpr ImportSizeLevel ImportLevel(std::uint64_t bytes) noexcept {
    return bytes >= kImportGigabyte ? ImportSizeLevel::Over1GB
        : bytes >= 500 * kImportMegabyte ? ImportSizeLevel::Over500MB
        : bytes >= 100 * kImportMegabyte ? ImportSizeLevel::Over100MB : ImportSizeLevel::Small;
}

struct ImportSize {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;

    [[nodiscard]] bool Add(const ImportSize& other) noexcept {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (other.bytes > maximum - bytes || other.files > maximum - files) return false;
        bytes += other.bytes;
        files += other.files;
        return true;
    }

    [[nodiscard]] bool AddFile(const std::filesystem::path& path) {
        if (!IsLocalDropPath(path) || !DropPathHasSafeAncestors(path)) return false;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(),
                                 GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
            return false;
        const std::uint64_t length = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        return Add({length, 1});
    }
};

[[nodiscard]] inline bool MeasureImportFiles(const std::vector<std::wstring>& paths, ImportSize& result) {
    ImportSize measured;
    for (const auto& path : paths) if (!measured.AddFile(path)) return false;
    result = measured;
    return true;
}

[[nodiscard]] inline std::wstring ImportSizeNumber(std::uint64_t bytes) {
    const auto unit = bytes >= kImportGigabyte ? kImportGigabyte : kImportMegabyte;
    const auto fraction = bytes % unit * 100 / unit;
    return std::to_wstring(bytes / unit) + L"." + (fraction < 10 ? L"0" : L"") + std::to_wstring(fraction);
}

} // namespace workspace_import
