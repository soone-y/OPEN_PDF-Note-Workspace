#include "ui/transient_display.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    assert(ui::TransientDisplayDurationMs(L"") == 2000);
    assert(ui::TransientDisplayDurationMs(L"A") == 2060);
    assert(ui::TransientDisplayDurationMs(L"A \r\n\tB") == 2120);
    const std::wstring supplementary{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE42)};
    assert(ui::TransientDisplayDurationMs(supplementary) == 2060);
    assert(ui::TransientDisplayDurationMs(std::wstring(50, L'a')) == 5000);
    assert(ui::TransientDisplayDurationMs(std::wstring(133, L'a')) == 9980);
    assert(ui::TransientDisplayDurationMs(std::wstring(134, L'a')) == 10000);
    assert(ui::TransientDisplayDurationMs(std::wstring(100000, L'a')) == 10000);

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    assert(InitCommonControlsEx(&controls));
    HWND owner = CreateWindowW(L"STATIC", L"Transient test owner", WS_POPUP,
                              0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(owner);
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(tooltip);
    wchar_t tipText[] = L"C:\\folder\\file.pdf";
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFOW_V2_SIZE;
    tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = owner;
    tool.uId = 1;
    tool.lpszText = tipText;
    assert(SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)));
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 300);
    const std::wstring shortPath = L"C:\\folder\\file.pdf";
    const std::wstring longPath = L"C:\\" + std::wstring(200, L'a') + L"\\file.pdf";
    for (const auto& text : {shortPath, longPath}) {
        assert(ui::ConfigureTransientTooltip(tooltip, text));
        assert(SendMessageW(tooltip, TTM_GETDELAYTIME, TTDT_INITIAL, 0) == 400);
        assert(SendMessageW(tooltip, TTM_GETDELAYTIME, TTDT_AUTOPOP, 0) == ui::TransientDisplayDurationMs(text));
        assert(GetWindowLongPtrW(tooltip, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
    }
    const auto priorFocus = GetFocus();
    assert(SendMessageW(tooltip, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);
    assert(SendMessageW(tooltip, WM_NCHITTEST, 0, 0) == HTCLIENT);
    for (const auto& button : {std::pair<UINT, UINT>{WM_LBUTTONDOWN, WM_LBUTTONUP},
                              {WM_RBUTTONDOWN, WM_RBUTTONUP}, {WM_MBUTTONDOWN, WM_MBUTTONUP}}) {
        SendMessageW(tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
        assert(IsWindowVisible(tooltip));
        SendMessageW(tooltip, button.first, 0, 0);
        SendMessageW(tooltip, button.second, 0, 0);
        assert(!IsWindowVisible(tooltip));
        assert(GetCapture() != tooltip && GetFocus() == priorFocus);
    }
    DestroyWindow(tooltip); DestroyWindow(owner);
    std::cout << "Transient display tests passed: Unicode length, whitespace, 10-second cap, tooltip updates and click dismissal.\n";
}
