#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "office/docx_space_protection.h"

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 3 && argc != 4) {
        std::wcerr << L"usage: docx_space_protection_tests.exe <source.docx> <dest.docx> [--quiet]\n";
        return 2;
    }

    const std::filesystem::path source(argv[1]);
    const std::filesystem::path dest(argv[2]);
    const bool quiet = argc == 4 && std::wstring(argv[3]) == L"--quiet";
    std::wstring err;
    if (!office::TransformDocxForSpaceProtection(source, dest, &err)) {
        if (!quiet) {
            std::wcerr << L"TransformDocxForSpaceProtection failed: " << err << L"\n";
        }
        return 1;
    }

    const std::string sample = "神経診断学実習　小テスト";
    const std::string protectedText = office::ProtectJapaneseTokensAfterIdeographicSpaceUtf8(sample);
    if (protectedText.find("\xE5\xB0\x8F\xE2\x81\xA0\xE3\x83\x86\xE2\x81\xA0") == std::string::npos) {
        std::cerr << "WORD JOINER was not inserted into the expected Japanese token\n";
        return 1;
    }

    std::wcout << L"docx space protection transform succeeded: " << dest.wstring() << L"\n";
    return 0;
}
