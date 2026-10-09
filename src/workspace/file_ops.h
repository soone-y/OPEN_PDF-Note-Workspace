// file: main/file_ops.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>

// Recovers any interrupted file rename/move operations from the previous session.
bool RecoverPendingFileOperationTransactionsForSession(
    HWND owner, const std::filesystem::path& sessionRoot);

// Begins a rename or move operation for the currently active file.
bool RenameOrMoveCurrentOperationTarget(HWND owner, bool isPdf, bool renameOnly);

// Organize one file without prompts. PDF/image sidecars share its conflict-free
// basename; notes use the same journaled link/identity rewrite as manual moves.
// On failure the caller stops the batch; recovery data and originals are kept.
[[nodiscard]] bool OrganizeSessionFile(HWND owner, const std::filesystem::path& source,
                         const std::filesystem::path& destinationDirectory,
                         std::filesystem::path* movedTo);

// Process-local workspace operation history. It is deliberately separate from
// focused document editing history and only exposes a transaction while its
// recorded session is still active.
bool CanExecuteWorkspaceOperationUndoRedo(bool undo);
bool ExecuteWorkspaceOperationUndoRedo(HWND owner, bool undo);

// Shows the dialog for managing staged uncommitted file differences.
void ShowStageManagerDialog(HWND owner);

// Tests the transaction mechanism
bool RunUiAutomationOperationTransactionRecoveryScenario(std::wstring* outError);
