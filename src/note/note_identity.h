#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace note {

struct NoteId {
    uint64_t value = 0;

    bool valid() const { return value != 0; }
};

inline bool operator==(NoteId lhs, NoteId rhs) { return lhs.value == rhs.value; }
inline bool operator!=(NoteId lhs, NoteId rhs) { return !(lhs == rhs); }

struct ViewId {
    uint64_t value = 0;

    bool valid() const { return value != 0; }
};

inline bool operator==(ViewId lhs, ViewId rhs) { return lhs.value == rhs.value; }
inline bool operator!=(ViewId lhs, ViewId rhs) { return !(lhs == rhs); }

struct NoteIdentity {
    NoteId note_id{};
    std::wstring path_key;
    bool transient = false;

    bool valid() const { return note_id.valid(); }
};

struct ViewIdentity {
    ViewId view_id{};
    NoteId note_id{};
    uint64_t binding_revision = 0;

    bool valid() const { return view_id.valid() && note_id.valid(); }
};

struct SnapshotIdentity {
    NoteId note_id{};
    uint64_t content_revision = 0;
    uint64_t persistence_revision = 0;
    uint64_t content_fingerprint = 0;

    bool valid() const {
        return note_id.valid() &&
               (content_revision != 0 || content_fingerprint != 0);
    }
};

struct NoteDerivedSnapshotIdentity {
    NoteId note_id{};
    uint64_t source_revision = 0;

    bool valid() const {
        return note_id.valid() && source_revision != 0;
    }
};

inline bool operator==(const NoteDerivedSnapshotIdentity& lhs,
                       const NoteDerivedSnapshotIdentity& rhs) {
    return lhs.note_id == rhs.note_id &&
           lhs.source_revision == rhs.source_revision;
}

inline bool operator!=(const NoteDerivedSnapshotIdentity& lhs,
                       const NoteDerivedSnapshotIdentity& rhs) {
    return !(lhs == rhs);
}

uint64_t FingerprintSnapshotBytes(std::string_view bytes);
SnapshotIdentity BuildSnapshotIdentity(NoteId noteId,
                                       uint64_t contentRevision,
                                       uint64_t persistenceRevision,
                                       std::string_view bytes);
bool SameSnapshotIdentity(const SnapshotIdentity& lhs, const SnapshotIdentity& rhs);
bool SameSnapshotOwner(const SnapshotIdentity& lhs, const SnapshotIdentity& rhs);
bool SameSnapshotContent(const SnapshotIdentity& lhs, const SnapshotIdentity& rhs);

// Runtime identities outlive path aliases. Views and snapshots consume ids, not paths.
class NoteIdentityRegistry {
public:
    NoteIdentity ResolvePath(const std::wstring& normalizedPathKey);
    NoteIdentity CreateTransient();
    bool ImportPath(NoteId noteId, const std::wstring& normalizedPathKey);
    bool BindPath(NoteId noteId, const std::wstring& normalizedPathKey);

    std::optional<NoteIdentity> FindNote(NoteId noteId) const;
    std::optional<NoteIdentity> FindPath(const std::wstring& normalizedPathKey) const;

    ViewIdentity CreateView(NoteId noteId);
    std::optional<ViewIdentity> RebindView(ViewId viewId, NoteId noteId);
    std::optional<ViewIdentity> FindView(ViewId viewId) const;
    void ForgetView(ViewId viewId);

    size_t note_count() const;
    size_t view_count() const;

private:
    NoteId AllocateNoteId();
    ViewId AllocateViewId();

    uint64_t note_id_seed_ = 0;
    uint64_t view_id_seed_ = 0;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, NoteIdentity> notes_by_id_;
    std::unordered_map<std::wstring, uint64_t> note_id_by_path_;
    std::unordered_map<uint64_t, ViewIdentity> views_by_id_;
};

NoteIdentityRegistry& RuntimeNoteIdentityRegistry();

} // namespace note
