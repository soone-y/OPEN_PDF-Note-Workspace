#pragma once

#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cwchar>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

// UI-thread-only, optional bitmap preparation for plain settings labels.
// Native controls remain responsible for printing their text and for input.
namespace settings_scroll_prepaint {
// Some Win32 headers omit this message at the configured SDK target level.
inline constexpr UINT dpiChangedAfterParent = 0x02E3; // WM_DPICHANGED_AFTERPARENT

struct Colors {
    COLORREF background;
    COLORREF text;
    [[nodiscard]] bool operator==(const Colors& other) const {
        return background == other.background && text == other.text;
    }
};
using ReadColors = Colors (*)();
inline constexpr UINT_PTR subclassId = 0x53505245;
// Keep page lookup compatible with the application's common controls version.
inline constexpr wchar_t pageProperty[] = L"PdfNote.SettingsScrollPrepaint.Page";
// Timer ledger for settings page HWNDs: prepaint=0x5350, palette autosave=1.
inline constexpr UINT_PTR timerId = 0x5350;
inline constexpr size_t maximumBytes = 4 * 1024 * 1024;
inline constexpr size_t maximumLabelBytes = 256 * 1024;
inline constexpr UINT intervalMs = 50;

struct Page;
struct Label {
    Page* page = nullptr;
    HWND window = nullptr;
    HBITMAP bitmap = nullptr;
    int width = 0, height = 0;
    size_t bytes = 0;
    Colors colors{};
    uint64_t revision = 0;
    bool attempted = false;
    bool printing = false;
    void Clear();
    [[nodiscard]] bool Matches() const;
    [[nodiscard]] bool Prepare();
    [[nodiscard]] bool Paint();
};

struct Page {
    HWND owner = nullptr, viewport = nullptr;
    ReadColors readColors = nullptr;
    std::vector<std::unique_ptr<Label>> labels;
    size_t bytes = 0;
    bool timer = false;
    bool destroyed = false;
    unsigned callbacks = 0;
    ~Page();
    void Stop() {
        if (timer) KillTimer(owner, timerId);
        timer = false;
    }
    void Wake() {
        if (!destroyed && !timer && !labels.empty())
            timer = SetTimer(owner, timerId, intervalMs, nullptr) != 0;
    }
    void Clear() {
        for (auto& label : labels) label->Clear();
    }
    void Warm();
};

// Printing and native message dispatch can reenter the owner and destroy it.
// Keep its labels alive until every nested callback has unwound.
struct CallbackScope {
    Page& page;
    explicit CallbackScope(Page& owner) : page(owner) { ++page.callbacks; }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    ~CallbackScope() {
        if (--page.callbacks == 0 && page.destroyed) delete &page;
    }
};

inline void Label::Clear() {
    ++revision;
    if (bitmap) DeleteObject(bitmap);
    bitmap = nullptr;
    page->bytes -= bytes;
    bytes = 0;
    attempted = false;
}

inline bool Label::Matches() const {
    RECT rect{};
    return bitmap && GetClientRect(window, &rect) &&
        rect.right == width && rect.bottom == height && colors == page->readColors();
}

[[nodiscard]] inline bool Label::Prepare() {
    attempted = true;
    const uint64_t preparedRevision = revision;
    const HWND preparedWindow = window;
    RECT rect{};
    if (!GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0) return false;
    const auto needed = static_cast<uint64_t>(rect.right) * rect.bottom * 4;
    if (needed > maximumLabelBytes || needed > maximumBytes - page->bytes) return false;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP prepared = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ previous = prepared ? SelectObject(dc, prepared) : nullptr;
    if (!previous || previous == HGDI_ERROR) {
        if (prepared) DeleteObject(prepared);
        DeleteDC(dc);
        return false;
    }
    // A memory DC has no screen clipping, including for labels just outside
    // the viewport. Let the original STATIC implementation draw the bitmap.
    const Colors preparedColors = page->readColors();
    HBRUSH background = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(window),
        WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(window)));
    if (!page->destroyed && window == preparedWindow && revision == preparedRevision) {
        FillRect(dc, &rect, background ? background : GetSysColorBrush(COLOR_WINDOW));
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
    }
    SelectObject(dc, previous);
    DeleteDC(dc);
    if (page->destroyed || window != preparedWindow || revision != preparedRevision ||
        !(preparedColors == page->readColors())) {
        DeleteObject(prepared);
        attempted = false;
        return false;
    }
    bitmap = prepared;
    width = rect.right;
    height = rect.bottom;
    colors = preparedColors;
    bytes = static_cast<size_t>(needed);
    page->bytes += bytes;
    return true;
}

[[nodiscard]] inline bool Label::Paint() {
    HDC memory = CreateCompatibleDC(nullptr);
    if (!memory) return false;
    HGDIOBJ previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR) { DeleteDC(memory); return false; }
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    const RECT& rect = paint.rcPaint;
    const bool copied = dc && BitBlt(dc, rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, memory, rect.left, rect.top, SRCCOPY);
    EndPaint(window, &paint);
    SelectObject(memory, previous);
    DeleteDC(memory);
    if (!copied) {
        Clear();
        InvalidateRect(window, &rect, TRUE);
    }
    return copied;
}

inline LRESULT CALLBACK LabelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR data) {
    auto& label = *reinterpret_cast<Label*>(data);
    CallbackScope scope(*label.page);
    if (message == WM_PRINT || message == WM_PRINTCLIENT) {
        const bool previous = label.printing;
        label.printing = true;
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        label.printing = previous;
        return result;
    }
    switch (message) {
    case WM_ERASEBKGND:
        if (!label.printing && label.Matches()) return 1;
        break;
    case WM_PAINT:
        if (!label.printing && !wParam && label.Matches() && label.Paint()) return 0;
        break;
    case WM_SETTEXT:
    case WM_SETFONT:
    case WM_STYLECHANGED:
    case WM_ENABLE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
    case WM_UPDATEUISTATE:
    case dpiChangedAfterParent:
        label.Clear();
        label.page->Wake();
        break;
    case WM_WINDOWPOSCHANGED:
        if (lParam && !(reinterpret_cast<const WINDOWPOS*>(lParam)->flags & SWP_NOSIZE) && !label.Matches()) {
            label.Clear();
            label.page->Wake();
        }
        break;
    case WM_NCDESTROY:
        label.Clear();
        label.window = nullptr;
        RemoveWindowSubclass(window, LabelProc, subclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

inline Page::~Page() {
    Stop();
    for (auto& label : labels) {
        if (label->window) RemoveWindowSubclass(label->window, LabelProc, subclassId);
        label->Clear();
    }
}

inline void Page::Warm() {
    if (destroyed) return;
    if (!IsWindowVisible(owner) || !IsWindowVisible(viewport)) { Stop(); Clear(); return; }
    // WM_TIMER is low priority. Also yield to queued input and active drags.
    if (HIWORD(GetQueueStatus(QS_INPUT)) || GetCapture()) return;
    RECT area{};
    if (!GetWindowRect(viewport, &area)) { Stop(); return; }
    const int margin = std::max(1L, (area.bottom - area.top) / 2);
    area.top -= margin;
    area.bottom += margin;
    // Release distant labels before allocating for the new viewport.
    for (auto& label : labels) {
        RECT rect{}, intersection{};
        if (!label->window || !IsWindowVisible(label->window) ||
            !GetWindowRect(label->window, &rect) || !IntersectRect(&intersection, &area, &rect)) {
            label->Clear();
        } else if (label->bitmap && !label->Matches()) {
            label->Clear();
        }
    }
    const ULONGLONG started = GetTickCount64();
    int prepared = 0;
    for (auto& label : labels) {
        RECT rect{}, intersection{};
        if (!label->window || label->attempted || !IsWindowVisible(label->window) ||
            !GetWindowRect(label->window, &rect) || !IntersectRect(&intersection, &area, &rect)) continue;
        // At most two labels and about 2 ms of work per tick. A failed optional
        // allocation falls back to native painting without repeated retries.
        const bool ready = label->Prepare();
        if (destroyed) return;
        // A synchronous text/theme change discarded the candidate. Keep the
        // next idle slice scheduled so the current content can be prepared.
        if (!ready && !label->attempted) return;
        if (++prepared == 2 || GetTickCount64() - started >= 2) return;
    }
    Stop();
}

inline LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR data) {
    auto* page = reinterpret_cast<Page*>(data);
    CallbackScope scope(*page);
    if (message == WM_NCDESTROY) {
        RemovePropW(window, pageProperty);
        RemoveWindowSubclass(window, PageProc, subclassId);
        page->destroyed = true;
        page->Stop();
        return DefSubclassProc(window, message, wParam, lParam);
    }
    if (message == WM_TIMER && wParam == timerId) { page->Warm(); return 0; }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (page->destroyed) return result;
    switch (message) {
    case WM_SHOWWINDOW:
        if (!wParam) { page->Stop(); page->Clear(); break; }
        page->Wake();
        break;
    case WM_SIZE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
    case dpiChangedAfterParent:
        page->Clear();
        page->Wake();
        break;
    default:
        break;
    }
    return result;
}

[[nodiscard]] inline bool Attach(HWND owner, HWND viewport, ReadColors readColors) {
    if (!owner || !viewport || !readColors) return false;
    if (GetPropW(owner, pageProperty)) return true;
    auto page = std::unique_ptr<Page>(new (std::nothrow) Page);
    if (!page) return false;
    page->owner = owner;
    page->viewport = viewport;
    page->readColors = readColors;
    try {
        // Only direct plain-text labels are eligible. Interactive controls,
        // owner-drawn previews, borders and transparent overlays stay native.
        for (HWND child = GetWindow(viewport, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t type[32]{};
            if (!GetClassNameW(child, type, 32) || _wcsicmp(type, L"STATIC") != 0) continue;
            const auto style = GetWindowLongPtrW(child, GWL_STYLE);
            const auto textType = style & SS_TYPEMASK;
            if (textType != SS_LEFT && textType != SS_CENTER && textType != SS_RIGHT && textType != SS_LEFTNOWORDWRAP) continue;
            if ((style & (WS_BORDER | SS_SUNKEN)) ||
                (GetWindowLongPtrW(child, GWL_EXSTYLE) & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_TRANSPARENT))) continue;
            if (page->labels.size() == 256) break;
            auto label = std::make_unique<Label>();
            label->page = page.get();
            label->window = child;
            page->labels.push_back(std::move(label));
            if (!SetWindowSubclass(child, LabelProc, subclassId,
                reinterpret_cast<DWORD_PTR>(page->labels.back().get()))) page->labels.pop_back();
        }
    } catch (const std::bad_alloc&) { return false; }
    if (!SetPropW(owner, pageProperty, page.get())) return false;
    if (!SetWindowSubclass(owner, PageProc, subclassId, reinterpret_cast<DWORD_PTR>(page.get()))) {
        RemovePropW(owner, pageProperty);
        return false;
    }
    page->Wake();
    (void)page.release();
    return true;
}

inline void Wake(HWND viewport) {
    auto* page = static_cast<Page*>(GetPropW(viewport, pageProperty));
    if (!page) {
        if (const HWND owner = GetParent(viewport)) {
            page = static_cast<Page*>(GetPropW(owner, pageProperty));
        }
    }
    if (page && page->viewport == viewport) page->Wake();
}

} // namespace settings_scroll_prepaint
