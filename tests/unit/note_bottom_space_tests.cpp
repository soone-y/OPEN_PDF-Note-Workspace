#include "note/note_bottom_space.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <richedit.h>
#include <commctrl.h>
#include "ui/bottom_space_frame.h"
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;
void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct NativeFrameCase {
    ui::BottomSpaceFrame frame;
    int fixedDip = 96;
};
LRESULT CALLBACK NativeFrameProc(HWND window, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<NativeFrameCase*>(data);
    if (msg == WM_NCCALCSIZE && lp) {
        const LRESULT result = DefSubclassProc(window, msg, wp, lp);
        RECT& client = wp ? reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0]
            : *reinterpret_cast<RECT*>(lp);
        state.frame.Apply(client, note::BottomSpacePx(0, state.fixedDip, 400, 25,
            96, 16, client.bottom - client.top));
        return state.frame.padding_px > 0 ? 0 : result;
    }
    return DefSubclassProc(window, msg, wp, lp);
}
void CheckNativeRawSpace() {
    const HWND foreground = GetForegroundWindow();
    const HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
    Expect(richEdit != nullptr, "native RichEdit available");
    const HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 600, 400, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    Expect(owner && edit, "hidden native fixture created");
    HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH, L"Consolas");
    Expect(font != nullptr, "native fixture font created");
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    std::wstring text;
    for (int i = 0; i < 100; ++i) text += L"line\r\n";
    text += L"last";
    Expect(SetWindowTextW(edit, text.c_str()) != FALSE, "native fixture text set");
    const auto readText = [edit] {
        std::wstring value(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
        const int length = GetWindowTextW(edit, value.data(), static_cast<int>(value.size()));
        value.resize(static_cast<size_t>(length));
        return value;
    };
    const auto source = readText();
    SendMessageW(edit, EM_SETSEL, 17, 23);
    NativeFrameCase state;
    Expect(SetWindowSubclass(edit, NativeFrameProc, 1, reinterpret_cast<DWORD_PTR>(&state)) != FALSE,
        "native frame subclass installed");
    SetWindowPos(edit, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT format{};
    GetClientRect(edit, &format);
    Expect(format.bottom == 304 && state.frame.available_height_px == 400, "native client reserves actual footer");
    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
    SendMessageW(edit, EM_REQUESTRESIZE, 0, 0);
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    Expect(start == 17 && end == 23, "native format change preserves selection");
    Expect(readText() == source, "native format change preserves text");
    RECT applied{};
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&applied));
    Expect(applied.bottom == format.bottom, "native raw footer reserved");
    const int textLength = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, textLength, textLength);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    // Hidden controls need an explicit scroll; no foreground/focus is taken.
    SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
    CHARRANGE caret{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&caret));
    POINTL point{};
    SendMessageW(edit, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&point), caret.cpMin);
    if (point.y < applied.top || point.y >= applied.bottom) {
        std::cerr << "native caret y=" << point.y << " format=" << applied.top << ',' << applied.bottom
                  << " offset=" << caret.cpMin << '\n';
    }
    Expect(point.y >= applied.top && point.y < applied.bottom, "native end caret stays above footer");
    SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
    POINT before{}, after{};
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&before));
    SendMessageW(edit, WM_VSCROLL, SB_LINEDOWN, 0);
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&after));
    Expect(before.y > 0 && before.y == after.y, "native lower scroll limit stable");
    state.fixedDip = 0;
    SetWindowPos(edit, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT restored{};
    GetClientRect(edit, &restored);
    Expect(restored.bottom == 400 && state.frame.padding_px == 0, "disabling restores full native client");
    Expect(readText() == source, "native resize retains source");
    Expect(GetForegroundWindow() == foreground && !IsWindowVisible(owner), "native check stays in background");
    DestroyWindow(owner);
    DeleteObject(font);
    FreeLibrary(richEdit);
}
}

int main() {
    using note::BottomSpacePx;
    Expect(BottomSpacePx(0, 32, 400, 25, 96, 16, 600) == 32, "fixed default");
    Expect(BottomSpacePx(0, 32, 400, 25, 144, 24, 600) == 48, "fixed follows DPI");
    Expect(BottomSpacePx(1, 96, 400, 25, 96, 16, 600) == 64, "font ratio");
    Expect(BottomSpacePx(1, 96, 400, 25, 144, 24, 600) == 96, "font ratio follows font pixels");
    Expect(BottomSpacePx(2, 96, 400, 25, 96, 16, 600) == 150, "viewport ratio");
    Expect(BottomSpacePx(2, 96, 400, 25, 96, 16, 1000) == 250, "viewport resize");
    Expect(BottomSpacePx(0, 0, 400, 25, 96, 16, 600) == 0, "fixed disabled");
    Expect(BottomSpacePx(1, 96, 0, 25, 96, 16, 600) == 0, "font disabled");
    Expect(BottomSpacePx(2, 96, 400, 0, 96, 16, 600) == 0, "viewport disabled");
    Expect(BottomSpacePx(0, 2000, 400, 25, 96, 16, 60) == 44, "short pane retains a line");
    Expect(BottomSpacePx(0, 96, 400, 25, 96, 16, 0) == 0, "hidden pane");
    Expect(BottomSpacePx(2, 96, 400, 90, 96, 16, 10) == 0, "smaller than a line");
    Expect(BottomSpacePx(0, -5, 400, 25, 96, 16, 600) == 0, "negative input bounded");
    Expect(BottomSpacePx(2, 96, 400, 999, 96, 16, 600) == 540, "viewport ratio bounded");
    Expect(note::BottomSpaceExtent(1000, 96) == 1096, "same range for scrollbar and wheel");
    Expect(note::BottomSpaceExtent(0, 96) == 96, "empty document extent");
    Expect(note::BottomSpaceExtent(UINT64_MAX, 96) == 2147483647, "extent saturation");
    Expect(note::BottomSpaceExtent(2147483640, 96) == 2147483647, "addition saturation");
    Expect(note::BottomSpaceExtent(1000, -10) == 1000, "invalid negative padding");
    // The final legal origin leaves the requested padding below the last line.
    Expect(note::BottomSpaceExtent(1000, 96) - 600 == 496, "scroll to trailing area");
    using note::RevealCaretWithBottomSpace;
    Expect(RevealCaretWithBottomSpace(400, 940, 960, 600, 32) == 400, "visible caret retains origin");
    Expect(RevealCaretWithBottomSpace(400, 950, 970, 600, 32) == 402, "caret avoids bottom margin");
    uint64_t origin = 402;
    for (uint64_t bottom = 990; bottom <= 1090; bottom += 20) {
        origin = RevealCaretWithBottomSpace(origin, bottom - 20, bottom, 600, 32);
        Expect(origin + 600 - bottom == 32, "successive newlines preserve margin");
    }
    Expect(RevealCaretWithBottomSpace(origin, 1070, 1090, 600, 32) == origin, "repeated geometry stays stable");
    Expect(RevealCaretWithBottomSpace(400, 900, 920, 500, 32) == 452, "shrinking viewport restores margin");
    Expect(RevealCaretWithBottomSpace(452, 900, 920, 500, 96) == 516, "larger preference restores margin");
    Expect(RevealCaretWithBottomSpace(400, 380, 400, 600, 32) == 380, "upward navigation remains visible");
    Expect(RevealCaretWithBottomSpace(400, 980, 1000, 600, 0) == 400, "zero margin uses full height");
    Expect(RevealCaretWithBottomSpace(400, 1000, 1020, 600, 0) == 420, "zero margin still reveals caret");
    Expect(RevealCaretWithBottomSpace(400, 1000, 1020, 0, 32) == 400, "hidden viewport retains origin");
    Expect(RevealCaretWithBottomSpace(0, 0, 800, 600, 32) == 232 &&
        RevealCaretWithBottomSpace(232, 0, 800, 600, 32) == 232, "oversized caret alignment stays stable");
    Expect(RevealCaretWithBottomSpace(UINT64_MAX - 100, UINT64_MAX - 50,
        UINT64_MAX - 10, 600, 32) == UINT64_MAX - 100, "reveal arithmetic does not overflow");
    int value = 17;
    Expect(note::ParseBottomSpaceValue(L"2000", 2000, value) && value == 2000, "maximum accepted");
    Expect(note::ParseBottomSpaceValue(L"0", 90, value) && value == 0, "zero accepted");
    for (const auto* invalid : {L"", L"-1", L"1.5", L"10px", L"99999", L"91", L" 25"}) {
        value = 17;
        Expect(!note::ParseBottomSpaceValue(invalid, 90, value) && value == 17, "invalid retains prior value");
    }
    CheckNativeRawSpace();
    std::cout << "PASS: note bottom space " << checks << " checks\n";
}
