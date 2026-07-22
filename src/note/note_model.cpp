#include "note/note_model.h"

#include <algorithm>
#include <filesystem>

namespace note {

std::wstring DeriveTitleFromFileName(std::wstring_view fileName) {
    if (fileName.empty()) return L"";
    std::filesystem::path path{std::wstring(fileName)};
    std::wstring stem = path.stem().wstring();
    if (!stem.empty()) return stem;
    return std::wstring(fileName);
}

std::vector<size_t> BuildLineStarts(std::wstring_view raw) {
    std::vector<size_t> starts;
    starts.reserve(64);
    starts.push_back(0);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r') {
            size_t next = i + 1;
            if (next < raw.size() && raw[next] == L'\n') {
                ++next;
                ++i;
            }
            starts.push_back(next);
        } else if (raw[i] == L'\n') {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

static size_t AdvanceLineBreak(std::wstring_view raw, size_t pos) {
    if (pos >= raw.size()) return pos;
    if (raw[pos] == L'\r') {
        size_t next = pos + 1;
        if (next < raw.size() && raw[next] == L'\n') {
            ++next;
        }
        return next;
    }
    if (raw[pos] == L'\n') {
        return pos + 1;
    }
    return pos;
}

static size_t FindLineIndexForOffset(const std::vector<size_t>& starts, size_t offset) {
    if (starts.empty()) return 0;
    auto it = std::upper_bound(starts.begin(), starts.end(), offset);
    if (it == starts.begin()) return 0;
    return static_cast<size_t>(std::distance(starts.begin(), it - 1));
}

static size_t FindAffectedLineRangeEnd(std::wstring_view raw, size_t offset) {
    offset = std::min(offset, raw.size());
    while (offset < raw.size() && raw[offset] != L'\r' && raw[offset] != L'\n') {
        ++offset;
    }
    return AdvanceLineBreak(raw, offset);
}

static std::vector<size_t> BuildLineStartsInRange(std::wstring_view raw,
                                                  size_t rangeStart,
                                                  size_t rangeEnd) {
    std::vector<size_t> starts;
    rangeStart = std::min(rangeStart, raw.size());
    rangeEnd = std::min(rangeEnd, raw.size());
    starts.push_back(rangeStart);
    for (size_t i = rangeStart; i < rangeEnd; ++i) {
        if (raw[i] == L'\r') {
            size_t next = i + 1;
            if (next < raw.size() && raw[next] == L'\n') {
                ++next;
                ++i;
            }
            if (next <= raw.size() && (starts.empty() || starts.back() != next)) {
                starts.push_back(next);
            }
        } else if (raw[i] == L'\n') {
            const size_t next = i + 1;
            if (next <= raw.size() && (starts.empty() || starts.back() != next)) {
                starts.push_back(next);
            }
        }
    }
    return starts;
}

static bool TryUpdateLineStartsForEdit(NoteTextModel* model,
                                       const std::vector<size_t>& oldStarts,
                                       size_t start,
                                       size_t deletedLen,
                                       size_t insertedLen,
                                       size_t oldSize,
                                       size_t oldAffectedEnd) {
    if (!model || oldStarts.empty() || oldStarts.front() != 0) return false;
    if (start > oldSize || deletedLen > oldSize - start) return false;
    if (oldAffectedEnd > oldSize) return false;

    const size_t oldFirstLine = FindLineIndexForOffset(oldStarts, start);
    if (oldFirstLine >= oldStarts.size()) return false;

    const size_t oldRangeStart = oldStarts[oldFirstLine];
    const size_t newAffectedEnd = FindAffectedLineRangeEnd(model->raw, start + insertedLen);

    auto oldResumeIt = std::upper_bound(oldStarts.begin(), oldStarts.end(), oldAffectedEnd);
    const ptrdiff_t delta = static_cast<ptrdiff_t>(model->raw.size()) -
                            static_cast<ptrdiff_t>(oldSize);

    std::vector<size_t> nextStarts;
    nextStarts.reserve(oldStarts.size() + 4);
    for (size_t i = 0; i < oldFirstLine; ++i) {
        nextStarts.push_back(oldStarts[i]);
    }

    std::vector<size_t> rebuilt = BuildLineStartsInRange(model->raw, oldRangeStart, newAffectedEnd);
    for (size_t pos : rebuilt) {
        if (nextStarts.empty() || nextStarts.back() != pos) {
            nextStarts.push_back(pos);
        }
    }

    for (auto it = oldResumeIt; it != oldStarts.end(); ++it) {
        const ptrdiff_t shifted = static_cast<ptrdiff_t>(*it) + delta;
        if (shifted < 0) return false;
        const size_t pos = std::min(static_cast<size_t>(shifted), model->raw.size());
        if (nextStarts.empty() || nextStarts.back() != pos) {
            nextStarts.push_back(pos);
        }
    }

    if (nextStarts.empty() || nextStarts.front() != 0 ||
        !std::is_sorted(nextStarts.begin(), nextStarts.end())) {
        return false;
    }
    model->line_starts = std::move(nextStarts);
    return true;
}

NoteTextModel MakeNoteTextModel(NoteMetadata meta, std::wstring raw, uint64_t revision) {
    if (meta.title.empty()) {
        meta.title = DeriveTitleFromFileName(meta.file_name);
    }

    NoteTextModel model;
    model.meta = std::move(meta);
    model.raw = std::move(raw);
    model.revision = revision;
    model.line_starts = BuildLineStarts(model.raw);
    return model;
}

std::optional<TextEdit> ComputeNoteTextEdit(std::wstring_view before,
                                            std::wstring_view after) {
    if (before == after) return TextEdit{};

    size_t prefix = 0;
    const size_t minLength = std::min(before.size(), after.size());
    while (prefix < minLength && before[prefix] == after[prefix]) ++prefix;

    const size_t beforeRemaining = before.size() - prefix;
    const size_t afterRemaining = after.size() - prefix;
    const size_t maxSuffix = std::min(beforeRemaining, afterRemaining);
    size_t suffix = 0;
    while (suffix < maxSuffix &&
           before[before.size() - 1 - suffix] ==
               after[after.size() - 1 - suffix]) {
        ++suffix;
    }

    TextEdit edit;
    edit.start = {prefix};
    edit.deleted_len = before.size() - prefix - suffix;
    edit.inserted_text = std::wstring(
        after.substr(prefix, after.size() - prefix - suffix));
    return edit;
}

void ApplyTextEdit(NoteTextModel* model, const TextEdit& edit) {
    if (!model) return;
    const size_t start = std::min(edit.start.value, model->raw.size());
    const size_t deletedLen = std::min(edit.deleted_len, model->raw.size() - start);
    const size_t oldSize = model->raw.size();
    const size_t oldAffectedEnd = FindAffectedLineRangeEnd(model->raw, start + deletedLen);
    const std::vector<size_t>& oldStarts = model->line_starts;
    model->raw.replace(start, deletedLen, edit.inserted_text);
    model->revision += 1;
    if (!TryUpdateLineStartsForEdit(model,
                                    oldStarts,
                                    start,
                                    deletedLen,
                                    edit.inserted_text.size(),
                                    oldSize,
                                    oldAffectedEnd)) {
        model->line_starts = BuildLineStarts(model->raw);
    }
}

LineColumn ResolveLineColumn(const NoteTextModel& model, size_t offset) {
    LineColumn out{};
    const size_t clamped = std::min(offset, model.raw.size());
    auto it = std::upper_bound(model.line_starts.begin(), model.line_starts.end(), clamped);
    if (it == model.line_starts.begin()) {
        out.line = 1;
        out.column = static_cast<int>(clamped) + 1;
        return out;
    }

    const size_t lineIndex = static_cast<size_t>(std::distance(model.line_starts.begin(), it - 1));
    const size_t lineStart = *(it - 1);
    out.line = static_cast<int>(lineIndex + 1);
    out.column = static_cast<int>(clamped - lineStart + 1);
    return out;
}

void SetNoteDocumentSourceRevision(NoteDocument* doc, uint64_t sourceRevision) {
    if (!doc) return;
    doc->source_identity.source_revision = sourceRevision;
}

void SetNoteDocumentSourceIdentity(NoteDocument* doc,
                                   NoteDerivedSnapshotIdentity sourceIdentity) {
    if (!doc) return;
    doc->source_identity = sourceIdentity;
}

bool NoteDocumentMatchesSourceRevision(const NoteDocument& doc, uint64_t sourceRevision) {
    return sourceRevision != 0 &&
           doc.source_identity.source_revision == sourceRevision;
}

bool NoteDocumentMatchesSourceIdentity(
    const NoteDocument& doc,
    NoteDerivedSnapshotIdentity sourceIdentity) {
    return sourceIdentity.valid() && doc.source_identity == sourceIdentity;
}

bool NoteDocumentMatchesTextModel(const NoteDocument& doc, const NoteTextModel& model) {
    return NoteDocumentMatchesSourceRevision(doc, model.revision);
}

void SetNoteLayoutSourceRevision(NoteLayout* layout, uint64_t sourceRevision) {
    if (!layout) return;
    layout->source_identity.source_revision = sourceRevision;
}

void SetNoteLayoutSourceIdentity(NoteLayout* layout,
                                 NoteDerivedSnapshotIdentity sourceIdentity) {
    if (!layout) return;
    layout->source_identity = sourceIdentity;
}

bool NoteLayoutMatchesSourceRevision(const NoteLayout& layout, uint64_t sourceRevision) {
    return sourceRevision != 0 &&
           layout.source_identity.source_revision == sourceRevision;
}

bool NoteLayoutMatchesSourceIdentity(
    const NoteLayout& layout,
    NoteDerivedSnapshotIdentity sourceIdentity) {
    return sourceIdentity.valid() && layout.source_identity == sourceIdentity;
}

bool NoteLayoutMatchesTextModel(const NoteLayout& layout, const NoteTextModel& model) {
    return NoteLayoutMatchesSourceRevision(layout, model.revision);
}

} // namespace note
