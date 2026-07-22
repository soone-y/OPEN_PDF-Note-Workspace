// file: settings.h
#pragma once

#include <windows.h>

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
void ShowPaletteSettingsDialog(HWND owner);
