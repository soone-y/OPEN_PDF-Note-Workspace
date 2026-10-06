#pragma once
#include <windows.h>
#include <filesystem>

[[nodiscard]] std::filesystem::path WorkspaceMemoPath(const std::filesystem::path& root);
void ShowWorkspaceMemoWindow(HWND mainOwner);
// Called before main accelerators. All memo keystrokes stay in this editor.
[[nodiscard]] bool HandleWorkspaceMemoMessage(const MSG& message);
[[nodiscard]] bool SaveWorkspaceMemoForExit(bool interactive = true);
// Saves the old root and resets its document only when the switch is committed.
[[nodiscard]] bool PrepareWorkspaceMemoRootChange();
void ResetWorkspaceMemoForRootChange();
