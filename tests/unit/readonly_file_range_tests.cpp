#include "readonly_viewer/file_range.h"
#include "readonly_viewer/byte_dump.h"
#include <bitset>
#include <cstdlib>
#include <iostream>
#include <random>

int main() {
    namespace range = readonly_viewer::file_range;
    size_t checks = 0;
    const auto check = [&](bool value) {
        ++checks;
        if (!value) { std::cerr << "Failed byte-range check " << checks << '\n'; std::exit(1); }
    };
    const auto maximum = std::numeric_limits<uintmax_t>::max();
    for (const auto size : {uintmax_t{0}, uintmax_t{1}, uintmax_t{63}, range::kMaximumBytes,
                           uintmax_t{1} << 40, maximum}) {
        for (int part = 0; part <= 1000; ++part) {
            const auto expected = static_cast<uintmax_t>(static_cast<unsigned __int128>(size) * part / 1000);
            check(range::OffsetAt(size, part) == expected);
        }
    }
    std::mt19937_64 random(317);
    for (int n = 0; n < 10000; ++n) {
        const uintmax_t size = random() | 1, offset = random() % size;
        check(range::PositionOf(size, offset) == static_cast<int>(static_cast<unsigned __int128>(offset) * 1000 / size));
    }
    check(range::PositionOf(0, 0) == 0);
    check(range::PositionOf(maximum, maximum) == 1000);
    check(range::OffsetAt(maximum, -1) == 0);
    check(range::OffsetAt(maximum, 1001) == maximum);
    check(range::ParseBytes(L"00017") == 17);
    check(range::ParseBytes(L"0XfF") == 255);
    check(range::ParseBytes(std::to_wstring(maximum)) == maximum);
    for (const auto text : {L"", L"-1", L"+1", L"1.5", L"1e3", L"0x", L"0xg", L" 1", L"1 ",
                            L"18446744073709551616", L"0x10000000000000000"})
        check(!range::ParseBytes(text));
    check(!range::Make(0, 0, 1));
    check(!range::Make(maximum, maximum - 2, 3));
    check(!range::Make(maximum, 0, range::kMaximumBytes + 1));
    check(!range::Make(maximum, 0, 0));
    check(range::Make(maximum, maximum - 1, 1)->end == maximum);
    check(range::Make(maximum, 0, range::kMaximumBytes)->end == range::kMaximumBytes);
    const auto last = range::Move(maximum, {maximum - 20, maximum - 4}, true);
    check(last.start == maximum - 4 && last.end == maximum);
    const auto first = range::Move(100, {3, 19}, false);
    check(first.start == 0 && first.end == 16);
    std::cout << "Byte-range checks: " << checks << " PASS\n";
    checks = 0;
    namespace dump = readonly_viewer::byte_dump;
    const std::vector<unsigned char> bytes{0, 1, 0x41, 0x7e, 0x7f, 0x80, 0xff, 0x20, 0x42};
    const auto original = bytes;
    const auto hex = dump::Rows(bytes, 5, dump::Format::Hex);
    const auto bits = dump::Rows(bytes, 5, dump::Format::Binary);
    check(hex.find(L"00000005  00 01 41 7e 7f 80 ff 20  42") == 0);
    check(bits.find(L"00000005  00000000 00000001 01000001 01111110  01111111 10000000 11111111 00100000") == 0);
    check(bits.find(L"|..A~... |\n0000000d  01000010") != std::wstring::npos);
    check(bytes == original);
    check(dump::Rows({}, 0, dump::Format::Binary).empty());
    check(dump::Rows(bytes, maximum, dump::Format::Hex).empty());
    check(dump::Rows(bytes, maximum - 8, dump::Format::Binary).find(L"ffffffffffffffff  01000010") != std::wstring::npos);
    for (unsigned value = 0; value < 256; ++value) {
        const auto expected = std::bitset<8>(value).to_string();
        const std::wstring wide(expected.begin(), expected.end());
        check(dump::Rows({static_cast<unsigned char>(value)}, 0, dump::Format::Binary).find(wide + L" ") != std::wstring::npos);
    }
    const std::vector<unsigned char> long_bytes(65537, 0xff);
    const auto limited_bits = dump::Rows(long_bytes, 0, dump::Format::Binary);
    const auto limited_hex = dump::Rows(long_bytes, 0, dump::Format::Hex);
    check(std::count(limited_bits.begin(), limited_bits.end(), L'\n') == 8192);
    check(std::count(limited_hex.begin(), limited_hex.end(), L'\n') == 4096);
    std::cout << "Byte-dump checks: " << checks << " PASS\n";
}
