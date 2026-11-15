#include "workspace/session_order.h"

#include <algorithm>
#include <cwctype>

int KanjiDigit(wchar_t ch) {
    switch (ch) {
    case L'零': case L'〇': return 0;
    case L'一': return 1;
    case L'二': return 2;
    case L'三': return 3;
    case L'四': return 4;
    case L'五': return 5;
    case L'六': return 6;
    case L'七': return 7;
    case L'八': return 8;
    case L'九': return 9;
    default: return -1;
    }
}

int FullwidthDigit(wchar_t ch) {
    if (ch >= 0xFF10 && ch <= 0xFF19) return static_cast<int>(ch - 0xFF10);
    return -1;
}

int RomanDigit(wchar_t ch) {
    wchar_t u = static_cast<wchar_t>(towupper(ch));
    switch (u) {
    case L'I': return 1;
    case L'V': return 5;
    case L'X': return 10;
    case L'L': return 50;
    case L'C': return 100;
    case L'D': return 500;
    case L'M': return 1000;
    default: return -1;
    }
}

std::optional<int> ParseRomanNumber(const std::wstring& s) {
    std::vector<int> vals;
    for (wchar_t ch : s) {
        int v = RomanDigit(ch);
        if (v >= 0) vals.push_back(v);
    }
    if (vals.empty()) return std::nullopt;
    int total = 0;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i + 1 < vals.size() && vals[i] < vals[i + 1]) {
            total -= vals[i];
        } else {
            total += vals[i];
        }
    }
    return total;
}

std::optional<int> ParseKanjiNumber(const std::wstring& s) {
    int total = 0;
    int current = 0;
    bool found = false;
    for (wchar_t ch : s) {
        int d = KanjiDigit(ch);
        if (d >= 0) {
            current = current * 10 + d;
            found = true;
            continue;
        }
        if (ch == L'十') {
            current = (current == 0) ? 10 : current * 10;
            found = true;
        } else if (ch == L'百') {
            current = (current == 0) ? 100 : current * 100;
            found = true;
        } else if (ch == L'千') {
            current = (current == 0) ? 1000 : current * 1000;
            found = true;
        }
    }
    if (!found) return std::nullopt;
    total += current;
    return total;
}

std::optional<int> ExtractNumericKey(const std::wstring& name) {
    auto digitAt = [&](size_t index) -> int {
        if (index >= name.size()) return -1;
        const int fullwidth = FullwidthDigit(name[index]);
        if (fullwidth >= 0) return fullwidth;
        return (name[index] >= L'0' && name[index] <= L'9')
            ? static_cast<int>(name[index] - L'0')
            : -1;
    };
    auto parseDigitRun = [&](size_t start) -> std::optional<int> {
        std::wstring digits;
        for (size_t index = start; index < name.size(); ++index) {
            const int digit = digitAt(index);
            if (digit < 0) break;
            digits.push_back(static_cast<wchar_t>(L'0' + digit));
        }
        if (digits.empty()) return std::nullopt;
        try {
            return std::stoi(digits);
        } catch (...) {
            return std::nullopt;
        }
    };

    // Prefer the ordinal marker used by ordinary session names.  This avoids
    // treating a year and a period number as one number (for example,
    // "2026年度 第2回" is session 2, not 20262).
    for (size_t index = 0; index < name.size(); ++index) {
        if (name[index] != L'第') continue;
        size_t digitStart = index + 1;
        while (digitStart < name.size() && iswspace(name[digitStart])) ++digitStart;
        if (auto value = parseDigitRun(digitStart)) return value;
    }

    // Without an ordinal marker, use only the first contiguous digit run.
    // Separate numbers must not be concatenated: "資料2_3" sorts as 2.
    for (size_t index = 0; index < name.size(); ++index) {
        if (digitAt(index) >= 0) return parseDigitRun(index);
    }

    auto roman = ParseRomanNumber(name);
    if (roman) return roman;
    return ParseKanjiNumber(name);
}

SessionNumberingMode ParseSessionNumberingMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"max_number" || m == L"max" || m == L"maximum") return SessionNumberingMode::MaxNumberPlusOne;
    return SessionNumberingMode::CountPlusOne;
}


int NextSessionNumberForSuggestions(const std::vector<SessionEntry>& sessions,
                                           SessionNumberingMode mode) {
    if (mode == SessionNumberingMode::MaxNumberPlusOne) {
        int maxNumber = 0;
        for (const auto& s : sessions) {
            if (auto n = ExtractNumericKey(s.displayName)) maxNumber = std::max(maxNumber, *n);
        }
        return std::max(1, maxNumber + 1);
    }
    return std::max(1, static_cast<int>(sessions.size()) + 1);
}

void SortSessionsForDisplay(std::vector<SessionEntry>& sessions, SessionSortMode mode) {
    if (mode == SessionSortMode::Name) {
        std::sort(sessions.begin(), sessions.end(),
            [](const SessionEntry& a, const SessionEntry& b) { return a.displayName < b.displayName; });
        return;
    }
    std::sort(sessions.begin(), sessions.end(), [&](const SessionEntry& a, const SessionEntry& b) {
        auto na = ExtractNumericKey(a.displayName);
        auto nb = ExtractNumericKey(b.displayName);
        if (na && nb && *na != *nb) {
            return mode == SessionSortMode::NumericDesc ? *na > *nb : *na < *nb;
        }
        if (na && !nb) return true;
        if (!na && nb) return false;
        return mode == SessionSortMode::NumericDesc ? a.displayName > b.displayName
                                                    : a.displayName < b.displayName;
    });
}

SessionSortMode ParseSessionSortMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"numeric_desc" || m == L"desc" || m == L"number_desc") return SessionSortMode::NumericDesc;
    if (m == L"name" || m == L"lexical") return SessionSortMode::Name;
    return SessionSortMode::NumericAsc;
}
