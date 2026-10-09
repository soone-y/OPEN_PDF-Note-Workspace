#pragma once

#include "core/annot_commands.h"

// Pure, exact command preconditions shared by editing and stage replay.
inline bool AnnotationEquals(const Annotation& a, const Annotation& b) {
    if (a.type != b.type) return false;
    if (a.shapeKind != b.shapeKind) return false;
    if (a.shapeDrawMode != b.shapeDrawMode) return false;
    if (a.arrowHead != b.arrowHead) return false;
    if (a.shapeRotation != b.shapeRotation) return false;
    if (a.pageIndex != b.pageIndex) return false;
    if (a.x1 != b.x1 || a.y1 != b.y1 || a.x2 != b.x2 || a.y2 != b.y2) return false;
    if (a.width != b.width || a.alpha != b.alpha || a.fontPt != b.fontPt) return false;
    if (a.color != b.color) return false;
    if (a.text != b.text || a.fontName != b.fontName) return false;
    if (a.linkId != b.linkId || a.linkNotePath != b.linkNotePath) return false;
    if (a.mathKind != b.mathKind || a.backgroundAssistMode != b.backgroundAssistMode) return false;
    if (a.path.size() != b.path.size()) return false;
    for (size_t i = 0; i < a.path.size(); ++i) {
        if (a.path[i].x != b.path[i].x || a.path[i].y != b.path[i].y) return false;
    }
    if (a.dash.size() != b.dash.size()) return false;
    for (size_t i = 0; i < a.dash.size(); ++i) {
        if (a.dash[i] != b.dash[i]) return false;
    }
    if (a.quads.size() != b.quads.size()) return false;
    for (size_t i = 0; i < a.quads.size(); ++i) {
        if (a.quads[i] != b.quads[i]) return false;
    }
    return true;
}

inline int FindAnnotIndexByIdInList(const std::vector<Annotation>& annots, std::wstring_view id) {
    if (id.empty()) return -1;
    for (size_t i = 0; i < annots.size(); ++i) {
        if (annots[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

inline bool AnnotationAtMatches(const std::vector<Annotation>& annots,
                                int index,
                                const Annotation& expected) {
    return !expected.id.empty() && index >= 0 &&
           index < static_cast<int>(annots.size()) &&
           annots[static_cast<size_t>(index)].id == expected.id &&
           AnnotationEquals(annots[static_cast<size_t>(index)], expected);
}

inline bool AnnotationListsMatchExactly(const std::vector<Annotation>& left,
                                        const std::vector<Annotation>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].id != right[i].id || !AnnotationEquals(left[i], right[i])) {
            return false;
        }
    }
    return true;
}

inline bool TryApplyAnnotCommandForward(std::vector<Annotation>* annots,
                                        const AnnotCommand& cmd) {
    if (!annots) return false;
    switch (cmd.kind) {
    case AnnotCommandKind::Add:
        if (cmd.after.id.empty() || cmd.afterIndex < 0 ||
            cmd.afterIndex > static_cast<int>(annots->size()) ||
            FindAnnotIndexByIdInList(*annots, cmd.after.id) >= 0) {
            return false;
        }
        annots->insert(annots->begin() + cmd.afterIndex, cmd.after);
        return true;
    case AnnotCommandKind::Remove:
        if (!AnnotationAtMatches(*annots, cmd.beforeIndex, cmd.before)) return false;
        annots->erase(annots->begin() + cmd.beforeIndex);
        return true;
    case AnnotCommandKind::Update:
        if (cmd.before.id.empty() || cmd.after.id.empty() ||
            cmd.before.id != cmd.after.id ||
            !AnnotationAtMatches(*annots, cmd.beforeIndex, cmd.before)) {
            return false;
        }
        if (cmd.beforeIndex == cmd.afterIndex) {
            (*annots)[static_cast<size_t>(cmd.beforeIndex)] = cmd.after;
            return true;
        }
        annots->erase(annots->begin() + cmd.beforeIndex);
        if (cmd.afterIndex < 0 || cmd.afterIndex > static_cast<int>(annots->size())) {
            return false;
        }
        annots->insert(annots->begin() + cmd.afterIndex, cmd.after);
        return true;
    case AnnotCommandKind::Reorder:
        if (!AnnotationListsMatchExactly(*annots, cmd.snapshot)) return false;
        *annots = cmd.afterSnapshot;
        return true;
    case AnnotCommandKind::ClearAll:
        if (!AnnotationListsMatchExactly(*annots, cmd.snapshot)) return false;
        annots->clear();
        return true;
    }
    return false;
}

inline bool TryApplyAnnotCommandReverse(std::vector<Annotation>* annots,
                                        const AnnotCommand& cmd) {
    if (!annots) return false;
    switch (cmd.kind) {
    case AnnotCommandKind::Add:
        if (!AnnotationAtMatches(*annots, cmd.afterIndex, cmd.after)) return false;
        annots->erase(annots->begin() + cmd.afterIndex);
        return true;
    case AnnotCommandKind::Remove:
        if (cmd.before.id.empty() || cmd.beforeIndex < 0 ||
            cmd.beforeIndex > static_cast<int>(annots->size()) ||
            FindAnnotIndexByIdInList(*annots, cmd.before.id) >= 0) {
            return false;
        }
        annots->insert(annots->begin() + cmd.beforeIndex, cmd.before);
        return true;
    case AnnotCommandKind::Update:
        if (cmd.before.id.empty() || cmd.after.id.empty() ||
            cmd.before.id != cmd.after.id ||
            !AnnotationAtMatches(*annots, cmd.afterIndex, cmd.after)) {
            return false;
        }
        if (cmd.beforeIndex == cmd.afterIndex) {
            (*annots)[static_cast<size_t>(cmd.afterIndex)] = cmd.before;
            return true;
        }
        annots->erase(annots->begin() + cmd.afterIndex);
        if (cmd.beforeIndex < 0 || cmd.beforeIndex > static_cast<int>(annots->size())) {
            return false;
        }
        annots->insert(annots->begin() + cmd.beforeIndex, cmd.before);
        return true;
    case AnnotCommandKind::Reorder:
        if (!AnnotationListsMatchExactly(*annots, cmd.afterSnapshot)) return false;
        *annots = cmd.snapshot;
        return true;
    case AnnotCommandKind::ClearAll:
        if (!annots->empty()) return false;
        *annots = cmd.snapshot;
        return true;
    }
    return false;
}

inline bool ApplyAnnotCommandToList(std::vector<Annotation>* annots, const AnnotCommand& cmd) {
    if (!annots) return false;
    std::vector<Annotation> next;
    try {
        next = *annots;
    } catch (...) {
        return false;
    }
    if (!TryApplyAnnotCommandForward(&next, cmd)) return false;
    *annots = std::move(next);
    return true;
}

