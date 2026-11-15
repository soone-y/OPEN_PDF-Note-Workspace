#pragma once
#include "core/vertical_text.h"
#include <windows.h>
#include <string>

namespace vertical_text {
inline wchar_t Presentation(wchar_t ch) {
    switch (ch) {
    case L'、': return 0xfe11; case L'。': return 0xfe12;
    case L'「': return 0xfe41; case L'」': return 0xfe42;
    case L'『': return 0xfe43; case L'』': return 0xfe44;
    case L'（': case L'(': return 0xfe35; case L'）': case L')': return 0xfe36;
    case L'ー': case L'—': return 0xfe31;
    default: return ch;
    }
}
inline RECT Cell(const RECT& inner, int pitch, Position p) {
    const int right = inner.right - p.column * pitch;
    return {right - pitch, inner.top + p.row * pitch, right, inner.top + (p.row + 1) * pitch};
}
// Uses the caller's selected font/color and preserves its clipping state.
inline void Draw(HDC dc, const RECT& inner, std::wstring_view text, int pitch) {
    pitch = std::max(1, pitch);
    const auto layout = Build(text, std::max(1, static_cast<int>((inner.bottom - inner.top) / pitch)));
    const int saved = SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc, inner.left, inner.top, inner.right, inner.bottom);
    for (const auto& glyph : layout.glyphs) {
        const auto cell = Cell(inner, pitch, glyph.position);
        if (cell.right <= inner.left) break;
        std::wstring display(text.substr(glyph.start, glyph.length));
        if (display.size() == 1) {
            const wchar_t alternate = Presentation(display[0]);
            WORD index = 0xffff;
            if (alternate != display[0] && GetGlyphIndicesW(dc, &alternate, 1, &index,
                    GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && index != 0xffff) display[0] = alternate;
        }
        SIZE size{};
        if (GetTextExtentPoint32W(dc, display.data(), static_cast<int>(display.size()), &size))
            TextOutW(dc, cell.left + (pitch - size.cx) / 2, cell.top,
                     display.data(), static_cast<int>(display.size()));
    }
    RestoreDC(dc, saved);
}
} // namespace vertical_text
