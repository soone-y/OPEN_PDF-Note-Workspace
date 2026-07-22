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
