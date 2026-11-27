#include "note_view/note_math_preview.h"
#include "math/math_source.h"
#include "note/note_math.h"

#include <algorithm>
#include <cmath>
#include <limits>

MathDisplay BuildMathPreview(std::wstring body, bool markupLines) {
    MathDisplay display;
    display.body = std::move(body);
    std::wstring tex = display.body;
    if (markupLines && tex.find_first_of(L"\r\n") != std::wstring::npos &&
        tex.find(L"\\begin{") == std::wstring::npos) {
        std::wstring rows = L"\\begin{aligned}&";
        for (size_t i = 0; i < tex.size(); ++i) {
            if (tex[i] == L'\r' || tex[i] == L'\n') {
                if (tex[i] == L'\r' && i + 1 < tex.size() && tex[i + 1] == L'\n') ++i;
                rows += L"\\\\&";
            } else {
                rows += tex[i];
            }
        }
        tex = rows + L"\\end{aligned}";
    }
    display.formula = mathrender::Parse(tex);
    return display;
}

bool BuildMathInputPreview(std::wstring_view raw, MathDisplay* out, bool* latexInput) {
    const auto analysis = note::AnalyzeMathBoxInput(raw);
    if (!analysis.has_input) return false;
    bool latex = analysis.flavor == note::MathInputFlavor::Latex;
    MathDisplay display;
    if (!analysis.diagnostics.empty()) {
        display.body = analysis.trimmed_text;
    } else if (!analysis.has_wrapping) {
        display = BuildMathPreview(analysis.content_text, false);
        latex = bool(display.formula);
        if (!latex) display = BuildMathPreview(mathrender::NormalizeMarkupSource(analysis.content_text), true);
    } else {
        display = BuildMathPreview(latex ? analysis.content_text :
            mathrender::NormalizeMarkupSource(analysis.content_text), !latex);
    }
    if (out) *out = std::move(display);
    if (latexInput) *latexInput = latex;
    return true;
}

void DrawMathDisplayNoWrap(const MathDisplay& display, HDC dc, const RECT& rect,
                           int fontPx, mathrender::RenderStyle style, int scrollX) {
    if (!dc || rect.right <= rect.left || rect.bottom <= rect.top) return;
    const int saved = SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    mathrender::Layout layout;
    if (display.formula) layout = mathrender::Measure(*display.formula, dc, fontPx, style);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    bool rendered = false;
    if (display.formula && layout.width > 0 && layout.height > 0) {
        const double scale = std::min({1.0, double(width) / layout.width, double(height) / layout.height});
        const int drawWidth = std::min(width, static_cast<int>(std::ceil(layout.width * scale)));
        const int drawHeight = std::min(height, static_cast<int>(std::ceil(layout.height * scale)));
        const int left = rect.left + (width - drawWidth) / 2;
        const int top = rect.top + (height - drawHeight) / 2;
        const RECT target{left, top, left + drawWidth, top + drawHeight};
        rendered = mathrender::DrawInBox(*display.formula, dc, target, fontPx, GetTextColor(dc), style);
    }
    if (!rendered) {
        RECT textRect = rect;
        textRect.left -= std::max(0, scrollX);
        DrawTextW(dc, display.body.c_str(), static_cast<int>(std::min(display.body.size(),
            static_cast<size_t>(std::numeric_limits<int>::max()))), &textRect,
            DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS);
    }
    RestoreDC(dc, saved);
}
