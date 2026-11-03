#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace open_work {

// UI-thread diagnostic only: no document reads, allocation, IO, or wall-clock
// pass criteria. Normal operation pays only a disabled thread-local check.
enum class Kind : std::size_t {
    NoteStageCheck,
    NoteEditorSyncFullRead,
    NoteFullParse,
    NoteSnapshotFingerprint,
    PdfViewportCacheUpdate,
    PdfCacheBeforeRestore,
    PdfBitmapQueued,
    Count
};

using Counts = std::array<uint64_t, static_cast<std::size_t>(Kind::Count)>;
inline thread_local Counts totals{};
inline thread_local unsigned observationDepth = 0;

inline void Record(Kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (observationDepth != 0 && index < totals.size()) {
        ++totals[index];
    }
}

class Observation {
public:
    Observation() noexcept : before_(totals) { ++observationDepth; }
    ~Observation() { --observationDepth; }
    Observation(const Observation&) = delete;
    Observation& operator=(const Observation&) = delete;

    [[nodiscard]] Counts Read() const noexcept {
        Counts result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = totals[i] - before_[i];
        }
        return result;
    }

private:
    Counts before_;
};

[[nodiscard]] inline uint64_t Get(const Counts& counts, Kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < counts.size() ? counts[index] : 0;
}

} // namespace open_work
