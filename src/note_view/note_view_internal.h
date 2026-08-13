#pragma once

#include "core/app_core.h"
#include "math/math_render.h"

#include <memory>
#include <string>
#include <vector>

struct MathDisplay {
    enum class LatexBlockLayout { None, Aligned, Cases, Matrix };
    MathKind kind = MathKind::Latex;
    std::wstring body; // delimiters removed
    std::unique_ptr<mathrender::Node> node;
    std::vector<std::wstring> lines;
    LatexBlockLayout latexBlock = LatexBlockLayout::None;
    std::vector<std::vector<std::wstring>> table;
};

int ScaleY(HWND hWnd, int pxAt96Dpi);
COLORREF AdjustColorBrightness(COLORREF base, int delta);
COLORREF BlendColor(COLORREF a, COLORREF b, double t);

bool ResolveCtrlInsertNavKey(HWND hWnd, WPARAM wParam, WPARAM* outNavKey, wchar_t* outSuppressedChar);
bool ForwardImeNavigationKey(HWND hWnd, WPARAM navKey);
bool IsImeComposingOnEditWindow(HWND hWnd);
bool MoveEditCaretForInsertNav(HWND hWnd, WPARAM navKey);

bool BuildBottomMathPreviewDisplay(const std::wstring& rawText, MathDisplay* outDisplay, MathKind* outKind);
void DrawMathDisplayNoWrap(const MathDisplay& m, HDC hdc, const RECT& rc, int fontPx,
                           mathrender::RenderStyle style, int scrollX, bool enhanceDepth);
