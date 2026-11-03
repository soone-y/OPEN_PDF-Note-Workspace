#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>

namespace note {

// Display-only preferences. Never add source characters or paragraph formats.
enum class BottomSpaceMode { Fixed = 0, Font = 1, Viewport = 2 };

[[nodiscard]] inline int BottomSpacePx(int mode, int fixedDip, int fontPercent,
    int viewportPercent, int dpi, int fontPx, int viewportPx) noexcept {
    const int viewport = std::max(0, viewportPx);
    const int font = std::max(1, fontPx);
    int64_t requested = 0;
    switch (static_cast<BottomSpaceMode>(std::clamp(mode, 0, 2))) {
    case BottomSpaceMode::Fixed:
        requested = (int64_t{std::clamp(fixedDip, 0, 2000)} * std::clamp(dpi, 48, 768) + 48) / 96;
        break;
    case BottomSpaceMode::Font:
        requested = (int64_t{std::clamp(fontPercent, 0, 2000)} * font + 50) / 100;
        break;
    case BottomSpaceMode::Viewport:
        requested = (int64_t{std::clamp(viewportPercent, 0, 90)} * viewport + 50) / 100;
        break;
    }
    // Keep at least one base text line usable even in a very short pane.
    return static_cast<int>(std::min<int64_t>(requested, std::max(0, viewport - font)));
}

[[nodiscard]] inline uint64_t BottomSpaceExtent(uint64_t content, int padding) noexcept {
    // Win32 SCROLLINFO has a signed 32-bit range. Saturate before addition.
    constexpr uint64_t limit = static_cast<uint64_t>(std::numeric_limits<int>::max());
    const uint64_t bounded = std::min(content, limit);
    return bounded + std::min<uint64_t>(std::max(0, padding), limit - bounded);
}

// Called only for active caret/source changes. Passive repaint and wheel
// scrolling must retain their origin, even when the caret is out of view.
[[nodiscard]] inline uint64_t RevealCaretWithBottomSpace(uint64_t origin,
    uint64_t caretTop, uint64_t caretBottom, int viewportPx, int paddingPx) noexcept {
    if (viewportPx <= 0) return origin;
    const uint64_t usable = static_cast<uint64_t>(viewportPx -
        std::clamp(paddingPx, 0, viewportPx - 1));
    // An oversized caret band cannot fit. Use one stable alignment rather
    // than alternating between its top and bottom on successive edits.
    if (caretBottom > caretTop && caretBottom - caretTop > usable) return caretBottom - usable;
    if (caretTop < origin) return caretTop;
    if (caretBottom > origin && caretBottom - origin > usable) return caretBottom - usable;
    return origin;
}

[[nodiscard]] inline bool ParseBottomSpaceValue(std::wstring_view text, int maximum, int& value) noexcept {
    if (text.empty() || text.size() > 4) return false;
    int parsed = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        parsed = parsed * 10 + (ch - L'0');
        if (parsed > maximum) return false;
    }
    value = parsed;
    return true;
}

} // namespace note
