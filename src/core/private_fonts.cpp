#include "core/private_fonts.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_fontMutex;
std::vector<std::wstring> g_loadedFontFiles;
bool g_attemptedLoad = false;

bool HasFontExtension(const std::filesystem::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return ext == L".ttf" || ext == L".otf" || ext == L".ttc";
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix) {
    if (!prefix) return false;
    const size_t prefixLen = wcslen(prefix);
    return value.size() >= prefixLen && value.compare(0, prefixLen, prefix) == 0;
}

bool ShouldLoadLibreOfficePrivateFontFile(const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return StartsWith(name, L"liberationsans-") ||
           StartsWith(name, L"liberationserif-") ||
           StartsWith(name, L"liberationmono-") ||
           StartsWith(name, L"carlito-") ||
           StartsWith(name, L"caladea-") ||
           name == L"opens___.ttf";
}

std::filesystem::path ModuleDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = 0;
    for (;;) {
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) return {};
        if (len < buffer.size() - 1) break;
        buffer.resize(buffer.size() * 2, L'\0');
    }
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path();
}

std::vector<std::filesystem::path> FontDirectoryCandidates() {
    std::vector<std::filesystem::path> candidates;
    const auto exeDir = ModuleDirectory();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir / L"libreoffice" / L"image" / L"Fonts");
        candidates.push_back(exeDir / L"third_party" / L"libreoffice" / L"image" / L"Fonts");
        candidates.push_back(exeDir / L".." / L"third_party" / L"libreoffice" / L"image" / L"Fonts");
        candidates.push_back(exeDir / L".." / L".." / L"third_party" / L"libreoffice" / L"image" / L"Fonts");
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / L"third_party" / L"libreoffice" / L"image" / L"Fonts");
    }
    return candidates;
}

std::filesystem::path FindBundledFontDirectory() {
    for (const auto& candidate : FontDirectoryCandidates()) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return {};
}

} // namespace

void LoadLibreOfficePrivateFontsForProcess() {
    std::lock_guard<std::mutex> lock(g_fontMutex);
    if (g_attemptedLoad) return;
    g_attemptedLoad = true;

    const auto fontDir = FindBundledFontDirectory();
    if (fontDir.empty()) return;

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(fontDir, ec)) {
        if (ec) return;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto path = entry.path();
        if (HasFontExtension(path) && ShouldLoadLibreOfficePrivateFontFile(path)) {
            files.push_back(path);
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& path : files) {
        const std::wstring nativePath = path.wstring();
        if (AddFontResourceExW(nativePath.c_str(), FR_PRIVATE, nullptr) > 0) {
            g_loadedFontFiles.push_back(nativePath);
        }
    }
}

void UnloadLibreOfficePrivateFontsForProcess() {
    std::lock_guard<std::mutex> lock(g_fontMutex);
    for (auto it = g_loadedFontFiles.rbegin(); it != g_loadedFontFiles.rend(); ++it) {
        RemoveFontResourceExW(it->c_str(), FR_PRIVATE, nullptr);
    }
    g_loadedFontFiles.clear();
}
