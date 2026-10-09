#include "diagnostics/artifact_usage.h"
#include "core/path_safety.h"
#include <cstddef>
#include <cwchar>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace artifact_usage {
namespace {
class File {
public:
    explicit File(HANDLE h) : handle(h) {}
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    HANDLE handle;
};
constexpr std::array<const wchar_t*, kAreaCount> kRelativeRootNames = {
    L"", L"__log__", L"__tmp__", L"__escape__",
    L"__escape__/backup", L"__escape__/operation_transactions", L"__memo__/backups"};
bool Prefix(const std::filesystem::path& parent, const std::filesystem::path& child) {
    auto p = parent.begin(), c = child.begin();
    for (; p != parent.end(); ++p, ++c) if (c == child.end() || _wcsicmp(p->c_str(), c->c_str())) return false;
    return true;
}
class Scanner {
public:
    Scanner(Result& output, const std::atomic_bool& stopped, Limits bounds) noexcept
        : result(output), cancel(stopped), limits(bounds), start(GetTickCount64()) {}
    void Initialize() {
        // Per-scan ownership: no destructible global path objects shared with
        // a worker when the UI/process is closing. Allocation stays in Scan's
        // exception boundary.
        for (size_t i = 0; i < kAreaCount; ++i) relativeRoots[i] = kRelativeRootNames[i];
    }
    void All(State state, DWORD error = 0) {
        for (auto& value : result.areas) { value.state = state; if (!value.error) value.error = error; }
    }
    bool Continue(unsigned depth) {
        if (stopped) return false;
        if (cancel.load(std::memory_order_relaxed)) { stopped = true; All(State::Canceled); return false; }
        if (depth > limits.depth || visited >= limits.entries || GetTickCount64() - start >= limits.milliseconds) {
            stopped = true; All(State::Limited); return false;
        }
        return true;
    }
    void Failed(const std::filesystem::path& relative, DWORD error) {
        for (size_t i = 0; i < kAreaCount; ++i) {
            // An unreadable ancestor also makes its descendant areas unknown.
            if (!Prefix(relativeRoots[i], relative) && !Prefix(relative, relativeRoots[i])) continue;
            auto& value = result.areas[i]; value.state = State::Partial;
            ++value.skipped; if (!value.error) value.error = error;
        }
    }
    [[nodiscard]] std::unique_ptr<File> Directory(const std::filesystem::path& path, bool enumerate, DWORD& error) {
        // Keep local ancestors stable while resolving children. Read/list only;
        // write sharing permits edits, but directory replacement is denied until
        // this short bounded inspection releases the handles.
        File candidate(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(),
            FILE_READ_ATTRIBUTES | (enumerate ? FILE_LIST_DIRECTORY : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (candidate.handle == INVALID_HANDLE_VALUE) { error = GetLastError(); return {}; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(candidate.handle, &info)) { error = GetLastError(); return {}; }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { error = ERROR_DIRECTORY; return {}; }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            error = ERROR_REPARSE_TAG_INVALID; return {};
        }
        auto file = std::make_unique<File>(candidate.handle);
        candidate.handle = INVALID_HANDLE_VALUE;
        return file;
    }
    void Walk(const std::filesystem::path& path, const std::filesystem::path& relative, unsigned depth) {
        if (!Continue(depth)) return;
        DWORD error = 0;
        auto directory = Directory(path, true, error);
        if (!directory) { Failed(relative, error); return; }
        // Fixed-size aligned buffer bounds memory per recursive frame. Inspect
        // directory metadata through its pinned handle, never through a wildcard
        // that might enumerate CWD or follow a substituted parent.
        std::vector<std::uint64_t> storage(8192);
        const size_t capacity = storage.size() * sizeof(std::uint64_t);
        for (;;) {
            if (!Continue(depth)) return;
            if (!GetFileInformationByHandleEx(directory->handle, FileIdBothDirectoryInfo, storage.data(),
                                               static_cast<DWORD>(capacity))) {
                error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) Failed(relative, error);
                return;
            }
            size_t offset = 0;
            for (;;) {
                if (!Continue(depth)) return;
                constexpr size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                if (offset > capacity - header) { Failed(relative, ERROR_INVALID_DATA); return; }
                const auto* row = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
                    reinterpret_cast<const char*>(storage.data()) + offset);
                if (row->FileNameLength % sizeof(wchar_t) || row->FileNameLength > capacity - offset - header) {
                    Failed(relative, ERROR_INVALID_DATA); return;
                }
                const std::wstring name(row->FileName, row->FileNameLength / sizeof(wchar_t));
                if (name != L"." && name != L"..") {
                    ++visited;
                    const auto child = relative / name;
                    if (name.empty() || name.find(L'\0') != std::wstring::npos || name.find_first_of(L"\\/:") != std::wstring::npos ||
                        (path / name).wstring().size() > 32760) { Failed(child, ERROR_INVALID_NAME); }
                    else if (row->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) Failed(child, ERROR_REPARSE_TAG_INVALID);
                    else if (row->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        Walk(path / name, child, depth + 1);
                        if (!Continue(depth)) return;
                    } else if (row->EndOfFile.QuadPart < 0) Failed(child, ERROR_INVALID_DATA);
                    else {
                        const auto bytes = static_cast<std::uint64_t>(row->EndOfFile.QuadPart);
                        for (size_t i = 0; i < kAreaCount; ++i) if (Prefix(relativeRoots[i], child)) {
                            auto& value = result.areas[i];
                            if (bytes > std::numeric_limits<std::uint64_t>::max() - value.bytes) Failed(child, ERROR_ARITHMETIC_OVERFLOW);
                            else { value.bytes += bytes; ++value.files; }
                        }
                    }
                }
                if (!row->NextEntryOffset) break;
                if (row->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) ||
                    row->NextEntryOffset < header + row->FileNameLength || row->NextEntryOffset > capacity - offset) {
                    Failed(relative, ERROR_INVALID_DATA); return;
                }
                offset += row->NextEntryOffset;
            }
        }
    }
    Result& result;
    const std::atomic_bool& cancel;
    Limits limits;
    ULONGLONG start;
    std::uint64_t visited = 0;
    bool stopped = false;
    std::array<std::filesystem::path, kAreaCount> relativeRoots{};
};
}

Result Scan(const std::filesystem::path& workspace, const std::atomic_bool& cancel, Limits limits) noexcept {
    Result result;
    Scanner scanner(result, cancel, limits);
    try {
        scanner.Initialize();
        const auto value = workspace.wstring();
        const UINT drive = value.size() >= 3 && value[1] == L':' && (value[2] == L'\\' || value[2] == L'/')
            ? GetDriveTypeW(workspace.root_path().c_str()) : DRIVE_UNKNOWN;
        if (value.size() > 32760 || (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK))
            scanner.All(State::Unavailable, ERROR_INVALID_NAME);
        else if (scanner.Continue(0)) {
            DWORD error = 0;
            std::vector<std::unique_ptr<File>> ancestors;
            auto path = workspace.root_path();
            auto pin = scanner.Directory(path, false, error);
            if (pin) ancestors.push_back(std::move(pin));
            for (const auto& part : workspace.relative_path()) {
                if (error || !scanner.Continue(0)) break;
                if (part == L"." || part == L".." || part.wstring().find(L':') != std::wstring::npos) { error = ERROR_INVALID_NAME; break; }
                path /= part; pin = scanner.Directory(path, false, error);
                if (pin) ancestors.push_back(std::move(pin));
            }
            if (error) scanner.All(State::Unavailable, error);
            else if (scanner.Continue(0)) {
                path = workspace / L"__pdf_note_workspace__";
                auto resource = scanner.Directory(path, true, error);
                if (!resource) scanner.All(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? State::Missing : State::Unavailable, error);
                else scanner.Walk(path, {}, 0);
            }
        }
    } catch (...) { scanner.All(State::Partial, ERROR_NOT_ENOUGH_MEMORY); }
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    result.checkedAt = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return result;
}
}
