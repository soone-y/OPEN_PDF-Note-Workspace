#pragma once

#include "core/path_safety.h"
#include <cwctype>
#include <set>

namespace workspace_import {

enum class DropIssue { None, WrongKind, InvalidSource, InvalidDestination, Collision, Overlap };

// Read-only preflight for a whole drop. The UI thread owns the fixed source /
// destination lists. Reject every known conflict before any copy or selection
// change. Final filesystem operations must still refuse replacement after races.
[[nodiscard]] inline std::wstring DropPathKey(const std::filesystem::path& path) {
    auto text = path.wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.rfind(L"\\\\?\\", 0) == 0) text.erase(0, 4);
    text = std::filesystem::path(text).lexically_normal().wstring();
    while (text.size() > 3 && text.back() == L'\\') text.pop_back();
    for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

[[nodiscard]] inline bool DropPathContains(const std::wstring& root, const std::wstring& child) {
    if (root.empty() || child.size() < root.size()) return false;
    const bool samePrefix = CompareStringOrdinal(root.data(), static_cast<int>(root.size()),
        child.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL;
    return samePrefix && (child.size() == root.size() || root.back() == L'\\' || child[root.size()] == L'\\');
}

struct DropPathLess {
    [[nodiscard]] bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        // Match Windows ordinal case comparison, including non-ASCII names;
        // the C locale's towlower alone only folds ASCII on common builds.
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

[[nodiscard]] inline bool IsLocalDropPath(const std::filesystem::path& path) {
    const auto text = DropPathKey(path);
    return text.size() >= 3 && text[0] >= L'a' && text[0] <= L'z' && text[1] == L':' &&
        text[2] == L'\\' && GetDriveTypeW(std::filesystem::path(text).root_path().c_str()) != DRIVE_REMOTE;
}

[[nodiscard]] inline bool DropPathHasSafeAncestors(std::filesystem::path path) {
    auto name = path.wstring();
    std::replace(name.begin(), name.end(), L'/', L'\\');
    if (name.rfind(L"\\\\?\\", 0) == 0) name.erase(0, 4);
    path = std::filesystem::path(name);
    auto current = path.root_path();
    const auto relative = path.relative_path();
    // Check parents first: never query a descendant through a reparse point.
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        if (*part == L"." || *part == L"..") return false;
        current /= *part;
        const DWORD attrs = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(current).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
            return true; // A missing parent cannot contain existing descendants.
        }
        auto next = part;
        const bool leaf = ++next == relative.end();
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) || (!leaf && !(attrs & FILE_ATTRIBUTE_DIRECTORY))) return false;
    }
    return true;
}

[[nodiscard]] inline DropIssue CheckDropSources(const std::vector<std::wstring>& sources, bool directories) {
    for (const auto& name : sources) {
        const std::filesystem::path path(name);
        if (!IsLocalDropPath(path) || !DropPathHasSafeAncestors(path)) return DropIssue::InvalidSource;
        const DWORD attrs = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DEVICE)) return DropIssue::InvalidSource;
        if (((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) != directories) return DropIssue::WrongKind;
    }
    return DropIssue::None;
}

[[nodiscard]] inline DropIssue CheckDropCopyPlan(const std::vector<std::wstring>& sources,
                                                const std::vector<std::filesystem::path>& destinations,
                                                bool directories) {
    if (sources.empty() || sources.size() != destinations.size()) return DropIssue::InvalidSource;
    const auto sourceIssue = CheckDropSources(sources, directories);
    if (sourceIssue != DropIssue::None) return sourceIssue;
    std::set<std::wstring, DropPathLess> sourceKeys, destinationKeys;
    for (size_t index = 0; index < sources.size(); ++index) {
        const auto source = DropPathKey(sources[index]);
        const auto& destination = destinations[index];
        if (!sourceKeys.insert(source).second) return DropIssue::Overlap;
        if (!IsLocalDropPath(destination) || !DropPathHasSafeAncestors(destination)) return DropIssue::InvalidDestination;
        if (!destinationKeys.insert(DropPathKey(destination)).second) return DropIssue::Collision;
        const DWORD attrs = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(destination).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) return DropIssue::Collision;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return DropIssue::InvalidDestination;
        if (!directories) {
            auto companion = destination;
            auto extension = companion.extension().wstring();
            for (auto& ch : extension) ch = static_cast<wchar_t>(std::towlower(ch));
            if (extension == L".pdf" || extension == L".clrop") {
                companion.replace_extension(extension == L".pdf" ? L".clrop" : L".pdf");
                if (GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(companion).c_str()) != INVALID_FILE_ATTRIBUTES)
                    return DropIssue::Collision;
                const DWORD companionError = GetLastError();
                if (companionError != ERROR_FILE_NOT_FOUND && companionError != ERROR_PATH_NOT_FOUND)
                    return DropIssue::InvalidDestination;
            }
        }
    }
    if (directories) {
        for (const auto& source : sourceKeys) {
            for (const auto& other : sourceKeys)
                if (source != other && DropPathContains(source, other)) return DropIssue::Overlap;
            for (const auto& destination : destinationKeys)
                if (DropPathContains(source, destination) || DropPathContains(destination, source)) return DropIssue::Overlap;
        }
    }
    return DropIssue::None;
}

} // namespace workspace_import
