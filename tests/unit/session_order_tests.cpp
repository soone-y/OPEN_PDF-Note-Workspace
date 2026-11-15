#include "workspace/session_order.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    const std::pair<std::wstring, std::optional<int>> numericCases[] = {
        {L"第2回（3限）", 2}, {L"2026年度 第2回", 2}, {L"資料2_3", 2},
        {L"第１０回", 10}, {L"第 ３回", 3}, {L"第十二回", 102},
        {L"第IV回", 4}, {L"ix", 9}, {L"補講", std::nullopt},
        {L"", std::nullopt}, {L"0", 0}, {L"0012", 12},
        {L"2147483647", 2147483647}, {L"2147483648", std::nullopt},
        {L"999999999999999999999", std::nullopt},
    };
    // Keep the legacy kanji interpretation (十二 -> 102) during extraction.
    for (size_t i = 0; i < std::size(numericCases); ++i) {
        const auto& [name, expected] = numericCases[i];
        if (ExtractNumericKey(name) != expected) {
            std::cerr << "numeric interpretation case " << i << '\n';
            return 1;
        }
    }
    Require(KanjiDigit(L'九') == 9 && KanjiDigit(L'十') == -1, "kanji digit");
    Require(FullwidthDigit(L'５') == 5 && FullwidthDigit(L'5') == -1, "fullwidth digit");
    Require(RomanDigit(L'x') == 10 && RomanDigit(L'?') == -1, "roman digit");
    Require(ParseRomanNumber(L"IV") == 4 && !ParseRomanNumber(L"補講"), "roman parse");
    Require(ParseKanjiNumber(L"十二") == 102 && !ParseKanjiNumber(L"補講"), "legacy kanji parse");

    for (const auto* mode : {L"numeric_desc", L"desc", L"number_desc", L"DESC"}) {
        Require(ParseSessionSortMode(mode) == SessionSortMode::NumericDesc, "descending alias");
    }
    for (const auto* mode : {L"name", L"lexical", L"NAME"}) {
        Require(ParseSessionSortMode(mode) == SessionSortMode::Name, "name alias");
    }
    for (const auto* mode : {L"", L"numeric_asc", L"unknown"}) {
        Require(ParseSessionSortMode(mode) == SessionSortMode::NumericAsc, "sort default");
    }
    for (const auto* mode : {L"max_number", L"max", L"maximum", L"MAX"}) {
        Require(ParseSessionNumberingMode(mode) == SessionNumberingMode::MaxNumberPlusOne,
                "max numbering alias");
    }
    for (const auto* mode : {L"", L"count", L"unknown"}) {
        Require(ParseSessionNumberingMode(mode) == SessionNumberingMode::CountPlusOne,
                "count numbering default");
    }

    const std::vector<SessionEntry> original = {
        {L"第10回", L"ten"}, {L"補講", L"extra"}, {L"第2回", L"two"},
        {L"第1回", L"one"}, {L"第2回_追加", L"two-extra"},
    };
    const std::vector<std::wstring> ascending = {L"one", L"two", L"two-extra", L"ten", L"extra"};
    const std::vector<std::wstring> descending = {L"ten", L"two-extra", L"two", L"one", L"extra"};
    for (const auto mode : {SessionSortMode::NumericAsc, SessionSortMode::NumericDesc}) {
        auto entries = original;
        SortSessionsForDisplay(entries, mode);
        const auto& expected = mode == SessionSortMode::NumericAsc ? ascending : descending;
        Require(entries.size() == expected.size(), "sort count");
        for (size_t i = 0; i < entries.size(); ++i) {
            Require(entries[i].path == expected[i], "numeric sort and tie order");
            const auto source = std::find_if(original.begin(), original.end(), [&](const auto& entry) {
                return entry.path == entries[i].path;
            });
            Require(source != original.end() && source->displayName == entries[i].displayName,
                    "entry name/path association");
        }
    }
    auto named = original;
    SortSessionsForDisplay(named, SessionSortMode::Name);
    Require(std::is_sorted(named.begin(), named.end(), [](const auto& a, const auto& b) {
        return a.displayName < b.displayName;
    }), "name sort");
    Require(NextSessionNumberForSuggestions(original, SessionNumberingMode::CountPlusOne) == 6,
            "count numbering");
    Require(NextSessionNumberForSuggestions(original, SessionNumberingMode::MaxNumberPlusOne) == 11,
            "max numbering");
    std::vector<SessionEntry> empty;
    for (const auto mode : {SessionSortMode::Name, SessionSortMode::NumericAsc, SessionSortMode::NumericDesc}) {
        SortSessionsForDisplay(empty, mode);
        Require(empty.empty(), "empty sort");
    }
    Require(NextSessionNumberForSuggestions(empty, SessionNumberingMode::CountPlusOne) == 1,
            "empty count");
    Require(NextSessionNumberForSuggestions(empty, SessionNumberingMode::MaxNumberPlusOne) == 1,
            "empty maximum");
    Require(NextSessionNumberForSuggestions({{L"補講", L"extra"}},
            SessionNumberingMode::MaxNumberPlusOne) == 1, "unnumbered maximum");
    std::cout << "[PASS] session order: numeric formats, aliases, sorting, entry identity, numbering, empty input\n";
}
