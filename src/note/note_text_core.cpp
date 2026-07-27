#include "note/note_text_core.h"

#include <limits>
#include <utility>

namespace note {

void NoteTextCore::Reset(NoteId noteId,
                         NoteMetadata metadata,
                         std::wstring raw,
                         uint64_t contentRevision,
                         uint64_t persistenceRevision) {
    note_id_ = noteId;
    model_ = MakeNoteTextModel(std::move(metadata), std::move(raw), contentRevision);
    persistence_revision_ = persistenceRevision;
}

NoteTextApplyResult NoteTextCore::Apply(const TextEdit& edit) {
    if (!note_id_.valid()) return NoteTextApplyResult::InvalidOwner;
    if (edit.start.value > model_.raw.size() ||
        edit.deleted_len > model_.raw.size() - edit.start.value) {
        return NoteTextApplyResult::InvalidRange;
    }
    if (edit.deleted_len == 0 && edit.inserted_text.empty()) {
        return NoteTextApplyResult::NoChange;
    }
    if (model_.revision == std::numeric_limits<uint64_t>::max()) {
        return NoteTextApplyResult::RevisionExhausted;
    }
    ApplyTextEdit(&model_, edit);
    return NoteTextApplyResult::Applied;
}

void NoteTextCore::SetPersistenceRevision(uint64_t persistenceRevision) {
    persistence_revision_ = persistenceRevision;
}

std::wstring NoteTextCore::BuildStorageTextCrlf() const {
    std::wstring storage;
    storage.reserve(model_.raw.size() + model_.line_starts.size());
    for (size_t i = 0; i < model_.raw.size(); ++i) {
        const wchar_t ch = model_.raw[i];
        if (ch == L'\r') {
            if (i + 1 < model_.raw.size() && model_.raw[i + 1] == L'\n') ++i;
            storage += L"\r\n";
        } else if (ch == L'\n') {
            storage += L"\r\n";
        } else {
            storage.push_back(ch);
        }
    }
    return storage;
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
