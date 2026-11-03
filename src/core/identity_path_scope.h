#pragma once

#include "core/path_safety.h"
#include <cwctype>
#include <optional>
#include <string_view>

// Catalog aliases are data, never write permissions. External aliases require
// a currently registered root at every runtime lookup/rebind; catalogs and
// recovery evidence remain owned by the workspace.
namespace identity_path_scope {
inline constexpr std::wstring_view kExternalPrefix = L"@external:";

[[nodiscard]] inline bool IsExternal(std::wstring_view key) {
    return key.substr(0, kExternalPrefix.size()) == kExternalPrefix;
}

[[nodiscard]] inline bool SafeRelative(const std::filesystem::path& path) {
    if (path.empty() || path.has_root_path()) return false;
    for (const auto& part : path) {
        if (part == L".." || part == L".") return false;
        for (wchar_t ch : part.wstring())
            if (ch < 0x20 || ch == L':' || ch == L'*' || ch == L'?' ||
                ch == L'\"' || ch == L'<' || ch == L'>' || ch == L'|') return false;
    }
    return true;
}

// Pure lexical normalization: do not canonicalize an untrusted path before
// rejecting UNC/device paths and checking reparse points along its ancestry.
[[nodiscard]] inline std::optional<std::filesystem::path> LocalPath(std::wstring raw) {
    std::replace(raw.begin(), raw.end(), L'/', L'\\');
    if (raw.rfind(L"\\\\?\\", 0) == 0) raw.erase(0, 4);
    if (raw.size() < 3 || !((raw[0] >= L'a' && raw[0] <= L'z') ||
        (raw[0] >= L'A' && raw[0] <= L'Z')) || raw[1] != L':' || raw[2] != L'\\')
        return std::nullopt;
    std::filesystem::path path(raw);
    if (!path.relative_path().empty() && !SafeRelative(path.relative_path())) return std::nullopt;
    raw = path.lexically_normal().wstring();
    std::transform(raw.begin(), raw.end(), raw.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    while (raw.size() > 3 && raw.back() == L'\\') raw.pop_back();
    return std::filesystem::path(raw);
}

[[nodiscard]] inline bool Below(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto prefix = root.wstring();
    if (prefix.empty()) return false;
    if (prefix.back() != L'\\') prefix.push_back(L'\\');
    const auto value = path.wstring();
    return value.size() > prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] inline bool NoReparseAncestors(const std::filesystem::path& path) {
    const auto drive = GetDriveTypeW(path.root_path().c_str());
    if (drive == DRIVE_REMOTE || drive == DRIVE_UNKNOWN || drive == DRIVE_NO_ROOT_DIR) return false;
    auto cursor = path.root_path();
    for (const auto& part : path.relative_path()) {
        cursor /= part;
        const DWORD attrs = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(cursor).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    }
    return true;
}

[[nodiscard]] inline bool ValidExternalKey(std::wstring_view key) {
    if (!IsExternal(key)) return false;
    const auto raw = key.substr(kExternalPrefix.size());
    const auto path = LocalPath(std::wstring(raw));
    return path && !path->relative_path().empty() && path->wstring() == raw;
}

[[nodiscard]] inline std::optional<std::wstring> KeyForPath(
    const std::filesystem::path& workspace,
    const std::vector<std::filesystem::path>& registeredRoots,
    const std::wstring& absolutePath) {
    const auto path = LocalPath(absolutePath);
    const auto root = LocalPath(workspace.wstring());
    if (!path || !root) return std::nullopt;
    if (Below(*path, *root)) {
        if (!NoReparseAncestors(*path)) return std::nullopt;
        return path->lexically_relative(*root).wstring();
    }
    for (const auto& registered : registeredRoots) {
        const auto external = LocalPath(registered.wstring());
        if (external && Below(*path, *external) && NoReparseAncestors(*path))
            return std::wstring(kExternalPrefix) + path->wstring();
    }
    return std::nullopt;
}
} // namespace identity_path_scope
