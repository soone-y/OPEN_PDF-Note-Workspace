#pragma once

#include "note/note_identity.h"
#include "note/note_model.h"

#include <string_view>
#include <map>

namespace note {

enum class NoteTextApplyResult {
    Applied,
    NoChange,
    InvalidOwner,
    InvalidRange,
    RevisionExhausted,
};

class NoteTextCore {
public:
    void Reset(NoteId noteId,
               NoteMetadata metadata,
               std::wstring raw,
               uint64_t contentRevision,
               uint64_t persistenceRevision);
    [[nodiscard]] NoteTextApplyResult Apply(const TextEdit& edit);
    void SetPersistenceRevision(uint64_t persistenceRevision);

    bool valid() const { return note_id_.valid(); }
    bool MatchesRaw(std::wstring_view raw) const { return model_.raw == raw; }
    std::wstring BuildStorageTextCrlf() const;
    NoteId note_id() const { return note_id_; }
    uint64_t content_revision() const { return model_.revision; }
    uint64_t persistence_revision() const { return persistence_revision_; }
    const NoteTextModel& model() const { return model_; }

private:
    NoteId note_id_{};
    NoteTextModel model_;
    uint64_t persistence_revision_ = 0;
};

// UI-thread registry. Multiple views of one NoteId share the same core instance.
class NoteTextCoreRegistry {
public:
    NoteTextCore* Reset(NoteId noteId,
                        NoteMetadata metadata,
                        std::wstring raw,
                        uint64_t contentRevision,
                        uint64_t persistenceRevision);
    NoteTextCore* Find(NoteId noteId);
    const NoteTextCore* Find(NoteId noteId) const;
    NoteTextCore* FindForView(const ViewIdentity& viewIdentity);
    const NoteTextCore* FindForView(const ViewIdentity& viewIdentity) const;
    bool Forget(NoteId noteId);
    size_t size() const { return cores_.size(); }

private:
    std::map<uint64_t, NoteTextCore> cores_;
};

} // namespace note
