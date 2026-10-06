#pragma once

#include "note/note_identity.h"
#include "note/note_model.h"
#include "note/note_source_line_map.h"
#include "note/note_text_piece_sequence.h"

#include <string_view>
#include <map>

namespace note {

enum class NoteTextApplyResult {
    Applied,
    NoChange,
    InvalidOwner,
    InvalidRange,
    RevisionExhausted,
    StorageFailure,
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

    bool valid() const { return note_id_.valid() && canonical_.initialized(); }
    [[nodiscard]] size_t text_length() const noexcept { return canonical_.text_length(); }
    // Canonical logical rows are maintained in a persistent source-line map.
    // Input and presentation guards therefore need neither model() nor
    // absolute-offset rewrites in the unchanged document tail.
    [[nodiscard]] size_t logical_line_count() const noexcept {
        return source_line_map_.line_count();
    }
    [[nodiscard]] const NoteSourceLineMap::Snapshot& source_line_map() const noexcept {
        return source_line_map_;
    }
    bool MatchesRaw(std::wstring_view raw) const { return canonical_.Equals(raw); }
    [[nodiscard]] bool RangeMatches(Utf16CodeUnitOffset start,
                                    std::wstring_view text) const noexcept {
        return canonical_.EqualsRange(start, text);
    }
    [[nodiscard]] NoteTextPieceSequence::Snapshot TakeSnapshot() const noexcept {
        return canonical_.TakeSnapshot();
    }
    [[nodiscard]] bool MatchesSnapshot(
        const NoteTextPieceSequence::Snapshot& snapshot) const noexcept {
        return canonical_.MatchesSnapshot(snapshot);
    }
    [[nodiscard]] bool SharesExactRange(
        const NoteTextPieceSequence::Snapshot& before,
        Utf16CodeUnitOffset beforeStart,
        Utf16CodeUnitOffset currentStart,
        size_t length) const {
        return canonical_.SharesExactRange(before, beforeStart, currentStart, length);
    }
    [[nodiscard]] std::wstring CopyRawRange(Utf16CodeUnitOffset start, size_t length) const {
        return canonical_.CopyRange(start, length);
    }
    std::wstring BuildStorageTextCrlf() const;
    NoteId note_id() const { return note_id_; }
    uint64_t content_revision() const { return content_revision_; }
    uint64_t persistence_revision() const { return persistence_revision_; }
    // In-process diagnostic counter for the long-note hot-path tests.  It is
    // reset with the core and is never persisted or presented to users.
    [[nodiscard]] uint64_t model_materialization_count() const noexcept {
        return materialization_count_;
    }
    const NoteTextModel& model() const;

private:
    void MaterializeModel() const;

    NoteId note_id_{};
    NoteMetadata metadata_;
    NoteTextPieceSequence canonical_;
    NoteSourceLineMap::Snapshot source_line_map_;
    mutable NoteTextModel materialized_model_;
    mutable bool materialized_model_current_ = false;
    mutable uint64_t materialization_count_ = 0;
    uint64_t content_revision_ = 0;
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
