#include "pdf_view/pdf_script_notice_text.h"
#include <iostream>
#include <stdexcept>

void Check(bool value) { if (!value) throw std::runtime_error("notice text contract failed"); }
int main() {
    using namespace pdf_script_notice;
    Check(Preview(L"<script>evil()</script> %s & file.js", 100) == L"<script>evil()</script> %s & file.js");
    Check(Preview(L"name\n\t\x202e", 100) == L"name\\u000A\\u0009\\u202E");
    Check(Preview(std::wstring(L"a\0b", 3), 100) == L"a\\u0000b");
    Check(Preview(L"abcdef", 3) == L"abc…");
    const std::wstring pair{static_cast<wchar_t>(0xd83d), static_cast<wchar_t>(0xde00)};
    Check(Preview(pair, 1) == L"…");
    Check(Preview(pair, 2) == pair);
    Check(Preview(std::wstring(1, static_cast<wchar_t>(0xd83d)), 3) == L"\\uD83D");
    Check(Read([](unsigned short* p, unsigned long bytes) -> unsigned long {
        if (p) { Check(bytes == 10); p[0] = 'A'; p[1] = 0x65e5; p[2] = 'B'; p[3] = 0; }
        return 8;
    }) == L"A日B");
    for (unsigned long invalid : {0ul, 1ul, 3ul, 1024ul * 1024 + 2}) {
        int calls = 0;
        Check(Read([&](unsigned short*, unsigned long) { ++calls; return invalid; }).empty());
        Check(calls == 1);
    }
    Check(Read([](unsigned short* p, unsigned long) -> unsigned long {
        if (!p) return 4;
        p[0] = 'x'; p[1] = 0; return 2;
    }).empty());
    Check(Read([](unsigned short* p, unsigned long) -> unsigned long {
        if (p) { p[0] = 'x'; p[1] = 'y'; }
        return 4;
    }).empty());
    Check(Read([](unsigned short* p, unsigned long) -> unsigned long {
        if (p) return 0;
        return 4;
    }).empty());
    std::cout << "PDF_SCRIPT_NOTICE_TEXT_PASS\n";
}
