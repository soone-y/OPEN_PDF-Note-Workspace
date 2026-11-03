#include "ui/themed_push_buttons.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks = 0, draws = 0, customDraws = 0, clicks = 0;
UINT lastState = 0;
void Require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool Draw(const DRAWITEMSTRUCT* item) {
    ++draws;
    lastState = item->itemState;
    SetDCBrushColor(item->hDC, RGB(41, 92, 143));
    FillRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetTextColor(item->hDC, RGB(255, 255, 255));
    SetBkMode(item->hDC, TRANSPARENT);
    wchar_t label[128]{};
    GetWindowTextW(item->hwndItem, label, 128);
    RECT text = item->rcItem;
    text.left += 6;
    DrawTextW(item->hDC, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return true;
}
LRESULT CALLBACK Parent(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_COMMAND && HIWORD(wp) == BN_CLICKED) { ++clicks; return 0; }
    if (message == WM_DRAWITEM) {
        // Deliberately no generic-button handler: this was the missing-parent
        // failure. A dedicated owner-drawn swatch retains its own handler.
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlID == 99) { ++customDraws; return TRUE; }
    }
    return DefWindowProcW(window, message, wp, lp);
}
HWND Make(HWND parent, DWORD type, int id) {
    HWND button = CreateWindowExW(0, L"BUTTON", L"Save and close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | type,
        0, 0, 160, 30, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    Require(button != nullptr, "create button");
    return button;
}
LONG_PTR Type(HWND button) { return GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK; }
void Print(HWND button, bool managed) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), 160, -30, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(dc && bitmap && bits, "create print target");
    HGDIOBJ old = SelectObject(dc, bitmap);
    const unsigned before = draws;
    SendMessageW(button, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
    Require(draws == before + (managed ? 1 : 0), "native print routes exactly once");
    if (managed) {
        Require(GetPixel(dc, 2, 27) == RGB(41, 92, 143), "renderer paints caller's target");
        unsigned textPixels = 0;
        for (int y = 0; y < 30; ++y) for (int x = 6; x < 155; ++x)
            if (GetPixel(dc, x, y) == RGB(255, 255, 255)) ++textPixels;
        Require(textPixels > 10, "button label reaches print target");
    }
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}
bool HostAttached(HWND parent) {
    return GetPropW(parent, ui::themed_push_buttons::detail::kParentState) != nullptr;
}
}

int main() {
    using ui::themed_push_buttons::Apply;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "initialize controls");
        WNDCLASSW wc{}; wc.lpfnWndProc = Parent; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ThemePushButtonContractTest";
        Require(RegisterClassW(&wc) != 0, "register hidden fixture");
        HWND root = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 400, 300,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        Require(root != nullptr && !IsWindowVisible(root), "fixture stays hidden");
        HWND nested = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD, 0, 0, 200, 100,
                                      root, nullptr, wc.hInstance, nullptr);
        Require(nested != nullptr, "create nested parent without drawing handler");
        HWND normal = Make(root, BS_PUSHBUTTON, 1);
        HWND primary = Make(root, BS_DEFPUSHBUTTON, 2);
        HWND nestedButton = Make(nested, BS_PUSHBUTTON | BS_MULTILINE, 3);
        const std::vector<DWORD> nativeTypes{BS_AUTOCHECKBOX, BS_CHECKBOX, BS_AUTO3STATE,
                                            BS_RADIOBUTTON, BS_AUTORADIOBUTTON, BS_GROUPBOX, BS_OWNERDRAW};
        std::vector<HWND> native;
        for (DWORD type : nativeTypes) native.push_back(Make(root, type, type == BS_OWNERDRAW ? 99 : 10 + static_cast<int>(type)));
        SendMessageW(native[0], BM_SETCHECK, BST_CHECKED, 0);
        Require(Apply(root, true, Draw), "apply theme");
        for (HWND button : {normal, primary, nestedButton}) Require(Type(button) == BS_OWNERDRAW, "ordinary/default/nested button converted");
        for (size_t i = 0; i < native.size(); ++i) Require(Type(native[i]) == nativeTypes[i], "dedicated and toggle types retained");
        Require(SendMessageW(native[0], BM_GETCHECK, 0, 0) == BST_CHECKED, "checkbox state retained");
        Print(normal, true); Print(primary, true); Print(nestedButton, true);
        Print(native.back(), false); Require(customDraws == 1, "dedicated drawing stays with parent");
        Require((SendMessageW(primary, WM_GETDLGCODE, 0, 0) & DLGC_DEFPUSHBUTTON) != 0, "default keyboard role retained");
        SendMessageW(primary, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        Require(Type(primary) == BS_OWNERDRAW, "focus-style change keeps themed drawing");
        Require((SendMessageW(primary, WM_GETDLGCODE, 0, 0) & DLGC_UNDEFPUSHBUTTON) != 0, "default role can change");
        SendMessageW(primary, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        SendMessageW(normal, BM_SETSTATE, TRUE, 0); Print(normal, true);
        Require((lastState & ODS_SELECTED) != 0, "pressed state reaches renderer");
        SendMessageW(normal, BM_SETSTATE, FALSE, 0);
        EnableWindow(normal, FALSE); Print(normal, true);
        Require((lastState & ODS_DISABLED) != 0, "disabled state reaches renderer");
        EnableWindow(normal, TRUE); SendMessageW(normal, BM_CLICK, 0, 0);
        Require(clicks == 1, "native click command retained");
        HWND added = Make(nested, BS_PUSHBUTTON, 70);
        Require(Apply(root, true, Draw), "reapply discovers new buttons without registration lists");
        Print(added, true);
        Require(Apply(root, false, Draw), "restore native controls");
        Require(Type(normal) == BS_PUSHBUTTON && Type(primary) == BS_DEFPUSHBUTTON, "native button roles restored");
        Require((GetWindowLongPtrW(nestedButton, GWL_STYLE) & BS_MULTILINE) != 0, "other style bits preserved");
        Require(!HostAttached(root) && !HostAttached(nested), "last restore removes parent hooks");
        for (int i = 0; i < 20; ++i) {
            Require(Apply(root, true, Draw) && Apply(root, true, Draw), "repeated apply is idempotent");
            Require(Apply(root, false, Draw), "repeated restore succeeds");
            Require(!HostAttached(root) && !HostAttached(nested), "cycle releases parent hooks");
        }
        Require(Apply(root, true, Draw), "apply before child destruction");
        Require(DestroyWindow(nested) != FALSE && !HostAttached(nested), "nested destruction releases hooks");
        Require(DestroyWindow(root) != FALSE, "parent destruction releases remaining buttons");
        Require(!Apply(nullptr, true, Draw), "invalid root fails");
        std::cout << "Themed push buttons PASS: " << checks << " checks; hidden native controls, print routing, dedicated drawing, keyboard roles and lifecycle.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
