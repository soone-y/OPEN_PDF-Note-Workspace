#pragma once
#include "settings/palette_presets_store.h"
#include <sddl.h>
#include <string>
#include <vector>

// Test-only helpers. Caller supplies its already validated out/tests fixture.
// Worker performs no writes; named events coordinate a real cross-process lock.
namespace palette_test {
[[nodiscard]] inline bool RunLockWorker(const std::filesystem::path& fixture, const std::wstring& token) {
    const auto readyName = L"Local\\PDFNotePaletteTestReady_" + token;
    const auto releaseName = L"Local\\PDFNotePaletteTestRelease_" + token;
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, releaseName.c_str());
    bool ok = false;
    if (ready && release) {
        palette_stock::WriteLock lock(fixture / L"palette_presets_fixture");
        ok = lock.held() && SetEvent(ready) && WaitForSingleObject(release, 10000) == WAIT_OBJECT_0;
    }
    if (ready) CloseHandle(ready);
    if (release) CloseHandle(release);
    return ok;
}

class OwnedLockWorker {
public:
    OwnedLockWorker(const std::filesystem::path& fixture, const std::filesystem::path& driver) {
        const auto token = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        const auto readyName = L"Local\\PDFNotePaletteTestReady_" + token;
        const auto releaseName = L"Local\\PDFNotePaletteTestRelease_" + token;
        std::wstring command = L"\"" + driver.wstring() + L"\" --palette-lock-helper " + token;
        // Working directory matches the driver's owned-fixture prefix check.
        const auto working = fixture.parent_path().parent_path().parent_path();
        ready_ = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        if (!ready_) return;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(ready_); ready_ = nullptr; return; }
        release_ = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!release_) return;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(release_); release_ = nullptr; return; }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        if (!CreateProcessW(driver.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, working.c_str(), &startup, &process_)) return;
        readyObserved_ = WaitForSingleObject(ready_, 5000) == WAIT_OBJECT_0;
    }
    ~OwnedLockWorker() {
        if (release_) SetEvent(release_);
        if (process_.hProcess) {
            if (WaitForSingleObject(process_.hProcess, 1000) != WAIT_OBJECT_0) {
                // Only the exact helper process created by this object.
                TerminateProcess(process_.hProcess, 79);
                WaitForSingleObject(process_.hProcess, 1000);
            }
            CloseHandle(process_.hThread); CloseHandle(process_.hProcess);
        }
        if (release_) CloseHandle(release_);
        if (ready_) CloseHandle(ready_);
    }
    [[nodiscard]] bool ready() const { return readyObserved_; }
    [[nodiscard]] bool ReleaseAndJoin() {
        DWORD code = 1;
        return readyObserved_ && SetEvent(release_) && WaitForSingleObject(process_.hProcess, 5000) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(process_.hProcess, &code) && code == 0;
    }
    OwnedLockWorker(const OwnedLockWorker&) = delete;
    OwnedLockWorker& operator=(const OwnedLockWorker&) = delete;
private:
    HANDLE ready_ = nullptr, release_ = nullptr;
    PROCESS_INFORMATION process_{};
    bool readyObserved_ = false;
};

// Preserve the exact original ACL; deny writes only to the owned recovery folder.
class DenyDirectoryWrite {
public:
    explicit DenyDirectoryWrite(const std::filesystem::path& directory)
        : path_(ToExtendedWin32PathIfAbsoluteLocal(directory)) {
        DWORD needed = 0;
        GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
        if (!needed) return;
        security_.resize(needed);
        if (!GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, security_.data(), needed, &needed)) return;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;OICI;GW;;;WD)(A;OICI;GA;;;WD)",
            SDDL_REVISION_1, &descriptor, nullptr)) return;
        applied_ = SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, descriptor) != FALSE;
        LocalFree(descriptor);
    }
    ~DenyDirectoryWrite() { const bool restored = Restore(); (void)restored; }
    [[nodiscard]] bool applied() const { return applied_; }
    [[nodiscard]] bool Restore() {
        if (!applied_) return true;
        if (!SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, security_.data())) return false;
        applied_ = false; return true;
    }
    DenyDirectoryWrite(const DenyDirectoryWrite&) = delete;
    DenyDirectoryWrite& operator=(const DenyDirectoryWrite&) = delete;
private:
    std::wstring path_;
    std::vector<unsigned char> security_;
    bool applied_ = false;
};
} // namespace palette_test
