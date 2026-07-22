#pragma once

#include "note/note_workspace_index.h"

#include <memory>
#include <mutex>
#include <vector>

namespace note {

// Process-level model service. Kernel access is UI-thread owned; immutable index
// resolution is thread-safe so future background search can share the same cache.
class NoteWorkspaceService {
public:
    LocalNoteKernel* ResetKernel(NoteId noteId,
                                 NoteMetadata metadata,
                                 std::wstring raw,
                                 uint64_t contentRevision,
                                 uint64_t persistenceRevision,
                                 NoteContentKind contentKind);
    LocalNoteKernel* FindKernel(NoteId noteId);
    const LocalNoteKernel* FindKernel(NoteId noteId) const;
    LocalNoteKernel* FindKernelForView(const ViewIdentity& viewIdentity);
    const LocalNoteKernel* FindKernelForView(
        const ViewIdentity& viewIdentity) const;
    bool ForgetKernel(NoteId noteId);
    size_t kernel_count() const { return kernels_.size(); }

    std::shared_ptr<const WorkspaceNoteIndexSnapshot> ResolveIndex(
        SnapshotIdentity snapshotIdentity,
        std::wstring targetPath,
        NoteMetadata metadata,
        std::wstring raw,
        NoteContentKind contentKind);
    std::shared_ptr<const WorkspaceNoteIndexSnapshot> ResolveIndexFromKernel(
        SnapshotIdentity snapshotIdentity,
        std::wstring targetPath,
        const LocalNoteKernel& kernel);

    void InvalidateIndexes(NoteId noteId);
    void ClearIndexes();
    size_t index_cache_size() const;

private:
    struct CachedIndex {
        SnapshotIdentity identity{};
        std::wstring target_path;
        NoteContentKind content_kind = NoteContentKind::Markdown;
        uint64_t access_sequence = 0;
        std::shared_ptr<const WorkspaceNoteIndexSnapshot> snapshot;
    };

    std::shared_ptr<const WorkspaceNoteIndexSnapshot> FindCachedIndexLocked(
        const SnapshotIdentity& identity,
        const std::wstring& targetPath,
        NoteContentKind contentKind);
    void InsertCachedIndexLocked(
        SnapshotIdentity identity,
        std::wstring targetPath,
        NoteContentKind contentKind,
        std::shared_ptr<const WorkspaceNoteIndexSnapshot> snapshot);

    static constexpr size_t kMaxCachedIndexes = 128;
    LocalNoteKernelRegistry kernels_;
    mutable std::mutex index_mutex_;
    uint64_t index_access_sequence_ = 0;
    std::vector<CachedIndex> index_cache_;
};

NoteWorkspaceService& RuntimeNoteWorkspaceService();

} // namespace note
