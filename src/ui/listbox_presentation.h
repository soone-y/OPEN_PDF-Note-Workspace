#pragma once

#include <windows.h>
#include <string>

namespace ui {

inline constexpr const wchar_t* kListBoxUpdateProperty = L"PdfNote.ListBoxUpdate";

// The selection reconciler supplies the logical open index, independently of
// the native selection (which can temporarily point at an item that failed).
inline void RememberOpenListIndex(HWND list, int index) {
    if (!list) return;
    if (index < 0) RemovePropW(list, L"PdfNote.OpenListIndex");
    else SetPropW(list, L"PdfNote.OpenListIndex", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(index) + 1));
}

[[nodiscard]] inline int OpenListIndex(HWND list) {
    const auto stored = reinterpret_cast<UINT_PTR>(GetPropW(list, L"PdfNote.OpenListIndex"));
    return stored ? static_cast<int>(stored - 1) : -1;
}

inline void ClampFittingListBoxScroll(HWND list) {
    RECT client{};
    if (!GetClientRect(list, &client)) return;
    const auto extent = SendMessageW(list, LB_GETHORIZONTALEXTENT, 0, 0);
    if (extent >= 0 && extent <= client.right - client.left)
        SendMessageW(list, WM_HSCROLL, SB_LEFT, 0);
}

// Nested scopes and initially hidden controls keep their existing visibility.
// Only the scope which disabled drawing enables it; paint is deferred until
// the complete contents, extent and selection have been installed.
class ListBoxUpdate {
public:
    explicit ListBoxUpdate(HWND list) : list_(list) {
        if (!list_ || GetPropW(list_, kListBoxUpdateProperty) || !(GetWindowLongPtrW(list_, GWL_STYLE) & WS_VISIBLE)) return;
        // LISTBOX handles WM_SETREDRAW itself and need not clear WS_VISIBLE.
        // Use explicit ownership rather than the style bit to detect nesting.
        redraw_ = SetPropW(list_, kListBoxUpdateProperty, reinterpret_cast<HANDLE>(1)) != FALSE;
        if (redraw_) SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    }
    ~ListBoxUpdate() {
        if (redraw_ && GetPropW(list_, kListBoxUpdateProperty)) {
            SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
            RemovePropW(list_, kListBoxUpdateProperty);
            RedrawWindow(list_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
        }
    }
    ListBoxUpdate(const ListBoxUpdate&) = delete;
    ListBoxUpdate& operator=(const ListBoxUpdate&) = delete;
private:
    HWND list_;
    bool redraw_ = false;
};

[[nodiscard]] inline std::wstring ListBoxText(HWND list, int index) {
    const auto length = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
    if (length == LB_ERR || length < 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const auto copied = SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text.data()));
    if (copied == LB_ERR || copied < 0) return {};
    text.resize(static_cast<size_t>(copied));
    return text;
}

// Native wheel scrolling requests a separate client image, without WM_PAINT.
// Screen erasure is deferred to our buffer, but a print caller asking for an
// erased background needs it painted into its own DC before the native rows.
inline void EraseListBoxPrintBackground(HWND list, HDC dc, LPARAM flags, COLORREF background) {
    if (!(flags & PRF_ERASEBKGND) || !dc) return;
    RECT client{};
    if (!GetClientRect(list, &client)) return;
    const auto oldColor = SetDCBrushColor(dc, background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, oldColor);
}

// Native owner-draw LISTBOX does not paint its empty area without an erase.
// Print its rows over a complete background in memory, then present one image.
inline void DrawListBoxClient(HWND list, HDC dc, const RECT& client, COLORREF background) {
    const int saved = SaveDC(dc);
    const auto oldColor = SetDCBrushColor(dc, background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, oldColor);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
    const auto oldFont = font ? SelectObject(dc, font) : nullptr;
    SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    if (oldFont) SelectObject(dc, oldFont);
    if (saved) RestoreDC(dc, saved);
}

[[nodiscard]] inline bool PaintBufferedListBox(HWND list, COLORREF background) {
    if (GetPropW(list, kListBoxUpdateProperty)) {
        ValidateRect(list, nullptr);
        return true; // The owning update scope invalidates the finished list.
    }
    RECT client{};
    if (!GetClientRect(list, &client) || client.right <= 0 || client.bottom <= 0) return false;
    HDC source = GetDC(list);
    if (!source) return false;
    HDC memory = CreateCompatibleDC(source);
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(source, client.right, client.bottom) : nullptr;
    ReleaseDC(list, source);
    const auto oldBitmap = bitmap ? SelectObject(memory, bitmap) : nullptr;
    PAINTSTRUCT paint{};
    const HDC target = BeginPaint(list, &paint);
    if (target) {
        // Low-memory fallback still repaints the complete invalid client area.
        DrawListBoxClient(list, bitmap ? memory : target, client, background);
        const auto& dirty = paint.rcPaint;
        if (bitmap) BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                           memory, dirty.left, dirty.top, SRCCOPY);
        EndPaint(list, &paint);
    }
    if (oldBitmap) SelectObject(memory, oldBitmap);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    return target != nullptr;
}

} // namespace ui
