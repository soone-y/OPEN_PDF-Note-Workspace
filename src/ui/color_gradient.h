#pragma once
#include "settings/palette_color_math.h"
#include <windowsx.h>
#include <vector>

// Parent owns State and its HSV model for the child lifetime. This control
// edits only that model. Parent authorizes input and owns all persistence.
namespace ui::color_gradient {
inline constexpr int kBegin = 0x100, kChanged = 0x101, kEnd = 0x102, kCancel = 0x103;
struct State {
    palette_color::Hsv* color = nullptr;
    bool hue = false, dragging = false;
    palette_color::Hsv before{};
    std::vector<DWORD> pixels;
    int cachedHue = -1, cachedWidth = 0, cachedHeight = 0;
};
inline LRESULT Notify(HWND window, int code) {
    return SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), code), reinterpret_cast<LPARAM>(window));
}
inline void ChangeAt(HWND window, State& state, int x, int y) {
    const auto previous = *state.color;
    RECT rc{}; GetClientRect(window, &rc);
    if (state.hue) state.color->h = palette_color::Position(x, rc.right, 3599);
    else {
        state.color->s = palette_color::Position(x, rc.right, 1000);
        state.color->v = 1000 - palette_color::Position(y, rc.bottom, 1000);
    }
    if (state.color->h != previous.h || state.color->s != previous.s || state.color->v != previous.v)
        Notify(window, kChanged);
}
inline void Cancel(HWND window, State& state) {
    if (!state.dragging) return;
    state.dragging = false;
    *state.color = state.before;
    if (GetCapture() == window) ReleaseCapture();
    Notify(window, kCancel);
}
inline void Paint(HWND window, State& state, HDC dc, const RECT& rc) {
    const int w = std::min(256, static_cast<int>(rc.right)), h = state.hue ? 1 : std::min(128, static_cast<int>(rc.bottom));
    if (w <= 0 || h <= 0) return;
    const int hue = state.hue ? 0 : state.color->h;
    if (state.cachedHue != hue || state.cachedWidth != w || state.cachedHeight != h) {
        state.pixels.resize(static_cast<size_t>(w) * h); // Bounded even at high DPI.
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const auto rgb = palette_color::ToRgb(state.hue
                ? palette_color::Hsv{palette_color::Position(x, w, 3599), 1000, 1000}
                : palette_color::Hsv{hue, palette_color::Position(x, w, 1000), 1000 - palette_color::Position(y, h, 1000)});
            state.pixels[static_cast<size_t>(y) * w + x] = (static_cast<DWORD>(GetRValue(rgb)) << 16) |
                (static_cast<DWORD>(GetGValue(rgb)) << 8) | GetBValue(rgb);
        }
        state.cachedHue = hue; state.cachedWidth = w; state.cachedHeight = h;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, w, h, state.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    const int x = MulDiv(state.hue ? state.color->h : state.color->s, std::max(0L, rc.right - 1), state.hue ? 3599 : 1000);
    const int y = MulDiv(1000 - state.color->v, std::max(0L, rc.bottom - 1), 1000);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    for (int outline = 0; outline < 2; ++outline) {
        HPEN pen = CreatePen(PS_SOLID, 1, outline ? RGB(255,255,255) : RGB(0,0,0));
        const HGDIOBJ oldPen = SelectObject(dc, pen);
        if (state.hue) Rectangle(dc, x - 2 + outline, outline, x + 3 - outline, rc.bottom - outline);
        else Ellipse(dc, x - 5 + outline, y - 5 + outline, x + 6 - outline, y + 6 - outline);
        SelectObject(dc, oldPen); DeleteObject(pen);
    }
    SelectObject(dc, oldBrush);
    if (GetFocus() == window) { RECT focus = rc; InflateRect(&focus, -2, -2); DrawFocusRect(dc, &focus); }
}
inline void PaintBuffered(HWND window, State& state, HDC dc, const RECT& rc) {
    if (rc.right <= 0 || rc.bottom <= 0) return;
    // Own temporary GDI objects per paint. Restoration also happens if the
    // bounded gradient cache allocation throws. Only the finished frame is shown.
    struct Surface {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        ~Surface() {
            if (oldBitmap && oldBitmap != HGDI_ERROR) SelectObject(dc, oldBitmap);
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
        }
    } surface;
    surface.dc = CreateCompatibleDC(dc);
    if (surface.dc) surface.bitmap = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    if (surface.bitmap) surface.oldBitmap = SelectObject(surface.dc, surface.bitmap);
    if (!surface.oldBitmap || surface.oldBitmap == HGDI_ERROR) {
        Paint(window, state, dc, rc); // Allocation failure: still draw the actual color, without background erasure.
        return;
    }
    Paint(window, state, surface.dc, rc);
    if (!BitBlt(dc, 0, 0, rc.right, rc.bottom, surface.dc, 0, 0, SRCCOPY)) Paint(window, state, dc, rc);
}
inline LRESULT Dispatch(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_CREATE) {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        if (!state || !state->color) return -1;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return 0;
    }
    if (!state) return DefWindowProcW(window, msg, wp, lp);
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | ((state->dragging && lp && reinterpret_cast<MSG*>(lp)->wParam == VK_ESCAPE) ? DLGC_WANTMESSAGE : 0);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps); RECT rc{}; GetClientRect(window, &rc);
        try { PaintBuffered(window, *state, dc, rc); }
        catch (...) { FillRect(dc, &rc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1)); }
        EndPaint(window, &ps); return 0;
    }
    case WM_SETFOCUS:
        Notify(window, BN_SETFOCUS); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        if (!Notify(window, kBegin)) return 0;
        SetFocus(window); state->before = *state->color; SetCapture(window);
        state->dragging = GetCapture() == window;
        if (state->dragging) ChangeAt(window, *state, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (state->dragging) ChangeAt(window, *state, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP:
        if (state->dragging) {
            ChangeAt(window, *state, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            state->dragging = false; if (GetCapture() == window) ReleaseCapture(); Notify(window, kEnd);
        }
        return 0;
    case WM_CAPTURECHANGED: Cancel(window, *state); return 0;
    case WM_KEYDOWN: {
        if (wp == VK_ESCAPE && state->dragging) { Cancel(window, *state); return 0; }
        if (wp != VK_LEFT && wp != VK_RIGHT && wp != VK_UP && wp != VK_DOWN && wp != VK_HOME && wp != VK_END) break;
        if (!Notify(window, kBegin)) return 0;
        const int direction = wp == VK_LEFT || wp == VK_DOWN ? -1 : 1;
        if (state->hue) state->color->h = wp == VK_HOME ? 0 : wp == VK_END ? 3599 : std::clamp(state->color->h + direction * 10, 0, 3599);
        else if (wp == VK_UP || wp == VK_DOWN) state->color->v = std::clamp(state->color->v + direction * 10, 0, 1000);
        else state->color->s = wp == VK_HOME ? 0 : wp == VK_END ? 1000 : std::clamp(state->color->s + direction * 10, 0, 1000);
        Notify(window, kChanged); Notify(window, kEnd); return 0;
    }
    case WM_NCDESTROY:
        state->dragging = false; if (GetCapture() == window) ReleaseCapture();
        SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
    default: break;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
inline LRESULT CALLBACK Proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    try { return Dispatch(window, msg, wp, lp); }
    catch (...) {
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state && state->dragging) {
            // Cancel while state is still borrowed; no exception crosses Win32.
            try { Cancel(window, *state); } catch (...) {}
        }
        return msg == WM_CREATE ? -1 : 0;
    }
}
[[nodiscard]] inline HWND Create(HWND parent, int id, const wchar_t* label, State& state) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = L"PdfNoteColorGradientClass";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowExW(WS_EX_CLIENTEDGE, wc.lpszClassName, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 100, 40, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, &state);
}
} // namespace ui::color_gradient
