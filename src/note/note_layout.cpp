#include "note_layout.h"

#include <utility>

namespace note {

bool operator==(const NoteLayoutLineMetrics& lhs,
                const NoteLayoutLineMetrics& rhs) {
    return lhs.line_height_permille == rhs.line_height_permille &&
           lhs.max_font_px == rhs.max_font_px &&
           lhs.top_margin_percent_override == rhs.top_margin_percent_override &&
           lhs.visual_ascent_px == rhs.visual_ascent_px &&
           lhs.visual_descent_px == rhs.visual_descent_px;
}

bool operator!=(const NoteLayoutLineMetrics& lhs,
                const NoteLayoutLineMetrics& rhs) {
    return !(lhs == rhs);
}

bool NoteLayoutMetricsSnapshot::Matches(
    NoteDerivedSnapshotIdentity identity) const {
    return source_identity.valid() && source_identity == identity;
}

void NoteLayoutMetricsSnapshot::Commit(
    NoteDerivedSnapshotIdentity identity,
    std::vector<NoteLayoutLineMetrics> nextLines) {
    source_identity = identity;
    lines = std::move(nextLines);
}

void NoteLayoutMetricsSnapshot::Reset() {
    source_identity = {};
    lines.clear();
}

bool SameParagraphSpacing(const NoteLayoutLineMetrics& lhs,
                          const NoteLayoutLineMetrics& rhs) {
    return lhs.line_height_permille == rhs.line_height_permille &&
           lhs.max_font_px == rhs.max_font_px;
}

bool SameParagraphSpacing(const std::vector<NoteLayoutLineMetrics>& lhs,
                          const std::vector<NoteLayoutLineMetrics>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!SameParagraphSpacing(lhs[i], rhs[i])) return false;
    }
    return true;
}

bool SameVisualMetrics(const NoteLayoutLineMetrics& lhs,
                       const NoteLayoutLineMetrics& rhs) {
    return lhs.top_margin_percent_override == rhs.top_margin_percent_override &&
           lhs.visual_ascent_px == rhs.visual_ascent_px &&
           lhs.visual_descent_px == rhs.visual_descent_px;
}

bool SameVisualMetrics(const std::vector<NoteLayoutLineMetrics>& lhs,
                       const std::vector<NoteLayoutLineMetrics>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!SameVisualMetrics(lhs[i], rhs[i])) return false;
    }
    return true;
}

bool CanAdvanceLayoutSnapshot(const NoteLayoutMetricsSnapshot& previous,
                              NoteDerivedSnapshotIdentity nextIdentity) {
    return previous.source_identity.valid() && nextIdentity.valid() &&
           previous.source_identity.note_id == nextIdentity.note_id &&
           previous.source_identity.source_revision <= nextIdentity.source_revision;
}

bool LayoutMetricsUnchangedOutsideRange(
    const std::vector<NoteLayoutLineMetrics>& before,
    const std::vector<NoteLayoutLineMetrics>& after,
    size_t first,
    size_t last) {
    if (before.size() != after.size() || first > last || last >= after.size()) {
        return false;
    }
    for (size_t line = 0; line < after.size(); ++line) {
        if (line >= first && line <= last) continue;
        if (before[line] != after[line]) return false;
    }
    return true;
}

} // namespace note
