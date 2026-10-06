#include "workspace/workspace_memo_store.h"
#include "core/atomic_write.h"
#include <cstddef>
#include <cstring>
#include <vector>

namespace workspace_memo {
#ifdef WORKSPACE_MEMO_TESTING
namespace testing {
static FaultHook faultHook = nullptr;
void SetFaultHook(FaultHook hook) { faultHook = hook; }
}
#define WMEMO_FAIL(point) (testing::faultHook && testing::faultHook(testing::FaultPoint::point))
#else
#define WMEMO_FAIL(point) false
#endif
namespace {
class File {
public:
    explicit File(HANDLE handle) : handle_(handle) {}
    ~File() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

// Validate each existing path component before I/O; no UNC, device path,
// drive-relative path or reparse traversal into a different/local-remote store.
[[nodiscard]] bool SafePath(const std::filesystem::path& path) {
    const auto value = path.wstring();
    if (value.size() < 3 || value[1] != L':' || value[2] != L'\\') return false;
    const UINT driveType = GetDriveTypeW(path.root_path().c_str());
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE &&
        driveType != DRIVE_RAMDISK && driveType != DRIVE_CDROM) return false;
    auto current = path.root_path();
    for (const auto& part : path.relative_path()) {
        if (part == L".." || part == L"." || part.wstring().find(L':') != std::wstring::npos) return false;
        current /= part;
        const auto name = ToExtendedWin32PathIfAbsoluteLocal(current);
        const DWORD attr = GetFileAttributesW(name.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        } else if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    }
    return true;
}

[[nodiscard]] bool ReadHandle(HANDLE file, size_t limit, std::string& bytes) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > limit) return false;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    return bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size());
}

[[nodiscard]] bool Read(const std::filesystem::path& path, size_t limit, std::string& bytes, bool& exists) {
    if (!SafePath(path)) return false;
    File file(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(), GENERIC_READ,
                         FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        bytes.clear(); exists = false; return true;
    }
    exists = true;
    return ReadHandle(file.get(), limit, bytes);
}

[[nodiscard]] bool Decode(const std::string& bytes, std::wstring& text) {
    if (bytes.size() > kMaxBytes || bytes.find('\0') != std::string::npos) return false;
    size_t offset = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    const int count = static_cast<int>(bytes.size() - offset);
    if (!count) { text.clear(); return true; }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset, count, nullptr, 0);
    if (!length) return false;
    std::wstring decoded(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset, count, decoded.data(), length)) return false;
    // Native multiline EDIT requires CRLF; preserve the byte baseline separately.
    text.clear();
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i] == L'\r') {
            if (i + 1 < decoded.size() && decoded[i + 1] == L'\n') ++i;
            text += L"\r\n";
        } else if (decoded[i] == L'\n') text += L"\r\n";
        else text += decoded[i];
    }
    return true;
}

[[nodiscard]] bool Encode(std::wstring_view text, std::string& bytes) {
    if (text.size() > kMaxEditorUnits || text.find(L'\0') != std::wstring_view::npos) return false;
    // Disk/checkpoint size is measured in UTF-8 with LF, not UTF-16 CRLF units.
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            normalized += L'\n';
        } else normalized += text[i];
    }
    text = normalized;
    if (text.empty()) { bytes.clear(); return true; }
    const int count = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count, nullptr, 0, nullptr, nullptr);
    if (!length || static_cast<size_t>(length) > kMaxBytes) return false;
    bytes.resize(static_cast<size_t>(length));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count, bytes.data(), length, nullptr, nullptr) != 0;
}

std::filesystem::path Recovery(const std::filesystem::path& path) { return path.parent_path() / L"workspace_memo.recovery"; }

[[nodiscard]] bool Write(const std::filesystem::path& path, const std::string& bytes) {
    std::wstring error;
    return SafePath(path) && atomic_write::AtomicWriteUtf8(path, bytes, path.parent_path(), path.parent_path() / L"recovery_failed", &error);
}

std::string Record(const std::string& base, bool exists, const std::string& draft) {
    return std::string("WMEMO1\n") + (exists ? "1\n" : "0\n") +
        std::to_string(base.size()) + "\n" + base + draft;
}

// A failed delete must not leave an obsolete dirty envelope after a successful
// external adoption. Settle it durably instead; failure blocks close/exit.
[[nodiscard]] bool SettleRecovery(const std::filesystem::path& path,
                                 const std::string& base, bool exists, const std::wstring& text) {
    const auto recovery = Recovery(path);
    if (!SafePath(recovery)) return false;
    if (!WMEMO_FAIL(CleanupDelete)) {
        if (DeleteFileW(ToExtendedWin32PathIfAbsoluteLocal(recovery).c_str())) return true;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
    }
    std::string draft;
    return Encode(text, draft) && !WMEMO_FAIL(CleanupWrite) && Write(recovery, Record(base, exists, draft));
}

[[nodiscard]] bool WriteDraft(HANDLE file, const std::string& bytes) {
    if (WMEMO_FAIL(TempWrite)) return false;
    DWORD written = 0;
    if (!bytes.empty() && (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
                          written != bytes.size())) return false;
    return !WMEMO_FAIL(TempFlush) && FlushFileBuffers(file) != FALSE;
}

// An exclusive DELETE-capable handle denies external writes/deletes during the
// baseline check and backup rename. Install the flushed draft WITHOUT replace:
// if an external editor creates the original in the short rename/install gap,
// keep it, keep our backup/draft and fail. Crash in that gap leaves the complete
// original in backups and baseline+draft in recovery, never only a partial file.
[[nodiscard]] bool RenameHandle(HANDLE file, const std::filesystem::path& path) {
    const std::wstring name = ToExtendedWin32PathIfAbsoluteLocal(path);
    // Include an explicit terminator even when the aligned allocation has no
    // spare padding; SetFileInformationByHandle resolves this Win32 path.
    const size_t size = offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t);
    std::vector<unsigned long long> buffer((size + 7) / 8, 0);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, name.data(), info->FileNameLength);
    return SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(size)) != FALSE;
}
}

std::filesystem::path MemoPath(const std::filesystem::path& root) {
    return root.empty() ? std::filesystem::path{} : root / L"__resource__" / L"__memo__" / L"workspace_memo.txt";
}

bool ValidateText(std::wstring_view text) { std::string bytes; return Encode(text, bytes); }

Result Document::Load(const std::filesystem::path& root) {
    ready_ = false; path_ = MemoPath(root); text_.clear(); baseline_.clear(); exists_ = false;
    if (!Read(path_, kMaxBytes, baseline_, exists_)) return Result::IoError;
    if (!Decode(baseline_, text_)) return Result::InvalidText;
    std::string recovery; bool present = false;
    if (!Read(Recovery(path_), kMaxBytes * 2 + 64, recovery, present)) return Result::IoError;
    if (!present) { ready_ = true; return Result::Ok; }
    // Versioned recovery envelope; byte-counted baseline and draft, not Markdown.
    if (recovery.compare(0, 7, "WMEMO1\n") != 0 || recovery.size() < 10 ||
        (recovery[7] != '0' && recovery[7] != '1') || recovery[8] != '\n') return Result::InvalidText;
    const auto end = recovery.find('\n', 9);
    if (end == std::string::npos || end == 9 || end - 9 > 8) return Result::InvalidText;
    size_t count = 0;
    for (size_t i = 9; i < end; ++i) {
        if (recovery[i] < '0' || recovery[i] > '9') return Result::InvalidText;
        count = count * 10 + static_cast<size_t>(recovery[i] - '0');
    }
    if (count > kMaxBytes || count > recovery.size() - end - 1) return Result::InvalidText;
    const std::string base = recovery.substr(end + 1, count);
    const std::string draft = recovery.substr(end + 1 + count);
    std::wstring restored, validatedBase;
    if (!Decode(base, validatedBase) || !Decode(draft, restored)) return Result::InvalidText;
    ready_ = true;
    // Compare decoded text too: BOM/CRLF in the original need not match the LF
    // draft bytes. An envelope with no local edits must not revive an old base.
    if (restored == text_ || (exists_ && restored == validatedBase)) return Result::Ok;
    text_ = std::move(restored);
    const bool conflict = base != baseline_ || (recovery[7] == '1') != exists_;
    baseline_ = base; exists_ = recovery[7] == '1';
    return conflict ? Result::Conflict : Result::Recovered;
}

Result Document::Checkpoint(const std::wstring& text) {
    if (!ready_) return Result::IoError;
    std::string bytes;
    if (!Encode(text, bytes)) return Result::InvalidText;
    if (WMEMO_FAIL(CheckpointWrite) || !Write(Recovery(path_), Record(baseline_, exists_, bytes))) return Result::IoError;
    text_ = text;
    return Result::Ok;
}

Result Document::Save(const std::wstring& text) {
    if (!ready_) return Result::IoError;
    std::string bytes;
    if (!Encode(text, bytes)) return Result::InvalidText;
    std::string current; bool present = false;
    if (!Read(path_, kMaxBytes, current, present)) return Result::IoError;
    std::wstring currentText;
    if (!Decode(current, currentText)) return Result::InvalidText;
    if (currentText == text && present) {
        if (!SettleRecovery(path_, current, present, text)) return Result::IoError;
        baseline_ = current; exists_ = present; text_ = text;
        return Result::Ok;
    }
    if (current != baseline_ || present != exists_) {
        // No local edits: accept the external version without writing it back.
        std::wstring baseText;
        if (!Decode(baseline_, baseText)) return Result::InvalidText;
        if (text == baseText) {
            if (!SettleRecovery(path_, current, present, currentText)) return Result::IoError;
            baseline_ = current; exists_ = present; text_ = currentText;
            return Result::Ok;
        }
        const auto checkpoint = Checkpoint(text);
        return checkpoint == Result::Ok ? Result::Conflict : checkpoint;
    }
    const auto checkpoint = Checkpoint(text);
    if (checkpoint != Result::Ok) return checkpoint;
    std::filesystem::path temporary; HANDLE handle = INVALID_HANDLE_VALUE; std::wstring error;
    if (!atomic_write::CreateUniqueTempFile(path_, path_.parent_path(), &temporary, &handle, &error)) return Result::IoError;
    { File draft(handle); if (!WriteDraft(draft.get(), bytes)) return Result::IoError; }
    File original(CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path_).c_str(), GENERIC_READ | DELETE,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    std::filesystem::path backup;
    if (original.get() != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(original.get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return Result::IoError;
        if (!exists_ || !ReadHandle(original.get(), kMaxBytes, current) || current != baseline_) return Result::Conflict;
        const auto backupDir = path_.parent_path() / L"backups";
        if (!SafePath(backupDir) || !atomic_write::EnsureDirectoryExists(backupDir)) return Result::IoError;
        backup = atomic_write::MakeUniqueDestInDir(backupDir, L"workspace_memo.txt");
        if (WMEMO_FAIL(BackupRename) || !RenameHandle(original.get(), backup)) return Result::IoError;
        if (WMEMO_FAIL(AfterBackup)) return Result::IoError;
    } else {
        const DWORD code = GetLastError();
        if (exists_ || (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)) return Result::IoError;
    }
    if (WMEMO_FAIL(Install) || !MoveFileExW(ToExtendedWin32PathIfAbsoluteLocal(temporary).c_str(),
                     ToExtendedWin32PathIfAbsoluteLocal(path_).c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (!backup.empty() && !WMEMO_FAIL(Rollback)) { const bool restored = RenameHandle(original.get(), path_); (void)restored; }
        return Result::IoError;
    }
    if (WMEMO_FAIL(AfterInstall)) return Result::IoError;
    baseline_ = bytes; exists_ = true; text_ = text;
    return SettleRecovery(path_, bytes, true, text) ? Result::Ok : Result::IoError;
}

Result Document::Reload(const std::wstring& text) {
    if (!ready_) return Result::IoError;
    std::string bytes;
    if (!Encode(text, bytes)) return Result::InvalidText;
    const auto dir = path_.parent_path() / L"backups";
    if (!SafePath(dir) || !atomic_write::EnsureDirectoryExists(dir)) return Result::IoError;
    const auto draft = atomic_write::MakeUniqueDestInDir(dir, L"workspace_memo.conflict.txt");
    if (!Write(draft, bytes)) return Result::IoError;
    std::string current; bool present = false; std::wstring decoded;
    if (!Read(path_, kMaxBytes, current, present)) return Result::IoError;
    if (!Decode(current, decoded)) return Result::InvalidText;
    // A complete archive exists before replacing the recovery envelope.
    const auto oldBase = baseline_; const bool oldExists = exists_;
    baseline_ = current; exists_ = present;
    const auto result = Checkpoint(decoded);
    if (result != Result::Ok) { baseline_ = oldBase; exists_ = oldExists; return result; }
    text_ = std::move(decoded);
    return Result::Ok;
}
}
#undef WMEMO_FAIL
