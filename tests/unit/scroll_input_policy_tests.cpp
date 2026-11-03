#include "ui/scroll_input_win32.h"
#include "ui/listbox_scroll.h"
#include "ui/toolbar_scroll.h"
#include "ui/combobox_guard.h"
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;
void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool Near(double a, double b) { return std::abs(a - b) < 1e-8; }

HWND toolbarControls[3]{};
const int toolbarRows[] = {0, 80, 220};
int toolbarContent = 260, toolbarPage = 100, toolbarKey = 7;
void LayoutTestToolbar(HWND host) {
    const int offset = ui::toolbar_scroll::Offset(host, toolbarContent, toolbarPage, toolbarKey);
    for (int index = 0; index < 3; ++index)
        SetWindowPos(toolbarControls[index], nullptr, 4, toolbarRows[index] - offset, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
RECT ToolbarControlBounds(HWND control, HWND host) {
    RECT bounds{};
    Expect(GetWindowRect(control, &bounds) != FALSE, "toolbar option bounds readable");
    MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}
void CheckNativeToolbar() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
        nullptr, nullptr, nullptr, nullptr);
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VSCROLL, 0, 0, 180, toolbarPage,
        parent, nullptr, nullptr, nullptr);
    Expect(parent && host && ui::toolbar_scroll::Install(host, LayoutTestToolbar) &&
        ui::toolbar_scroll::Install(host, LayoutTestToolbar), "toolbar host installs idempotently");
    toolbarControls[0] = CreateWindowExW(0, L"BUTTON", L"Tool", WS_CHILD | WS_VISIBLE,
        4, 0, 120, 24, host, nullptr, nullptr, nullptr);
    toolbarControls[1] = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        4, 80, 120, 80, host, nullptr, nullptr, nullptr);
    toolbarControls[2] = CreateWindowExW(0, L"BUTTON", L"Last option", WS_CHILD | WS_VISIBLE,
        4, 220, 120, 24, host, nullptr, nullptr, nullptr);
    for (const auto* label : {L"First", L"Second"})
        SendMessageW(toolbarControls[1], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(toolbarControls[1], CB_SETCURSEL, 1, 0);
    ui::GuardComboAgainstAccidentalChange(toolbarControls[1], parent);
    for (HWND control : toolbarControls)
        Expect(ui::toolbar_scroll::InstallChild(control, host), "toolbar option installs scroll/focus routing");
    ui::toolbar_scroll::SetRange(host, toolbarContent, toolbarPage, toolbarKey);
    LayoutTestToolbar(host);
    Expect((GetWindowLongPtrW(host, GWL_STYLE) & WS_VSCROLL) != 0,
        "overflowing toolbar exposes a vertical scrollbar");
    Expect((GetWindowLongPtrW(toolbarControls[2], GWL_STYLE) & WS_VISIBLE) != 0,
        "lower options stay available rather than being hidden");
    SendMessageW(host, WM_VSCROLL, SB_BOTTOM, 0);
    Expect(GetScrollPos(host, SB_VERT) == 160, "toolbar scrollbar reaches final option");
    auto bounds = ToolbarControlBounds(toolbarControls[2], host);
    Expect(bounds.top >= 0 && bounds.bottom <= toolbarPage, "last toolbar option fits inside viewport after scrolling");
    SendMessageW(host, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(toolbarControls[2], WM_SETFOCUS, 0, 0);
    bounds = ToolbarControlBounds(toolbarControls[2], host);
    Expect(GetScrollPos(host, SB_VERT) == 144 && bounds.bottom == toolbarPage,
        "keyboard focus reveals an offscreen option without taking real focus");
    SendMessageW(toolbarControls[0], WM_SETFOCUS, 0, 0);
    Expect(GetScrollPos(host, SB_VERT) == 0, "keyboard focus can return to the first toolbar row");
    SendMessageW(host, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(toolbarControls[1], WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0);
    const int grouped = GetScrollPos(host, SB_VERT);
    SendMessageW(host, WM_VSCROLL, SB_BOTTOM, 0);
    for (int packet = 0; packet < 4; ++packet)
        SendMessageW(toolbarControls[1], WM_MOUSEWHEEL, MAKEWPARAM(0, 30), 0);
    Expect(GetScrollPos(host, SB_VERT) == grouped, "split and grouped toolbar wheel input preserve distance");
    Expect(SendMessageW(toolbarControls[1], CB_GETCURSEL, 0, 0) == 1,
        "closed toolbar dropdown scrolls the host and retains its option");
    UINT lines = 3;
    Expect(SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0) != FALSE, "toolbar wheel setting readable");
    Expect(lines == 0 ? grouped == 160 : grouped < 160, "closed option wheel reaches toolbar before its PDF guard");
    SendMessageW(host, WM_VSCROLL, SB_BOTTOM, 0);
    ++toolbarKey;
    Expect(ui::toolbar_scroll::Offset(host, toolbarContent, toolbarPage, toolbarKey) == 0,
        "switching tools starts at the first option row");
    ui::toolbar_scroll::SetRange(host, toolbarContent, toolbarPage, toolbarKey);
    Expect(GetScrollPos(host, SB_VERT) == 0, "new tool publishes the reset scroll position");
    SendMessageW(host, WM_VSCROLL, SB_BOTTOM, 0);
    toolbarContent = 60;
    ui::toolbar_scroll::SetRange(host, toolbarContent, toolbarPage, toolbarKey);
    LayoutTestToolbar(host);
    Expect(GetScrollPos(host, SB_VERT) == 0 && !(GetWindowLongPtrW(host, GWL_STYLE) & WS_VSCROLL),
        "fitting content removes the scrollbar and stale scroll offset");
    toolbarContent = 260;
    ui::toolbar_scroll::SetRange(host, toolbarContent, toolbarPage, toolbarKey);
    LayoutTestToolbar(host);
    Expect((GetWindowLongPtrW(host, GWL_STYLE) & WS_VSCROLL) != 0 && GetScrollPos(host, SB_VERT) == 0,
        "narrowing a previously fitting toolbar restores its scrollbar");
    Expect(DestroyWindow(parent) != FALSE, "toolbar host and child subclasses clean up with their windows");
}

void CheckNativeLists() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
        nullptr, nullptr, nullptr, nullptr);
    Expect(parent != nullptr, "owned native parent created");
    HWND lists[2]{};
    UINT rows = 3;
    Expect(SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &rows, 0) != FALSE, "system list wheel preference readable");
    for (auto& list : lists) {
        list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            0, 0, 240, 180, parent, nullptr, nullptr, nullptr);
        Expect(list && ui::InstallListBoxScroll(list) && ui::InstallListBoxScroll(list), "scroll subclass installs idempotently");
        for (int n = 0; n < 1000; ++n) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"owned row"));
        SendMessageW(list, LB_SETCURSEL, 10, 0);
        SendMessageW(list, LB_SETTOPINDEX, 100, 0);
    }
    if (rows == WHEEL_PAGESCROLL) {
        RECT client{}; Expect(GetClientRect(lists[0], &client) != FALSE, "page height readable");
        const auto height = SendMessageW(lists[0], LB_GETITEMHEIGHT, 0, 0);
        rows = static_cast<UINT>(std::max<LRESULT>(1, client.bottom / std::max<LRESULT>(1, height) - 1));
    }
    Expect(rows < 100, "native fixture has room for the configured wheel movement");
    for (int packet = 0; packet < 120; ++packet) {
        for (const auto list : lists) {
            SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-1)), 0);
            Expect(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == 100 + (packet + 1) * rows / 120,
                "native panes retain only their own fractional movement");
            Expect(SendMessageW(list, LB_GETCURSEL, 0, 0) == 10, "wheel leaves native selection unchanged");
        }
    }
    for (const auto list : lists) {
        SendMessageW(list, LB_SETTOPINDEX, 100, 0);
        SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-120)), 0);
        Expect(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == 100 + rows, "native grouped and split distance agree");
        SendMessageW(list, LB_SETTOPINDEX, 0, 0);
        SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(1)), 0);
        SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-120)), 0);
        Expect(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == rows, "top edge cannot buffer motion against a reversal");
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-120)), 0);
        Expect(SendMessageW(list, LB_GETCOUNT, 0, 0) == 0, "empty native list consumes wheel safely");
    }
    Expect(DestroyWindow(parent) != FALSE, "native scroll states destroyed with their windows");
}
}

int main() {
    using scroll_input::WheelInput;
    using scroll_input::PixelRemainder;
    WheelInput wheel;
    Expect(!wheel.IsPrecise(120, 0), "ordinary mouse stays ordinary");
    Expect(!wheel.IsPrecise(240, 10), "mouse packet grouping does not mark it precise");
    Expect(wheel.IsPrecise(1, 20), "fractional input begins precision gesture");
    Expect(wheel.IsPrecise(120, 30), "120 in a precision gesture keeps its classification");
    Expect(wheel.IsPrecise(240, 40), "grouped precision input cannot acquire mouse acceleration");
    Expect(wheel.IsPrecise(-120, 50), "reversal keeps precision classification");
    Expect(!wheel.IsPrecise(120, 301), "gesture expires across idle boundary");
    Expect(wheel.IsPrecise(120, 600, true), "known touchpad is precise even on its first full delta");
    wheel.Reset();
    Expect(!wheel.IsPrecise(120, 601), "reset prevents a new document inheriting precision state");
    Expect(wheel.IsPrecise(1, 0xfffffff0u), "precision begins before timestamp wrap");
    Expect(wheel.IsPrecise(120, 20), "timestamp wrap retains recent gesture");

    wheel.Reset();
    Expect(Near(wheel.Filter(false, 10, 0, true), 10), "initial movement is immediate");
    Expect(Near(wheel.Filter(false, -0.5, 1, true), 0), "tiny reversal is buffered");
    Expect(Near(wheel.Filter(false, 5, 2, true), 5), "original direction discards reversal jitter");
    Expect(Near(wheel.Filter(false, -0.5, 3, true), 0), "intentional slow reversal begins buffered");
    Expect(Near(wheel.Filter(false, -0.5, 4, true), 0), "slow reversal accumulates");
    Expect(Near(wheel.Filter(false, -1, 5, true), -2), "deliberate reversal releases its full distance");
    Expect(Near(wheel.Filter(false, -1, 6, true), -1), "new direction now follows immediately");
    Expect(Near(wheel.Filter(false, 8, 7, true), 8), "large reversal is immediate");
    Expect(Near(wheel.Filter(true, -0.25, 7, true), -0.25), "horizontal axis is independent");
    Expect(Near(wheel.Filter(false, -0.25, 158, true), -0.25), "first movement after idle is immediate");
    Expect(Near(wheel.Filter(false, 0.1, 159, false), 0.1), "OFF bypasses reversal suppression");
    Expect(Near(wheel.Filter(false, -0.1, 160, false), -0.1), "OFF follows both directions exactly");
    Expect(Near(wheel.Filter(false, 0.1, 161, true), 0.1), "reenabling cannot replay old pending movement");
    wheel.Reset();
    Expect(Near(wheel.Filter(false, -0.1, 162, true), -0.1), "reset clears direction and pending movement");
    Expect(Near(wheel.Filter(false, std::numeric_limits<double>::infinity(), 163, true), 0), "invalid input rejected");
    Expect(Near(wheel.Filter(false, 0.1, 164, true), 0.1), "invalid input cannot poison the next movement");

    // Same signed distance is retained regardless of packet partition. These
    // are actual subpixel values used by note horizontal scrolling (40/120).
    for (int sign : {-1, 1}) {
        PixelRemainder pixels;
        int total = 0;
        for (int i = 0; i < 120; ++i) total += pixels.Take(sign * 40.0 / 120.0);
        Expect(total == sign * 40, "unit wheel packets preserve fractional distance");
        Expect(pixels.Take(sign * 40.0) == total, "grouped and split packets have equal movement");
        pixels.Reset();
        total = 0;
        for (int delta : {1, 7, 32, 80}) total += pixels.Take(sign * delta * 40.0 / 120.0);
        Expect(total == sign * 40, "irregular wheel packets preserve fractional distance");
    }
    PixelRemainder first;
    PixelRemainder second;
    Expect(first.Take(0.75) == 0 && second.Take(0.75) == 0, "views cannot borrow each other's fractional input");
    Expect(first.Take(0.25) == 1, "remainder belongs to its original view");
    second.Reset();
    Expect(second.Take(0.25) == 0, "document change clears old fractional pixels");
    Expect(first.Take(std::numeric_limits<double>::quiet_NaN()) == 0, "invalid fractional input is discarded");
    Expect(first.Take(1e20) == std::numeric_limits<int>::max(), "large input cannot overflow integer coordinates");
    for (int sign : {-1, 1}) {
        scroll_input::ListWheelInput split, grouped;
        int distance = 0;
        for (int n = 0; n < 120; ++n) distance += split.Take(sign, 4);
        Expect(distance == -sign * 4 && distance == grouped.Take(sign * 120, 4),
            "list movement preserves exact distance for unit and grouped packets");
    }
    scroll_input::ListWheelInput listA, listB;
    Expect(listA.Take(-15, 4) == 0 && listB.Take(-15, 4) == 0,
        "lists do not share native fractional rows");
    Expect(listA.Take(-15, 4) == 1 && listB.Take(-15, 4) == 1,
        "each list releases only its own accumulated row");
    Expect(listA.Take(-15, 4) == 0 && listA.Take(-15, 4) == 1,
        "slow input keeps its fractional distance without a timing threshold");
    Expect(listA.Take(15, 4) == 0 && listA.Take(15, 4) == -1,
        "reversal preserves signed distance");
    Expect(listA.Take(-15, 4) == 0 && listA.Take(-15, 0) == 0,
        "system zero-line preference prevents list scrolling");
    Expect(listA.Take(-15, 8) == 1, "setting change starts with clean fractional state");
    listA.Reset();
    Expect(listA.Take(-15, 8) == 1 && listA.Take(-15, 8) == 1,
        "consecutive grouped list inputs preserve distance");
    Expect(listA.Take(std::numeric_limits<int>::min(), std::numeric_limits<uint32_t>::max()) ==
        std::numeric_limits<int>::max(), "large list input saturates without overflow");
    CheckNativeLists();
    CheckNativeToolbar();
    std::cout << "Scroll input policy: " << checks << " checks passed\n";
}
