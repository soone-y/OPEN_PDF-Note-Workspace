#include "note/note_text_core.h"

#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
void NoteTextCore::Reset(NoteId noteId,
                         NoteMetadata metadata,
                         std::wstring raw,
                         uint64_t contentRevision,
                         uint64_t persistenceRevision) {
    NoteTextPieceSequence next;
    NoteSourceLineMap::Snapshot nextLineMap;
    if (!NoteSourceLineMap::Build(raw, &nextLineMap)) {
        note_id_ = {};
        metadata_ = {};
        canonical_ = NoteTextPieceSequence{};
        source_line_map_ = {};
        materialized_model_ = {};
        materialized_model_current_ = false;
        materialization_count_ = 0;
        content_revision_ = 0;
        persistence_revision_ = 0;
        return;
    }
    if (next.Reset(std::move(raw)) != NoteTextPieceSequenceMutationResult::Applied) {
        // A failed in-memory replacement must fail closed.  The caller cannot
        // obtain or save a stale prior note through this core; the persisted
        // source remains untouched and can be reloaded after recovery.
        note_id_ = {};
        metadata_ = {};
        canonical_ = NoteTextPieceSequence{};
        source_line_map_ = {};
        materialized_model_ = {};
        materialized_model_current_ = false;
        materialization_count_ = 0;
        content_revision_ = 0;
        persistence_revision_ = 0;
        return;
    }
    if (metadata.title.empty()) {
        metadata.title = DeriveTitleFromFileName(metadata.file_name);
    }
    note_id_ = noteId;
    metadata_ = std::move(metadata);
    canonical_ = std::move(next);
    source_line_map_ = std::move(nextLineMap);
    materialized_model_ = {};
    materialized_model_current_ = false;
    materialization_count_ = 0;
    content_revision_ = contentRevision;
    persistence_revision_ = persistenceRevision;
}

NoteTextApplyResult NoteTextCore::Apply(const TextEdit& edit) {
    if (!note_id_.valid()) return NoteTextApplyResult::InvalidOwner;
    if (edit.start.value > canonical_.text_length() ||
        edit.deleted_len > canonical_.text_length() - edit.start.value) {
        return NoteTextApplyResult::InvalidRange;
    }
    if (edit.deleted_len == 0 && edit.inserted_text.empty()) {
        return NoteTextApplyResult::NoChange;
    }
    if (content_revision_ == std::numeric_limits<uint64_t>::max()) {
        return NoteTextApplyResult::RevisionExhausted;
    }
    std::optional<NoteSourceLineLocation> firstLine =
        NoteSourceLineMap::FindByOffset(source_line_map_, edit.start);
    const Utf16CodeUnitOffset oldEditEnd{edit.start.value + edit.deleted_len};
    const auto lastLine = NoteSourceLineMap::FindByOffset(source_line_map_, oldEditEnd);
    if (!firstLine.has_value() || !lastLine.has_value() ||
        firstLine->line_index.value > lastLine->line_index.value ||
        lastLine->next_start < firstLine->start) {
        return NoteTextApplyResult::StorageFailure;
    }

    // A lone CR owns the preceding row while its following position is the
    // next row's start. Inserting a LF at that position creates one CRLF
    // transport unit, so the preceding descriptor must be rebuilt too. This
    // is the only cross-row newline pairing: inserting CR after LF remains
    // two logical breaks and is already represented by the local range.
    if (edit.start == firstLine->start && firstLine->line_index.value > 0 &&
        !edit.inserted_text.empty() && edit.inserted_text.front() == L'\n' &&
        edit.start.value > 0 && canonical_.EqualsRange(
            {edit.start.value - 1}, std::wstring_view(L"\r"))) {
        firstLine = NoteSourceLineMap::LineAt(
            source_line_map_, {firstLine->line_index.value - 1});
        if (!firstLine.has_value() || lastLine->next_start < firstLine->start) {
            return NoteTextApplyResult::StorageFailure;
        }
    }
    const size_t sourceRangeLength = lastLine->next_start - firstLine->start;
    const size_t localEditStart = edit.start - firstLine->start;
    if (localEditStart > sourceRangeLength ||
        edit.deleted_len > sourceRangeLength - localEditStart) {
        return NoteTextApplyResult::StorageFailure;
    }
    const size_t retainedLocalLength = sourceRangeLength - edit.deleted_len;
    if (edit.inserted_text.size() >
        std::numeric_limits<size_t>::max() - retainedLocalLength) {
        return NoteTextApplyResult::StorageFailure;
    }
    std::wstring beforeRange;
    std::wstring afterRange;
    std::vector<NoteSourceLineSpec> replacementLines;
    NoteSourceLineMap::Snapshot nextLineMap;
    try {
        beforeRange = canonical_.CopyRange(firstLine->start, sourceRangeLength);
        if (beforeRange.size() != sourceRangeLength) return NoteTextApplyResult::StorageFailure;
        afterRange.reserve(retainedLocalLength + edit.inserted_text.size());
        afterRange.append(beforeRange, 0, localEditStart);
        afterRange += edit.inserted_text;
        afterRange.append(beforeRange, localEditStart + edit.deleted_len,
                          sourceRangeLength - localEditStart - edit.deleted_len);
    } catch (const std::exception&) {
        return NoteTextApplyResult::StorageFailure;
    }
    const bool rangeEndsAtDocumentEnd =
        lastLine->line_index.value + 1 == source_line_map_.line_count();
    if (!NoteSourceLineMap::BuildLineSpecs(
            afterRange, rangeEndsAtDocumentEnd, &replacementLines) ||
        replacementLines.empty() ||
        !NoteSourceLineMap::Replace(
            source_line_map_, firstLine->line_index,
            {lastLine->line_index.value + 1}, replacementLines, &nextLineMap)) {
        return NoteTextApplyResult::StorageFailure;
    }
    const size_t retainedDocumentLength = canonical_.text_length() - edit.deleted_len;
    if (edit.inserted_text.size() >
            std::numeric_limits<size_t>::max() - retainedDocumentLength ||
        nextLineMap.text_length() != retainedDocumentLength + edit.inserted_text.size()) {
        return NoteTextApplyResult::StorageFailure;
    }
    const NoteTextPieceSequenceMutationResult result = canonical_.Apply(edit);
    if (result == NoteTextPieceSequenceMutationResult::InvalidRange) {
        return NoteTextApplyResult::InvalidRange;
    }
    if (result != NoteTextPieceSequenceMutationResult::Applied) {
        return NoteTextApplyResult::StorageFailure;
    }
    source_line_map_ = std::move(nextLineMap);
    ++content_revision_;
    materialized_model_current_ = false;
    return NoteTextApplyResult::Applied;
}

void NoteTextCore::SetPersistenceRevision(uint64_t persistenceRevision) {
    persistence_revision_ = persistenceRevision;
}

std::wstring NoteTextCore::BuildStorageTextCrlf() const {
    const std::wstring raw = canonical_.Materialize();
    std::wstring storage;
    storage.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const wchar_t ch = raw[i];
        if (ch == L'\r') {
            if (i + 1 < raw.size() && raw[i + 1] == L'\n') ++i;
            storage += L"\r\n";
        } else if (ch == L'\n') {
            storage += L"\r\n";
        } else {
            storage.push_back(ch);
        }
    }
    return storage;
}

const NoteTextModel& NoteTextCore::model() const {
    MaterializeModel();
    return materialized_model_;
}

void NoteTextCore::MaterializeModel() const {
    if (materialized_model_current_) return;
    materialized_model_ = MakeNoteTextModel(metadata_, canonical_.Materialize(), content_revision_);
    materialized_model_current_ = true;
    if (materialization_count_ != std::numeric_limits<uint64_t>::max()) {
        ++materialization_count_;
    }
}

NoteTextCore* NoteTextCoreRegistry::Reset(NoteId noteId,
                                          NoteMetadata metadata,
                                          std::wstring raw,
                                          uint64_t contentRevision,
                                          uint64_t persistenceRevision) {
    if (!noteId.valid()) return nullptr;
    NoteTextCore& core = cores_[noteId.value];
    core.Reset(noteId, std::move(metadata), std::move(raw),
               contentRevision, persistenceRevision);
    return &core;
}

NoteTextCore* NoteTextCoreRegistry::Find(NoteId noteId) {
    if (!noteId.valid()) return nullptr;
    const auto it = cores_.find(noteId.value);
    return it != cores_.end() ? &it->second : nullptr;
}

const NoteTextCore* NoteTextCoreRegistry::Find(NoteId noteId) const {
    if (!noteId.valid()) return nullptr;
    const auto it = cores_.find(noteId.value);
    return it != cores_.end() ? &it->second : nullptr;
}

NoteTextCore* NoteTextCoreRegistry::FindForView(const ViewIdentity& viewIdentity) {
    return viewIdentity.valid() ? Find(viewIdentity.note_id) : nullptr;
}

const NoteTextCore* NoteTextCoreRegistry::FindForView(
    const ViewIdentity& viewIdentity) const {
    return viewIdentity.valid() ? Find(viewIdentity.note_id) : nullptr;
}

bool NoteTextCoreRegistry::Forget(NoteId noteId) {
    return noteId.valid() && cores_.erase(noteId.value) != 0;
}

} // namespace note
