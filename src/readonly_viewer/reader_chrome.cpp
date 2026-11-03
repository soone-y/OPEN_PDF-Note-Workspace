#include "reader_chrome.h"
#include "core/localization.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <richedit.h>
#include <new>

namespace reader_next {
void SearchOverview::Clear() {
    active_ = false; next_ = 0; totalLines_ = 1;
    query_.clear(); pendingLines_.clear(); lines_.clear();
}
bool SearchOverview::Reset(HWND editor, const std::wstring& query, int overviewHeight) {
    Clear();
    active_ = !query.empty();
    if (!active_) return true;
    overviewHeight_ = std::max(1, overviewHeight);
    totalLines_ = std::max(1, static_cast<int>(SendMessageW(editor, EM_GETLINECOUNT, 0, 0)));
    try { query_ = query; }
    catch (const std::bad_alloc&) { return false; }
    return true;
}
SearchOverview::Result SearchOverview::Step(HWND editor) {
    if (!active_ || query_.empty()) return Result::Complete;
    // Bound each UI turn; a dense document must still allow painting/input.
    const ULONGLONG started = GetTickCount64();
    try {
        for (int count = 0; count < 256; ++count) {
            FINDTEXTEXW find{};
            find.lpstrText = const_cast<wchar_t*>(query_.c_str());
            find.chrg = {next_, -1};
            if (SendMessageW(editor, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&find)) < 0) {
                lines_.swap(pendingLines_); pendingLines_.clear();
                return Result::Complete;
            }
            if (find.chrgText.cpMax <= next_) return Result::Failed;
            const int first = static_cast<int>(SendMessageW(editor, EM_EXLINEFROMCHAR, 0, find.chrgText.cpMin));
            const int last = static_cast<int>(SendMessageW(editor, EM_EXLINEFROMCHAR, 0, find.chrgText.cpMax - 1));
            if (first < 0 || last < first || last >= totalLines_) return Result::Failed;
            // Store one representative per painted pixel row, bounding memory
            // by the band height even when millions of lines match.
            const auto row = [this](int line) { return static_cast<long long>(line) * overviewHeight_ / totalLines_; };
            for (int line = first; line <= last; ++line)
                if (pendingLines_.empty() || row(pendingLines_.back()) != row(line)) pendingLines_.push_back(line);
            next_ = find.chrgText.cpMax;
            if (GetTickCount64() - started >= 8) break;
        }
    } catch (const std::bad_alloc&) {
        pendingLines_.clear(); lines_.clear(); return Result::Failed;
    }
    return Result::Pending;
}
namespace {
HFONT uiFont = nullptr, headingFont = nullptr;
HBRUSH canvas = nullptr, sidebar = nullptr, toolbar = nullptr;
void Fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
void Text(HDC dc, RECT rect, const std::wstring& value, HFONT font, COLORREF color, UINT flags) {
    auto old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.c_str(), -1, &rect, flags);
    SelectObject(dc, old);
}
}
int Px(HWND owner, int value) {
    using GetDpi = UINT(WINAPI*)(HWND);
    static auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return MulDiv(value, getDpi ? getDpi(owner) : 96, 96);
}
void Cleanup() {
    if (uiFont) DeleteObject(uiFont);
    if (headingFont) DeleteObject(headingFont);
    if (canvas) DeleteObject(canvas);
    if (sidebar) DeleteObject(sidebar);
    if (toolbar) DeleteObject(toolbar);
    uiFont = headingFont = nullptr;
    canvas = sidebar = toolbar = nullptr;
}
void Initialize(HWND owner) {
    Cleanup();
    uiFont = CreateFontW(-Px(owner, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    headingFont = CreateFontW(-Px(owner, 17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    canvas = CreateSolidBrush(kCanvas);
    sidebar = CreateSolidBrush(kSidebar);
    toolbar = CreateSolidBrush(kToolbar);
}
void StyleControl(HWND control, bool tree) {
    if (!control) return;
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
    if (tree) {
        SetWindowTheme(control, L"Explorer", nullptr);
        TreeView_SetBkColor(control, kSidebar);
        TreeView_SetTextColor(control, kInk);
        TreeView_SetItemHeight(control, Px(GetParent(control), 28));
        TreeView_SetIndent(control, Px(GetParent(control), 18));
        auto style = GetWindowLongPtrW(control, GWL_STYLE);
        SetWindowLongPtrW(control, GWL_STYLE, (style & ~(TVS_HASLINES | TVS_LINESATROOT)) | TVS_FULLROWSELECT | TVS_TRACKSELECT);
    }
}
HWND Button(HWND owner, UINT id, const wchar_t* label) {
    HWND button = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, owner, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    StyleControl(button);
    return button;
}
void SetActive(HWND button, bool active) {
    if (!button) return;
    if (active) SetPropW(button, L"ReaderNextActive", reinterpret_cast<HANDLE>(1));
    else RemovePropW(button, L"ReaderNextActive");
    InvalidateRect(button, nullptr, FALSE);
}
bool DrawButton(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_BUTTON) return false;
    const bool active = GetPropW(item.hwndItem, L"ReaderNextActive") != nullptr;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    Fill(item.hDC, item.rcItem, active ? kSelection : ((item.itemState & ODS_SELECTED) ? RGB(207, 219, 228) : kToolbar));
    wchar_t label[160]{};
    GetWindowTextW(item.hwndItem, label, 160);
    Text(item.hDC, item.rcItem, label, uiFont, disabled ? RGB(150, 159, 168) : (active ? kAccent : kInk),
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (active) {
        RECT line = item.rcItem; line.top = line.bottom - 2;
        Fill(item.hDC, line, kAccent);
    }
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem; InflateRect(&focus, -3, -3); DrawFocusRect(item.hDC, &focus);
    }
    return true;
}
void DrawTab(const DRAWITEMSTRUCT& item, const std::wstring& title, bool selected) {
    RECT rect = item.rcItem;
    Fill(item.hDC, rect, selected ? kPaper : RGB(207, 217, 227));
    if (selected) { RECT line = rect; line.bottom = line.top + 3; Fill(item.hDC, line, kAccent); }
    RECT text = rect; text.left += 12; text.right -= 24;
    Text(item.hDC, text, title, uiFont, selected ? kInk : kMuted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT close = rect; close.left = close.right - 20;
    Text(item.hDC, close, L"×", uiFont, kMuted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}
void Paint(HWND owner, int split, int toolbarBottom, bool empty) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(owner, &ps);
    RECT client{}; GetClientRect(owner, &client);
    Fill(dc, client, kCanvas);
    RECT left{0, 0, split, client.bottom}; Fill(dc, left, kSidebar);
    RECT tools{split, 0, client.right, toolbarBottom}; Fill(dc, tools, kToolbar);
    RECT edge{split, 0, split + 1, client.bottom}; Fill(dc, edge, RGB(173, 188, 201));
    RECT brand{Px(owner, 18), Px(owner, 12), split - Px(owner, 12), Px(owner, 38)};
    if (split > Px(owner, 180)) Text(dc, brand, localization::Text(L"reader_next.brand"), headingFont, kInk, DT_SINGLELINE | DT_VCENTER);
    RECT rule{split + Px(owner, 12), toolbarBottom - 1, client.right - Px(owner, 12), toolbarBottom};
    Fill(dc, rule, RGB(176, 192, 205));
    if (empty) {
        const bool compact = client.bottom - toolbarBottom < Px(owner, 480);
        const int x = split + std::max(Px(owner, 40), (static_cast<int>(client.right) - split - Px(owner, 560)) / 2);
        const int y = toolbarBottom + (compact ? Px(owner, 28) : std::max(Px(owner, 50), (static_cast<int>(client.bottom) - toolbarBottom) / 5));
        const auto shortcuts = localization::Text(L"reader_next.welcome_keys");
        RECT keys{x, y, client.right - Px(owner, 32), y};
        auto oldFont = SelectObject(dc, uiFont);
        DrawTextW(dc, shortcuts.c_str(), -1, &keys, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        Text(dc, keys, shortcuts, uiFont, kInk, DT_WORDBREAK | DT_NOPREFIX);
        RECT body{x, keys.bottom + Px(owner, 18), client.right - Px(owner, 32), client.bottom - Px(owner, 24)};
        Text(dc, body, localization::Text(L"reader_next.welcome_body"), uiFont, kMuted, DT_WORDBREAK | DT_NOPREFIX);
    }
    EndPaint(owner, &ps);
}
HBRUSH ControlBrush(HWND control, HDC dc) {
    SetTextColor(dc, kMuted); SetBkMode(dc, TRANSPARENT);
    const int id = GetDlgCtrlID(control);
    return (id == 817 || id == 818) ? canvas : sidebar;
}
LRESULT DrawTree(NMTVCUSTOMDRAW& draw) {
    if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const bool selected = (draw.nmcd.uItemState & CDIS_SELECTED) != 0;
        const bool hot = (draw.nmcd.uItemState & CDIS_HOT) != 0;
        draw.clrText = selected ? kAccent : kInk;
        draw.clrTextBk = selected ? kSelection : (hot ? RGB(210, 224, 235) : kSidebar);
        return CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
}
}
