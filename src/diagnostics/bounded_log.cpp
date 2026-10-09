#include "diagnostics/bounded_log.h"
#include "core/path_safety.h"

#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <vector>

namespace diagnostic_log {
namespace {
constexpr std::string_view kStopLine =
    "[diagnostic logging stopped: 10 MB workspace limit; delete diagnostic logs to resume]\n";

class Handles {
public:
    ~Handles() { for (HANDLE h : values) CloseHandle(h); }
    Handles(const Handles&) = delete;
    Handles& operator=(const Handles&) = delete;
    Handles() = default;
    void Add(HANDLE h) {
        try { values.push_back(h); } catch (...) { CloseHandle(h); throw; }
    }
    std::vector<HANDLE> values;
};
class BudgetLock {
public:
    ~BudgetLock() {
        if (acquired) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
    [[nodiscard]] bool Acquire(HANDLE workspace) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(workspace, &info)) return false;
        // The directory identity also unifies case, separators and short-name
        // aliases. Global serializes writers from different Windows sessions.
        const auto name = L"Global\\PdfNoteDiagnosticBudget_" + std::to_wstring(info.dwVolumeSerialNumber) +
            L"_" + std::to_wstring(info.nFileIndexHigh) + L"_" + std::to_wstring(info.nFileIndexLow);
        handle = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle) return false;
        const DWORD wait = WaitForSingleObject(handle, 0);
        acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        return acquired;
    }
private:
    HANDLE handle = nullptr;
    bool acquired = false;
};

// Pin every local ancestor without FILE_SHARE_DELETE before creating/opening
// children. Directory substitution cannot redirect a write outside the workspace.
[[nodiscard]] bool PinDirectory(const std::filesystem::path& path, bool create, Handles& pins) {
    const auto name = ToExtendedWin32PathIfAbsoluteLocal(path);
    HANDLE h = CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE && create && GetLastError() == ERROR_FILE_NOT_FOUND) {
        if (!CreateDirectoryW(name.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        h = CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    pins.Add(h);
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
[[nodiscard]] bool PinWorkspace(const std::filesystem::path& workspace, Handles& pins) {
    const auto value = workspace.wstring();
    if (value.size() < 3 || value[1] != L':' || (value[2] != L'\\' && value[2] != L'/')) return false;
    const UINT drive = GetDriveTypeW(workspace.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) return false;
    auto path = workspace.root_path();
    if (!PinDirectory(path, false, pins)) return false;
    for (const auto& component : workspace.relative_path()) {
        if (component == L"." || component == L".." || component.wstring().find(L':') != std::wstring::npos) return false;
        path /= component;
        if (!PinDirectory(path, false, pins)) return false;
    }
    return true;
}

struct OpenedLogs {
    // Release the mutex only after file/directory handles have closed.
    BudgetLock lock;
    Handles pins;
    Handles files;
    std::filesystem::path directory;
    HANDLE target = INVALID_HANDLE_VALUE;
    Status status;
};
[[nodiscard]] bool ReadStop(HANDLE file, std::uint64_t bytes, bool& stopped) {
    if (bytes < kStopLine.size()) return true;
    LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(bytes - kStopLine.size());
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) return false;
    std::string tail(kStopLine.size(), '\0'); DWORD read = 0;
    if (!ReadFile(file, tail.data(), static_cast<DWORD>(tail.size()), &read, nullptr) || read != tail.size()) return false;
    stopped = stopped || tail == kStopLine;
    return true;
}
[[nodiscard]] bool Open(const std::filesystem::path& workspace, const wchar_t* targetName, OpenedLogs& logs) {
    if (!PinWorkspace(workspace, logs.pins)) return false;
    const auto root = workspace.lexically_normal();
    if (!logs.lock.Acquire(logs.pins.values.back())) return false;
    const auto managed = root / L"__pdf_note_workspace__";
    logs.directory = managed / L"__log__";
    if (!targetName && !PathExistsWin32(managed)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        logs.status.available = true; return true;
    }
    if (!PinDirectory(managed, targetName != nullptr, logs.pins)) return false;
    if (!targetName && !PathExistsWin32(logs.directory)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        logs.status.available = true; return true;
    }
    if (!PinDirectory(logs.directory, targetName != nullptr, logs.pins)) return false;
    for (const auto* name : kFileNames) {
        const bool target = targetName && std::wcscmp(name, targetName) == 0;
        const DWORD access = GENERIC_READ | (target ? FILE_APPEND_DATA : 0);
        HANDLE h = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(logs.directory / name).c_str(), access,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
            return false;
        }
        logs.files.Add(h);
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(h, &info) || info.nNumberOfLinks != 1 ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(h, &size) || size.QuadPart < 0) return false;
        const auto bytes = static_cast<std::uint64_t>(size.QuadPart);
        if (bytes > std::numeric_limits<std::uint64_t>::max() - logs.status.bytes ||
            !ReadStop(h, bytes, logs.status.stopped)) return false;
        logs.status.bytes += bytes;
        if (target) logs.target = h;
    }
    logs.status.available = true;
    logs.status.stopped = logs.status.stopped || logs.status.bytes >= kMaxBytes ||
        kMaxBytes - std::min(kMaxBytes, logs.status.bytes) < kStopLine.size();
    return true;
}
[[nodiscard]] bool Write(HANDLE file, std::string_view payload) {
    DWORD written = 0;
    return WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
        written == payload.size() && FlushFileBuffers(file);
}
}

Result Append(const std::filesystem::path& workspace, const wchar_t* fileName, std::string_view payload) noexcept {
    try {
        bool known = false;
        if (fileName) for (const auto* name : kFileNames) known = known || std::wcscmp(name, fileName) == 0;
        if (!known || payload.empty()) return Result::Unavailable;
        OpenedLogs logs;
        if (!Open(workspace, fileName, logs)) return Result::Unavailable;
        if (logs.status.stopped) return Result::LimitReached;
        // Reserve the short terminal line so that a rejected append can persist
        // the stop without exceeding the aggregate budget or creating metadata.
        const bool fits = payload.size() <= kMaxBytes - logs.status.bytes - kStopLine.size();
        if (logs.target == INVALID_HANDLE_VALUE) {
            logs.target = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(logs.directory / fileName).c_str(),
                FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (logs.target == INVALID_HANDLE_VALUE) return Result::Unavailable;
            logs.files.Add(logs.target);
        }
        if (!Write(logs.target, fits ? payload : kStopLine)) return Result::Unavailable;
        return fits ? Result::Written : Result::LimitReached;
    } catch (...) { return Result::Unavailable; }
}
Status Inspect(const std::filesystem::path& workspace) noexcept {
    try {
        OpenedLogs logs;
        if (Open(workspace, nullptr, logs)) return logs.status;
    } catch (...) {}
    return {};
}
}
