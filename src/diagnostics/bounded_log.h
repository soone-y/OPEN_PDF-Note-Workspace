#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace diagnostic_log {
// Decimal MB. Only these diagnostic files share the budget; recovery data,
// user-created ZIP archives and the separately bounded write-check ledger do not.
inline constexpr std::uint64_t kMaxBytes = 10'000'000;
inline constexpr std::array<const wchar_t*, 5> kFileNames = {
    L"preview_trace.log", L"switch_timing.log", L"crash.log",
    L"startup_watchdog.log", L"office_conversion.log"};
enum class Result { Written, LimitReached, Unavailable };
struct Status {
    bool available = false;
    bool stopped = false;
    std::uint64_t bytes = 0;
};
// No exceptions or UI escape this optional diagnostic boundary. Unknown files,
// unsafe paths, busy handles and inspection failures cause no diagnostic write.
// Existing bytes are never truncated/deleted. A terminal log line persists the
// stop across restarts; explicit deletion of the diagnostic logs permits resume.
[[nodiscard]] Result Append(const std::filesystem::path& workspace,
                            const wchar_t* fileName, std::string_view payload) noexcept;
[[nodiscard]] Status Inspect(const std::filesystem::path& workspace) noexcept;
}
