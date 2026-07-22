#pragma once

#include <filesystem>
#include <string>

namespace cache_dir_policy {

inline constexpr wchar_t kManagedDefault[] = L"__resource__/__tmp__";

enum class CacheDirDecision {
    ManagedDefault,
    UnsafeCustom,
};

inline std::wstring NormalizeCacheDirText(const std::wstring& cacheDir) {
    return std::filesystem::path(cacheDir).lexically_normal().generic_wstring();
}

inline CacheDirDecision ResolveCacheDirDecision(const std::wstring& cacheDir) {
    if (cacheDir.empty()) return CacheDirDecision::ManagedDefault;
    const std::wstring norm = NormalizeCacheDirText(cacheDir);
    if (norm == NormalizeCacheDirText(kManagedDefault)) return CacheDirDecision::ManagedDefault;
    return CacheDirDecision::UnsafeCustom;
}

inline bool IsManagedCacheDir(const std::wstring& cacheDir) {
    return ResolveCacheDirDecision(cacheDir) != CacheDirDecision::UnsafeCustom;
}

inline std::wstring EffectiveCacheDir(const std::wstring& cacheDir) {
    (void)cacheDir;
    return kManagedDefault;
}

} // namespace cache_dir_policy
