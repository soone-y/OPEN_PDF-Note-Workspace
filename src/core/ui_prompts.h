// file: core/ui_prompts.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

enum class PromptCreateNameResult {
    Cancel,
    Create,
    Explorer
};

bool PromptNewLectureName(HWND owner, std::wstring& outName);
bool PromptNewSessionName(HWND owner, std::wstring& outName);
// The caller owns validation; a failed check keeps the same dialog and input
// alive. No accepted value is published until the check succeeds.
using SimpleTextValidator = std::function<bool(HWND, const std::wstring&)>;
bool PromptSimpleText(HWND owner, const std::wstring& title,
                      const std::wstring& initial, std::wstring& out,
                      const SimpleTextValidator& validate = {});

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

struct SavePathPromptSelection {
    SavePathPromptResult action = SavePathPromptResult::Cancel;
    std::filesystem::path directory;
    std::wstring fileName;
};

// Select a destination without creating or overwriting it. The local browser
// owns folder navigation and name entry; the caller owns the native save dialog
// and validates the full destination before any write. Native handoff retains
// the folder/name edited in the local browser. Cancel grants no write permission.
[[nodiscard]] SavePathPromptSelection PromptSavePath(HWND owner, const std::wstring& title,
                                                     const std::filesystem::path& directory,
                                                     const std::wstring& defaultName,
                                                     const std::wstring& defaultExtension);
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
