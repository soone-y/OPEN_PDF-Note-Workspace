#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct WorkspaceLogArchiveResult {
    bool ok = false;
    bool empty = false;
    bool inspectionFailed = false;
    size_t archivedCount = 0;
    std::wstring archiveFileName;
    std::wstring error;
};

struct WorkspaceLogDeleteResult {
    bool ok = false;
    bool empty = false;
    bool inspectionFailed = false;
    size_t deletedCount = 0;
    std::vector<std::wstring> failures;
    std::wstring error;
};

struct WorkspaceLogDeletePreview {
    bool ok = false;
    bool empty = false;
    bool inspectionFailed = false;
    size_t count = 0;
    std::wstring directory;
    std::wstring displayList;
    std::wstring error;
};

bool HasWorkspaceLogFiles();
WorkspaceLogArchiveResult ArchiveWorkspaceLogs();
WorkspaceLogDeleteResult DeleteWorkspaceLogs();
WorkspaceLogDeletePreview PreviewWorkspaceLogDeletion();
