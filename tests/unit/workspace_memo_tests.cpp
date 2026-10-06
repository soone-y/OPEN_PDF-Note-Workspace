#include "workspace/workspace_memo_store.h"
#include "core/atomic_write.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using workspace_memo::Document;
using workspace_memo::Result;
namespace fs = std::filesystem;
static int checks = 0;
static void Require(bool value, const char* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
static void Write(const fs::path& path, const std::string& value) {
    std::wstring error;
    Require(atomic_write::AtomicWriteUtf8(path, value, path.parent_path(), &error), "fixture write");
}
static std::string Read(const fs::path& path) {
    std::string bytes;
    Require(ReadFileBytesWin32(path, bytes), "fixture read");
    return bytes;
}
using workspace_memo::testing::FaultPoint;
static unsigned faultMask = 0;
static bool crashAfterBackup = false;
static fs::path collisionPath;
static unsigned Bit(FaultPoint point) { return 1u << static_cast<unsigned>(point); }
static bool Inject(FaultPoint point) {
    if (point == FaultPoint::AfterBackup && crashAfterBackup) {
        TerminateProcess(GetCurrentProcess(), 77); // no destructors: actual abrupt child stop
    }
    if (point == FaultPoint::Install && !collisionPath.empty()) {
        Write(collisionPath, "concurrent original");
        return false; // let the real no-replace install and rollback fail
    }
    return (faultMask & Bit(point)) != 0;
}
static void TestBoundaries(const fs::path& fixtures) {
    const auto root = fixtures / L"limits";
    const auto path = workspace_memo::MemoPath(root);
    Write(path, std::string(600000, '\n'));
    Document doc;
    Require(doc.Load(root) == Result::Ok && doc.text().size() == 1200000, "LF expansion loads");
    Require(doc.Save(doc.text()) == Result::Ok && Read(path) == std::string(600000, '\n'), "LF expansion unchanged save");
    std::wstring exact;
    exact.reserve(workspace_memo::kMaxEditorUnits);
    for (size_t i = 0; i < workspace_memo::kMaxBytes; ++i) exact += L"\r\n";
    Require(workspace_memo::ValidateText(exact), "exact LF byte limit with doubled editor units");
    Require(doc.Save(exact) == Result::Ok && Read(path).size() == workspace_memo::kMaxBytes, "exact LF byte limit saves");
    Require(doc.Checkpoint(exact + L'a') == Result::InvalidText, "LF byte overflow rejected");
    Require(!workspace_memo::ValidateText(std::wstring(workspace_memo::kMaxBytes / 3 + 1, L'検')), "Japanese UTF-8 byte limit");
    const auto recovery = path.parent_path() / L"workspace_memo.recovery";
    Require(doc.Checkpoint(exact) == Result::Ok, "clean envelope fixture");
    Write(path, "external after clean checkpoint");
    Document clean;
    Require(clean.Load(root) == Result::Ok && clean.text() == L"external after clean checkpoint", "clean envelope never revives old original");

    Require(clean.Checkpoint(clean.text()) == Result::Ok, "readonly cleanup fixture");
    Require(SetFileAttributesW(recovery.c_str(), FILE_ATTRIBUTE_READONLY), "readonly recovery");
    Write(path, "next external");
    Require(clean.Save(clean.text()) == Result::IoError && clean.text() == L"external after clean checkpoint", "failed settle does not report adoption success");
    Require(Read(path) == "next external", "cleanup failure preserves external original");
    Require(SetFileAttributesW(recovery.c_str(), FILE_ATTRIBUTE_NORMAL), "restore recovery attribute");
    Require(clean.Save(clean.text()) == Result::Ok && clean.text() == L"next external", "cleanup retry adopts external");
    Document retry;
    Require(retry.Load(root) == Result::Ok && retry.text() == L"next external", "cleanup retry restart");
    for (size_t padding = 0; padding < 4; ++padding) {
        const auto alignedRoot = fixtures / (L"rename_alignment" + std::wstring(padding, L'x'));
        Document aligned;
        Require(aligned.Load(alignedRoot) == Result::Ok && aligned.Save(L"base") == Result::Ok, "rename alignment fixture");
        Require(aligned.Checkpoint(L"restored") == Result::Ok, "rename alignment recovery");
        Document alignedRestart;
        Require(alignedRestart.Load(alignedRoot) == Result::Recovered && alignedRestart.Save(alignedRestart.text()) == Result::Ok, "all path buffer alignments save recovery");
        Require(Read(workspace_memo::MemoPath(alignedRoot)) == "restored", "aligned rename original complete");
    }
}
static void TestFailures(const fs::path& fixtures) {
    workspace_memo::testing::SetFaultHook(Inject);
    struct Reset { ~Reset() { workspace_memo::testing::SetFaultHook(nullptr); faultMask = 0; collisionPath.clear(); } } reset;
    for (const auto point : {FaultPoint::CheckpointWrite, FaultPoint::TempWrite, FaultPoint::TempFlush,
                             FaultPoint::BackupRename, FaultPoint::Install}) {
        const auto root = fixtures / (L"failure_" + std::to_wstring(static_cast<unsigned>(point)));
        Document doc;
        Require(doc.Load(root) == Result::Ok && doc.Save(L"base") == Result::Ok, "fault fixture");
        Require(doc.Checkpoint(L"previous draft") == Result::Ok, "previous recovery fixture");
        faultMask = Bit(point);
        Require(doc.Save(L"next draft") == Result::IoError, "injected save phase failure");
        faultMask = 0;
        const auto path = workspace_memo::MemoPath(root);
        Require(Read(path) == "base", "failure leaves original at original path");
        if (point != FaultPoint::CheckpointWrite) {
            bool retained = false;
            for (const auto& entry : fs::directory_iterator(path.parent_path())) {
                if (entry.path().filename().wstring().find(L"workspace_memo.txt.__atomic__.") == 0) {
                    retained = true;
                    Require(Read(entry.path()) == (point == FaultPoint::TempWrite ? "" : "next draft"), "failed temporary write remains inspectable");
                }
            }
            Require(retained, "failed save temporary file retained");
        }
        Document restarted;
        Require(restarted.Load(root) == Result::Recovered &&
                restarted.text() == (point == FaultPoint::CheckpointWrite ? L"previous draft" : L"next draft"), "failure preserves full recovery");
        Require(doc.Save(L"next draft") == Result::Ok, "phase failure retry");
    }
    const auto rollbackRoot = fixtures / L"rollback_failure";
    Document rollback;
    Require(rollback.Load(rollbackRoot) == Result::Ok && rollback.Save(L"base") == Result::Ok, "rollback fixture");
    faultMask = Bit(FaultPoint::Install) | Bit(FaultPoint::Rollback);
    Require(rollback.Save(L"draft") == Result::IoError, "install and rollback failure");
    faultMask = 0;
    const auto rollbackPath = workspace_memo::MemoPath(rollbackRoot);
    Require(!fs::exists(rollbackPath) && Read(rollbackPath.parent_path() / L"backups" / L"workspace_memo.txt") == "base", "failed rollback preserves original backup");
    Document restart;
    Require(restart.Load(rollbackRoot) == Result::Conflict && restart.text() == L"draft", "failed rollback draft recovers");

    const auto collisionRoot = fixtures / L"install_collision";
    Document collision;
    Require(collision.Load(collisionRoot) == Result::Ok && collision.Save(L"base") == Result::Ok, "collision fixture");
    collisionPath = workspace_memo::MemoPath(collisionRoot);
    Require(collision.Save(L"draft") == Result::IoError, "real install collision fails");
    collisionPath.clear();
    Require(Read(workspace_memo::MemoPath(collisionRoot)) == "concurrent original", "never overwrite concurrent new original");
    Require(restart.Load(collisionRoot) == Result::Conflict && restart.text() == L"draft", "collision full draft survives");

    const auto installedRoot = fixtures / L"installed_before_stop";
    Document installed;
    Require(installed.Load(installedRoot) == Result::Ok && installed.Save(L"base") == Result::Ok, "post-install fixture");
    faultMask = Bit(FaultPoint::AfterInstall);
    Require(installed.Save(L"installed draft") == Result::IoError, "interrupt after flushed draft installation");
    faultMask = 0;
    Require(Read(workspace_memo::MemoPath(installedRoot)) == "installed draft", "post-install original is complete");
    Require(restart.Load(installedRoot) == Result::Ok && restart.text() == L"installed draft", "completed installation recognized despite stale envelope");
    Require(installed.Save(L"installed draft") == Result::Ok, "post-install interruption retry");

    const auto cleanupRoot = fixtures / L"delete_failure";
    Document cleanup;
    Require(cleanup.Load(cleanupRoot) == Result::Ok && cleanup.Save(L"base") == Result::Ok && cleanup.Checkpoint(L"base") == Result::Ok, "delete failure fixture");
    Write(workspace_memo::MemoPath(cleanupRoot), "external");
    faultMask = Bit(FaultPoint::CleanupDelete);
    Require(cleanup.Save(L"base") == Result::Ok && cleanup.text() == L"external", "delete failure settles recovery durably");
    faultMask = 0;
    Write(workspace_memo::MemoPath(cleanupRoot), "external again");
    Require(restart.Load(cleanupRoot) == Result::Ok && restart.text() == L"external again", "settled envelope cannot revive old text");
    Require(cleanup.Checkpoint(L"edited") == Result::Ok, "cleanup failure dirty draft");
    faultMask = Bit(FaultPoint::CleanupDelete) | Bit(FaultPoint::CleanupWrite);
    Require(cleanup.Save(L"external") == Result::IoError, "delete and settle failure blocks adoption");
    faultMask = 0;
    Require(restart.Load(cleanupRoot) == Result::Conflict && restart.text() == L"edited", "cleanup failure preserves dirty recovery");
}
static void TestAbruptStop(const fs::path& fixtures) {
    const auto root = fixtures / L"actual_child_stop";
    Document doc;
    Require(doc.Load(root) == Result::Ok && doc.Save(L"base") == Result::Ok, "abrupt stop fixture");
    std::wstring executable(1024, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        Require(length != 0, "test executable path");
        if (length < executable.size()) { executable.resize(length); break; }
        executable.resize(executable.size() * 2);
    }
    Require(SetEnvironmentVariableW(L"WORKSPACE_MEMO_TEST_CRASH_ROOT", root.c_str()), "child fixture environment");
    struct Environment { ~Environment() { SetEnvironmentVariableW(L"WORKSPACE_MEMO_TEST_CRASH_ROOT", nullptr); } } environment;
    std::wstring command = L"\"" + executable + L"\" --crash";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process), "launch disposable crash child");
    struct Close { PROCESS_INFORMATION process; ~Close() { CloseHandle(process.hThread); CloseHandle(process.hProcess); } } close{process};
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 78); WaitForSingleObject(process.hProcess, 1000); }
    Require(wait == WAIT_OBJECT_0, "crash child terminated");
    DWORD code = 0;
    Require(GetExitCodeProcess(process.hProcess, &code) && code == 77, "abrupt child exit reached real backup phase");
    const auto path = workspace_memo::MemoPath(root);
    Require(!fs::exists(path) && Read(path.parent_path() / L"backups" / L"workspace_memo.txt") == "base", "abrupt stop original backup complete");
    Document restarted;
    Require(restarted.Load(root) == Result::Conflict && restarted.text() == L"crash draft", "actual interrupted save recovers full draft");
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--crash") {
            const DWORD length = GetEnvironmentVariableW(L"WORKSPACE_MEMO_TEST_CRASH_ROOT", nullptr, 0);
            Require(length > 0, "crash root provided");
            std::wstring root(length, L'\0');
            const DWORD copied = GetEnvironmentVariableW(L"WORKSPACE_MEMO_TEST_CRASH_ROOT", root.data(), length);
            Require(copied > 0 && copied < length, "crash root captured"); root.resize(copied);
            const auto prefix = (fs::current_path() / L"out" / L"tests").wstring() + L"\\";
            Require(root.compare(0, prefix.size(), prefix) == 0, "child restricted to disposable tests");
            Document child; Require(child.Load(root) == Result::Ok, "crash child load");
            crashAfterBackup = true; workspace_memo::testing::SetFaultHook(Inject);
            const auto result = child.Save(L"crash draft"); (void)result;
            return 2; // hook must terminate before returning
        }
        // Only ever creates disposable, unique fixtures under out/tests.
        std::error_code ec;
        const auto current = fs::current_path(ec);
        Require(!ec, "fixture current directory");
        const auto fixtures = current / L"out" / L"tests" /
            (L"workspace_memo_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        fs::create_directories(fixtures, ec); Require(!ec, "fixture directory");
        const auto root = fixtures / L"Unicode_日本語 workspace";
        fs::create_directories(root, ec); Require(!ec, "workspace directory");
        const auto path = workspace_memo::MemoPath(root);
        const auto recovery = path.parent_path() / L"workspace_memo.recovery";
        Document doc;
        Require(doc.Load(root) == Result::Ok && doc.text().empty(), "missing means new empty memo");
        Require(doc.Save(L"検索語\r\n資料・12頁 😀") == Result::Ok, "save UTF-8 Unicode");
        const auto initial = Read(path);
        Require(initial.find("\xE6\xA4\x9C") != std::string::npos, "UTF-8 original");
        Document restarted;
        Require(restarted.Load(root) == Result::Ok && restarted.text() == L"検索語\r\n資料・12頁 😀", "reload Unicode");
        Require(restarted.Checkpoint(L"unsaved draft") == Result::Ok, "durable checkpoint");
        Require(Read(path) == initial, "checkpoint never writes original");
        Document crashed;
        Require(crashed.Load(root) == Result::Recovered && crashed.text() == L"unsaved draft", "recover after crash");
        Require(crashed.Save(crashed.text()) == Result::Ok, "integrate recovered draft");
        Require(Read(path.parent_path() / L"backups" / L"workspace_memo.txt") == initial, "previous original backed up");
        Write(path, "external\nversion");
        Require(crashed.Checkpoint(L"local draft") == Result::Ok, "local edited after external change");
        Require(crashed.Save(L"local draft") == Result::Conflict && Read(path) == "external\nversion", "external conflict no overwrite");
        Document conflict;
        Require(conflict.Load(root) == Result::Conflict && conflict.text() == L"local draft", "conflict survives restart");
        Require(conflict.Reload(conflict.text()) == Result::Ok && conflict.text() == L"external\r\nversion", "explicit reload external text");
        Require(Read(path.parent_path() / L"backups" / L"workspace_memo.conflict.txt") == "local draft", "reload archives local draft");
        Require(conflict.Save(conflict.text()) == Result::Ok, "unchanged LF original not rewritten");
        Require(Read(path) == "external\nversion", "LF original preserved");
        Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), "readonly fixture");
        Require(conflict.Save(L"blocked save") == Result::IoError, "readonly save fails");
        Require(Read(path) == "external\nversion" && fs::exists(recovery), "readonly original and draft retained");
        Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL), "restore fixture attributes");
        Require(conflict.Save(L"blocked save") == Result::Ok, "retry failure");
        HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lock != INVALID_HANDLE_VALUE, "lock fixture");
        const auto blocked = conflict.Save(L"sharing blocked");
        CloseHandle(lock);
        Require(blocked == Result::IoError && Read(path) == "blocked save", "sharing lock cannot lose original");
        Require(conflict.Save(L"") == Result::Ok && Read(path).empty(), "empty memo supported");
        Require(conflict.Save(std::wstring(workspace_memo::kMaxBytes + 1, L'a')) == Result::InvalidText, "limit protected");
        Require(conflict.Save(std::wstring(1, static_cast<wchar_t>(0xD800))) == Result::InvalidText, "reject unpaired surrogate");
        Write(path, "\xFF");
        Document invalid;
        Require(invalid.Load(root) == Result::InvalidText && !invalid.writable(), "invalid UTF-8 blocks editing");
        Require(invalid.Save(L"overwrite") == Result::IoError && Read(path) == "\xFF", "invalid original never overwritten");
        Write(path, "valid"); Write(recovery, "invalid envelope");
        Require(invalid.Load(root) == Result::InvalidText && !invalid.writable(), "corrupt recovery blocks save");
        Require(Read(recovery) == "invalid envelope", "corrupt recovery retained");
        const auto fresh = fixtures / L"other";
        fs::create_directories(fresh, ec); Require(!ec, "fresh directory");
        Document other;
        Require(other.Load(fresh) == Result::Ok && other.Save(L"other memo") == Result::Ok, "second workspace isolated");
        Require(Read(path) == "valid", "new root never modifies old root");
        Write(workspace_memo::MemoPath(fresh), "outside change");
        Require(other.Save(L"other memo") == Result::Ok && other.text() == L"outside change", "no edits adopts external original");
        Document adopted;
        Require(adopted.Load(fresh) == Result::Ok && adopted.text() == L"outside change", "adoption removes stale recovery");
        const auto deleted = fixtures / L"deleted";
        fs::create_directories(deleted, ec); Require(!ec, "crash directory");
        Document gap;
        Require(gap.Load(deleted) == Result::Ok && gap.Save(L"base") == Result::Ok, "gap fixture");
        Require(gap.Checkpoint(L"draft") == Result::Ok, "gap draft");
        // Simulate the protected rename/install crash gap, not destructive user data.
        const auto gapPath = workspace_memo::MemoPath(deleted);
        fs::rename(gapPath, gapPath.parent_path() / L"crash-backup.txt", ec); Require(!ec, "simulate rename gap");
        Document gapRestart;
        Require(gapRestart.Load(deleted) == Result::Conflict && gapRestart.text() == L"draft", "missing original crash gap still recoverable");
        Require(Read(gapPath.parent_path() / L"crash-backup.txt") == "base", "gap original retained");
        Require(doc.Load(L"C:relative") == Result::IoError, "reject drive-relative path");
        Require(doc.Load(L"\\\\server\\share") == Result::IoError, "reject UNC before I/O");
        auto deep = fixtures;
        for (int i = 0; i < 13; ++i) deep /= L"long_component_1234567890";
        Require(atomic_write::EnsureDirectoryExists(deep), "long fixture directory");
        Document longPath;
        Require(longPath.Load(deep) == Result::Ok && longPath.Save(L"long path") == Result::Ok, "long path save");
        Document longRestart;
        Require(longRestart.Load(deep) == Result::Ok && longRestart.text() == L"long path", "long path reload");
        TestBoundaries(fixtures);
        TestFailures(fixtures);
        TestAbruptStop(fixtures);
        std::cout << "Workspace memo: " << checks << " checks passed. Fixtures retained under out/tests.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
