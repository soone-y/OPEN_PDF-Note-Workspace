#include "ui/core/wheel_routing.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <windowsx.h>

namespace {

HHOOK g_hWheelHook = nullptr;
HHOOK g_hCallWndHook = nullptr;
bool g_inWheelHookForward = false;
HWND g_lastWheelListTarget = nullptr;
int g_wheelScrollRemainder1000 = 0; // in "lines * 1000" units

HWND FindComboListBoxUnderCursor(POINT screenPt) {
    HWND h = WindowFromPoint(screenPt);
    while (h) {
        wchar_t cls[64]{};
        if (GetClassNameW(h, cls, static_cast<int>(std::size(cls))) > 0) {
            if (wcscmp(cls, L"ComboLBox") == 0) return h;
        }
        h = GetParent(h);
    }
    return nullptr;
}

int ListBoxVisibleCount(HWND list) {
    if (!list) return 1;
    RECT rc{};
    GetClientRect(list, &rc);
    int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    int itemH = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
    if (itemH <= 0) itemH = 16;
    return std::max(1, h / itemH);
}

void ScrollListBoxByWheel(HWND list, int delta) {
    if (!list || !IsWindow(list)) return;

    if (g_lastWheelListTarget != list) {
        g_lastWheelListTarget = list;
        g_wheelScrollRemainder1000 = 0;
    }

    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);

    if (lines == WHEEL_PAGESCROLL) {
        g_wheelScrollRemainder1000 += (static_cast<long long>(-delta) * 1000) / WHEEL_DELTA;
        int notches = g_wheelScrollRemainder1000 / 1000;
        if (notches == 0) return;
        g_wheelScrollRemainder1000 -= notches * 1000;

        int step = std::max(1, ListBoxVisibleCount(list) - 1);
        int top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
        int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
        if (top < 0 || count <= 0) return;

        int move = std::abs(notches) * step;
        int newTop = top + ((notches >= 0) ? move : -move);
        newTop = std::clamp(newTop, 0, std::max(0, count - 1));
        if (newTop != top) {
            SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(newTop), 0);
        }
        return;
    }

    int step = std::max(1, static_cast<int>(lines));
    g_wheelScrollRemainder1000 += static_cast<int>((static_cast<long long>(-delta) * step * 1000) / WHEEL_DELTA);
    int moveLines = g_wheelScrollRemainder1000 / 1000;
    if (moveLines == 0) return;
    g_wheelScrollRemainder1000 -= moveLines * 1000;

    int top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
    int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    if (top < 0 || count <= 0) return;

    int newTop = top + moveLines;
    newTop = std::clamp(newTop, 0, std::max(0, count - 1));
    if (newTop != top) {
        SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(newTop), 0);
    }
}

BOOL CALLBACK EnumChildFindDroppedComboProc(HWND hWnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<std::pair<POINT, HWND*>*>(lParam);
    if (!ctx || !ctx->second) return TRUE;

    wchar_t cls[64]{};
    if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) <= 0) return TRUE;
    if (wcscmp(cls, L"ComboBox") != 0) return TRUE;

    if (SendMessageW(hWnd, CB_GETDROPPEDSTATE, 0, 0) == 0) return TRUE;

    COMBOBOXINFO cbi{};
    cbi.cbSize = sizeof(cbi);
    if (!GetComboBoxInfo(hWnd, &cbi) || !cbi.hwndList) return TRUE;

    HWND list = cbi.hwndList; // typically class "ComboLBox"
    RECT rcList{};
    if (GetWindowRect(list, &rcList) && PtInRect(&rcList, ctx->first)) {
        *ctx->second = list;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK EnumTopFindDroppedComboProc(HWND hWnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<std::pair<POINT, HWND*>*>(lParam);
    if (!ctx || !ctx->second) return TRUE;
    if (*ctx->second) return FALSE;

    EnumChildWindows(hWnd, EnumChildFindDroppedComboProc, lParam);
    return (*ctx->second) ? FALSE : TRUE;
}

HWND FindDroppedComboListAtPoint(POINT screenPt) {
    HWND found = nullptr;
    std::pair<POINT, HWND*> ctx{ screenPt, &found };
    EnumThreadWindows(GetCurrentThreadId(), EnumTopFindDroppedComboProc, reinterpret_cast<LPARAM>(&ctx));
    return found;
}

LRESULT CALLBACK ThreadMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL)) {
        if (!g_inWheelHookForward) {
            const auto* hs = reinterpret_cast<const MOUSEHOOKSTRUCTEX*>(lParam);
            if (hs) {
                POINT pt = hs->pt;
                HWND target = FindComboListBoxUnderCursor(pt);
                if (!target) target = FindDroppedComboListAtPoint(pt);
                if (target && IsWindowVisible(target)) {
                    g_inWheelHookForward = true;
                    if (wParam == WM_MOUSEWHEEL) {
                        const short delta = static_cast<short>(HIWORD(hs->mouseData));
                        ScrollListBoxByWheel(target, static_cast<int>(delta));
                    }
                    g_inWheelHookForward = false;
                    return 1; // swallow: avoid scrolling/zooming PDF while dropdown is open
                }
            }
        }
    }
    return CallNextHookEx(g_hWheelHook, nCode, wParam, lParam);
}

LRESULT CALLBACK ThreadCallWndHookProc(int nCode, WPARAM, LPARAM lParam) {
    if (nCode == HC_ACTION && !g_inWheelHookForward) {
        auto* cwp = reinterpret_cast<CWPSTRUCT*>(lParam);
        if (cwp) {
            UINT msg = cwp->message;
            const bool isWheel = (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL);
            const bool isPointerWheel = (msg == WM_POINTERWHEEL || msg == WM_POINTERHWHEEL);
            if (isWheel || isPointerWheel) {
                POINT pt{ GET_X_LPARAM(cwp->lParam), GET_Y_LPARAM(cwp->lParam) };
                if (pt.x == 0 && pt.y == 0) {
                    GetCursorPos(&pt);
                }

                HWND target = FindComboListBoxUnderCursor(pt);
                if (!target) target = FindDroppedComboListAtPoint(pt);

                if (target && IsWindowVisible(target)) {
                    g_inWheelHookForward = true;
                    if (isWheel) {
                        if (msg == WM_MOUSEWHEEL) {
                            const short delta = static_cast<short>(HIWORD(cwp->wParam));
                            ScrollListBoxByWheel(target, static_cast<int>(delta));
                        }
                    } else {
                        const short delta = static_cast<short>(HIWORD(cwp->wParam));
                        if (msg == WM_POINTERWHEEL) {
                            ScrollListBoxByWheel(target, static_cast<int>(delta));
                        }
                    }
                    g_inWheelHookForward = false;

                    cwp->message = WM_NULL;
                    cwp->wParam = 0;
                    cwp->lParam = 0;
                }
            }
        }
    }
    return CallNextHookEx(g_hCallWndHook, nCode, 0, lParam);
}

} // namespace

bool RouteWheelToComboListIfNeeded(const MSG& msg) {
    if (msg.message != WM_MOUSEWHEEL && msg.message != WM_MOUSEHWHEEL) return false;
    POINT screenPt{ GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam) };
    if (screenPt.x == 0 && screenPt.y == 0) {
        DWORD pos = GetMessagePos();
        screenPt.x = GET_X_LPARAM(pos);
        screenPt.y = GET_Y_LPARAM(pos);
    }

    if (HWND comboList = FindComboListBoxUnderCursor(screenPt)) {
        SendMessageW(comboList, msg.message, msg.wParam, msg.lParam);
        return true;
    }

    if (HWND dropWnd = FindDroppedComboListAtPoint(screenPt)) {
        SendMessageW(dropWnd, msg.message, msg.wParam, msg.lParam);
        return true;
    }
    return false;
}

void InstallMainWheelRoutingHooks() {
    if (!g_hWheelHook) {
        g_hWheelHook = SetWindowsHookExW(WH_MOUSE, ThreadMouseHookProc, nullptr, GetCurrentThreadId());
    }
    if (!g_hCallWndHook) {
        g_hCallWndHook = SetWindowsHookExW(WH_CALLWNDPROC, ThreadCallWndHookProc, nullptr, GetCurrentThreadId());
    }
}

void UninstallMainWheelRoutingHooks() {
    if (g_hWheelHook) {
        UnhookWindowsHookEx(g_hWheelHook);
        g_hWheelHook = nullptr;
    }
    if (g_hCallWndHook) {
        UnhookWindowsHookEx(g_hCallWndHook);
        g_hCallWndHook = nullptr;
    }
}
