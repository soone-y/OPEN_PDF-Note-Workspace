#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

[[nodiscard]] std::filesystem::path WorkspaceMemoPath(const std::filesystem::path& root);
void ShowWorkspaceMemoWindow(HWND mainOwner);
// Called before main accelerators. All memo keystrokes stay in this editor.
[[nodiscard]] bool HandleWorkspaceMemoMessage(const MSG& message);
[[nodiscard]] bool SaveWorkspaceMemoForExit(bool interactive = true);
// Captures the open memo editor text so a failed exit save can hand it to the
// user (clipboard) instead of blocking exit. False when it cannot be read.
[[nodiscard]] bool CaptureWorkspaceMemoEditorText(std::wstring* out);
// Saves the old root and resets its document only when the switch is committed.
[[nodiscard]] bool PrepareWorkspaceMemoRootChange();
void ResetWorkspaceMemoForRootChange();
