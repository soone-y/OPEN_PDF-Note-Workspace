#pragma once
#include <filesystem>
#include <string_view>

namespace office::handoff_paths {
// UTF-16 units, leaving room for the extended prefix and terminating NUL.
inline constexpr size_t kMaximumPathLength = 32760;
// The Windows process launcher and LibreOffice's launcher both need a short
// explicit working directory. Writable paths are passed separately in full.
inline constexpr size_t kMaximumWorkingDirectoryLength = 254; // 258 minus the extended prefix.
inline constexpr size_t kMaximumCommandLineLength = 32766;

// The nine fixed unquoted arguments receive quotes in LibreOffice's loader.
// Its OOO_CWD argument escapes backslashes/$ and doubles trailing backslashes.
[[nodiscard]] inline size_t ConverterCommandLineLength(std::wstring_view command,
                                                      const std::filesystem::path& workingDirectory) {
    const auto directory = workingDirectory.wstring();
    size_t length = command.size() + 18 + std::wstring_view(L" \"-env:OOO_CWD=2").size() + 1;
    size_t trailing = 0;
    for (const wchar_t ch : directory) {
        length += ch == L'\\' || ch == L'$' ? 2 : 1;
        trailing = ch == L'\\' ? trailing + 2 : 0;
    }
    return length + trailing;
}
}
