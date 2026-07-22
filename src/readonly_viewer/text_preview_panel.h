#pragma once

#include <windows.h>
#include <string>

namespace readonly_viewer {

// Register the Window Class for the Text preview panel
bool RegisterTextPreviewPanelClass(HINSTANCE hInstance);

// Create the Text preview panel as a child of the given parent window
HWND CreateTextPreviewPanel(HWND hParent, HINSTANCE hInstance, int id);

// Open a Text/Markdown file in the given Text preview panel
void TextPreviewPanel_OpenFile(HWND hwnd, const std::wstring& filePath, const std::wstring& rawText, bool isBinary);

// Get the currently loaded file path (if any)
std::wstring TextPreviewPanel_GetFilePath(HWND hwnd);

// Close the currently loaded file and release resources
void TextPreviewPanel_CloseFile(HWND hwnd);

enum class TextPreviewViewMode {
    Decorated,
    Raw,
    Hex,
    Diagram,
};

// Set the view mode for the text panel
void TextPreviewPanel_SetViewMode(HWND hwnd, TextPreviewViewMode mode);

// Command IDs that the TextPreviewPanel can process via WM_COMMAND
constexpr UINT kCmdTextZoomIn = 201;
constexpr UINT kCmdTextZoomOut = 202;
constexpr UINT kCmdTextZoomReset = 203;

} // namespace readonly_viewer
