#pragma once

#include <windows.h>

#include "ui/layout.h"

namespace ui {

enum class SplitHit { None, VLeft, VRight, HMain, HLeftTop, HLeftMid };

SplitHit HitTest(const SplitBands& bands, POINT ptClient);
bool HandleSetCursor(HWND hWnd);
void PaintSplitBands(HDC hdc, const SplitBands& bands);

// MainWndProc timer ID; keep unique with the MainWndProc timer registry in core/app_core.h.
constexpr UINT_PTR kSplitDragTimerId = 0x6A11;

bool BeginDrag(HWND hWnd, POINT ptClient);
void UpdateDrag(HWND hWnd, POINT ptClient);
bool EndDrag(HWND hWnd);
void CancelDrag(HWND hWnd);
void OnTimer(HWND hWnd);

} // namespace ui


