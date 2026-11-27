#pragma once

#include "core/app_core.h"
#include "math/math_render.h"

#include <memory>
#include <string>
#include <vector>

#include "note_view/note_math_preview.h"

int ScaleY(HWND hWnd, int pxAt96Dpi);
COLORREF AdjustColorBrightness(COLORREF base, int delta);
COLORREF BlendColor(COLORREF a, COLORREF b, double t);

bool ResolveCtrlInsertNavKey(HWND hWnd, WPARAM wParam, WPARAM* outNavKey, wchar_t* outSuppressedChar);
bool ForwardImeNavigationKey(HWND hWnd, WPARAM navKey);
bool IsImeComposingOnEditWindow(HWND hWnd);
bool MoveEditCaretForInsertNav(HWND hWnd, WPARAM navKey);

bool BuildBottomMathPreviewDisplay(const std::wstring& rawText, MathDisplay* outDisplay, MathKind* outKind);
