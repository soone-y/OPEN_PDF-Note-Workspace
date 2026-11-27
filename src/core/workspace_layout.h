#pragma once

// Layout of the management folder "__pdf_note_workspace__" inside a workspace.
//
// Every folder directly below the management root has one role and one answer
// to "may the user delete it?". The answer and what the app does after a
// deletion are written to DELETE_OR_KEEP.txt (see workspace_delete_guide) and
// to the help documents. Paths to these folders are built only here, so a
// layout change is one edit and tests can forbid literal folder names
// elsewhere (tests/scripts/run_path_safety_tests.ps1).
//
// Older layouts (__tmp__/__stage__, __escape__/...) are not read, moved or
// deleted. Their files stay where they are.
//
// Design: docs/internal/workspace_管理フォルダー階層整理_設計_2026-11-26.md

#include <array>
#include <filesystem>

namespace workspace_layout {

inline constexpr wchar_t kRootName[] = L"__pdf_note_workspace__";

// Kept: deleting loses settings, work in progress, or protected copies.
inline constexpr wchar_t kSettingsName[] = L"__settings__";
inline constexpr wchar_t kThemeName[] = L"__theme__";
inline constexpr wchar_t kMemoName[] = L"__memo__";
inline constexpr wchar_t kUnsavedName[] = L"__unsaved__";        // stage: edits not yet integrated
inline constexpr wchar_t kRecoveryName[] = L"__recovery__";      // recovery reviews and stage copies
inline constexpr wchar_t kOperationsName[] = L"__operations__";  // multi-file operation records
inline constexpr wchar_t kOperationsSetAsideName[] = L"__operations_set_aside__";
inline constexpr wchar_t kBackupName[] = L"__backup__";          // copies the user restores from
inline constexpr wchar_t kDeletedName[] = L"__deleted__";        // items deleted in the app
inline constexpr wchar_t kFailedName[] = L"__failed__";          // unwritable content, unreadable files
// Deletable while the app is closed.
inline constexpr wchar_t kRecentName[] = L"__recent__";          // last-open maps, PDF view positions
inline constexpr wchar_t kLogName[] = L"__log__";
inline constexpr wchar_t kTmpName[] = L"__tmp__";                // atomic-write temps, Office conversion
// Written only by the UI-automation test mode.
inline constexpr wchar_t kTestName[] = L"__test__";

inline constexpr wchar_t kDeleteGuideFileName[] = L"DELETE_OR_KEEP.txt";

enum class Deletion {
    Keep,            // never delete
    WhileClosed,     // delete only while the app is closed
    TestOnly,        // created by automated tests only
};

struct FolderInfo {
    const wchar_t* name;
    Deletion deletion;
};

// Every folder directly below the management root, in the order the guide
// and the help documents list them.
inline constexpr std::array<FolderInfo, 14> kFolders = {{
    {kSettingsName, Deletion::Keep},
    {kThemeName, Deletion::Keep},
    {kMemoName, Deletion::Keep},
    {kUnsavedName, Deletion::Keep},
    {kRecoveryName, Deletion::Keep},
    {kOperationsName, Deletion::Keep},
    {kOperationsSetAsideName, Deletion::Keep},
    {kBackupName, Deletion::Keep},
    {kDeletedName, Deletion::Keep},
    {kFailedName, Deletion::Keep},
    {kRecentName, Deletion::WhileClosed},
    {kLogName, Deletion::WhileClosed},
    {kTmpName, Deletion::WhileClosed},
    {kTestName, Deletion::TestOnly},
}};

// All functions return an empty path for an empty workspace root.
inline std::filesystem::path Root(const std::filesystem::path& workspace) {
    return workspace.empty() ? std::filesystem::path{} : workspace / kRootName;
}
inline std::filesystem::path Sub(const std::filesystem::path& workspace, const wchar_t* name) {
    const auto root = Root(workspace);
    return root.empty() ? root : root / name;
}

inline std::filesystem::path Settings(const std::filesystem::path& ws) { return Sub(ws, kSettingsName); }
inline std::filesystem::path Theme(const std::filesystem::path& ws) { return Sub(ws, kThemeName); }
inline std::filesystem::path Memo(const std::filesystem::path& ws) { return Sub(ws, kMemoName); }
inline std::filesystem::path Unsaved(const std::filesystem::path& ws) { return Sub(ws, kUnsavedName); }
inline std::filesystem::path Recovery(const std::filesystem::path& ws) { return Sub(ws, kRecoveryName); }
inline std::filesystem::path Operations(const std::filesystem::path& ws) { return Sub(ws, kOperationsName); }
inline std::filesystem::path OperationsSetAside(const std::filesystem::path& ws) {
    return Sub(ws, kOperationsSetAsideName);
}
inline std::filesystem::path Backup(const std::filesystem::path& ws) { return Sub(ws, kBackupName); }
inline std::filesystem::path Deleted(const std::filesystem::path& ws) { return Sub(ws, kDeletedName); }
inline std::filesystem::path Failed(const std::filesystem::path& ws) { return Sub(ws, kFailedName); }
inline std::filesystem::path Recent(const std::filesystem::path& ws) { return Sub(ws, kRecentName); }
inline std::filesystem::path Log(const std::filesystem::path& ws) { return Sub(ws, kLogName); }
inline std::filesystem::path Tmp(const std::filesystem::path& ws) { return Sub(ws, kTmpName); }
inline std::filesystem::path Test(const std::filesystem::path& ws) { return Sub(ws, kTestName); }
inline std::filesystem::path DeleteGuide(const std::filesystem::path& ws) {
    const auto root = Root(ws);
    return root.empty() ? root : root / kDeleteGuideFileName;
}

}  // namespace workspace_layout
