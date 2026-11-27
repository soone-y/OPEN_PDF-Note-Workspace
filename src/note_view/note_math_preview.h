#pragma once

#include "math/math_render.h"
#include <string>

struct MathDisplay {
    std::wstring body;
    std::unique_ptr<mathrender::Formula> formula;
};

[[nodiscard]] MathDisplay BuildMathPreview(std::wstring body, bool markupLines);
[[nodiscard]] bool BuildMathInputPreview(std::wstring_view raw, MathDisplay* display,
                                        bool* latexInput);
void DrawMathDisplayNoWrap(const MathDisplay& display, HDC dc, const RECT& rect,
                           int fontPx, mathrender::RenderStyle style, int scrollX);
