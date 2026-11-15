#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

// UTF-16 layout in upright cells, top to bottom and columns right to left.
// Indices remain offsets into the original text; wrapping never edits it.
namespace vertical_text {
struct Position { int column = 0, row = 0; };
struct Glyph { size_t start = 0, length = 0; Position position; };
struct Column { size_t start = 0, length = 0; };
struct Layout {
    std::vector<Glyph> glyphs;
    std::vector<Position> carets;
    std::vector<Column> columns;
    int rows = 1;
};
inline bool Continuation(wchar_t ch) {
    return (ch >= 0x300 && ch <= 0x36f) || (ch >= 0x1ab0 && ch <= 0x1aff) ||
        (ch >= 0x1dc0 && ch <= 0x1dff) || (ch >= 0xfe00 && ch <= 0xfe0f) ||
        (ch >= 0x3099 && ch <= 0x309a) || (ch >= 0x20d0 && ch <= 0x20ff);
}
inline size_t ScalarEnd(std::wstring_view text, size_t at) {
    return at + ((text[at] >= 0xd800 && text[at] <= 0xdbff && at + 1 < text.size() &&
        text[at + 1] >= 0xdc00 && text[at + 1] <= 0xdfff) ? 2 : 1);
}
inline size_t ClusterEnd(std::wstring_view text, size_t at) {
    size_t end = ScalarEnd(text, at);
    while (end < text.size()) {
        if (Continuation(text[end])) end = ScalarEnd(text, end);
        else if (text[end] == 0x200d && end + 1 < text.size() &&
                 text[end + 1] != L'\r' && text[end + 1] != L'\n') end = ScalarEnd(text, end + 1);
        else break;
    }
    return end;
}
[[nodiscard]] inline Layout Build(std::wstring_view text, int capacity) {
    Layout out;
    capacity = std::max(1, capacity);
    out.carets.resize(text.size() + 1);
    out.columns.push_back({0, 0});
    Position p;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'\r' || text[i] == L'\n') {
            size_t end = i + 1;
            if (text[i] == L'\r' && end < text.size() && text[end] == L'\n') ++end;
            for (size_t j = i; j < end; ++j) out.carets[j] = p;
            out.columns.back().length = i - out.columns.back().start;
            ++p.column; p.row = 0;
            out.columns.push_back({end, 0});
            out.carets[end] = p;
            i = end;
            continue;
        }
        if (p.row == capacity) {
            out.columns.back().length = i - out.columns.back().start;
            ++p.column; p.row = 0;
            out.columns.push_back({i, 0});
        }
        const size_t end = ClusterEnd(text, i);
        out.glyphs.push_back({i, end - i, p});
        for (size_t j = i; j < end; ++j) out.carets[j] = p;
        ++p.row;
        out.rows = std::max(out.rows, p.row);
        out.carets[end] = p;
        i = end;
    }
    out.columns.back().length = text.size() - out.columns.back().start;
    return out;
}
[[nodiscard]] inline Position Caret(const Layout& layout, size_t index) {
    return layout.carets.empty() ? Position{} : layout.carets[std::min(index, layout.carets.size() - 1)];
}
[[nodiscard]] inline size_t Hit(const Layout& layout, int column, double row) {
    if (layout.columns.empty()) return 0;
    column = std::clamp(column, 0, static_cast<int>(layout.columns.size()) - 1);
    size_t best = layout.columns[column].start;
    double distance = 1e30;
    const auto consider = [&](size_t index, Position p) {
        if (p.column != column) return;
        const double d = std::abs(p.row - row);
        if (d < distance) { distance = d; best = index; }
    };
    for (const auto& glyph : layout.glyphs) {
        consider(glyph.start, Caret(layout, glyph.start));
        consider(glyph.start + glyph.length, {glyph.position.column, glyph.position.row + 1});
    }
    consider(layout.carets.size() - 1, layout.carets.back());
    return best;
}
} // namespace vertical_text
