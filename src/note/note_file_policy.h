#pragma once

#include "note/note_content_kind.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace note {

[[nodiscard]] inline std::wstring NoteFileExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return extension;
}

[[nodiscard]] inline bool IsStandardNotePath(const std::filesystem::path& path) {
    const auto extension = NoteFileExtension(path);
    return extension == L".txt" || extension == L".csv" || extension == L".md" ||
           extension == L".markdown" || extension == L".note" || extension == L".tex" ||
           extension == L".icr" || extension == L".clro";
}

// A rename must not disguise text as a supported binary document, executable,
// shell shortcut, or the application's annotation/recovery data. Other names
// require explicit confirmation; this policy never discovers or writes files.
[[nodiscard]] inline bool IsForbiddenNoteRenamePath(const std::filesystem::path& path) {
    const auto extension = NoteFileExtension(path);
    constexpr std::wstring_view forbidden[] = {
        L".pdf", L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".tif", L".tiff", L".ico",
        L".doc", L".docx", L".docm", L".dot", L".dotx", L".dotm", L".odt", L".ott", L".rtf",
        L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".ppsm", L".pot", L".potx", L".potm", L".odp", L".otp",
        L".xls", L".xlsx", L".xlsm", L".xlsb", L".xlt", L".xltx", L".xltm", L".ods", L".ots",
        L".exe", L".com", L".dll", L".sys", L".scr", L".cpl", L".msi", L".msp", L".msix", L".appx",
        L".bat", L".cmd", L".ps1", L".psm1", L".psd1", L".vbs", L".vbe", L".js", L".jse", L".wsf", L".wsh", L".hta",
        L".lnk", L".url", L".scf", L".reg", L".clrop", L".stage"
    };
    return std::any_of(std::begin(forbidden), std::end(forbidden), [&](std::wstring_view value) {
        return extension == value;
    });
}

[[nodiscard]] inline std::wstring NormalizeAdditionalNoteExtension(std::wstring value) {
    if (value.empty()) return {};
    if (value.front() != L'.') value.insert(value.begin(), L'.');
    if (value.size() < 2 || value.size() > 64) return {};
    for (size_t i = 1; i < value.size(); ++i) {
        const wchar_t ch = value[i];
        if (ch <= L' ' || iswspace(ch) || std::wstring_view(L".\\/:*?\"<>|,;").find(ch) != std::wstring_view::npos) return {};
        value[i] = static_cast<wchar_t>(towlower(ch));
    }
    return value;
}

[[nodiscard]] inline bool IsAdditionalNotePath(const std::filesystem::path& path,
                                               const std::vector<std::wstring>& extensions) {
    if (IsForbiddenNoteRenamePath(path)) return false;
    const auto extension = NoteFileExtension(path);
    if (extension.empty()) return false;
    return std::any_of(extensions.begin(), extensions.end(), [&](const std::wstring& value) {
        return extension == NormalizeAdditionalNoteExtension(value);
    });
}

// Settings accept a hand-edited list separated by whitespace, commas or
// semicolons. Reject the entire draft on an invalid/restricted entry.
[[nodiscard]] inline bool ParseAdditionalNoteExtensions(std::wstring_view text,
                                                        std::vector<std::wstring>* output) {
    if (!output) return false;
    std::vector<std::wstring> parsed;
    size_t pos = 0;
    while (pos < text.size()) {
        if (iswspace(text[pos]) || text[pos] == L',' || text[pos] == L';') { ++pos; continue; }
        const size_t start = pos;
        while (pos < text.size() && !iswspace(text[pos]) && text[pos] != L',' && text[pos] != L';') ++pos;
        auto extension = NormalizeAdditionalNoteExtension(std::wstring(text.substr(start, pos - start)));
        if (extension.empty() || IsForbiddenNoteRenamePath(L"note" + extension)) return false;
        if (!IsStandardNotePath(L"note" + extension) &&
            std::find(parsed.begin(), parsed.end(), extension) == parsed.end()) parsed.push_back(std::move(extension));
        if (parsed.size() > 128) return false;
    }
    *output = std::move(parsed);
    return true;
}

[[nodiscard]] inline NoteContentKind ContentKindForNotePath(const std::filesystem::path& path) {
    const auto extension = NoteFileExtension(path);
    if (extension == L".tex") return NoteContentKind::TeXSource;
    if (!path.empty() && (extension == L".txt" || extension == L".csv" || !IsStandardNotePath(path))) {
        return NoteContentKind::PlainText;
    }
    return NoteContentKind::Markdown;
}

} // namespace note
