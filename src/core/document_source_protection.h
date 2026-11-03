#pragma once

#include "core/path_safety.h"
#include <winternl.h>
#include <climits>
#include <cstring>
#include <cwctype>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>

// Shared by document ownership and atomic output, including standalone users of
// atomic_write.h. No file is created, changed, or recovered by this boundary.
namespace document_source_protection {
#if defined(PDF_NOTE_SOURCE_PROTECTION_TESTING)
using BeforeOpenHook = void (*)(const std::wstring&) noexcept;
inline std::atomic<BeforeOpenHook> beforeOpenForTesting{nullptr};
#endif
inline std::wstring Key(const std::filesystem::path& path) {
    // MinGW filesystem does not recognize an extended drive path as absolute.
    // Resolve its ordinary drive form so both spellings share one owner key.
    const auto& raw = path.native();
    const bool extendedDrive = raw.size() >= 7 && raw.compare(0, 4, L"\\\\?\\") == 0 &&
        raw[5] == L':' && raw[6] == L'\\';
    const std::filesystem::path local = extendedDrive ? std::filesystem::path(raw.substr(4)) : path;
    if (local.empty() || !local.is_absolute() || IsUncPath(local)) return {};
    std::error_code error;
    auto full = std::filesystem::weakly_canonical(local, error);
    if (error) full = local.lexically_normal();
    auto key = ToExtendedWin32PathIfAbsoluteLocal(full);
    for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
    return key;
}

// Open existing local files only, retaining data-read sharing semantics that
// enforce denial of DELETE. FILE_COMPLETE_IF_OPLOCKED returns a usable handle
// without waiting for the external oplock acknowledgement. We never read data
// through this handle. Do not fall back to a potentially blocking CreateFile.
[[nodiscard]] inline HANDLE OpenWithoutOplockWait(const std::wstring& path, DWORD* error) {
    using Create = decltype(&NtCreateFile);
    using MapError = decltype(&RtlNtStatusToDosError);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const FARPROC createAddress = module ? GetProcAddress(module, "NtCreateFile") : nullptr;
    const FARPROC errorAddress = module ? GetProcAddress(module, "RtlNtStatusToDosError") : nullptr;
    Create create = nullptr;
    MapError mapError = nullptr;
    static_assert(sizeof(create) == sizeof(createAddress) && sizeof(mapError) == sizeof(errorAddress));
    std::memcpy(&create, &createAddress, sizeof(create));
    std::memcpy(&mapError, &errorAddress, sizeof(mapError));
    if (!create || !mapError) { *error = ERROR_CALL_NOT_IMPLEMENTED; return INVALID_HANDLE_VALUE; }
    if (path.size() < 7 || path.compare(0, 4, L"\\\\?\\") != 0 ||
        path[5] != L':' || path[6] != L'\\' || path.size() > USHRT_MAX / sizeof(wchar_t)) {
        *error = ERROR_BAD_PATHNAME; return INVALID_HANDLE_VALUE;
    }
    std::wstring nativePath;
    try { nativePath = path; }
    catch (...) { *error = ERROR_NOT_ENOUGH_MEMORY; return INVALID_HANDLE_VALUE; }
    nativePath[1] = L'?'; // \\?\C:\... -> \??\C:\...
    UNICODE_STRING name{};
    name.Buffer = nativePath.data();
    name.Length = name.MaximumLength = static_cast<USHORT>(nativePath.size() * sizeof(wchar_t));
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK io{};
    HANDLE handle = nullptr;
    const NTSTATUS status = create(&handle, FILE_GENERIC_READ, &attributes, &io, nullptr,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_COMPLETE_IF_OPLOCKED,
        nullptr, 0);
    if (status < 0) { *error = mapError(status); return INVALID_HANDLE_VALUE; }
    if (!handle || handle == INVALID_HANDLE_VALUE) { *error = ERROR_INVALID_HANDLE; return INVALID_HANDLE_VALUE; }
    *error = ERROR_SUCCESS;
    return handle;
}

struct Source {
    Source() = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Source() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    [[nodiscard]] bool Open(const std::wstring& path, bool allowMissing, DWORD* error) {
#if defined(PDF_NOTE_SOURCE_PROTECTION_TESTING)
        if (const auto hook = beforeOpenForTesting.load()) hook(path);
#endif
        handle = OpenWithoutOplockWait(path, error);
        if (handle == INVALID_HANDLE_VALUE) {
            if (allowMissing && (*error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND)) {
                *error = ERROR_SUCCESS;
                return true;
            }
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            *error = ERROR_INVALID_DATA;
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
            return false;
        }
        return true;
    }
};
inline std::recursive_mutex mutex;
inline std::map<std::wstring, std::shared_ptr<Source>> sources;
inline std::condition_variable_any recoveryWake;
inline std::atomic<bool> pendingProtection{false};
inline std::atomic<bool> recoveryFailed{false};
inline std::atomic<HWND> notificationWindow{nullptr};
inline std::atomic<UINT> notificationMessage{0};

inline void NotifyProtectionState() noexcept {
    const HWND window = notificationWindow.load();
    if (window) PostMessageW(window, notificationMessage.load(), 0, 0);
}
// Called under the registry lock. Notifications carry no file data or pointers;
// only the UI owner interprets them, and it reads the current state again.
inline void UpdateProtectionStateLocked() {
    bool pending = false;
    for (const auto& entry : sources)
        pending = pending || entry.second->handle == INVALID_HANDLE_VALUE;
    if (pendingProtection.exchange(pending) != pending) NotifyProtectionState();
    recoveryWake.notify_all();
}
[[nodiscard]] inline bool HasPendingProtection() noexcept { return pendingProtection.load(); }
[[nodiscard]] inline bool HasRecoveryFailure() noexcept { return recoveryFailed.load(); }
inline void SetNotificationWindow(HWND window, UINT message) noexcept {
    if (!window) notificationWindow.store(nullptr);
    notificationMessage.store(message);
    notificationWindow.store(window);
}

// Process-owned worker: it never writes, moves or creates a file. Retry only
// registered originals, including their remapped physical paths. Destruction
// joins the worker before the registry/mutex globals are destroyed.
class RecoveryWorker {
public:
    RecoveryWorker() : thread_([this] { Run(); }) {}
    ~RecoveryWorker() {
        { const std::lock_guard<std::recursive_mutex> lock(mutex); stopping_ = true; }
        recoveryWake.notify_all();
        thread_.join();
    }
    RecoveryWorker(const RecoveryWorker&) = delete;
    RecoveryWorker& operator=(const RecoveryWorker&) = delete;
private:
    void Run() noexcept {
        try {
            std::unique_lock<std::recursive_mutex> lock(mutex);
            while (!stopping_) {
                recoveryWake.wait(lock, [this] { return stopping_ || HasPendingProtection(); });
                if (stopping_) break;
                recoveryWake.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping_; });
                if (stopping_) break;
                for (const auto& entry : sources) {
                    if (entry.second->handle == INVALID_HANDLE_VALUE) {
                        DWORD error = 0;
                        (void)entry.second->Open(entry.second->path, false, &error);
                    }
                }
                UpdateProtectionStateLocked();
            }
        } catch (...) {
            recoveryFailed.store(true);
            NotifyProtectionState();
        }
    }
    bool stopping_ = false;
    std::thread thread_;
};
[[nodiscard]] inline bool EnsureRecoveryWorker(DWORD* error) {
    try { static RecoveryWorker worker; }
    catch (...) { *error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    if (HasRecoveryFailure()) { *error = ERROR_GEN_FAILURE; return false; }
    return true;
}

// The document owner calls Acquire before loading and Release only on close,
// failed open, or successful switch. A missing original may still have a stage
// to recover; it is tracked but cannot be locked until the app publishes it.
[[nodiscard]] inline bool Acquire(const std::filesystem::path& path, DWORD* error) {
    const std::lock_guard<std::recursive_mutex> lock(mutex);
    const auto key = Key(path);
    if (key.empty()) { *error = ERROR_BAD_PATHNAME; return false; }
    if (!EnsureRecoveryWorker(error)) return false;
    for (const auto& entry : sources) {
        if (entry.second->path == key) {
            // An existing owner can follow its completed move while recovery
            // is pending. This does not admit an unrelated, unprotectable open.
            sources[key] = entry.second;
            UpdateProtectionStateLocked();
            return true;
        }
    }
    auto source = std::make_shared<Source>();
    if (!source->Open(key, true, error)) return false;
    source->path = key;
    sources[key] = std::move(source);
    UpdateProtectionStateLocked();
    return true;
}
inline void Release(const std::filesystem::path& path) {
    const std::lock_guard<std::recursive_mutex> lock(mutex);
    sources.erase(Key(path));
    UpdateProtectionStateLocked();
}
inline void ReleaseAll() {
    const std::lock_guard<std::recursive_mutex> lock(mutex);
    sources.clear();
    UpdateProtectionStateLocked();
}

// Windows share restrictions also apply to this process. Suspend only the
// app-owned handles for one replacement/rename syscall sequence, restore them
// before retry waits or returning. Failed reacquisition remains registered for
// process-owned recovery; file mutation and protection are separate outcomes.
// Stage, backup, and the existing transaction recovery remain the data owners.
class Mutation {
public:
    Mutation(const std::filesystem::path& from, const std::filesystem::path& to, bool subtree = false)
        : lock_(mutex) {
        if (sources.empty()) return;
        const auto fromKey = Key(from), toKey = Key(to);
        if (fromKey.empty() || toKey.empty()) { error_ = ERROR_BAD_PATHNAME; return; }
        for (const auto& entry : sources) {
            const auto& path = entry.second->path;
            const bool child = subtree && path.size() > fromKey.size() &&
                path.compare(0, fromKey.size(), fromKey) == 0 && path[fromKey.size()] == L'\\';
            bool duplicate = false;
            for (const auto& prior : entries_) if (prior.source == entry.second) duplicate = true;
            if (!duplicate && (path == fromKey || child)) {
                entries_.push_back({path, toKey + path.substr(fromKey.size()), entry.second});
            }
        }
        for (auto& entry : entries_) {
            if (entry.source->handle != INVALID_HANDLE_VALUE) CloseHandle(entry.source->handle);
            entry.source->handle = INVALID_HANDLE_VALUE;
        }
    }
    ~Mutation() { if (!finished_) { DWORD ignored = 0; (void)Finish(false, &ignored); } }
    Mutation(const Mutation&) = delete;
    Mutation& operator=(const Mutation&) = delete;
    [[nodiscard]] bool ready() const { return error_ == ERROR_SUCCESS; }
    [[nodiscard]] DWORD error() const { return error_; }
    [[nodiscard]] bool Finish(bool changed, DWORD* error) {
        if (finished_) { *error = ERROR_INVALID_STATE; return false; }
        finished_ = true;
        bool ok = ready();
        *error = error_;
        for (auto& entry : entries_) {
            const auto& target = changed ? entry.to : entry.from;
            DWORD openError = 0;
            if (!entry.source->Open(target, false, &openError)) { ok = false; *error = openError; }
            // The map key stays with the logical document owner until close.
            // A successful reopen can share this same handle at its new path.
            if (changed) entry.source->path.swap(entry.to);
        }
        UpdateProtectionStateLocked();
        lock_.unlock();
        return ok;
    }
    // Call only after a verified deletion has completed. Failure restores the
    // original handles; success retires their registrations without recreating
    // the deleted file or scheduling recovery for an intentionally removed path.
    [[nodiscard]] bool FinishRemoval(bool removed, DWORD* error) {
        if (!removed) return Finish(false, error);
        if (finished_) { *error = ERROR_INVALID_STATE; return false; }
        if (!ready()) return Finish(false, error);
        finished_ = true;
        *error = ERROR_SUCCESS;
        for (const auto& entry : entries_) {
            for (auto it = sources.begin(); it != sources.end();) {
                if (it->second == entry.source) it = sources.erase(it);
                else ++it;
            }
        }
        UpdateProtectionStateLocked();
        lock_.unlock();
        return true;
    }
private:
    struct Entry { std::wstring from, to; std::shared_ptr<Source> source; };
    std::unique_lock<std::recursive_mutex> lock_;
    std::vector<Entry> entries_;
    DWORD error_ = ERROR_SUCCESS;
    bool finished_ = false;
};

struct RenameResult {
    std::error_code operationError;
    DWORD protectionError = ERROR_SUCCESS;
    bool moved = false;
};
[[nodiscard]] inline RenameResult Rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    RenameResult result;
    Mutation mutation(from, to, true);
    if (!mutation.ready()) {
        result.operationError.assign(mutation.error(), std::system_category());
        return result;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    result.operationError = ec;
    result.moved = !ec;
    (void)mutation.Finish(result.moved, &result.protectionError);
    return result;
}
} // namespace document_source_protection
