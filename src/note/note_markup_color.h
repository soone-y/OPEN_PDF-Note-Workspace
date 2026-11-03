#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace note {

enum class NoteMarkupColorKind : uint8_t { Inherit, Explicit, HighContrast };
struct NoteMarkupColor {
    NoteMarkupColorKind kind = NoteMarkupColorKind::Inherit;
    uint32_t rgb = 0; // 0xRRGGBB, not Windows COLORREF byte order.
};

namespace markup_color_detail {
struct NamedColor { std::wstring_view aliases; uint32_t rgb; };
// Fixed vocabulary shared by display and export. Pale names are independent
// colours, not context-dependent transformations of the corresponding base.
inline constexpr NamedColor colors[] = {
    {L"black|黒|黒色", 0x000000}, {L"white|白|白色", 0xffffff},
    {L"gray|grey|灰|灰色", 0x808080}, {L"lightgray|lightgrey|薄灰|薄灰色", 0xd3d3d3},
    {L"red|赤|赤色", 0xff0000}, {L"orange|オレンジ|オレンジ色|橙|橙色", 0xffa500},
    {L"yellow|黄|黄色", 0xffff00}, {L"green|緑|緑色", 0x008000},
    {L"lime|ライム|明るい緑|明るい緑色", 0x00ff00}, {L"blue|青|青色", 0x0000ff},
    {L"navy|紺|紺色", 0x000080}, {L"cyan|シアン|水色", 0x00ffff},
    {L"purple|紫|紫色", 0x800080}, {L"magenta|マゼンタ|赤紫|赤紫色", 0xff00ff},
    {L"pink|ピンク|ピンク色|桃色", 0xffc0cb}, {L"brown|茶|茶色", 0xa52a2a},
    {L"lightred|薄赤|薄赤色", 0xffe4e4},
    {L"lightorange|薄橙|薄橙色|薄オレンジ", 0xfff0db},
    {L"lightyellow|薄黄|薄黄色", 0xfff9c4},
    {L"lightgreen|薄緑|薄緑色", 0xe5f3e5},
    {L"lightblue|薄青|薄青色", 0xe3f0ff},
    {L"lightpurple|薄紫|薄紫色", 0xf0e6fa},
    {L"lightpink|薄桃|薄桃色|薄ピンク", 0xfce4ec},
};

[[nodiscard]] inline bool Equal(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c + L'a' - L'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
[[nodiscard]] inline int Hex(wchar_t c) noexcept {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
} // namespace markup_color_detail

// Pure, bounded and allocation-free. Unknown input inherits the normal colour;
// callers own theme handling and never replace the user's source with a name
// or numeric value. Colour names have identical meaning for char and back.
[[nodiscard]] inline NoteMarkupColor ResolveNoteMarkupColor(std::wstring_view value) noexcept {
    const auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    using namespace markup_color_detail;
    if (value.empty() || value.size() > 32) return {};
    if (Equal(value, L"hicont") || Equal(value, L"hi-contrast") ||
        Equal(value, L"highcontrast") || Equal(value, L"high-contrast") || value == L"高コントラスト")
        return {NoteMarkupColorKind::HighContrast, 0};
    auto hex = value;
    if (hex.size() >= 2 && hex[0] == L'0' && (hex[1] == L'x' || hex[1] == L'X')) hex.remove_prefix(2);
    if (!hex.empty() && hex.front() == L'#') hex.remove_prefix(1);
    if (hex.size() == 6) {
        uint32_t rgb = 0;
        bool valid = true;
        for (wchar_t c : hex) {
            const int digit = Hex(c);
            if (digit < 0) { valid = false; break; }
            rgb = (rgb << 4) | static_cast<uint32_t>(digit);
        }
        if (valid) return {NoteMarkupColorKind::Explicit, rgb};
    }
    for (const auto& color : colors) {
        auto aliases = color.aliases;
        while (!aliases.empty()) {
            const size_t end = aliases.find(L'|');
            if (Equal(value, aliases.substr(0, end))) return {NoteMarkupColorKind::Explicit, color.rgb};
            if (end == std::wstring_view::npos) break;
            aliases.remove_prefix(end + 1);
        }
    }
    return {};
}

} // namespace note
