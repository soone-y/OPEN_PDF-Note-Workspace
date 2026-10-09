// file: settings.h
#pragma once

#include <windows.h>

enum class ToolMode;

void ApplyNoteFont();
void ApplyNoteSystem(HWND hWnd);
void UpdateMathListVisibility();
void UpdateAutoSaveTimer(HWND hWnd);
void UpdateAutoIntegrateTimer(HWND hWnd);
void ApplyBottomPaneEdgeStyle();
void UpdateBottomPaneMenuChecks();
void UpdateScrollDirectionMenuChecks();
void UpdatePdfSinglePageModeMenuCheck();

void ShowGeneralSettingsDialog(HWND owner);
void ShowNoteSettingsDialog(HWND owner);
void ShowMarkupSettingsDialog(HWND owner);
void ShowAnnotationSettingsDialog(HWND owner);
void ShowAnnotationSettingsForTool(HWND owner, ToolMode mode);
void ShowPaletteSettingsDialog(HWND owner);
void ShowSettingsAssetsDialog(HWND owner);
[[nodiscard]] bool HandleSettingsAssetsMessage(const MSG& message);
// The palette has its own editor so changing an annotation palette color never
// mutates the Windows common-dialog custom-color history.
bool ShowPaletteColorEditorDialog(HWND owner, COLORREF initial, COLORREF* outColor);
