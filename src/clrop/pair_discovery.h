#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace clrop::discovery {
enum class Mode { Directory, ForClrop };
enum class Evidence { Unknown, Match, Different, Missing };
enum class Grade { Hash, StructureAndSize, Structure, Size, Pages, Name, NoCandidate, Unreadable };
enum class Completion { Complete, Canceled, Limited, Failed };
struct Limits {
    size_t entries = 20000, pdfs = 512, clrops = 256, rows = 10000, pages = 10000;
    std::uint64_t hashBytes = 1024ull * 1024 * 1024, clropBytes = 32ull * 1024 * 1024;
    std::uint64_t resultBytes = 64ull * 1024 * 1024;
    unsigned milliseconds = 30000;
};
struct Request {
    Mode mode = Mode::Directory;
    std::filesystem::path directory, clrop;
    Limits limits;
};
struct Row {
    std::filesystem::path clrop, pdf;
    Grade grade = Grade::Unreadable;
    Evidence hash = Evidence::Unknown, size = Evidence::Unknown,
        pages = Evidence::Unknown, dimensions = Evidence::Unknown;
    bool orphan = false, sameName = false, preview = false;
    std::uint64_t expectedSize = 0, actualSize = 0;
    int expectedPages = 0, actualPages = 0;
    std::string expectedHash, actualHash;
};
struct Result {
    std::vector<Row> rows;
    Completion completion = Completion::Complete;
    size_t entries = 0, pdfs = 0, clrops = 0, skipped = 0;
    std::uint64_t hashedBytes = 0;
};
// Synchronous, read-only engine for an owning worker. Only the explicitly
// chosen directory's immediate children are enumerated. No path from CLROP
// expands the search. Cancellation/time limits are cooperative around I/O.
[[nodiscard]] Result Scan(const Request& request, const std::atomic_bool& cancel);
} // namespace clrop::discovery
