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

// Shows the dialog for managing staged uncommitted file differences.
void ShowStageManagerDialog(HWND owner);

// Tests the transaction mechanism
bool RunUiAutomationOperationTransactionRecoveryScenario(std::wstring* outError);
