// file: toolbar.h
#pragma once

#include "core/app_core.h"

void ApplyActiveColorForMode(HWND hWnd, ToolMode mode);
void ApplyPaletteCustomColor(HWND hWnd, COLORREF color);
bool SyncWidthComboToMode(ToolMode mode);
void UpdateToolbarUI(HWND hWnd);

