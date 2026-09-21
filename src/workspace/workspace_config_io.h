// file: main/workspace_config_io.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <filesystem>

// Config application
void ApplyConfigToUI(HWND hWnd);
bool UpdateWindowSizeConfig(HWND hWnd);
void ClearPdfAndNoteSelection();
void ResetSessionAndFiles();

// Resource directories
bool EnsureWorkspaceResourceDirs(std::filesystem::path* settingsDir);
bool EnsureWorkspaceResourceDirsWithErr(const std::wstring& root, std::filesystem::path* settingsDir, std::wstring* outErr);
bool VerifyWorkspaceWritableForEditing(HWND owner);

bool VerifyDirReadableWritableForEditing(HWND owner, const std::filesystem::path& dir, const wchar_t* labelId);
bool TryOpenDirForList(const std::filesystem::path& dir, std::wstring* outErr);
void ShowTempExternalLectureAccessWarning(HWND owner, const std::filesystem::path& dir, const std::wstring& readErr);
void ForgetWritableProbe(const std::filesystem::path& dir);
void RememberWritableProbe(const std::filesystem::path& dir);

// Saving & Presets
bool SaveNoteIfDirty(HWND hWnd);
void FinalizeManualSaveUi(HWND hWnd, bool updateWindowTitleAfterSave);
void SaveSettingsPreset(HWND hWnd);
void LoadSettingsPreset(HWND hWnd);
bool ExportSettingsPresetToFile(const std::filesystem::path& outputPath, std::wstring* outErr = nullptr);
bool ImportSettingsPresetFromFile(const std::filesystem::path& inputPath, std::wstring* outErr = nullptr,
                                  std::filesystem::path* outRecoveryBackup = nullptr);
void SaveAllManual(HWND hWnd);
void SaveCurrentNoteManual(HWND hWnd);

// Dialogs
void ShowRecoveryDialog(HWND hWnd);
void ShowRestoreBackupListDialogAndExecute(HWND hWnd);
void ShowDeleteSavedBackupDialog(HWND hWnd);
