#include "workspace/workspace_write_lock.h"
#include "core/atomic_write.h"
#include "core/empty_note_file.h"
#include <winioctl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS: " << message << '\n';
}
std::wstring Self() {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Check(size && size < path.size(), "locate owned executable");
    return {path.data(), size};
}
void Write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output << value;
    Check(static_cast<bool>(output), "prepare disposable original");
}
std::string Read(const fs::path& path) {
    std::string bytes;
    Check(ReadFileBytesWin32(path, bytes), "read disposable original");
    return bytes;
}
// Child uses ordinary Win32 file operations and knows nothing about app mutexes.
int ExternalProbe(const fs::path& path) {
    if (DeleteFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str())) return 11;
    const DWORD deleteError = GetLastError();
    const auto moved = fs::path(path.wstring() + L".external-move");
    if (MoveFileExW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(),
        ToExtendedWin32PathIfAbsoluteLocal(moved).c_str(), 0)) return 12;
    const DWORD moveError = GetLastError();
    return deleteError == ERROR_SHARING_VIOLATION && moveError == ERROR_SHARING_VIOLATION
        ? 0 : static_cast<int>((deleteError << 16) | moveError);
}
void BlockedExternally(const fs::path& path) {
    const auto executable = Self();
    std::wstring command = L"\"" + executable + L"\" --probe \"" + path.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, fs::path(executable).parent_path().c_str(), &startup, &child), "start owned external-operation child");
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 20); WaitForSingleObject(child.hProcess, 5000); }
    DWORD code = 0; const bool gotCode = GetExitCodeProcess(child.hProcess, &code) != FALSE;
    CloseHandle(child.hProcess);
    if (wait != WAIT_OBJECT_0 || !gotCode || code != 0) std::cerr << "External probe wait=" << wait << " exit=" << code << '\n';
    Check(wait == WAIT_OBJECT_0 && gotCode && code == 0, "external process cannot delete or move protected original");
}
void FreeToMove(const fs::path& path) {
    const auto moved = fs::path(path.wstring() + L".closed-move");
    Check(MoveFileExW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(),
        ToExtendedWin32PathIfAbsoluteLocal(moved).c_str(), 0), "closed original can move again");
    Check(MoveFileExW(ToExtendedWin32PathIfAbsoluteLocal(moved).c_str(),
        ToExtendedWin32PathIfAbsoluteLocal(path).c_str(), 0), "restore only the owned moved fixture");
}
void Open(const fs::path& path, const fs::path& previous = {}) {
    std::wstring error;
    DocumentOpenLockCandidate candidate(path, &error);
    if (!candidate.acquired()) std::wcerr << error << '\n';
    Check(candidate.acquired(), "acquire production document owner and source protection");
    candidate.CommitReplacing(previous);
}
std::wstring conflictPath;
std::atomic<bool> injectConflict{false};
std::atomic<HANDLE> injectedHandle{INVALID_HANDLE_VALUE};
void InjectDeleteAccess(const std::wstring& path) noexcept {
    if (path == conflictPath && injectConflict.exchange(false)) {
        injectedHandle.store(CreateFileW(path.c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    }
}
struct ProtectionConflict {
    explicit ProtectionConflict(const fs::path& path) {
        const std::lock_guard<std::recursive_mutex> lock(document_source_protection::mutex);
        conflictPath = document_source_protection::Key(path);
        injectConflict.store(true);
        document_source_protection::beforeOpenForTesting.store(InjectDeleteAccess);
    }
    ~ProtectionConflict() { Release(); }
    void Release() {
        const std::lock_guard<std::recursive_mutex> lock(document_source_protection::mutex);
        document_source_protection::beforeOpenForTesting.store(nullptr);
        const HANDLE handle = injectedHandle.exchange(INVALID_HANDLE_VALUE);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    void CheckInjected() {
        Check(injectedHandle.load() != INVALID_HANDLE_VALUE, "hold a real delete-access conflict after the filesystem operation");
        Check(document_source_protection::HasPendingProtection(), "failed protection is visible as pending");
    }
};
void WaitForAutomaticProtection() {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (document_source_protection::HasPendingProtection() && GetTickCount64() < deadline) Sleep(10);
    Check(!document_source_protection::HasPendingProtection() && !document_source_protection::HasRecoveryFailure(),
        "protection recovers automatically without reopening the document");
}
// Keep a real external-style oplock unacknowledged. The watchdog releases only
// this fixture's handle so a regressed, blocking CreateFile cannot hang tests.
class OplockFixture {
public:
    explicit OplockFixture(const fs::path& path) {
        request_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        owner_ = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        DWORD bytes = 0;
        ready_ = request_.hEvent && done_ && owner_ != INVALID_HANDLE_VALUE &&
            !DeviceIoControl(owner_, FSCTL_REQUEST_OPLOCK_LEVEL_1, nullptr, 0, nullptr, 0, &bytes, &request_) &&
            GetLastError() == ERROR_IO_PENDING;
        if (ready_) {
            try {
                watchdog_ = std::thread([this] {
                    if (WaitForSingleObject(done_, 3000) != WAIT_OBJECT_0) {
                        forcedRelease_.store(true);
                        CloseOwner();
                    }
                });
            } catch (...) { Finish(); throw; }
        }
    }
    ~OplockFixture() { Finish(); }
    OplockFixture(const OplockFixture&) = delete;
    OplockFixture& operator=(const OplockFixture&) = delete;
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] bool held() const { return ready_ && !forcedRelease_.load(); }
    void Finish() {
        if (done_) SetEvent(done_);
        if (watchdog_.joinable()) watchdog_.join();
        CloseOwner();
        if (done_) { CloseHandle(done_); done_ = nullptr; }
        if (request_.hEvent) { CloseHandle(request_.hEvent); request_.hEvent = nullptr; }
    }
private:
    void CloseOwner() {
        if (owner_ == INVALID_HANDLE_VALUE) return;
        if (ready_) {
            (void)CancelIoEx(owner_, &request_);
            DWORD bytes = 0;
            (void)GetOverlappedResult(owner_, &request_, &bytes, TRUE);
        }
        CloseHandle(owner_); owner_ = INVALID_HANDLE_VALUE;
    }
    HANDLE owner_ = INVALID_HANDLE_VALUE, done_ = nullptr;
    OVERLAPPED request_{};
    bool ready_ = false;
    std::atomic<bool> forcedRelease_{false};
    std::thread watchdog_;
};
void OplockTests(const fs::path& root) {
    const auto path = root / L"oplock-protected.txt";
    Write(path, "original bytes under an external oplock");
    {
        OplockFixture oplock(path);
        Check(oplock.ready(), "acquire a real fixture oplock before opening the document");
        Open(path);
        Check(oplock.held(), "source protection opens before the external oplock is acknowledged");
    }
    BlockedExternally(path);
    {
        ProtectionConflict conflict(path);
        DWORD error = 0;
        {
            document_source_protection::Mutation mutation(path, path);
            Check(!mutation.Finish(false, &error) && error == ERROR_SHARING_VIOLATION,
                "prepare pending recovery before the external oplock");
        }
        conflict.CheckInjected();
        std::unique_lock<std::recursive_mutex> lock(document_source_protection::mutex);
        conflict.Release();
        OplockFixture oplock(path);
        Check(oplock.ready(), "acquire a real oplock while recovery is paused");
        lock.unlock();
        WaitForAutomaticProtection();
        Check(oplock.held(), "automatic recovery completes before the external oplock is acknowledged");
        ReleaseDocumentOpenLock(path);
        Check(oplock.held(), "closing the protected document does not require the external oplock acknowledgement");
    }
    Check(Read(path) == "original bytes under an external oplock", "oplock protection and recovery preserve original bytes");
    FreeToMove(path);
}
void RecoveryTests(const fs::path& root) {
    const auto atomic = root / L"atomic-recovery.txt";
    Write(atomic, "before replacement"); Open(atomic);
    {
        ProtectionConflict conflict(atomic);
        std::wstring error;
        Check(!atomic_write::AtomicWriteUtf8(atomic, "published replacement", root / L"tmp", root / L"escape", &error),
            "published but unprotected save reports failure to retain caller recovery data");
        conflict.CheckInjected();
        Check(Read(atomic) == "published replacement" && error.find(L"was replaced") != std::wstring::npos,
            "save error distinguishes published bytes from failed protection");
        conflict.Release();
        WaitForAutomaticProtection(); BlockedExternally(atomic);
    }
    ReleaseDocumentOpenLock(atomic); FreeToMove(atomic);
    const auto from = root / L"move-from.txt", to = root / L"move-to.txt";
    Write(from, "moved original"); Open(from);
    {
        ProtectionConflict conflict(to);
        const auto result = document_source_protection::Rename(from, to);
        conflict.CheckInjected();
        Check(result.moved && !result.operationError && result.protectionError == ERROR_SHARING_VIOLATION &&
            !fs::exists(from) && Read(to) == "moved original", "completed move and failed protection have distinct results");
        Open(to, from);
        Check(document_source_protection::HasPendingProtection(), "logical owner follows a completed move during recovery");
        conflict.Release();
        WaitForAutomaticProtection(); BlockedExternally(to);
    }
    {
        ProtectionConflict conflict(to);
        const auto result = document_source_protection::Rename(to, root / L"no-parent" / L"destination.txt");
        conflict.CheckInjected();
        Check(!result.moved && result.operationError && result.operationError.value() != ERROR_SHARING_VIOLATION &&
            result.protectionError == ERROR_SHARING_VIOLATION && Read(to) == "moved original",
            "failed move keeps its filesystem error and original separately from protection failure");
        conflict.Release(); WaitForAutomaticProtection(); BlockedExternally(to);
    }
    {
        ProtectionConflict conflict(to);
        document_source_protection::Mutation mutation(to, to);
        DWORD error = 0;
        Check(!mutation.Finish(false, &error), "prepare close while protection is pending");
        conflict.CheckInjected();
        ReleaseDocumentOpenLock(to);
        Check(!document_source_protection::HasPendingProtection(), "close removes the pending recovery owner");
        conflict.Release(); Sleep(250); FreeToMove(to);
    }
    const auto directory = root / L"directory-from", movedDirectory = root / L"directory-to";
    const auto child = directory / L"note.txt", movedChild = movedDirectory / L"note.txt";
    Write(child, "directory original"); Open(child);
    {
        ProtectionConflict conflict(movedChild);
        const auto result = document_source_protection::Rename(directory, movedDirectory);
        conflict.CheckInjected();
        Check(result.moved && !result.operationError && result.protectionError == ERROR_SHARING_VIOLATION &&
            !fs::exists(directory) && Read(movedChild) == "directory original",
            "completed directory move is reported correctly despite failed child protection");
        conflict.Release(); WaitForAutomaticProtection(); BlockedExternally(movedChild);
    }
    ReleaseDocumentOpenLock(child); FreeToMove(movedChild);
}
void EmptyDeletionTests(const fs::path& root) {
    const auto path = root / L"empty deletion.clro";
    const std::vector<std::string> emptyBodies{"", "\xEF\xBB\xBF", "\xFF\xFE", "\xFE\xFF"};
    for (const auto& bytes : emptyBodies) {
        Write(path, bytes); Open(path);
        Check(empty_note_file::IsSavedEmpty(path), "zero-byte or BOM-only note has an empty body");
        DWORD error = 0;
        Check(empty_note_file::Remove(path, &error) == empty_note_file::Result::Deleted && !fs::exists(path),
            "explicit empty deletion succeeds despite this app's source handle");
        Check(!document_source_protection::HasPendingProtection(), "successful deletion does not request recovery of the removed file");
        ReleaseDocumentOpenLock(path);
    }
    for (const std::string bytes : {std::string(" "), std::string("\r\n"), std::string("\xEF\xBB\xBFx"),
                                   std::string("\xFF\xFE\0\0", 4), std::string("content")}) {
        Write(path, bytes); Open(path);
        DWORD error = 0;
        Check(!empty_note_file::IsSavedEmpty(path) &&
              empty_note_file::Remove(path, &error) == empty_note_file::Result::NotEmpty && Read(path) == bytes,
            "nonempty bytes, including whitespace and UTF-16 NUL, are preserved");
        BlockedExternally(path); ReleaseDocumentOpenLock(path);
    }
    Write(path, ""); Open(path);
    HANDLE other = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(other != INVALID_HANDLE_VALUE, "hold an independent reader during deletion");
    DWORD error = 0;
    const auto denied = empty_note_file::Remove(path, &error);
    CloseHandle(other);
    Check(denied == empty_note_file::Result::Failed && error == ERROR_SHARING_VIOLATION && fs::exists(path),
        "another handle still prevents deletion and preserves the original");
    BlockedExternally(path);
    Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), "make the empty original read-only");
    Check(empty_note_file::Remove(path, &error) == empty_note_file::Result::Failed && fs::exists(path),
        "read-only empty originals are retained on failure");
    Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL), "restore the owned empty original");
    BlockedExternally(path); ReleaseDocumentOpenLock(path);
    Check(!empty_note_file::IsSavedEmpty(root) &&
          empty_note_file::Remove(root, &error) != empty_note_file::Result::Deleted && fs::is_directory(root),
        "directories are never removed by the empty-note boundary");
    Check(empty_note_file::Remove(L"relative.clro", &error) == empty_note_file::Result::Failed,
        "relative paths never select a deletion target");
}

void Tests(const fs::path& root) {
    const auto pdf = root / L"sample.pdf", note = root / L"ノート 空白.txt";
    Write(pdf, "pdf fixture bytes"); Write(note, "original note");
    Open(pdf); Open(note);
    BlockedExternally(pdf); BlockedExternally(note);
    std::string bytes;
    Check(ReadFileBytesWin32(note, bytes) && bytes == "original note", "normal reads remain allowed while protected");
    const auto temp = root / L"tmp", escape = root / L"escape";
    std::wstring error;
    Check(atomic_write::AtomicWriteUtf8(note, "saved note", temp, escape, &error), "own atomic save succeeds with document open");
    Check(Read(note) == "saved note", "own atomic save publishes complete contents");
    BlockedExternally(note); BlockedExternally(pdf);
    Check(SetFileAttributesW(note.c_str(), FILE_ATTRIBUTE_READONLY), "make owned note read-only");
    Check(!atomic_write::AtomicWriteUtf8(note, "must not publish", temp, escape, &error), "read-only save fails without releasing protection");
    Check(Read(note) == "saved note", "failed save preserves original bytes");
    Check(SetFileAttributesW(note.c_str(), FILE_ATTRIBUTE_NORMAL), "restore owned note attributes");
    BlockedExternally(note);
    {
        HANDLE lock = CreateFileW(note.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(lock != INVALID_HANDLE_VALUE, "independent reader denies own replacement");
        const bool saved = atomic_write::AtomicWriteUtf8(note, "denied save", temp, escape, &error);
        CloseHandle(lock);
        Check(!saved && Read(note) == "saved note", "replacement failure preserves original and reports failure");
        BlockedExternally(note);
    }
    const auto failed = root / L"failed-open.txt"; Write(failed, "failed load");
    { DocumentOpenLockCandidate failedOpen(failed, &error); Check(failedOpen.acquired(), "prepare failed-open candidate"); }
    FreeToMove(failed); BlockedExternally(note);
    const auto next = root / L"next.txt"; Write(next, "next note");
    Open(next, note); FreeToMove(note); BlockedExternally(next); BlockedExternally(pdf);
    const auto renamed = root / L"renamed.txt";
    std::error_code moveError;
    moveError = document_source_protection::Rename(next, renamed).operationError;
    Check(!moveError && Read(renamed) == "next note", "app move rebinds protection to destination");
    BlockedExternally(renamed);
    Open(renamed, next); ReleaseDocumentOpenLock(renamed); FreeToMove(renamed);
    const auto directory = root / L"session", movedDirectory = root / L"moved-session";
    const auto child = directory / L"nested" / L"note.txt";
    const auto movedChild = movedDirectory / L"nested" / L"note.txt";
    Write(child, "directory child"); Open(child);
    moveError = document_source_protection::Rename(directory, movedDirectory).operationError;
    Check(!moveError, "own ancestor directory rename succeeds");
    BlockedExternally(movedChild);
    ReleaseDocumentOpenLock(child); FreeToMove(movedChild);
    Open(movedChild);
    moveError = document_source_protection::Rename(movedDirectory, directory).operationError;
    Check(!moveError, "own directory rollback restores source protection");
    BlockedExternally(child);
    ReleaseDocumentOpenLock(movedChild); FreeToMove(child);
    Open(child);
    moveError = document_source_protection::Rename(child, root / L"missing-parent" / L"note.txt").operationError;
    Check(static_cast<bool>(moveError) && Read(child) == "directory child", "failed move retains original bytes");
    BlockedExternally(child); ReleaseDocumentOpenLock(child); FreeToMove(child);
    const auto missing = root / L"stage-only.txt";
    Open(missing);
    Check(atomic_write::AtomicWriteUtf8(missing, "recovered stage", temp, escape, &error), "stage-only missing original can be safely published");
    BlockedExternally(missing);
    {
        document_source_protection::Mutation mutation(pdf, pdf);
        Check(mutation.ready(), "suspend only the mutation target");
        BlockedExternally(missing);
        HANDLE external = CreateFileW(pdf.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(external != INVALID_HANDLE_VALUE, "simulate external delete-access conflict during owned mutation");
        DWORD failure = 0;
        const bool restored = mutation.Finish(false, &failure);
        Check(!restored && failure == ERROR_SHARING_VIOLATION, "failed protection reacquisition is an explicit failure");
        Check(document_source_protection::HasPendingProtection(), "failed protection remains registered for recovery");
        CloseHandle(external);
        WaitForAutomaticProtection(); BlockedExternally(pdf);
    }
    ReleaseAllDocumentOpenLocks();
    FreeToMove(pdf); FreeToMove(missing);
    {
        HANDLE external = CreateFileW(pdf.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(external != INVALID_HANDLE_VALUE, "hold external delete access before opening");
        { DocumentOpenLockCandidate candidate(pdf, &error); Check(!candidate.acquired(), "open fails when source cannot be protected"); }
        CloseHandle(external);
        Open(pdf); BlockedExternally(pdf); ReleaseDocumentOpenLock(pdf); FreeToMove(pdf);
    }
    RecoveryTests(root);
    OplockTests(root);
    EmptyDeletionTests(root);
    const auto longPath = root / std::wstring(100, L'a') / std::wstring(100, L'b') / L"長いパス 空白.txt";
    const auto extendedPath = fs::path(ToExtendedWin32PathIfAbsoluteLocal(longPath));
    Write(extendedPath, "protected long local path"); Open(longPath);
    BlockedExternally(longPath);
    Check(atomic_write::AtomicWriteUtf8(extendedPath, "saved through native extended path", root / L"tmp", root / L"escape", &error),
        "internal extended-path save matches the displayed long-path protection");
    Check(Read(longPath) == "saved through native extended path", "extended-path save publishes complete bytes");
    {
        DocumentOpenLockCandidate unsupportedAlias(extendedPath, &error);
        Check(!unsupportedAlias.acquired(), "document input aliases stay rejected until stage identity supports them");
    }
    BlockedExternally(longPath);
    ReleaseDocumentOpenLock(longPath); FreeToMove(longPath);
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--probe") return ExternalProbe(argv[2]);
    try {
        const auto root = fs::path(Self()).parent_path() / (L"fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Check(fs::create_directory(root), "create fresh owned fixture below test executable");
        Tests(root);
        std::cout << "Document source protection tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        ReleaseAllDocumentOpenLocks(); std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
