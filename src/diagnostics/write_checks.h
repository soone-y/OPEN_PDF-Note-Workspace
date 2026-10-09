#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace write_checks {
enum class Kind : unsigned { SetupRead, SetupWrite, WorkspaceRead, WorkspaceWrite,
    SettingsWrite, TempWrite, RecoveryWrite, LogsWrite, PdfWrite, NoteWrite,
    OfficeTempWrite, PdfRead, OfficeConversion, NoteReadWrite, Count };
constexpr size_t kCount = static_cast<size_t>(Kind::Count);
enum class Outcome : unsigned { Passed, Failed, Unavailable, Canceled };
enum class Step : unsigned { Open, Write, Flush, Reopen, Compare, Delete, Convert, Pdf, Complete };
enum class Source : unsigned { Manual, Normal };
struct Result {
    Outcome outcome = Outcome::Failed;
    Step step = Step::Open;
    DWORD error = 0;
    std::uint64_t time = 0; // UTC FILETIME; display conversion happens only in the UI.
    std::wstring remaining;
    Source source = Source::Manual;
    std::wstring operationPath; // Evidence from normal use, never a disposable probe.
};
struct Record { Kind kind{}; std::wstring target; Result result; };

// One process-wide owner; the UI admits one worker at a time. Closing/reopening
// the window or switching workspace cannot reset this monotonic throttle.
class ManualLimit {
public:
    [[nodiscard]] bool Start(std::uint64_t now, bool cooldownEligible = false);
    void Finish(std::uint64_t now, bool unchanged);
    [[nodiscard]] std::uint64_t Remaining(std::uint64_t now) const;
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] unsigned RemainingRetries(std::uint64_t now) const { return Remaining(now) && !busy_ ? 2 - recoveryUsed_ : 0; }
    [[nodiscard]] bool RecoveryAvailable(std::uint64_t now) const { return RemainingRetries(now) > 0; }
private:
    unsigned count_ = 0;
    bool busy_ = false;
    unsigned recoveryUsed_ = 0;
    bool recoveryInFlight_ = false;
    std::uint64_t blockedUntil_ = 0;
};
[[nodiscard]] bool IsSafeLocalPath(const std::filesystem::path& path);
[[nodiscard]] Result ProbeDirectory(const std::filesystem::path& target, bool write);
[[nodiscard]] Result ProbeFile(const std::filesystem::path& target);
// Opens the original for read/write access without modifying it; reads bytes
// in bounded chunks, then tests a new disposable sibling. Never saves edits.
[[nodiscard]] Result ProbeNoteFile(const std::filesystem::path& target, const std::atomic_bool& cancel);
[[nodiscard]] std::uint64_t UtcNow();
[[nodiscard]] const wchar_t* LabelId(Kind kind);
[[nodiscard]] const wchar_t* StepId(Step step);
[[nodiscard]] bool Load(const std::filesystem::path& path, std::vector<Record>& records,
                        std::string& baseline, DWORD& error);
// Save removes oldest log entries above 64 records/64 KiB, retaining at least
// the newest entry. Records change only after verified atomic persistence.
// Corrupt/changed originals are protected; storage failure keeps UI results.
[[nodiscard]] bool Save(const std::filesystem::path& path, std::vector<Record>& records,
                        std::string& baseline, DWORD& error);
void Upsert(std::vector<Record>& records, Record record);
// Merge against a fresh, validated ledger; concurrent/stale results must not
// erase newer evidence. Failure preserves the caller's in-memory observations.
[[nodiscard]] bool SaveLatest(const std::filesystem::path& path, std::vector<Record>& records,
                              std::string& baseline, DWORD& error);
void EnableNormalObservations(bool persist = true) noexcept;
// Flush only the captured workspace's pending evidence at explicit lifecycle
// boundaries. Never called by a display timer; failure preserves pending data.
[[nodiscard]] bool FlushNormalObservations(const std::filesystem::path& workspace) noexcept;
// Memory only, nonblocking: safe for the dialog's display timer. Returns a
// revision, 0 if busy. IO and persistence belong to operation completion.
[[nodiscard]] std::uint64_t MergeNormalObservations(const std::filesystem::path& store,
    std::vector<Record>& records, bool& unsaved, DWORD& error);
void AcknowledgeNormalObservations(const std::filesystem::path& store,
    const std::vector<Record>& persisted) noexcept;
[[nodiscard]] const Record* Find(const std::vector<Record>& records, Kind kind,
                                const std::wstring& target);
}
