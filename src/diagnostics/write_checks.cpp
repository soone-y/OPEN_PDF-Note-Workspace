#include "diagnostics/write_checks.h"
#include "core/atomic_write.h"
#include "diagnostics/normal_operations.h"
#include <iomanip>
#include <sstream>
#include <utility>
#include <cwctype>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <limits>

namespace write_checks {
namespace {
constexpr size_t kMaxRecords = 256;
constexpr size_t kMaxBytes = 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
bool ReadBytes(const std::filesystem::path& path, std::string& bytes, DWORD& error) {
    bytes.clear(); error = 0;
    if (!IsSafeLocalPath(path)) { error = ERROR_INVALID_NAME; return false; }
    Handle file(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { error = 0; return true; }
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) { error = GetLastError(); return false; }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) { error = ERROR_INVALID_DATA; return false; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) { error = GetLastError(); return false; }
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaxBytes)) {
        error = ERROR_INVALID_DATA; return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { error = GetLastError(); return false; }
    if (read != bytes.size()) { error = ERROR_READ_FAULT; return false; }
    return true;
}
std::string Encode(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("invalid UTF-16");
    std::string out(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        out.data(), count, nullptr, nullptr)) throw std::runtime_error("UTF-8 encoding failed");
    return out;
}
bool Decode(const std::string& bytes, std::wstring& value) {
    value.clear();
    if (bytes.empty()) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!count) return false;
    value.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), value.data(), count) != 0;
}
bool ValidText(const std::wstring& text) {
    return text.size() <= 32768 && text.find_first_of(L"\r\n\0", 0, 3) == std::wstring::npos;
}
bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}
std::filesystem::path Normalized(std::filesystem::path path) {
    path = path.lexically_normal().make_preferred();
    while (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
    return path;
}
bool SameTarget(const std::wstring& a, const std::wstring& b) {
    if (a == b) return true;
    const auto left = Normalized(a), right = Normalized(b);
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
}

bool ManualLimit::Start(std::uint64_t now, bool cooldownEligible) {
    if (busy_) return false;
    if (Remaining(now)) {
        if (!cooldownEligible || recoveryUsed_ >= 2) return false;
        ++recoveryUsed_; recoveryInFlight_ = true;
    } else {
        if (blockedUntil_) { count_ = 0; blockedUntil_ = 0; recoveryUsed_ = 0; }
        recoveryInFlight_ = false;
    }
    busy_ = true; return true;
}
void ManualLimit::Finish(std::uint64_t now, bool unchanged) {
    if (!busy_) return;
    busy_ = false;
    if (recoveryInFlight_) { recoveryInFlight_ = false; return; }
    if (unchanged && ++count_ >= 5) { blockedUntil_ = now + 5 * 60 * 1000; recoveryUsed_ = 0; }
}
std::uint64_t ManualLimit::Remaining(std::uint64_t now) const {
    return now < blockedUntil_ ? blockedUntil_ - now : 0;
}
std::uint64_t UtcNow() {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    const auto clock = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // FILETIME clocks can give several completions the same tick. Keep evidence
    // ordered within this process, including concurrent normal/manual workers.
    static std::atomic<std::uint64_t> last{0};
    auto previous = last.load();
    for (;;) {
        const auto next = std::max(clock, previous + 1);
        if (last.compare_exchange_weak(previous, next)) return next;
    }
}
bool IsSafeLocalPath(const std::filesystem::path& path) {
    const auto value = path.wstring();
    if (value.size() < 3 || value[1] != L':' || (value[2] != L'\\' && value[2] != L'/')) return false;
    const auto drive = GetDriveTypeW(path.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK && drive != DRIVE_CDROM) return false;
    auto current = path.root_path();
    for (const auto& part : path.relative_path()) {
        if (part == L".." || part == L"." || part.wstring().find(L':') != std::wstring::npos) return false;
        current /= part;
        const DWORD attrs = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(current).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) return false;
        } else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    }
    return true;
}
Result ProbeDirectory(const std::filesystem::path& target, bool write) {
    Result result; result.time = UtcNow();
    if (!IsSafeLocalPath(target)) { result.outcome = Outcome::Unavailable; result.error = ERROR_INVALID_NAME; return result; }
    // Retain no-write/no-delete handles for existing ancestors until completion;
    // a concurrent directory rename or reparse edit cannot redirect the probe.
    std::vector<HANDLE> parents;
    struct CloseParents { std::vector<HANDLE>& handles; ~CloseParents() { for (HANDLE h : handles) CloseHandle(h); } } closeParents{parents};
    auto current = target.root_path();
    for (const auto& part : target.relative_path()) {
        current /= part;
        Handle h(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(current).c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (h.value == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            if (result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND || result.error == ERROR_DIRECTORY)
                result.outcome = Outcome::Unavailable;
            return result;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h.value, &info)) { result.error = GetLastError(); return result; }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { result.outcome = Outcome::Unavailable; result.error = ERROR_INVALID_NAME; return result; }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { result.outcome = Outcome::Unavailable; result.error = ERROR_DIRECTORY; return result; }
        parents.push_back(h.value); h.value = INVALID_HANDLE_VALUE;
    }
    if (!write) {
        Handle directory(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(target).c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (directory.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
        std::array<unsigned char, 4096> entries{};
        if (!GetFileInformationByHandleEx(directory.value, FileIdBothDirectoryInfo, entries.data(), static_cast<DWORD>(entries.size()))) {
            const DWORD err = GetLastError();
            if (err != ERROR_NO_MORE_FILES) { result.error = err; return result; }
        }
        result.outcome = Outcome::Passed; result.step = Step::Complete; return result;
    }
    std::filesystem::path probe;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned i = 0; i < 32; ++i) {
        probe = target / (L".pdf_note_write_check." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(result.time) + L"." + std::to_wstring(i) + L".tmp");
        handle = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(probe).c_str(), GENERIC_WRITE | GENERIC_READ | DELETE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        result.error = GetLastError();
        if (result.error != ERROR_FILE_EXISTS && result.error != ERROR_ALREADY_EXISTS) return result;
    }
    if (handle == INVALID_HANDLE_VALUE) return result;
    Handle created(handle);
    result.remaining = probe.wstring();
    BY_HANDLE_FILE_INFORMATION original{};
    const std::string payload = "PDF Note Workspace write check " + std::to_string(result.time);
    DWORD written = 0;
    result.step = Step::Write;
    bool ok = GetFileInformationByHandle(handle, &original) && WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) && written == payload.size();
    if (ok) { result.step = Step::Flush; ok = FlushFileBuffers(handle) != 0; }
    if (!ok) {
        result.error = GetLastError();
        if (!result.error) result.error = ERROR_WRITE_FAULT;
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) result.remaining.clear();
        return result;
    }
    if (!CloseHandle(handle)) { result.error = GetLastError(); result.remaining = probe.wstring(); return result; }
    created.value = INVALID_HANDLE_VALUE;
    result.step = Step::Reopen;
    Handle reopened(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(probe).c_str(), GENERIC_READ | DELETE, 0,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (reopened.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); result.remaining = probe.wstring(); return result; }
    BY_HANDLE_FILE_INFORMATION actual{};
    if (!GetFileInformationByHandle(reopened.value, &actual) || !SameFile(original, actual) || (actual.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        result.error = ERROR_FILE_INVALID; result.remaining = probe.wstring(); return result;
    }
    result.step = Step::Compare;
    std::string bytes(payload.size() + 1, '\0'); DWORD read = 0;
    ok = ReadFile(reopened.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
    if (!ok) result.error = GetLastError();
    else if (read != payload.size()) { ok = false; result.error = ERROR_READ_FAULT; }
    else if (bytes.compare(0, read, payload) != 0) { ok = false; result.error = ERROR_CRC; }
    const Step failureStep = result.step;
    result.step = Step::Delete;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(reopened.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
        result.error = GetLastError(); result.remaining = probe.wstring(); return result;
    }
    result.remaining.clear();
    if (!ok) { result.step = failureStep; return result; }
    result.error = 0; result.outcome = Outcome::Passed; result.step = Step::Complete; return result;
}
namespace {
Result ReadProbeFile(const std::filesystem::path& target, DWORD access, std::uint64_t maxBytes,
                     const std::atomic_bool* cancel) {
    Result result; result.time = UtcNow();
    if (!IsSafeLocalPath(target)) { result.outcome = Outcome::Unavailable; result.error = ERROR_INVALID_NAME; return result; }
    Handle file(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(target).c_str(), access,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        if (result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND || result.error == ERROR_DIRECTORY)
            result.outcome = Outcome::Unavailable;
        return result;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) { result.error = GetLastError(); return result; }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        result.outcome = Outcome::Unavailable; result.error = ERROR_INVALID_NAME; return result;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) { result.error = GetLastError(); return result; }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
        result.outcome = Outcome::Unavailable; result.error = ERROR_FILE_TOO_LARGE; return result;
    }
    std::array<char, 32768> bytes{};
    auto remaining = static_cast<std::uint64_t>(size.QuadPart);
    do {
        if (cancel && cancel->load()) { result.outcome = Outcome::Canceled; result.error = ERROR_CANCELLED; return result; }
        const auto count = static_cast<DWORD>(std::min<std::uint64_t>(remaining, bytes.size()));
        DWORD read = 0;
        if (count && !ReadFile(file.value, bytes.data(), count, &read, nullptr)) { result.error = GetLastError(); return result; }
        if (read != count) { result.error = ERROR_READ_FAULT; return result; }
        remaining -= count;
    } while (remaining);
    result.outcome = Outcome::Passed; result.step = Step::Complete; return result;
}
}
Result ProbeFile(const std::filesystem::path& target) {
    return ReadProbeFile(target, GENERIC_READ, kMaxBytes, nullptr);
}
Result ProbeNoteFile(const std::filesystem::path& target, const std::atomic_bool& cancel) {
    const auto read = ReadProbeFile(target, GENERIC_READ | GENERIC_WRITE,
        std::numeric_limits<std::uint64_t>::max(), &cancel);
    if (read.outcome != Outcome::Passed) return read;
    if (cancel.load()) { auto canceled = read; canceled.outcome = Outcome::Canceled; canceled.error = ERROR_CANCELLED; return canceled; }
    return ProbeDirectory(target.parent_path(), true);
}
const wchar_t* LabelId(Kind kind) {
    static constexpr std::array<const wchar_t*, kCount> ids = {L"checks.setup_read", L"checks.setup_write", L"checks.workspace_read", L"checks.workspace_write", L"checks.settings", L"checks.temp", L"checks.recovery", L"checks.logs", L"checks.pdf_write", L"checks.note_write", L"checks.office_temp", L"checks.pdf_read", L"checks.office_conversion", L"checks.note_read_write"};
    return ids.at(static_cast<size_t>(kind));
}
const wchar_t* StepId(Step step) {
    static constexpr std::array<const wchar_t*, 9> ids = {L"checks.step.open", L"checks.step.write", L"checks.step.flush", L"checks.step.reopen", L"checks.step.compare", L"checks.step.delete", L"checks.step.convert", L"checks.step.pdf", L"checks.step.complete"};
    return ids.at(static_cast<size_t>(step));
}
const Record* Find(const std::vector<Record>& records, Kind kind, const std::wstring& target) {
    for (const auto& row : records) if (row.kind == kind && SameTarget(row.target, target)) return &row;
    return nullptr;
}
void Upsert(std::vector<Record>& records, Record record) {
    for (auto& row : records) if (row.kind == record.kind && SameTarget(row.target, record.target)) { row = std::move(record); return; }
    records.push_back(std::move(record));
}
bool Load(const std::filesystem::path& path, std::vector<Record>& records, std::string& baseline, DWORD& error) {
    std::string bytes;
    if (!ReadBytes(path, bytes, error)) return false;
    std::vector<Record> parsed;
    if (!bytes.empty()) {
        std::istringstream in(bytes); std::string magic;
        if (!std::getline(in, magic) || (magic != "PDF_NOTE_WRITE_CHECKS_1" && magic != "PDF_NOTE_WRITE_CHECKS_2")) { error = ERROR_INVALID_DATA; return false; }
        const bool version2 = magic == "PDF_NOTE_WRITE_CHECKS_2";
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line); unsigned kind, outcome, step; std::string target, remaining; Record row;
            if (!(fields >> kind >> std::quoted(target) >> outcome >> step >> row.result.error >> row.result.time >> std::quoted(remaining)) ||
                kind >= kCount || outcome > static_cast<unsigned>(Outcome::Canceled) || step > static_cast<unsigned>(Step::Complete) || !row.result.time ||
                !Decode(target, row.target) || !Decode(remaining, row.result.remaining) || !ValidText(row.target) || !ValidText(row.result.remaining) || row.target.empty()) { error = ERROR_INVALID_DATA; return false; }
            if (version2) {
                unsigned source; std::string operation;
                if (!(fields >> source >> std::quoted(operation)) || source > static_cast<unsigned>(Source::Normal) ||
                    !Decode(operation, row.result.operationPath) || !ValidText(row.result.operationPath) ||
                    (source == static_cast<unsigned>(Source::Normal) && row.result.operationPath.empty())) { error = ERROR_INVALID_DATA; return false; }
                row.result.source = static_cast<Source>(source);
            }
            fields >> std::ws;
            if (!fields.eof() || parsed.size() >= kMaxRecords) { error = ERROR_INVALID_DATA; return false; }
            row.kind = static_cast<Kind>(kind); row.result.outcome = static_cast<Outcome>(outcome); row.result.step = static_cast<Step>(step);
            if (Find(parsed, row.kind, row.target)) { error = ERROR_INVALID_DATA; return false; }
            parsed.push_back(std::move(row));
        }
        if (bytes.back() != '\n') { error = ERROR_INVALID_DATA; return false; }
    }
    records = std::move(parsed); baseline = std::move(bytes); error = 0; return true;
}
bool Save(const std::filesystem::path& path, std::vector<Record>& records, std::string& baseline, DWORD& error) {
    if (records.size() > kMaxRecords * 2 || !IsSafeLocalPath(path)) { error = ERROR_INVALID_DATA; return false; }
    // Storage requires an existing local parent. Retain ancestors so a reparse
    // replacement cannot redirect the atomic writer after path validation.
    std::vector<HANDLE> parents;
    struct CloseParents { std::vector<HANDLE>& handles; ~CloseParents() { for (HANDLE h : handles) CloseHandle(h); } } closeParents{parents};
    auto current = path.root_path();
    for (const auto& part : path.parent_path().relative_path()) {
        current /= part;
        Handle h(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(current).c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (h.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h.value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { error = ERROR_INVALID_NAME; return false; }
        parents.push_back(h.value); h.value = INVALID_HANDLE_VALUE;
    }
    // Non-blocking cross-process serialization; stale snapshots never overwrite
    // another instance's result. No filesystem lock file is needed.
    const auto name = path.wstring(); std::uint64_t hash = 14695981039346656037ULL;
    for (wchar_t ch : name) { hash ^= static_cast<unsigned>(towlower(ch)); hash *= 1099511628211ULL; }
    Handle lock(CreateMutexW(nullptr, FALSE, (L"Local\\PdfNoteWriteChecks_" + std::to_wstring(hash)).c_str()));
    if (!lock.value || lock.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    const DWORD wait = WaitForSingleObject(lock.value, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { error = ERROR_BUSY; return false; }
    struct Unlock { HANDLE h; ~Unlock() { ReleaseMutex(h); } } unlock{lock.value};
    std::string existing;
    if (!ReadBytes(path, existing, error)) return false;
    if (existing != baseline) { error = ERROR_REVISION_MISMATCH; return false; }
    const std::string header = "PDF_NOTE_WRITE_CHECKS_2\n";
    std::vector<std::string> lines;
    size_t total = header.size();
    for (size_t index = 0; index < records.size(); ++index) {
        const auto& row = records[index];
        if (static_cast<size_t>(row.kind) >= kCount || static_cast<unsigned>(row.result.outcome) > static_cast<unsigned>(Outcome::Canceled) ||
            static_cast<unsigned>(row.result.step) > static_cast<unsigned>(Step::Complete) || !ValidText(row.target) || !ValidText(row.result.remaining) || row.target.empty() || !row.result.time ||
            static_cast<unsigned>(row.result.source) > static_cast<unsigned>(Source::Normal) || !ValidText(row.result.operationPath) ||
            (row.result.source == Source::Normal && row.result.operationPath.empty())) { error = ERROR_INVALID_DATA; return false; }
        for (size_t previous = 0; previous < index; ++previous) {
            if (records[previous].kind == row.kind && SameTarget(records[previous].target, row.target)) { error = ERROR_INVALID_DATA; return false; }
        }
        std::ostringstream out;
        out << static_cast<unsigned>(row.kind) << ' ' << std::quoted(Encode(row.target)) << ' ' << static_cast<unsigned>(row.result.outcome) << ' ' << static_cast<unsigned>(row.result.step) << ' ' << row.result.error << ' ' << row.result.time << ' ' << std::quoted(Encode(row.result.remaining)) << ' ' << static_cast<unsigned>(row.result.source) << ' ' << std::quoted(Encode(row.result.operationPath)) << '\n';
        lines.push_back(out.str()); total += lines.back().size();
    }
    std::vector<size_t> oldest(records.size()); std::iota(oldest.begin(), oldest.end(), 0);
    std::stable_sort(oldest.begin(), oldest.end(), [&](size_t a, size_t b) { return records[a].result.time < records[b].result.time; });
    std::vector<bool> keep(records.size(), true); size_t count = records.size();
    for (const auto index : oldest) {
        if (count <= 1 || (count <= 64 && total <= 64 * 1024)) break;
        keep[index] = false; --count; total -= lines[index].size();
    }
    std::string bytes = header; std::vector<Record> retained; retained.reserve(count);
    for (size_t index = 0; index < records.size(); ++index) if (keep[index]) {
        bytes += lines[index]; retained.push_back(records[index]);
    }
    if (bytes.size() > kMaxBytes) { error = ERROR_FILE_TOO_LARGE; return false; }
    std::wstring detail;
    if (!atomic_write::AtomicWriteUtf8(path, bytes, path.parent_path(), &detail)) { error = ERROR_WRITE_FAULT; return false; }
    std::string verified;
    if (!ReadBytes(path, verified, error) || verified != bytes) { if (!error) error = ERROR_CRC; return false; }
    records = std::move(retained); baseline = bytes; error = 0; return true;
}

namespace {
void MergeLatest(std::vector<Record>& into, const std::vector<Record>& from) {
    for (const auto& row : from) {
        const auto* previous = Find(into, row.kind, row.target);
        if (!previous || previous->result.time <= row.result.time) Upsert(into, row);
    }
}
struct NormalEntry { std::filesystem::path store; Record record; bool pending = true; DWORD error = 0; ULONGLONG lastReadSave = 0; };
std::mutex normalMemoryMutex, normalIoMutex;
std::vector<NormalEntry> normalEntries;
std::uint64_t normalRevision = 1;
std::atomic_bool normalPersistence{false};
bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
bool Within(const std::filesystem::path& file, const std::filesystem::path& parent) {
    for (auto p = file; !p.empty(); p = p.parent_path()) {
        if (SamePath(p, parent)) return true;
        if (p == p.parent_path()) break;
    }
    return false;
}
bool IsNormalRead(Kind kind) {
    return kind == Kind::SetupRead || kind == Kind::WorkspaceRead || kind == Kind::PdfRead;
}
bool PersistNormalPending(const std::filesystem::path& store, std::vector<Record> pending) {
    if (pending.empty()) return true;
    DWORD storageError = 0; std::string baseline;
    bool saved = false;
    if (IsSafeLocalPath(store) && atomic_write::EnsureDirectoryExists(store.parent_path()))
        saved = SaveLatest(store, pending, baseline, storageError);
    else storageError = ERROR_ACCESS_DENIED;
    if (saved) AcknowledgeNormalObservations(store, pending);
    else {
        std::lock_guard<std::mutex> lock(normalMemoryMutex);
        for (auto& entry : normalEntries) if (SamePath(entry.store, store) && entry.pending) entry.error = storageError;
        ++normalRevision;
    }
    return saved;
}
void ObserveNormal(const std::filesystem::path& owner, const std::filesystem::path& original,
                   Kind kind, const std::filesystem::path& explicitTarget, bool passed, DWORD error) noexcept {
    try {
        auto file = Normalized(original), root = Normalized(owner);
        // Managed files keep their owning workspace even when a worker completes
        // after the UI has switched workspace. Never read application globals here.
        for (auto p = file.parent_path(); root.empty() && (kind == Kind::Count || kind == Kind::OfficeTempWrite) && !p.empty(); p = p.parent_path()) {
            if (_wcsicmp(p.filename().c_str(), L"__pdf_note_workspace__") == 0) { root = p.parent_path(); break; }
            if (p == p.parent_path()) break;
        }
        if (root.empty() || !root.is_absolute() || file.empty() || !file.is_absolute()) return;
        const auto resource = root / L"__pdf_note_workspace__";
        const auto store = resource / L"__log__" / L"write_checks.log";
        if (SamePath(file, store)) return; // Recording never records itself.
        auto target = Normalized(explicitTarget);
        if (kind == Kind::Count) {
            if (_wcsicmp(file.filename().c_str(), L"pdf_note_workspace_setup.json") == 0) { kind = Kind::SetupWrite; target = file.parent_path(); }
            else if (Within(file, resource / L"__settings__")) { kind = Kind::SettingsWrite; target = resource / L"__settings__"; }
            else if (Within(file, resource / L"__tmp__")) { kind = Kind::TempWrite; target = resource / L"__tmp__"; }
            else if (Within(file, resource / L"__escape__")) { kind = Kind::RecoveryWrite; target = resource / L"__escape__"; }
            else if (Within(file, resource / L"__log__")) { kind = Kind::LogsWrite; target = resource / L"__log__"; }
            else if (Within(file, resource)) { kind = Kind::WorkspaceWrite; target = root; }
            else if (_wcsicmp(file.extension().c_str(), L".pdf") == 0 || _wcsicmp(file.extension().c_str(), L".clrop") == 0) { kind = Kind::PdfWrite; target = file.parent_path(); }
            else if (_wcsicmp(file.extension().c_str(), L".txt") == 0 || _wcsicmp(file.extension().c_str(), L".md") == 0) { kind = Kind::NoteWrite; target = file.parent_path(); }
            else if (Within(file, root)) { kind = Kind::WorkspaceWrite; target = root; }
            else return;
        }
        if (target.empty() || static_cast<size_t>(kind) >= kCount) return;
        Result result; result.time = UtcNow(); result.source = Source::Normal;
        result.operationPath = file.wstring(); result.error = passed ? 0 : error;
        result.outcome = passed ? Outcome::Passed : Outcome::Failed;
        result.step = passed ? Step::Complete : (IsNormalRead(kind) ? Step::Open : kind == Kind::OfficeConversion ? Step::Convert : Step::Write);
        const Record observation{kind, target.wstring(), result};
        std::lock_guard<std::mutex> io(normalIoMutex);
        std::vector<Record> pending;
        bool persistNow = true;
        {
            std::lock_guard<std::mutex> lock(normalMemoryMutex);
            auto found = std::find_if(normalEntries.begin(), normalEntries.end(), [&](const NormalEntry& entry) {
                return SamePath(entry.store, store) && entry.record.kind == kind && SameTarget(entry.record.target, observation.target);
            });
            if (found != normalEntries.end()) {
                if (IsNormalRead(kind)) {
                    const bool changed = found->record.result.outcome != observation.result.outcome ||
                        found->record.result.error != observation.result.error || found->record.result.operationPath != observation.result.operationPath;
                    persistNow = changed || !found->lastReadSave || GetTickCount64() - found->lastReadSave >= 10000;
                }
                if (found->record.result.time <= observation.result.time) {
                    const auto lastSave = found->lastReadSave;
                    const auto pendingError = found->pending ? found->error : 0;
                    *found = {store, observation, true, pendingError, lastSave};
                }
            }
            else normalEntries.push_back({store, observation, true, 0});
            // Same retention policy as the ledger, with an overall process cap.
            if (normalEntries.size() > kMaxRecords) {
                const auto oldest = std::min_element(normalEntries.begin(), normalEntries.end(), [](const NormalEntry& a, const NormalEntry& b) { return a.record.result.time < b.record.result.time; });
                normalEntries.erase(oldest);
            }
            for (const auto& entry : normalEntries) if (SamePath(entry.store, store) && entry.pending) pending.push_back(entry.record);
            ++normalRevision;
        }
        if (!normalPersistence.load()) return; // Startup has not acquired its workspace lock yet.
        // Repeated reads update memory immediately, but batch ledger writes.
        // First reads, outcome/error changes and normal writes persist at once.
        if (persistNow) { const bool saved = PersistNormalPending(store, std::move(pending)); (void)saved; }
    } catch (...) { /* Evidence must never change the real operation's result. */ }
}
}
bool SaveLatest(const std::filesystem::path& path, std::vector<Record>& records,
                std::string& baseline, DWORD& error) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        std::vector<Record> latest; std::string fresh;
        if (!Load(path, latest, fresh, error)) return false;
        MergeLatest(latest, records);
        if (Save(path, latest, fresh, error)) {
            records = std::move(latest); baseline = std::move(fresh); return true;
        }
        if (error != ERROR_REVISION_MISMATCH) return false;
    }
    return false;
}
void EnableNormalObservations(bool persist) noexcept {
    normalPersistence.store(persist);
    normalSink.store(&ObserveNormal);
}
bool FlushNormalObservations(const std::filesystem::path& workspace) noexcept {
    try {
        if (!normalPersistence.load()) return false;
        const auto root = Normalized(workspace);
        if (root.empty() || !root.is_absolute()) return false;
        const auto store = root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log";
        std::lock_guard<std::mutex> io(normalIoMutex);
        std::vector<Record> pending;
        {
            std::lock_guard<std::mutex> lock(normalMemoryMutex);
            for (const auto& entry : normalEntries) if (SamePath(entry.store, store) && entry.pending) pending.push_back(entry.record);
        }
        return PersistNormalPending(store, std::move(pending));
    } catch (...) { return false; }
}
std::uint64_t MergeNormalObservations(const std::filesystem::path& store, std::vector<Record>& records,
                                    bool& unsaved, DWORD& error) try {
    std::unique_lock<std::mutex> lock(normalMemoryMutex, std::try_to_lock);
    if (!lock.owns_lock()) return 0;
    for (const auto& entry : normalEntries) if (SamePath(entry.store, store)) {
        const auto* old = Find(records, entry.record.kind, entry.record.target);
        if (!old || old->result.time < entry.record.result.time) Upsert(records, entry.record);
        if (entry.pending) { unsaved = true; error = entry.error; }
    }
    // A long-open window must not accumulate every historical export target.
    // Keep the bounded pending set and manual results; discard only the oldest
    // cached normal evidence, within Save's existing input limit.
    while (records.size() > kMaxRecords * 2) {
        auto oldest = records.end();
        for (auto it = records.begin(); it != records.end(); ++it)
            if (it->result.source == Source::Normal && (oldest == records.end() || it->result.time < oldest->result.time)) oldest = it;
        if (oldest == records.end()) break;
        records.erase(oldest);
    }
    return normalRevision;
} catch (...) {
    return 0; // Best-effort display must not interrupt the application on allocation failure.
}
void AcknowledgeNormalObservations(const std::filesystem::path& store, const std::vector<Record>& persisted) noexcept {
    try {
        std::lock_guard<std::mutex> lock(normalMemoryMutex);
        for (auto& entry : normalEntries) if (SamePath(entry.store, store)) {
            const auto* row = Find(persisted, entry.record.kind, entry.record.target);
            if (row && row->result.time >= entry.record.result.time) {
                entry.pending = false; entry.error = 0;
                if (IsNormalRead(entry.record.kind)) entry.lastReadSave = GetTickCount64();
            }
        }
        ++normalRevision;
    } catch (...) {}
}
}
