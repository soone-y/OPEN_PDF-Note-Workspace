#include "ui/empty_view_guide.h"
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;
void Expect(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
SCROLLINFO Info(HWND window) {
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
    Expect(GetScrollInfo(window, SB_VERT, &info) != 0, "guide scrollbar is observable");
    return info;
}
}
int main() {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = L"EmptyGuideUnitTest"; cls.lpfnWndProc = DefWindowProcW;
    Expect(RegisterClassW(&cls) != 0, "register owned hidden test class");
    HWND owner = CreateWindowExW(0, cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                 0, 0, 960, 600, nullptr, nullptr, instance, nullptr);
    Expect(owner != nullptr && !IsWindowVisible(owner), "test window remains hidden");
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bitmapInfo{}; bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = 1000; bitmapInfo.bmiHeader.biHeight = -700;
    bitmapInfo.bmiHeader.biPlanes = 1; bitmapInfo.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Expect(dc && bitmap && pixels, "allocate owned drawing surface");
    const auto old = SelectObject(dc, bitmap);
    empty_view_guide::Content content;
    content.title = L"Guide"; content.usage = L"1. Open\n2. Annotate\n3. Save";
    content.generalTitle = L"Common actions"; content.annotationTitle = L"Current bindings";
    content.scrollHint = L"Scroll for more";
    content.displayHint = L"Change this guide to Shortcuts only or Hidden under Settings / Environment / Window and shared controls.";
    for (int i = 0; i < 40; ++i) content.annotation.push_back(L"Ctrl+Alt+" + std::to_wstring(i) + L"  —  Long tool description that wraps in a narrow pane");
    content.general.push_back(L"Ctrl+S — Save");
    ThemeColors theme;
    for (int fontSize : {12, 18, 24}) {
        HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        Expect(font != nullptr, "create test font");
        int wideMaximum = 0;
        for (int width : {940, 360}) {
            empty_view_guide::Paint(owner, dc, RECT{0, 0, width, 420}, content, font, theme);
            auto info = Info(owner);
            Expect(info.nMax >= static_cast<int>(info.nPage), "long guide can be scrolled at every font size");
            if (width == 940) wideMaximum = info.nMax;
            else Expect(info.nMax > wideMaximum, "narrow guide wraps without discarding rows");
            Expect(empty_view_guide::Scroll(owner, WM_KEYDOWN, VK_HOME), "Home handled");
            Expect(Info(owner).nPos == 0, "Home is clamped at zero");
            Expect(empty_view_guide::Scroll(owner, WM_KEYDOWN, VK_END), "End handled");
            info = Info(owner);
            Expect(info.nPos == info.nMax-static_cast<int>(info.nPage)+1, "End reaches last row");
            const int before = info.nPos;
            for (int i = 0; i < 3; ++i) Expect(empty_view_guide::Scroll(owner, WM_MOUSEWHEEL, MAKEWPARAM(0, 40)), "split wheel handled");
            const int splitPosition = Info(owner).nPos;
            Expect(empty_view_guide::Scroll(owner, WM_KEYDOWN, VK_END), "restore wheel origin");
            Expect(empty_view_guide::Scroll(owner, WM_MOUSEWHEEL, MAKEWPARAM(0, 120)), "whole wheel handled");
            Expect(Info(owner).nPos == splitPosition && before-splitPosition == 120, "split and whole wheel travel agree");
            Expect(empty_view_guide::Scroll(owner, WM_MOUSEWHEEL, MAKEWPARAM(0, 1)), "one-unit input handled");
            Expect(Info(owner).nPos == splitPosition-1, "one-unit touchpad input is retained");
            Expect(empty_view_guide::Scroll(owner, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 120)), "Ctrl wheel stays local");
            Expect(Info(owner).nPos == splitPosition-1, "Ctrl wheel does not scroll the guide");
            wchar_t text[8192]{}; GetWindowTextW(owner, text, 8192);
            Expect(std::wstring(text) == empty_view_guide::PlainText(content) && std::wstring(text).find(L"Ctrl+Alt+39") != std::wstring::npos,
                "accessible content includes every final binding");
            Expect(std::wstring(text).find(content.displayHint) != std::wstring::npos,
                "display-settings explanation is preserved for every font, width and scroll position");
            const int position = Info(owner).nPos;
            empty_view_guide::Clear(owner, false);
            Expect(GetWindowTextLengthW(owner) == 0 && Info(owner).nPos == position, "document transition clears guide text without resetting PDF scroll state");
        }
        DeleteObject(font);
    }
    empty_view_guide::Clear(owner);
    Expect(GetWindowTextLengthW(owner) == 0, "hidden mode clears accessible text");
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); DestroyWindow(owner);
    std::cout << "Empty view guide: " << checks << " checks passed\n";
}
