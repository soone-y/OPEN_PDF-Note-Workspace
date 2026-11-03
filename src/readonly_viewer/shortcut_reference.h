#pragma once
#include <algorithm>
#include "ui/application_shortcuts.h"
#include "ui/window_corners.h"
#include "core/localization.h"
#include "readonly_viewer/reader_chrome.h"

namespace readonly_viewer::shortcut_reference {
struct State { std::wstring text; };
inline LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CREATE) {
        auto* state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        const auto module = GetModuleHandleW(nullptr);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
            0, 0, 1, 1, window, reinterpret_cast<HMENU>(101), module, nullptr);
        HWND close = CreateWindowExW(0, L"BUTTON", localization::Text(L"common.close").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            0, 0, 1, 1, window, reinterpret_cast<HMENU>(IDCANCEL), module, nullptr);
        if (!edit || !close) return -1;
        reader_next::StyleControl(edit); reader_next::StyleControl(close);
        (void)ui::ApplyAuxiliaryWindowCorners(window);
        return 0;
    }
    if (message == WM_SIZE) {
        RECT rect{}; GetClientRect(window, &rect);
        const int pad = reader_next::Px(window, 12), row = reader_next::Px(window, 30);
        MoveWindow(GetDlgItem(window, 101), pad, pad, std::max(1L, rect.right - pad * 2), std::max(1L, rect.bottom - pad * 3 - row), TRUE);
        MoveWindow(GetDlgItem(window, IDCANCEL), std::max(pad, static_cast<int>(rect.right) - pad - row * 4),
                   std::max(pad, static_cast<int>(rect.bottom) - pad - row), row * 4, row, TRUE);
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        limits->ptMinTrackSize = {reader_next::Px(window, 360), reader_next::Px(window, 200)};
        return 0;
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORBTN)
        return reinterpret_cast<LRESULT>(reader_next::ControlBrush(reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp)));
    if (message == WM_DRAWITEM && reader_next::DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
    if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK))) {
        DestroyWindow(window); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
[[nodiscard]] inline bool Show(HWND owner) {
    State state;
    for (const auto& binding : application_shortcuts::bindings)
        if (application_shortcuts::Supports(binding, application_shortcuts::Surface::Reader))
            state.text += std::wstring(binding.chord) + L"  —  " + localization::Text(binding.readerLabel) + L"\r\n";
    state.text += L"\r\n" + localization::Text(L"shortcut.reader.context") + L"\r\n\r\n" + localization::Text(L"shortcut.reader.existing");
    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ReadonlyShortcutReference";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    const HWND previousFocus = GetFocus();
    MONITORINFO monitor{sizeof(MONITORINFO)};
    int width = reader_next::Px(owner, 680), height = reader_next::Px(owner, 560);
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    }
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName,
        localization::Text(L"shortcut.reference.title").c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, module, &state);
    if (!window) return false;
    EnableWindow(owner, FALSE); ShowWindow(window, SW_SHOW);
    MSG message{};
    bool success = true;
    while (IsWindow(window)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            else success = false;
            break;
        }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window);
    EnableWindow(owner, TRUE); SetActiveWindow(owner);
    if (IsWindow(previousFocus)) SetFocus(previousFocus);
    return success;
}
} // namespace readonly_viewer::shortcut_reference
