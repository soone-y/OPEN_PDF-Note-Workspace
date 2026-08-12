#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/core/atomic_write.h"

namespace fs = std::filesystem;

static std::string ReadAll(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return "";
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

static fs::path MakeTestRoot() {
    fs::path root = fs::temp_directory_path() / L"atomic_test_root";
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

int main() {
    fs::path root = MakeTestRoot();
    fs::path dest = root / L"test.txt";
    std::wstring err;
    std::string payload = "hello";
    
    std::cout << "Starting AtomicWrite..." << std::endl;
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), root / L"tmp", root / L"escape", &err);
    
    if (ok) {
        std::cout << "SUCCESS: " << ReadAll(dest) << std::endl;
    } else {
        std::wcout << L"FAIL: " << err << std::endl;
    }
    
    return ok ? 0 : 1;
}
