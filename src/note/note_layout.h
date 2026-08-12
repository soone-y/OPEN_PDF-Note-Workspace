#pragma once

#include <cstddef>
#include <vector>

#include "note_identity.h"

namespace note {

struct NoteLayoutLineMetrics {
    int line_height_permille = 1000;
    int max_font_px = 0;
    int top_margin_percent_override = 0;
    int visual_ascent_px = 0;
    int visual_descent_px = 0;
};

bool operator==(const NoteLayoutLineMetrics& lhs,
                const NoteLayoutLineMetrics& rhs);
bool operator!=(const NoteLayoutLineMetrics& lhs,
                const NoteLayoutLineMetrics& rhs);

struct NoteLayoutMetricsSnapshot {
    NoteDerivedSnapshotIdentity source_identity{};
    std::vector<NoteLayoutLineMetrics> lines;

    bool Matches(NoteDerivedSnapshotIdentity identity) const;
    void Commit(NoteDerivedSnapshotIdentity identity,
                std::vector<NoteLayoutLineMetrics> nextLines);
    void Reset();
};

bool SameParagraphSpacing(const NoteLayoutLineMetrics& lhs,
                          const NoteLayoutLineMetrics& rhs);
bool SameParagraphSpacing(const std::vector<NoteLayoutLineMetrics>& lhs,
                          const std::vector<NoteLayoutLineMetrics>& rhs);
bool SameVisualMetrics(const NoteLayoutLineMetrics& lhs,
                       const NoteLayoutLineMetrics& rhs);
bool SameVisualMetrics(const std::vector<NoteLayoutLineMetrics>& lhs,
                       const std::vector<NoteLayoutLineMetrics>& rhs);

// A stale layout may only seed an incremental update for a newer revision of
// the same note. Exact identity matching remains mandatory for normal reads.
bool CanAdvanceLayoutSnapshot(const NoteLayoutMetricsSnapshot& previous,
                              NoteDerivedSnapshotIdentity nextIdentity);

bool LayoutMetricsUnchangedOutsideRange(
    const std::vector<NoteLayoutLineMetrics>& before,
    const std::vector<NoteLayoutLineMetrics>& after,
    size_t first,
    size_t last);

} // namespace note
