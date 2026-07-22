#include "note/note_workspace_service.h"

#include <algorithm>
#include <utility>

namespace note {

LocalNoteKernel* NoteWorkspaceService::ResetKernel(
    NoteId noteId,
    NoteMetadata metadata,
    std::wstring raw,
    uint64_t contentRevision,
    uint64_t persistenceRevision,
    NoteContentKind contentKind) {
    InvalidateIndexes(noteId);
    return kernels_.Reset(noteId, std::move(metadata), std::move(raw),
                          contentRevision, persistenceRevision, contentKind);
}

LocalNoteKernel* NoteWorkspaceService::FindKernel(NoteId noteId) {
    return kernels_.Find(noteId);
}

const LocalNoteKernel* NoteWorkspaceService::FindKernel(NoteId noteId) const {
    return kernels_.Find(noteId);
}

LocalNoteKernel* NoteWorkspaceService::FindKernelForView(
    const ViewIdentity& viewIdentity) {
    return kernels_.FindForView(viewIdentity);
}

const LocalNoteKernel* NoteWorkspaceService::FindKernelForView(
    const ViewIdentity& viewIdentity) const {
    return kernels_.FindForView(viewIdentity);
}

bool NoteWorkspaceService::ForgetKernel(NoteId noteId) {
    InvalidateIndexes(noteId);
    return kernels_.Forget(noteId);
}

std::shared_ptr<const WorkspaceNoteIndexSnapshot>
NoteWorkspaceService::ResolveIndex(
    SnapshotIdentity snapshotIdentity,
    std::wstring targetPath,
    NoteMetadata metadata,
    std::wstring raw,
    NoteContentKind contentKind) {
    if (!snapshotIdentity.valid()) return nullptr;
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        if (auto cached = FindCachedIndexLocked(
                snapshotIdentity, targetPath, contentKind)) {
            return cached;
        }
    }

    auto built = std::make_shared<WorkspaceNoteIndexSnapshot>(
        BuildWorkspaceNoteIndex(snapshotIdentity, targetPath,
                                std::move(metadata), std::move(raw),
                                contentKind));
    if (!built->valid) return nullptr;

    std::lock_guard<std::mutex> lock(index_mutex_);
    if (auto cached = FindCachedIndexLocked(
            snapshotIdentity, targetPath, contentKind)) {
        return cached;
    }
    InsertCachedIndexLocked(snapshotIdentity, std::move(targetPath),
                            contentKind, built);
    return built;
}

std::shared_ptr<const WorkspaceNoteIndexSnapshot>
NoteWorkspaceService::ResolveIndexFromKernel(
    SnapshotIdentity snapshotIdentity,
    std::wstring targetPath,
    const LocalNoteKernel& kernel) {
    if (!snapshotIdentity.valid() || !kernel.valid() ||
        (kernel.content_kind() == NoteContentKind::Markdown &&
         (!kernel.CanReadSyntax() || !kernel.CanReadSemantic()))) {
        return nullptr;
    }
    const NoteContentKind contentKind = kernel.content_kind();
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        if (auto cached = FindCachedIndexLocked(
                snapshotIdentity, targetPath, contentKind)) {
            return cached;
        }
    }

    auto built = std::make_shared<WorkspaceNoteIndexSnapshot>(
        BuildWorkspaceNoteIndexFromKernel(
            snapshotIdentity, targetPath, kernel));
    if (!built->valid) return nullptr;

    std::lock_guard<std::mutex> lock(index_mutex_);
    if (auto cached = FindCachedIndexLocked(
            snapshotIdentity, targetPath, contentKind)) {
        return cached;
    }
    InsertCachedIndexLocked(snapshotIdentity, std::move(targetPath),
                            contentKind, built);
    return built;
}

void NoteWorkspaceService::InvalidateIndexes(NoteId noteId) {
    if (!noteId.valid()) return;
    std::lock_guard<std::mutex> lock(index_mutex_);
    index_cache_.erase(
        std::remove_if(index_cache_.begin(), index_cache_.end(),
                       [&](const CachedIndex& entry) {
                           return entry.identity.note_id == noteId;
                       }),
        index_cache_.end());
}

void NoteWorkspaceService::ClearIndexes() {
    std::lock_guard<std::mutex> lock(index_mutex_);
    index_cache_.clear();
}

size_t NoteWorkspaceService::index_cache_size() const {
    std::lock_guard<std::mutex> lock(index_mutex_);
    return index_cache_.size();
}

std::shared_ptr<const WorkspaceNoteIndexSnapshot>
NoteWorkspaceService::FindCachedIndexLocked(
    const SnapshotIdentity& identity,
    const std::wstring& targetPath,
    NoteContentKind contentKind) {
    for (CachedIndex& entry : index_cache_) {
        if (entry.target_path != targetPath ||
            entry.content_kind != contentKind ||
            !SameSnapshotIdentity(entry.identity, identity)) {
            continue;
        }
        entry.access_sequence = ++index_access_sequence_;
        return entry.snapshot;
    }
    return nullptr;
}

void NoteWorkspaceService::InsertCachedIndexLocked(
    SnapshotIdentity identity,
    std::wstring targetPath,
    NoteContentKind contentKind,
    std::shared_ptr<const WorkspaceNoteIndexSnapshot> snapshot) {
    if (index_cache_.size() >= kMaxCachedIndexes) {
        const auto oldest = std::min_element(
            index_cache_.begin(), index_cache_.end(),
            [](const CachedIndex& lhs, const CachedIndex& rhs) {
                return lhs.access_sequence < rhs.access_sequence;
            });
        if (oldest != index_cache_.end()) index_cache_.erase(oldest);
    }
    CachedIndex entry;
    entry.identity = identity;
    entry.target_path = std::move(targetPath);
    entry.content_kind = contentKind;
    entry.access_sequence = ++index_access_sequence_;
    entry.snapshot = std::move(snapshot);
    index_cache_.push_back(std::move(entry));
}

NoteWorkspaceService& RuntimeNoteWorkspaceService() {
    static NoteWorkspaceService service;
    return service;
}

} // namespace note
