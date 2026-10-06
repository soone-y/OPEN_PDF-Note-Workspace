#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace workspace_memo {
enum class Result { Ok, Recovered, Conflict, IoError, InvalidText };
inline constexpr size_t kMaxBytes = 1024 * 1024;
inline constexpr size_t kMaxEditorUnits = kMaxBytes * 2; // LF -> CRLF view expansion
[[nodiscard]] std::filesystem::path MemoPath(const std::filesystem::path& root);
// Same UTF-8/LF byte limit for input, checkpoints and saving. Never truncates.
[[nodiscard]] bool ValidateText(std::wstring_view text);

#ifdef WORKSPACE_MEMO_TESTING
namespace testing {
enum class FaultPoint { CheckpointWrite, TempWrite, TempFlush, BackupRename,
                        Install, Rollback, CleanupDelete, CleanupWrite, AfterBackup, AfterInstall };
using FaultHook = bool (*)(FaultPoint);
void SetFaultHook(FaultHook hook);
}
#endif

// UI-thread-owned document, pinned to one root (never reads the global root).
// Recovery stores both the observed original and the current draft atomically.
// A failed save never advances the baseline or discards a draft/backup.
class Document {
public:
    [[nodiscard]] Result Load(const std::filesystem::path& root);
    [[nodiscard]] Result Checkpoint(const std::wstring& text);
    [[nodiscard]] Result Save(const std::wstring& text);
    // Explicit reload preserves the current draft before accepting external text.
    [[nodiscard]] Result Reload(const std::wstring& text);
    const std::wstring& text() const { return text_; }
    const std::filesystem::path& path() const { return path_; }
    bool writable() const { return ready_; }
private:
    std::filesystem::path path_;
    std::string baseline_;
    std::wstring text_;
    bool exists_ = false;
    bool ready_ = false;
};
}
