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

struct BlankPdfDialogOptions {
    double widthPt = 595.0;
    double heightPt = 842.0;
    int pageCount = 1;
};

// Presents the supported blank-PDF paper sizes and validates the page count
// before the workspace creation flow asks for a destination file name.
bool PromptBlankPdfOptions(HWND owner, const std::wstring& title,
                           BlankPdfDialogOptions& out);

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
                        const std::wstring& message, std::wstring& out,
                        const std::wstring& confirmation = L"");
bool PromptSelectPath(HWND owner, const std::wstring& title,
                      const std::wstring& message,
                      const std::vector<std::wstring>& paths,
                      const std::wstring& initialPath, std::wstring& outPath);

bool PromptBackupList(HWND owner,
                      const std::filesystem::path& backupRoot,
                      const std::wstring& title,
                      const std::wstring& actionText,
                      std::filesystem::path& outPickedMeta);

bool TryParseZoomScale(const std::wstring& rawInput, double* outScale);
