// file: core/ui_prompts.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

enum class PromptCreateNameResult {
    Cancel,
    Create,
    Explorer
};

bool PromptNewLectureName(HWND owner, std::wstring& outName);
bool PromptNewSessionName(HWND owner, std::wstring& outName);
bool PromptSimpleText(HWND owner, const std::wstring& title,
                      const std::wstring& initial, std::wstring& out);

enum class SavePathPromptResult {
    Cancel,
    DirectInput,
    OpenSystemDialog,
};

// Output-only prompt: show the destination folder and file name separately.
SavePathPromptResult PromptSavePath(HWND owner, const std::wstring& title,
                                    const std::wstring& directory,
                                    const std::wstring& defaultName,
                                    std::wstring& outFileName);
PromptCreateNameResult PromptCreateName(HWND owner,
                                        const std::wstring& title,
                                        const std::wstring& label,
                                        const std::wstring& initial,
                                        const std::vector<std::wstring>& suggestions,
                                        bool showExplorerButton,
                                        std::wstring& out);
bool PromptPasswordText(HWND owner, const std::wstring& title,
                        const std::wstring& message, std::wstring& out);
bool PromptSelectPath(HWND owner, const std::wstring& title,
                      const std::wstring& message,
                      const std::vector<std::wstring>& paths,
                      const std::wstring& initialPath, std::wstring& outPath);

bool PromptRestoreBackupList(HWND owner, const std::filesystem::path& backupRoot, std::filesystem::path& outPickedMeta);

bool TryParseZoomScale(const std::wstring& rawInput, double* outScale);
