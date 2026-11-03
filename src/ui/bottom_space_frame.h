#pragma once

#include <windows.h>
#include <algorithm>

namespace ui {

// A native editor's formatting rectangle does not constrain its scroll page.
// Reserve actual non-client space so native scrolling, caret and IMM agree.
struct BottomSpaceFrame {
    int padding_px = 0;
    int available_height_px = 0;
    bool updating = false;

    void Apply(RECT& client, int requested) noexcept {
        available_height_px = std::max<LONG>(0, client.bottom - client.top);
        padding_px = std::clamp(requested, 0, std::max(0, available_height_px - 1));
        client.bottom -= padding_px;
    }

    [[nodiscard]] bool Paint(HWND window, COLORREF color) const noexcept {
        if (padding_px <= 0) return true;
        RECT bounds{}, client{};
        POINT origin{};
        if (!GetWindowRect(window, &bounds) || !GetClientRect(window, &client) ||
            !ClientToScreen(window, &origin)) return false;
        const LONG left = origin.x - bounds.left;
        const LONG top = origin.y - bounds.top + client.bottom;
        RECT footer{left, top, left + client.right, top + padding_px};
        HDC dc = GetWindowDC(window);
        if (!dc) return false;
        HBRUSH brush = CreateSolidBrush(color);
        const bool painted = brush && FillRect(dc, &footer, brush) != 0;
        if (brush) DeleteObject(brush);
        ReleaseDC(window, dc);
        return painted;
    }
};

} // namespace ui
