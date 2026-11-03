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
void ShowBottomPaneSettingsDialog(HWND owner);
void ShowNoteSettingsDialog(HWND owner);
void ShowOutputSettingsDialog(HWND owner, bool noteOutput = false);
void ShowMarkupSettingsDialog(HWND owner);
void ShowAnnotationSettingsDialog(HWND owner);
void ShowAnnotationSettingsForTool(HWND owner, ToolMode mode);
void ShowPaletteSettingsDialog(HWND owner);
void ShowAnnotationPaletteSettingsDialog(HWND owner);
void ShowSettingsAssetsDialog(HWND owner);
[[nodiscard]] bool HandleSettingsAssetsMessage(const MSG& message);
// Reuse the settings palette page for independent color selection. No palette
// file is written; cancellation leaves outColor unchanged.
[[nodiscard]] bool SelectColorInPaletteSettings(HWND owner, COLORREF initial, COLORREF* outColor);
