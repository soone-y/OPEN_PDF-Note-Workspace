#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <richedit.h>
#include "note/note_source_line_map.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
int checks = 0;
void Expect(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::wstring ReadText(HWND edit) {
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    const int length = GetWindowTextW(edit, value.data(), static_cast<int>(value.size()));
    value.resize(static_cast<size_t>(length));
    return value;
}
}

int main() {
    const HWND foreground = GetForegroundWindow();
    const HMODULE library = LoadLibraryW(L"Msftedit.dll");
    Expect(library != nullptr, "native RichEdit is available");
    const HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE,
        0, 0, 640, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Expect(edit != nullptr, "hidden native editor is created");
    using Map = note::NoteSourceLineMap;
    for (const std::wstring raw : {L"", L"abc", L"a\r\nb", L"a\n", L"a\rb",
            L"# Title\r\n\r\nbody", L"日本語\U0001F600\r\nlast", L"<c=red>red</>"}) {
        Map::Snapshot map;
        Expect(Map::Build(raw, &map), "source coordinate snapshot builds");
        Expect(SetWindowTextW(edit, raw.c_str()) != FALSE, "fixture source is loaded");
        SendMessageW(edit, EM_EMPTYUNDOBUFFER, 0, 0);
        GETTEXTLENGTHEX query{GTL_NUMCHARS | GTL_PRECISE, 1200};
        Expect(SendMessageW(edit, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&query), 0) ==
            static_cast<LRESULT>(map.rich_edit_text_length()), "snapshot matches actual native text length");
        const auto baseline = ReadText(edit);
        BYTE previous[256]{}, state[256]{};
        const bool readKeys = GetKeyboardState(previous) != FALSE;
        CopyMemory(state, previous, sizeof(state));
        state[VK_CONTROL] = 0x80;
        const bool setKeys = SetKeyboardState(state) != FALSE;
        SendMessageW(edit, WM_KEYDOWN, 'A', 1);
        SendMessageW(edit, WM_CHAR, 1, 1);
        const bool restoredKeys = SetKeyboardState(previous) != FALSE;
        Expect(readKeys && setKeys && restoredKeys, "Ctrl+A uses local thread keyboard state only");
        DWORD ctrlStart = 0, ctrlEnd = 0;
        SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&ctrlStart), reinterpret_cast<LPARAM>(&ctrlEnd));
        const auto start = Map::RichEditSelectionEndpointToCanonicalOffset(map, ctrlStart);
        const auto end = Map::RichEditSelectionEndpointToCanonicalOffset(map, ctrlEnd);
        Expect(start && end && start->value == 0 && end->value == raw.size(), "native Ctrl+A resolves to exact source selection");
        SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(map.rich_edit_text_length()));
        DWORD dragStart = 0, dragEnd = 0;
        SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&dragStart), reinterpret_cast<LPARAM>(&dragEnd));
        Expect(Map::RichEditSelectionEndpointToCanonicalOffset(map, dragStart) == start &&
            Map::RichEditSelectionEndpointToCanonicalOffset(map, dragEnd) == end,
            "Ctrl+A and a selection ending at the final text hit have identical canonical bounds");
        Expect(ReadText(edit) == baseline, "both selection methods preserve the text");
        const size_t eof = map.rich_edit_text_length();
        Expect(!Map::RichEditIndexToCanonicalOffset(map, eof + 1) &&
            Map::RichEditSelectionEndpointToCanonicalOffset(map, eof + 1) == end,
            "only the selection boundary accepts the implicit paragraph mark");
        Expect(!Map::RichEditSelectionEndpointToCanonicalOffset(map, eof + 2) &&
            !Map::RichEditSelectionEndpointToCanonicalOffset(map, std::numeric_limits<size_t>::max()),
            "arbitrary out-of-range positions are rejected");
        Expect(Map::RichEditSelectionEndpointToCanonicalOffset(map, eof) ==
            Map::RichEditSelectionEndpointToCanonicalOffset(map, eof + 1),
            "selecting only the implicit mark selects no source characters");
        SendMessageW(edit, EM_SETSEL, ctrlStart, ctrlEnd);
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"X"));
        Expect(ReadText(edit) == L"X", "replacement of Ctrl+A deletes all source text and retains the native terminator");
        Expect(SendMessageW(edit, EM_UNDO, 0, 0) && ReadText(edit) == baseline, "native undo restores the exact original text");
        Expect(SendMessageW(edit, EM_REDO, 0, 0) && ReadText(edit) == L"X", "native redo restores the replacement");
    }
    Expect(!Map::RichEditSelectionEndpointToCanonicalOffset({}, 0) &&
        !Map::RichEditSelectionEndpointToCanonicalOffset({}, 1), "uninitialized snapshots cannot prove an endpoint");
    Expect(!IsWindowVisible(edit) && GetForegroundWindow() == foreground, "native checks never take the foreground");
    DestroyWindow(edit);
    FreeLibrary(library);
    std::cout << "PASS: note selection transport " << checks << " checks\n";
}
