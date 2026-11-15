#pragma once

#include <optional>
#include <string>
#include <vector>

struct SessionEntry {
    std::wstring displayName;
    std::wstring path;
};

enum class SessionSortMode { NumericAsc, NumericDesc, Name };
enum class SessionNumberingMode { CountPlusOne, MaxNumberPlusOne };

// The caller owns the session list. These operations use only their arguments.
[[nodiscard]] SessionSortMode ParseSessionSortMode(const std::wstring& mode);
void SortSessionsForDisplay(std::vector<SessionEntry>& sessions, SessionSortMode mode);
[[nodiscard]] SessionNumberingMode ParseSessionNumberingMode(const std::wstring& mode);
[[nodiscard]] int NextSessionNumberForSuggestions(const std::vector<SessionEntry>& sessions,
                                                 SessionNumberingMode mode);

// Preserve the existing numeric interpretation, including legacy name formats.
[[nodiscard]] int KanjiDigit(wchar_t ch);
[[nodiscard]] int FullwidthDigit(wchar_t ch);
[[nodiscard]] int RomanDigit(wchar_t ch);
[[nodiscard]] std::optional<int> ParseRomanNumber(const std::wstring& text);
[[nodiscard]] std::optional<int> ParseKanjiNumber(const std::wstring& text);
[[nodiscard]] std::optional<int> ExtractNumericKey(const std::wstring& name);
