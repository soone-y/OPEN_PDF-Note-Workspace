#pragma once

#include "core/path_safety.h"
#include <filesystem>
#include <cwctype>

namespace workspace_import {

[[nodiscard]] inline std::wstring SourceStemKey(const std::filesystem::path& path) {
    std::wstring key = (path.parent_path() / path.stem()).lexically_normal().wstring();
    for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
    return key;
}

// Choose a local destination for a copied drop, preserving the extension.
// The final move must still reject replacement: another writer may race this check.
[[nodiscard]] inline bool ChooseNewDestination(const std::filesystem::path& requested,
                                                std::filesystem::path* chosen, DWORD* error,
                                                bool protectCompanion = false) {
    if (!chosen || !error || requested.empty() || !requested.is_absolute() || IsUncPath(requested) ||
        GetDriveTypeW(requested.root_path().c_str()) == DRIVE_REMOTE) {
        if (error) *error = ERROR_INVALID_NAME;
        return false;
    }
    const auto stem = requested.stem().wstring();
    const auto extension = requested.extension().wstring();
    std::wstring normalizedExtension = extension;
    for (auto& ch : normalizedExtension) ch = static_cast<wchar_t>(std::towlower(ch));
    for (int index = 1; index <= 10000; ++index) {
        const auto name = index == 1 ? requested.filename().wstring()
            : stem + L" (" + std::to_wstring(index) + L")" + extension;
        if (name.size() > 255) { *error = ERROR_FILENAME_EXCED_RANGE; return false; }
        const auto candidate = requested.parent_path() / name;
        const auto ioPath = ToExtendedWin32PathIfAbsoluteLocal(candidate);
        if (GetFileAttributesW(ioPath.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        const DWORD last = GetLastError();
        if (last != ERROR_FILE_NOT_FOUND && last != ERROR_PATH_NOT_FOUND) { *error = last; return false; }
        if (protectCompanion && (normalizedExtension == L".pdf" || normalizedExtension == L".clrop")) {
            auto companion = candidate;
            companion.replace_extension(normalizedExtension == L".pdf" ? L".clrop" : L".pdf");
            const auto companionPath = ToExtendedWin32PathIfAbsoluteLocal(companion);
            if (GetFileAttributesW(companionPath.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            const DWORD companionError = GetLastError();
            if (companionError != ERROR_FILE_NOT_FOUND && companionError != ERROR_PATH_NOT_FOUND) {
                *error = companionError; return false;
            }
        }
        *chosen = candidate;
        *error = ERROR_SUCCESS;
        return true;
    }
    *error = ERROR_FILE_EXISTS;
    return false;
}

// Input is a completed, flushed, closed temporary copy in the destination directory.
// Failure retains both the existing destination and the recoverable temporary copy.
[[nodiscard]] inline bool CommitNewFile(const std::filesystem::path& temporary,
                                        const std::filesystem::path& destination, DWORD* error) {
    if (!error || temporary.empty() || destination.empty() ||
        !temporary.is_absolute() || !destination.is_absolute() || IsUncPath(temporary) || IsUncPath(destination) ||
        GetDriveTypeW(destination.root_path().c_str()) == DRIVE_REMOTE ||
        temporary.parent_path() != destination.parent_path()) {
        if (error) *error = ERROR_INVALID_NAME;
        return false;
    }
    const auto source = ToExtendedWin32PathIfAbsoluteLocal(temporary);
    const auto target = ToExtendedWin32PathIfAbsoluteLocal(destination);
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        *error = GetLastError();
        return false;
    }
    *error = ERROR_SUCCESS;
    return true;
}

} // namespace workspace_import
