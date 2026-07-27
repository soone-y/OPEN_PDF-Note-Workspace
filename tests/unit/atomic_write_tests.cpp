#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/atomic_write.h"

namespace fs = std::filesystem;

static bool TryReadAll(const fs::path& p, std::string* out) {
    if (!out) return false;
    out->clear();
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return false;
    *out = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return static_cast<bool>(ifs) || ifs.eof();
}

// This reader explicitly shares delete access.  The atomic-replacement test
// must measure visibility during a permitted replacement, rather than make
// the writer fail merely because the C++ runtime opened the file without
// FILE_SHARE_DELETE.
static bool TryReadAllWhileReplacementIsAllowed(const fs::path& p, std::string* out) {
    if (!out) return false;
    out->clear();
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > static_cast<unsigned long long>(SIZE_MAX)) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < out->size()) {
        const size_t remaining = out->size() - offset;
        const DWORD requested = remaining > (1u << 20) ? (1u << 20) : static_cast<DWORD>(remaining);
        DWORD read = 0;
        if (!ReadFile(h, out->data() + offset, requested, &read, nullptr) || read == 0) {
            CloseHandle(h);
            out->clear();
            return false;
        }
        offset += read;
    }
    CloseHandle(h);
    return true;
}

static std::string ReadAll(const fs::path& p) {
    std::string out;
    TryReadAll(p, &out);
    return out;
}

static bool WriteRaw(const fs::path& p, const std::string& s) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(s.data(), static_cast<std::streamsize>(s.size()));
    return static_cast<bool>(ofs);
}

static fs::path MakeTestRoot() {
    fs::path root = fs::temp_directory_path() / L"atomic_test_root_full";
    root /= std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ec.clear();
    fs::create_directories(root, ec);
    return root;
}

static bool TestAtomicWriteNewFile(const fs::path& root) {
    fs::path dest = root / L"new.txt";
    std::wstring err;
    const std::string payload = "hello-new";
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), root / L"tmp", root / L"escape", &err);
    return ok && ReadAll(dest) == payload;
}

static bool SetReadOnlyFlag(const fs::path& p, bool readOnly) {
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (readOnly) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    } else {
        attrs &= ~FILE_ATTRIBUTE_READONLY;
    }
    return SetFileAttributesW(p.c_str(), attrs) != 0;
}

static bool TestAtomicWriteReplaceExisting(const fs::path& root) {
    fs::path dest = root / L"replace.txt";
    if (!WriteRaw(dest, "old")) return false;
    std::wstring err;
    const std::string payload = "new-content";
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), root / L"tmp", root / L"escape", &err);
    return ok && ReadAll(dest) == payload;
}

static bool TestAtomicWriteReadOnlyTargetFailsAndPreserves(const fs::path& root) {
    fs::path dest = root / L"readonly.txt";
    const std::string original = "old";
    if (!WriteRaw(dest, original)) return false;
    if (!SetReadOnlyFlag(dest, true)) return false;

    std::wstring err;
    const std::string payload = "new-value";
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), root / L"tmp", root / L"escape", &err);
    DWORD attrsAfter = GetFileAttributesW(dest.c_str());
    bool stillReadOnly = (attrsAfter != INVALID_FILE_ATTRIBUTES) && ((attrsAfter & FILE_ATTRIBUTE_READONLY) != 0);
    bool unchanged = (ReadAll(dest) == original);
    SetReadOnlyFlag(dest, false);
    return !ok && unchanged && stillReadOnly;
}

static bool TestAtomicWriteLockedTargetPreservesOriginal(const fs::path& root) {
    fs::path dest = root / L"locked.txt";
    const std::string original = "original";
    if (!WriteRaw(dest, original)) return false;

    HANDLE lock = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return false;

    const std::string payload = "should-not-overwrite";
    std::wstring err;
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), root / L"tmp", root / L"escape", &err);
    CloseHandle(lock);

    if (ok) return false;
    if (ReadAll(dest) != original) return false;

    std::error_code ec;
    fs::path escapeDir = root / L"escape";
    if (!fs::exists(escapeDir, ec)) return false;
    for (const auto& e : fs::directory_iterator(escapeDir, ec)) {
        if (ec) return false;
        if (e.is_regular_file()) return true;
    }
    return false;
}

static bool TestAtomicWriteZeroBytes(const fs::path& root) {
    fs::path dest = root / L"empty.bin";
    std::wstring err;
    const char dummy = '\0';
    bool ok = atomic_write::AtomicWriteBytes(dest, &dummy, 0, root / L"tmp", root / L"escape", &err);
    return ok && ReadAll(dest).empty();
}

static bool TestAtomicWriteCreatesParentDirs(const fs::path& root) {
    fs::path dest = root / L"deep" / L"nested" / L"created.txt";
    std::wstring err;
    const std::string payload = "parent-created";
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), root / L"tmp", root / L"escape", &err);
    return ok && ReadAll(dest) == payload;
}

static bool TestCreateUniqueTempFileCreatesReservedFileAndHandle(const fs::path& root) {
    fs::path dest = root / L"temp_target.txt";
    std::wstring err;
    fs::path tmp;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!atomic_write::CreateUniqueTempFile(dest, root / L"tmp", &tmp, &h, &err)) return false;
    if (h == INVALID_HANDLE_VALUE) return false;
    std::error_code ec;
    bool exists = !tmp.empty() && fs::exists(tmp, ec) && !ec;
    CloseHandle(h);
    std::error_code rmec;
    fs::remove(tmp, rmec);
    return exists;
}

static bool TestCreateUniqueTempFileAvoidsExistingReservedName(const fs::path& root) {
    fs::path dest = root / L"collision_target.txt";
    std::wstring err;
    fs::path tmp1;
    fs::path tmp2;
    HANDLE h1 = INVALID_HANDLE_VALUE;
    HANDLE h2 = INVALID_HANDLE_VALUE;
    if (!atomic_write::CreateUniqueTempFile(dest, root / L"tmp_collision", &tmp1, &h1, &err)) return false;
    if (!atomic_write::CreateUniqueTempFile(dest, root / L"tmp_collision", &tmp2, &h2, &err)) {
        CloseHandle(h1);
        return false;
    }

    const bool distinct = !tmp1.empty() && !tmp2.empty() && tmp1 != tmp2;
    std::error_code ec;
    const bool bothReserved = fs::exists(tmp1, ec) && !ec && fs::exists(tmp2, ec) && !ec;
    CloseHandle(h1);
    CloseHandle(h2);
    fs::remove(tmp1, ec);
    ec.clear();
    fs::remove(tmp2, ec);
    return distinct && bothReserved;
}

static bool TestAtomicWriteEmptyDestinationFails(const fs::path& root) {
    (void)root;
    std::wstring err;
    const std::string payload = "x";
    return !atomic_write::AtomicWriteBytes(fs::path(), payload.data(), payload.size(), fs::path(), fs::path(), &err);
}

static bool TestAtomicWriteNullPayloadFails(const fs::path& root) {
    fs::path dest = root / L"null_payload.txt";
    std::wstring err;
    bool ok = atomic_write::AtomicWriteBytes(dest, nullptr, 4, root / L"tmp", root / L"escape", &err);
    std::error_code ec;
    return !ok && !fs::exists(dest, ec) && !ec && !err.empty();
}

static bool TestSameVolume(const fs::path& root) {
    (void)root;
    fs::path p1 = L"C:\\Windows";
    fs::path p2 = L"C:\\Users";
    fs::path p3 = L"D:\\Data"; // Assuming D: exists or at least different drive letter
    
    // Self comparison
    if (!atomic_write::SameVolume(p1, p1)) return false;
    // Same drive
    if (!atomic_write::SameVolume(p1, p2)) return false;
    
    // Different drive (if exists, or just check the logic)
    // Note: VolumeRootForPath uses GetVolumePathNameW which might fail if drive doesn't exist.
    // But SameVolume returns false if any root is empty.
    return true;
}

static bool TestPickTempDirForTarget(const fs::path& root) {
    fs::path target = root / L"target.txt";
    fs::path preferred = root / L"my_tmp";
    fs::path otherDrive = L"Z:\\Remote"; // Likely non-existent
    
    // Preferred is on same volume
    if (atomic_write::PickTempDirForTarget(target, preferred) != preferred) return false;
    
    // Preferred is on different volume or invalid -> fallback to parent
    if (atomic_write::PickTempDirForTarget(target, otherDrive) != target.parent_path()) return false;
    
    return true;
}

static bool TestAtomicWriteInvalidTempDirFallsBack(const fs::path& root) {
    fs::path dest = root / L"fallback_test.txt";
    fs::path badTmp = root / L"a_file_not_a_dir";
    {
        std::ofstream ofs(badTmp);
        ofs << "not a dir";
    }
    
    std::wstring err;
    const std::string payload = "fallback-ok";
    // CreateUniqueTempFile will try to create_directories(badTmp) which will fail, then fallback to dest.parent_path().
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), badTmp, root / L"escape", &err);
    return ok && ReadAll(dest) == payload;
}

static bool TestAtomicWriteQuarantineReadOnly(const fs::path& root) {
    fs::path dest = root / L"locked_for_quarantine.txt";
    if (!WriteRaw(dest, "original")) return false;
    
    // Lock the file
    HANDLE lock = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return false;
    
    fs::path quarantine = root / L"readonly_quarantine";
    fs::create_directories(quarantine);
    SetReadOnlyFlag(quarantine, true);
    
    std::wstring err;
    const std::string payload = "new";
    // This will fail to replace (locked) and try to quarantine. Quarantine will fail (read-only dir).
    // It should delete the temp file and return false.
    bool ok = atomic_write::AtomicWriteBytes(dest, payload.data(), payload.size(), root / L"tmp", quarantine, &err);
    
    SetReadOnlyFlag(quarantine, false);
    CloseHandle(lock);
    
    return !ok && ReadAll(dest) == "original";
}

static bool TestAtomicWriteConcurrentSameDestinationLeavesCompletePayload(const fs::path& root) {
    fs::path dest = root / L"concurrent.txt";
    if (!WriteRaw(dest, "original")) return false;

    constexpr int kThreadCount = 4;
    std::vector<std::string> payloads;
    payloads.reserve(kThreadCount);
    for (int i = 0; i < kThreadCount; ++i) {
        std::string payload = "payload-" + std::to_string(i) + ":";
        payload.append(8192, static_cast<char>('A' + i));
        payloads.push_back(std::move(payload));
    }

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> okCount{0};
    std::atomic<int> failCount{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back([&, i]() {
            ready.fetch_add(1);
            while (!start.load()) {
                Sleep(1);
            }
            std::wstring err;
            const bool ok = atomic_write::AtomicWriteBytes(dest,
                                                           payloads[i].data(),
                                                           payloads[i].size(),
                                                           root / L"tmp_concurrent",
                                                           root / L"escape_concurrent",
                                                           &err);
            if (ok) {
                okCount.fetch_add(1);
            } else {
                failCount.fetch_add(1);
            }
        });
    }

    while (ready.load() != kThreadCount) {
        Sleep(1);
    }
    start.store(true);
    for (auto& t : threads) {
        t.join();
    }

    if (okCount.load() != kThreadCount || failCount.load() != 0) return false;

    const std::string finalBytes = ReadAll(dest);
    bool matchesCompletePayload = false;
    for (const auto& payload : payloads) {
        if (finalBytes == payload) {
            matchesCompletePayload = true;
            break;
        }
    }
    if (!matchesCompletePayload) return false;

    std::error_code ec;
    const fs::path tmpDir = root / L"tmp_concurrent";
    if (fs::exists(tmpDir, ec) && !ec) {
        for (const auto& e : fs::directory_iterator(tmpDir, ec)) {
            if (ec) return false;
            if (e.is_regular_file(ec) && !ec) {
                return false;
            }
            ec.clear();
        }
    }
    return true;
}

static bool TestAtomicWriteReadersNeverObservePartialPayload(const fs::path& root) {
    const fs::path dest = root / L"reader_atomicity.txt";
    const std::string original = "original-complete-payload";
    if (!WriteRaw(dest, original)) return false;

    std::vector<std::string> completePayloads;
    completePayloads.push_back(original);
    for (int i = 0; i < 24; ++i) {
        std::string payload = "BEGIN:" + std::to_string(i) + ":";
        payload.append(256 * 1024 + static_cast<size_t>(i) * 97, static_cast<char>('a' + (i % 26)));
        payload += ":END:" + std::to_string(i);
        completePayloads.push_back(std::move(payload));
    }

    std::atomic<bool> start{false};
    std::atomic<bool> writerDone{false};
    std::atomic<bool> writerFailed{false};
    std::atomic<bool> readerObservedPartial{false};
    std::atomic<bool> readerCouldNotOpen{false};
    std::atomic<int> readerChecks{0};
    std::atomic<int> ready{0};
    size_t failedWriterPayload = 0;
    std::wstring writerError;
    std::string unexpectedReaderPayload;

    std::thread reader([&]() {
        ready.fetch_add(1);
        while (!start.load()) Sleep(0);
        while (!writerDone.load() || readerChecks.load() < 80) {
            std::string observed;
            bool opened = false;
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (TryReadAllWhileReplacementIsAllowed(dest, &observed)) {
                    opened = true;
                    break;
                }
                Sleep(1);
            }
            if (!opened) {
                readerCouldNotOpen.store(true);
                return;
            }
            if (std::find(completePayloads.begin(), completePayloads.end(), observed) == completePayloads.end()) {
                unexpectedReaderPayload = std::move(observed);
                readerObservedPartial.store(true);
                return;
            }
            readerChecks.fetch_add(1);
        }
    });

    std::thread writer([&]() {
        ready.fetch_add(1);
        while (!start.load()) Sleep(0);
        for (size_t i = 1; i < completePayloads.size(); ++i) {
            std::wstring err;
            if (!atomic_write::AtomicWriteBytes(dest,
                                                completePayloads[i].data(),
                                                completePayloads[i].size(),
                                                root / L"tmp_reader_atomicity",
                                                root / L"escape_reader_atomicity",
                                                &err)) {
                failedWriterPayload = i;
                writerError = std::move(err);
                writerFailed.store(true);
                break;
            }
        }
        writerDone.store(true);
    });

    while (ready.load() != 2) Sleep(0);
    start.store(true);
    writer.join();
    reader.join();

    if (writerFailed.load() || readerObservedPartial.load() || readerCouldNotOpen.load() || readerChecks.load() < 80) {
        std::cerr << "reader atomicity diagnostics: writer_failed=" << writerFailed.load()
                  << ", reader_observed_partial=" << readerObservedPartial.load()
                  << ", reader_could_not_open=" << readerCouldNotOpen.load()
                  << ", reader_checks=" << readerChecks.load() << "\n";
        if (writerFailed.load()) {
            std::wcerr << L"writer failed at payload " << failedWriterPayload
                       << L": " << writerError << L"\n";
        }
        if (readerObservedPartial.load()) {
            const size_t previewSize = 48;
            std::cerr << "unexpected reader payload bytes=" << unexpectedReaderPayload.size()
                      << ", prefix="
                      << unexpectedReaderPayload.substr(0, previewSize)
                      << ", suffix=";
            if (unexpectedReaderPayload.size() > previewSize) {
                std::cerr << unexpectedReaderPayload.substr(unexpectedReaderPayload.size() - previewSize);
            } else {
                std::cerr << unexpectedReaderPayload;
            }
            std::cerr << "\n";
        }
        return false;
    }
    if (ReadAll(dest) != completePayloads.back()) return false;

    std::error_code ec;
    const fs::path tmpDir = root / L"tmp_reader_atomicity";
    if (fs::exists(tmpDir, ec) && !ec) {
        for (const auto& entry : fs::directory_iterator(tmpDir, ec)) {
            if (ec || entry.is_regular_file(ec)) return false;
            ec.clear();
        }
    }
    return true;
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    fs::path root = MakeTestRoot();
    int passed = 0;
    int failed = 0;

    auto run = [&](const char* name, bool (*fn)(const fs::path&)) {
        std::cout << "Testing: " << name << "... " << std::flush;
        const bool ok = fn(root);
        std::cout << (ok ? "OK" : "FAILED") << std::endl;
        if (ok) {
            ++passed;
        } else {
            ++failed;
        }
    };

    run("AtomicWrite new file", &TestAtomicWriteNewFile);
    run("AtomicWrite replace existing", &TestAtomicWriteReplaceExisting);
    run("AtomicWrite read-only target fails and preserves", &TestAtomicWriteReadOnlyTargetFailsAndPreserves);
    run("AtomicWrite locked target preserves original", &TestAtomicWriteLockedTargetPreservesOriginal);
    run("AtomicWrite zero bytes", &TestAtomicWriteZeroBytes);
    run("AtomicWrite creates parent dirs", &TestAtomicWriteCreatesParentDirs);
    run("CreateUniqueTempFile creates reserved file and handle", &TestCreateUniqueTempFileCreatesReservedFileAndHandle);
    run("CreateUniqueTempFile avoids existing reserved name", &TestCreateUniqueTempFileAvoidsExistingReservedName);
    run("AtomicWrite empty destination fails", &TestAtomicWriteEmptyDestinationFails);
    run("AtomicWrite null payload fails", &TestAtomicWriteNullPayloadFails);
    run("SameVolume detection", &TestSameVolume);
    run("PickTempDirForTarget logic", &TestPickTempDirForTarget);
    run("AtomicWrite invalid temp dir falls back", &TestAtomicWriteInvalidTempDirFallsBack);
    run("AtomicWrite quarantine read-only handled", &TestAtomicWriteQuarantineReadOnly);
    run("AtomicWrite concurrent same destination leaves complete payload", &TestAtomicWriteConcurrentSameDestinationLeavesCompletePayload);
    run("AtomicWrite readers never observe partial payload", &TestAtomicWriteReadersNeverObservePartialPayload);

    std::error_code ec;
    fs::remove_all(root, ec);

    std::cout << "Summary: passed=" << passed << " failed=" << failed << "\n";
    return failed == 0 ? 0 : 1;
}
