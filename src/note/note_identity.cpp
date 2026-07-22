#include "note/note_identity.h"

namespace note {

uint64_t FingerprintSnapshotBytes(std::string_view bytes) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t hash = kOffsetBasis;
    size_t offset = 0;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        offset = 3;
    }
    for (size_t i = offset; i < bytes.size(); ++i) {
        unsigned char byte = static_cast<unsigned char>(bytes[i]);
        if (byte == static_cast<unsigned char>('\r')) {
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n') ++i;
            byte = static_cast<unsigned char>('\n');
        }
        hash ^= static_cast<uint64_t>(byte);
        hash *= kPrime;
    }
    return hash != 0 ? hash : 1;
}

SnapshotIdentity BuildSnapshotIdentity(NoteId noteId,
                                       uint64_t contentRevision,
                                       uint64_t persistenceRevision,
                                       std::string_view bytes) {
    return SnapshotIdentity{
        noteId,
        contentRevision,
        persistenceRevision,
        FingerprintSnapshotBytes(bytes),
    };
}

bool SameSnapshotIdentity(const SnapshotIdentity& lhs, const SnapshotIdentity& rhs) {
    return lhs.note_id == rhs.note_id &&
           lhs.content_revision == rhs.content_revision &&
           lhs.persistence_revision == rhs.persistence_revision &&
           lhs.content_fingerprint == rhs.content_fingerprint;
}

bool SameSnapshotOwner(const SnapshotIdentity& lhs, const SnapshotIdentity& rhs) {
    return lhs.note_id.valid() && lhs.note_id == rhs.note_id;
}

bool SameSnapshotContent(const SnapshotIdentity& lhs, const SnapshotIdentity& rhs) {
    return SameSnapshotOwner(lhs, rhs) &&
           lhs.content_fingerprint != 0 &&
           lhs.content_fingerprint == rhs.content_fingerprint;
}

NoteId NoteIdentityRegistry::AllocateNoteId() {
    do {
        ++note_id_seed_;
    } while (note_id_seed_ == 0 || notes_by_id_.find(note_id_seed_) != notes_by_id_.end());
    return NoteId{note_id_seed_};
}

ViewId NoteIdentityRegistry::AllocateViewId() {
    do {
        ++view_id_seed_;
    } while (view_id_seed_ == 0 || views_by_id_.find(view_id_seed_) != views_by_id_.end());
    return ViewId{view_id_seed_};
}

NoteIdentity NoteIdentityRegistry::ResolvePath(const std::wstring& normalizedPathKey) {
    if (normalizedPathKey.empty()) return {};
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto pathIt = note_id_by_path_.find(normalizedPathKey);
    if (pathIt != note_id_by_path_.end()) {
        const auto noteIt = notes_by_id_.find(pathIt->second);
        if (noteIt != notes_by_id_.end()) return noteIt->second;
        note_id_by_path_.erase(pathIt);
    }

    NoteIdentity identity;
    identity.note_id = AllocateNoteId();
    identity.path_key = normalizedPathKey;
    notes_by_id_.emplace(identity.note_id.value, identity);
    note_id_by_path_.emplace(identity.path_key, identity.note_id.value);
    return identity;
}

NoteIdentity NoteIdentityRegistry::CreateTransient() {
    const std::lock_guard<std::mutex> lock(mutex_);
    NoteIdentity identity;
    identity.note_id = AllocateNoteId();
    identity.transient = true;
    notes_by_id_.emplace(identity.note_id.value, identity);
    return identity;
}

bool NoteIdentityRegistry::ImportPath(NoteId noteId,
                                      const std::wstring& normalizedPathKey) {
    if (!noteId.valid() || normalizedPathKey.empty()) return false;
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto pathIt = note_id_by_path_.find(normalizedPathKey);
    if (pathIt != note_id_by_path_.end() && pathIt->second != noteId.value) return false;
    const auto noteIt = notes_by_id_.find(noteId.value);
    if (noteIt != notes_by_id_.end() && noteIt->second.path_key != normalizedPathKey) {
        return false;
    }
    NoteIdentity identity;
    identity.note_id = noteId;
    identity.path_key = normalizedPathKey;
    notes_by_id_[noteId.value] = identity;
    note_id_by_path_[normalizedPathKey] = noteId.value;
    return true;
}

bool NoteIdentityRegistry::BindPath(NoteId noteId,
                                    const std::wstring& normalizedPathKey) {
    if (!noteId.valid() || normalizedPathKey.empty()) return false;
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto noteIt = notes_by_id_.find(noteId.value);
    if (noteIt == notes_by_id_.end()) return false;

    const auto pathIt = note_id_by_path_.find(normalizedPathKey);
    if (pathIt != note_id_by_path_.end() && pathIt->second != noteId.value) return false;

    NoteIdentity& identity = noteIt->second;
    if (!identity.path_key.empty() && identity.path_key != normalizedPathKey) {
        const auto oldPathIt = note_id_by_path_.find(identity.path_key);
        if (oldPathIt != note_id_by_path_.end() && oldPathIt->second == noteId.value) {
            note_id_by_path_.erase(oldPathIt);
        }
    }
    identity.path_key = normalizedPathKey;
    identity.transient = false;
    note_id_by_path_[normalizedPathKey] = noteId.value;
    return true;
}

std::optional<NoteIdentity> NoteIdentityRegistry::FindNote(NoteId noteId) const {
    if (!noteId.valid()) return std::nullopt;
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = notes_by_id_.find(noteId.value);
    if (it == notes_by_id_.end()) return std::nullopt;
    return it->second;
}

std::optional<NoteIdentity> NoteIdentityRegistry::FindPath(
    const std::wstring& normalizedPathKey) const {
    if (normalizedPathKey.empty()) return std::nullopt;
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto pathIt = note_id_by_path_.find(normalizedPathKey);
    if (pathIt == note_id_by_path_.end()) return std::nullopt;
    const auto noteIt = notes_by_id_.find(pathIt->second);
    if (noteIt == notes_by_id_.end()) return std::nullopt;
    return noteIt->second;
}

ViewIdentity NoteIdentityRegistry::CreateView(NoteId noteId) {
    if (!noteId.valid()) return {};
    const std::lock_guard<std::mutex> lock(mutex_);
    if (notes_by_id_.find(noteId.value) == notes_by_id_.end()) return {};
    ViewIdentity identity;
    identity.view_id = AllocateViewId();
    identity.note_id = noteId;
    identity.binding_revision = 1;
    views_by_id_.emplace(identity.view_id.value, identity);
    return identity;
}

std::optional<ViewIdentity> NoteIdentityRegistry::RebindView(ViewId viewId, NoteId noteId) {
    if (!viewId.valid() || !noteId.valid()) return std::nullopt;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (notes_by_id_.find(noteId.value) == notes_by_id_.end()) return std::nullopt;
    const auto it = views_by_id_.find(viewId.value);
    if (it == views_by_id_.end()) return std::nullopt;
    if (it->second.note_id != noteId) {
        it->second.note_id = noteId;
        ++it->second.binding_revision;
        if (it->second.binding_revision == 0) ++it->second.binding_revision;
    }
    return it->second;
}

std::optional<ViewIdentity> NoteIdentityRegistry::FindView(ViewId viewId) const {
    if (!viewId.valid()) return std::nullopt;
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = views_by_id_.find(viewId.value);
    if (it == views_by_id_.end()) return std::nullopt;
    return it->second;
}

void NoteIdentityRegistry::ForgetView(ViewId viewId) {
    if (!viewId.valid()) return;
    const std::lock_guard<std::mutex> lock(mutex_);
    views_by_id_.erase(viewId.value);
}

size_t NoteIdentityRegistry::note_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return notes_by_id_.size();
}

size_t NoteIdentityRegistry::view_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return views_by_id_.size();
}

NoteIdentityRegistry& RuntimeNoteIdentityRegistry() {
    static NoteIdentityRegistry registry;
    return registry;
}

} // namespace note
