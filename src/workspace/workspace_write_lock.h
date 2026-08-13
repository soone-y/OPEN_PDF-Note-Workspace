#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

// Registers the active workspace path for this window. Multiple windows may
// register the same canonical workspace path; destructive shared operations
// must use WorkspaceOperationLock below.
[[nodiscard]] bool AcquireWorkspaceWriteLock(const std::filesystem::path& workspaceRoot,
                                             std::wstring* outError);

void ReleaseWorkspaceWriteLock();

class WorkspaceOperationLock {
public:
    WorkspaceOperationLock(const std::filesystem::path& workspaceRoot, std::wstring* outError);
    ~WorkspaceOperationLock();

    WorkspaceOperationLock(const WorkspaceOperationLock&) = delete;
    WorkspaceOperationLock& operator=(const WorkspaceOperationLock&) = delete;

    [[nodiscard]] bool acquired() const { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

// An opened PDF, image, or note is exclusively owned by one application
// process. Unlike the workspace lock, this permits independent workspaces
// while preventing two windows from editing the same source file.
class DocumentOpenLockCandidate {
public:
    DocumentOpenLockCandidate(const std::filesystem::path& path, std::wstring* outError);
    ~DocumentOpenLockCandidate();

    DocumentOpenLockCandidate(const DocumentOpenLockCandidate&) = delete;
    DocumentOpenLockCandidate& operator=(const DocumentOpenLockCandidate&) = delete;

    [[nodiscard]] bool acquired() const { return acquired_; }

    // Keep the target lock after a successful open, then release the file
    // that was previously displayed (unless it is the same canonical file).
    void CommitReplacing(const std::filesystem::path& previousPath);

private:
    std::wstring key_;
    bool acquired_ = false;
    bool newlyAcquired_ = false;
};

[[nodiscard]] bool IsDocumentOpenLockTransitionActive();
void ReleaseDocumentOpenLock(const std::filesystem::path& path);
void ReleaseAllDocumentOpenLocks();
