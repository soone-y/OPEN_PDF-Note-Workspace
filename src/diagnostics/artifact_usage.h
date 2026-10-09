#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace artifact_usage {
enum class Area : unsigned { All, Logs, Temporary, Recovery, SaveBackups, Operations, MemoBackups, Count };
inline constexpr size_t kAreaCount = static_cast<size_t>(Area::Count);
enum class State { Complete, Partial, Canceled, Limited, Missing, Unavailable };
struct Totals {
    State state = State::Complete;
    std::uint64_t bytes = 0, files = 0;
    unsigned skipped = 0;
    unsigned long error = 0;
};
struct Result {
    std::array<Totals, kAreaCount> areas{};
    std::uint64_t checkedAt = 0; // UTC FILETIME at completion, not window-open time.
};
struct Limits {
    std::uint64_t entries = 100000;
    unsigned depth = 64;
    std::uint64_t milliseconds = 5000;
};
// Read-only snapshot of the captured workspace's management directory ONLY.
// Counts logical file sizes once per name (hardlinks/sparse files are not disk
// allocation). Parent rows contain child rows: callers must not sum the areas.
// Never follows reparse points or opens document bodies. No files are created,
// changed or deleted. Cancellation/limits/errors retain clearly partial counts.
[[nodiscard]] Result Scan(const std::filesystem::path& workspace,
                          const std::atomic_bool& cancel, Limits limits = {}) noexcept;
}
