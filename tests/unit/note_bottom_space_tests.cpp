#include "note/note_bottom_space.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <richedit.h>
#include <commctrl.h>
#include "ui/native_note_bottom_space.h"
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;
void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
LONG NativeLength(HWND edit) {
    GETTEXTLENGTHEX query{GTL_NUMCHARS | GTL_PRECISE, 1200};
    return static_cast<LONG>(SendMessageW(edit, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&query), 0));
}
LONG SpaceAfterAt(HWND edit, LONG position) {
    CHARRANGE before{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&before));
    const CHARRANGE range{position, position};
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    PARAFORMAT2 paragraph{};
    paragraph.cbSize = sizeof(paragraph);
    SendMessageW(edit, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&before));
    return paragraph.dySpaceAfter;
}
void CheckBottomPixels(HWND edit) {
    HDC dc = GetDC(edit);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, 600, 400);
    Expect(dc && memory && bitmap, "native pixel fixture created");
    HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT bounds{0, 0, 600, 400};
    FillRect(memory, &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SendMessageW(edit, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(edit, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
    int pixels = 0;
    for (int y = 304; y < 400; ++y) {
        for (int x = 0; x < 120; ++x) {
            const COLORREF pixel = GetPixel(memory, x, y);
            if (pixel != CLR_INVALID && pixel != RGB(255, 255, 255)) ++pixels;
        }
    }
    Expect(pixels > 100, "body glyphs paint inside the former white footer area");
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(edit, dc);
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
    SendMessageW(edit, EM_SETMODIFY, FALSE, 0);
    SendMessageW(edit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
    SendMessageW(edit, EM_SETSEL, 17, 23);
    ui::NativeNoteBottomSpace space;
    Expect(space.Apply(edit, 96, 96), "native trailing space applied");
    RECT client{}, format{};
    GetClientRect(edit, &client);
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
    Expect(client.bottom == 400 && format.bottom == 400, "padding retains the full client and text rectangle");
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    Expect(start == 17 && end == 23, "display spacing preserves selection");
    Expect(readText() == source && !SendMessageW(edit, EM_GETMODIFY, 0, 0), "display spacing preserves text and modified flag");
    Expect(SendMessageW(edit, EM_GETEVENTMASK, 0, 0) == (ENM_CHANGE | ENM_SELCHANGE),
        "display spacing restores native notifications");
    Expect(SpaceAfterAt(edit, 0) == 0 && SpaceAfterAt(edit, NativeLength(edit)) == 1440,
        "only final paragraph owns the padding");
    CheckBottomPixels(edit);
    SendMessageW(edit, EM_SETSEL, -1, -1);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
    space.RevealCaret(edit, 96, font);
    POINTL caret{};
    SendMessageW(edit, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&caret), NativeLength(edit));
    Expect(caret.y >= 0 && caret.y + 19 <= 304, "end caret has scrollable space below it");
    POINT before{}, after{};
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&before));
    SendMessageW(edit, WM_VSCROLL, SB_LINEDOWN, 0);
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&after));
    Expect(before.y > 0 && before.y == after.y, "native lower scroll limit stable");
    const LONG previousEnd = NativeLength(edit);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\r\nnext\r\nlast"));
    Expect(space.Apply(edit, 96, 96), "newline updates final paragraph spacing");
    Expect(SpaceAfterAt(edit, previousEnd) == 0 && SpaceAfterAt(edit, previousEnd + 1) == 0 &&
        SpaceAfterAt(edit, NativeLength(edit)) == 1440, "newlines never leave gaps between body paragraphs");
    for (int line = 0; line < 3; ++line) {
        SendMessageW(edit, EM_SETSEL, -1, -1);
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\r\nline"));
        Expect(space.Apply(edit, 96, 96), "successive newlines update the tail");
        space.RevealCaret(edit, 96, font);
        SendMessageW(edit, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&caret), NativeLength(edit));

        Expect(caret.y >= 0 && caret.y + 19 <= 304, "successive newlines retain native caret margin");
    }
    SendMessageW(edit, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(edit, EM_SETSEL, -1, -1);
    const auto beforeTyping = readText();
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"X"));
    Expect(space.Apply(edit, 48, 96), "spacing preference changes during editing");
    Expect(SendMessageW(edit, EM_GETMODIFY, 0, 0) != 0, "padding keeps the edited note dirty");
    Expect(SendMessageW(edit, EM_UNDO, 0, 0) && readText() == beforeTyping,
        "undo replays text without undoing display formatting");
    Expect(space.Apply(edit, 48, 96) && SendMessageW(edit, EM_REDO, 0, 0) && readText() == beforeTyping + L"X",
        "display formatting preserves the redo queue");
    Expect(space.Apply(edit, 48, 96) && SpaceAfterAt(edit, NativeLength(edit)) == 720,
        "undo and redo retain the current padding preference");
    const auto edited = readText();
    Expect(space.Apply(edit, 0, 96) && SpaceAfterAt(edit, NativeLength(edit)) == 0,
        "zero removes trailing space");
    Expect(readText() == edited, "disabling padding keeps edited text");
    space.Invalidate();
    Expect(SetWindowTextW(edit, L"short\r\nnote") && space.Apply(edit, 96, 96) &&
        SpaceAfterAt(edit, 0) == 0 && SpaceAfterAt(edit, NativeLength(edit)) == 1440,
        "replacement notes receive one final padding paragraph");
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
