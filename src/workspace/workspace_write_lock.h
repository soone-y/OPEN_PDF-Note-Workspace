#pragma once

#include <filesystem>
#include <string>

// Holds one local-process mutex for the active workspace.  A second writer
// must not start using the same workspace because stage integration and file
// replacement are intentionally local, crash-safe transactions rather than a
// multi-writer merge protocol.
[[nodiscard]] bool AcquireWorkspaceWriteLock(const std::filesystem::path& workspaceRoot,
                                             std::wstring* outError);

void ReleaseWorkspaceWriteLock();

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
