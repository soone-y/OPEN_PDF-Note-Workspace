#pragma once

#include "note/note_model.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace note {

enum class NoteEditKind {
    None,
    PlainText,
    LineBreakOnly,
    StructuralText,
};

enum class NoteDirtySyntaxFeature : uint32_t {
    None = 0,
    CodeFence = 1u << 0,
    LegacyStyle = 1u << 1,
    Math = 1u << 2,
    Link = 1u << 3,
    BlockStructure = 1u << 4,
};

enum class NoteDirtyPropagation {
    None,
    LocalLine,
    DelimiterRegion,
    FullDocument,
};

struct NoteDirtyLineRange {
    bool valid = false;
    size_t first = 0;
    size_t last = 0;
};

struct NoteDirtyGraph {
    bool has_edit = false;
    TextEdit edit;
    NoteEditKind edit_kind = NoteEditKind::None;
    NoteDirtySyntaxFeature syntax_features = NoteDirtySyntaxFeature::None;
    NoteDirtyPropagation propagation = NoteDirtyPropagation::None;
    bool content_dirty = false;
    bool syntax_dirty = false;
    bool structure_dirty = false;
    bool render_dirty = false;
    bool layout_dirty = false;
    bool render_stale = false;
    bool line_count_may_change = false;
    NoteDirtyLineRange stale_lines;
};

bool ContainsNoteStructuralText(std::wstring_view text);
bool HasNoteDirtySyntaxFeature(NoteDirtySyntaxFeature features,
                               NoteDirtySyntaxFeature feature);
bool IsOnlyNoteLineBreakText(std::wstring_view text);
bool TextEditsEqual(const TextEdit& lhs, const TextEdit& rhs);

NoteDirtyGraph BuildNoteDirtyGraph(std::wstring_view beforeText,
                                   const std::vector<size_t>& beforeLineStarts,
                                   const TextEdit& edit,
                                   bool renderActive);
bool NoteDirtyGraphAllowsRenderEarlyStop(const NoteDirtyGraph& graph,
                                         bool lineCountChanged);
bool NoteDirtyGraphAllowsLineSpacingFastPath(const NoteDirtyGraph& graph);

} // namespace note
