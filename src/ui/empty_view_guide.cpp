#include "ui/empty_view_guide.h"
#include <algorithm>
#include <cstring>
#include <windowsx.h>

namespace empty_view_guide {
namespace {
struct Row { RECT rect; std::wstring text; bool heading; };
SCROLLINFO ScrollInfo(UINT mask) {
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = mask; return info;
}
int Px(HWND owner, int value) {
    using DpiQuery = UINT(WINAPI*)(HWND);
    static const auto query = [] {
        const auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        DpiQuery function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }();
    if (query) {
        const UINT dpi = query(owner);
        if (dpi) return MulDiv(value, static_cast<int>(dpi), 96);
    }
    HDC dc = GetDC(owner);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(owner, dc);
    return MulDiv(value, dpi > 0 ? dpi : 96, 96);
}
int Measure(HDC dc, const std::wstring& text, int width) {
    RECT measured{0, 0, std::max(1, width), 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    return measured.bottom;
}
void SetAccessibleText(HWND owner, const std::wstring& text) {
    const int length = GetWindowTextLengthW(owner);
    std::wstring previous(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(owner, previous.data(), static_cast<int>(previous.size()));
    previous.resize(static_cast<size_t>(std::max(0, copied)));
    if (previous != text) SetWindowTextW(owner, text.c_str());
}
void SetPosition(HWND owner, int position) {
    auto info = ScrollInfo(SIF_RANGE | SIF_PAGE | SIF_POS);
    if (!GetScrollInfo(owner, SB_VERT, &info)) return;
    const int maximum = std::max(0, info.nMax - static_cast<int>(info.nPage) + 1);
    const int next = std::clamp(position, 0, maximum);
    if (next == info.nPos) return;
    info.fMask = SIF_POS; info.nPos = next;
    SetScrollInfo(owner, SB_VERT, &info, TRUE);
    InvalidateRect(owner, nullptr, FALSE);
}
}

std::wstring PlainText(const Content& content) {
    if (content.title.empty()) return {};
    std::wstring text = content.title + L"\r\n";
    if (!content.usage.empty()) text += content.usage + L"\r\n";
    text += content.generalTitle + L"\r\n";
    for (const auto& line : content.general) text += line + L"\r\n";
    text += content.annotationTitle + L"\r\n";
    for (const auto& line : content.annotation) text += line + L"\r\n";
    if (!content.displayHint.empty()) text += content.displayHint + L"\r\n";
    return text;
}

void Clear(HWND owner, bool resetScroll) {
    SetAccessibleText(owner, {});
    if (!resetScroll) return;
    ShowScrollBar(owner, SB_HORZ, FALSE);
    auto info = ScrollInfo(SIF_RANGE | SIF_PAGE | SIF_POS);
    SetScrollInfo(owner, SB_VERT, &info, TRUE);
}

void Paint(HWND owner, HDC dc, const RECT& client, const Content& content,
           HFONT font, const ThemeColors& theme) {
    if (content.title.empty()) { Clear(owner); return; }
    ShowScrollBar(owner, SB_HORZ, FALSE);
    SetAccessibleText(owner, PlainText(content));
    const int saved = SaveDC(dc);
    SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme.panelText);
    LOGFONTW description{};
    GetObjectW(GetCurrentObject(dc, OBJ_FONT), sizeof(description), &description);
    description.lfWeight = FW_BOLD;
    HFONT headingFont = CreateFontIndirectW(&description);
    const int pad = Px(owner, 32), gap = Px(owner, 24), rowGap = Px(owner, 8);
    const int width = std::max(1, std::min(Px(owner, 920), static_cast<int>(client.right-client.left) - pad*2));
    const int left = client.left + (client.right-client.left-width)/2;
    const bool columns = width >= Px(owner, 620);
    const int columnWidth = columns ? (width-gap)/2 : width;
    std::vector<Row> rows;
    int y = 0;
    const auto add = [&](const std::wstring& text, int x, int top, int rowWidth, bool heading) {
        const auto old = heading && headingFont ? SelectObject(dc, headingFont) : nullptr;
        const int height = Measure(dc, text, rowWidth);
        if (old) SelectObject(dc, old);
        rows.push_back({{x, top, x+rowWidth, top+height}, text, heading});
        return top + height + rowGap;
    };
    y = add(content.title, left, y, width, true) + rowGap;
    if (!content.usage.empty()) y = add(content.usage, left, y, width, false) + rowGap;
    int generalY = add(content.generalTitle, left, y, columnWidth, true);
    for (const auto& line : content.general) generalY = add(line, left, generalY, columnWidth, false);
    const int annotationX = columns ? left+columnWidth+gap : left;
    int annotationY = add(content.annotationTitle, annotationX,
                          columns ? y : generalY+rowGap, columnWidth, true);
    for (const auto& line : content.annotation)
        annotationY = add(line, annotationX, annotationY, columnWidth, false);
    const int height = std::max(generalY, annotationY);
    const int hintHeight = content.displayHint.empty() ? 0 : Measure(dc, content.displayHint, width);
    const int settingsHeight = hintHeight ? hintHeight+rowGap : 0;
    int available = std::max(1, static_cast<int>(client.bottom-client.top) - pad*2-settingsHeight);
    const bool overflow = height > available;
    const int footerHeight = overflow ? Measure(dc, content.scrollHint, width) + rowGap : 0;
    available = std::max(1, available-footerHeight);
    auto info = ScrollInfo(SIF_POS);
    GetScrollInfo(owner, SB_VERT, &info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0; info.nMax = std::max(0, height-1); info.nPage = available;
    info.nPos = std::clamp(info.nPos, 0, std::max(0, height-available));
    SetScrollInfo(owner, SB_VERT, &info, TRUE);
    const int top = client.top+pad + (overflow ? -info.nPos : (available-height)/2);
    IntersectClipRect(dc, client.left, client.top+pad, client.right, client.top+pad+available);
    for (auto& row : rows) {
        OffsetRect(&row.rect, 0, top);
        const auto old = row.heading && headingFont ? SelectObject(dc, headingFont) : nullptr;
        DrawTextW(dc, row.text.c_str(), static_cast<int>(row.text.size()), &row.rect, DT_WORDBREAK | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
    }
    RestoreDC(dc, saved);
    if (overflow) {
        const int footerSaved = SaveDC(dc);
        SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, theme.panelText);
        RECT footer{left, client.bottom-pad-settingsHeight-footerHeight+rowGap,
                    left+width, client.bottom-pad-settingsHeight};
        DrawTextW(dc, content.scrollHint.c_str(), -1, &footer, DT_WORDBREAK | DT_NOPREFIX);
        RestoreDC(dc, footerSaved);
    }
    if (!content.displayHint.empty()) {
        const int hintSaved = SaveDC(dc);
        SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, theme.panelText);
        RECT hint{left, client.bottom-pad-hintHeight, left+width, client.bottom-pad};
        DrawTextW(dc, content.displayHint.c_str(), -1, &hint, DT_WORDBREAK | DT_NOPREFIX);
        RestoreDC(dc, hintSaved);
    }
    if (headingFont) DeleteObject(headingFont);
}

bool Scroll(HWND owner, UINT message, WPARAM wParam) {
    auto info = ScrollInfo(SIF_ALL);
    if (!GetScrollInfo(owner, SB_VERT, &info)) return false;
    int position = info.nPos;
    const int line = Px(owner, 28), page = std::max(line, static_cast<int>(info.nPage)-line);
    if (message == WM_MOUSEWHEEL) {
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) == 0)
            position -= GET_WHEEL_DELTA_WPARAM(wParam); // Preserve even one-unit touchpad deltas.
    } else if (message == WM_VSCROLL) {
        switch (LOWORD(wParam)) {
        case SB_LINEUP: position -= line; break;
        case SB_LINEDOWN: position += line; break;
        case SB_PAGEUP: position -= page; break;
        case SB_PAGEDOWN: position += page; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = info.nMax; break;
        default: break;
        }
    } else if (message == WM_KEYDOWN) {
        switch (wParam) {
        case VK_UP: position -= line; break;
        case VK_DOWN: position += line; break;
        case VK_PRIOR: position -= page; break;
        case VK_NEXT: position += page; break;
        case VK_HOME: position = 0; break;
        case VK_END: position = info.nMax; break;
        default: return false;
        }
    } else return false;
    SetPosition(owner, position);
    return true;
}
}
