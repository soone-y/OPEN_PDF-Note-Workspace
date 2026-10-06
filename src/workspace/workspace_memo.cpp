#include "workspace/workspace_memo.h"
#include "workspace/workspace_memo_store.h"
#include "core/app_core.h"
#include "core/localization.h"
#include "ui/noop_nav_guard.h"
#include <imm.h>
#include <stdexcept>

namespace {
using workspace_memo::Result;
// Single UI-thread-owned manager. The root/identity live in Document, not in
// controls or entry-point dialogs; the view can close without destroying data.
workspace_memo::Document document;
HWND window = nullptr, editor = nullptr, status = nullptr, saveButton = nullptr;
HWND reloadButton = nullptr, description = nullptr;
bool populating = false, saving = false, viewValid = false;
std::wstring acceptedText;
wchar_t pendingHighSurrogate = 0;
constexpr int kTextId = 1, kSaveId = 2, kReloadId = 3;

std::wstring ReadEditor() {
    const int length = GetWindowTextLengthW(editor);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(editor, value.data(), length + 1);
    if (copied != length) throw std::runtime_error("memo text capture failed");
    value.resize(static_cast<size_t>(copied));
    return value;
}

const wchar_t* ResultId(Result result) {
    switch (result) {
    case Result::Ok: return L"workspace_memo.saved";
    case Result::Recovered: return L"workspace_memo.recovered";
    case Result::Conflict: return L"workspace_memo.conflict";
    case Result::InvalidText: return L"workspace_memo.invalid_text";
    default: return L"workspace_memo.save_failed";
    }
}

void SetStatus(const wchar_t* id) {
    if (status) SetWindowTextW(status, localization::Text(id).c_str());
}

void Populate() {
    viewValid = false;
    populating = true;
    acceptedText = document.text();
    pendingHighSurrogate = 0;
    const bool loaded = SetWindowTextW(editor, document.text().c_str()) != FALSE;
    SendMessageW(editor, EM_SETREADONLY, !document.writable(), 0);
    EnableWindow(saveButton, document.writable() && loaded);
    populating = false;
    viewValid = loaded;
    if (!loaded) throw std::runtime_error("memo view population failed");
}

std::wstring EditorLines(std::wstring_view text) {
    std::wstring normalized;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            normalized += L"\r\n";
        } else if (text[i] == L'\n') normalized += L"\r\n";
        else normalized += text[i];
    }
    return normalized;
}

// Validate the complete proposed edit before handing it to EDIT. The native
// UTF-16 limit alone would truncate pastes and cannot express a UTF-8 limit.
[[nodiscard]] bool CanReplace(std::wstring_view insertion, bool entire = false) {
    auto proposed = entire ? std::wstring{} : ReadEditor();
    DWORD begin = 0, end = 0;
    if (!entire) SendMessageW(editor, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
    const size_t first = std::min(static_cast<size_t>(begin), proposed.size());
    const size_t last = std::min(static_cast<size_t>(end), proposed.size());
    if (insertion.size() > workspace_memo::kMaxEditorUnits ||
        proposed.size() - (last - first) > workspace_memo::kMaxEditorUnits - insertion.size()) return false;
    proposed.replace(first, last - first, insertion);
    return workspace_memo::ValidateText(proposed);
}

[[nodiscard]] bool BoundedInsertion(const wchar_t* value, std::wstring_view& insertion) {
    if (!value) { insertion = {}; return true; }
    size_t length = 0;
    while (length <= workspace_memo::kMaxEditorUnits && value[length]) ++length;
    if (length > workspace_memo::kMaxEditorUnits) return false;
    insertion = std::wstring_view(value, length);
    return true;
}

void Paste(HWND hwnd) {
    if (!OpenClipboard(hwnd)) { SetStatus(L"workspace_memo.save_failed"); return; }
    struct Close { ~Close() { CloseClipboard(); } } close;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) { SetStatus(L"workspace_memo.input_rejected"); return; }
    const size_t units = GlobalSize(data) / sizeof(wchar_t);
    auto* value = static_cast<const wchar_t*>(GlobalLock(data));
    if (!value) { SetStatus(L"workspace_memo.save_failed"); return; }
    struct Unlock { HANDLE data; ~Unlock() { GlobalUnlock(data); } } unlock{data};
    size_t length = 0;
    while (length < units && length <= workspace_memo::kMaxEditorUnits && value[length]) ++length;
    if (length == units || length > workspace_memo::kMaxEditorUnits ||
        !CanReplace(std::wstring_view(value, length))) {
        SetStatus(L"workspace_memo.input_rejected"); return;
    }
    if (!length) return; // empty clipboard text must not erase a selection
    SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(value));
}

[[nodiscard]] bool CommitIme() {
    if (!editor) return true;
    HIMC context = ImmGetContext(editor);
    if (!context) return true;
    const LONG composing = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
    bool ok = composing != IMM_ERROR_GENERAL;
    if (ok && composing > 0) {
        ok = ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_COMPLETE, 0) != FALSE;
        // Some IMEs complete asynchronously. Never close with residual input:
        // keep the editor and let its normal IME messages finish before retry.
        const LONG remaining = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        ok = ok && remaining <= 0 && remaining != IMM_ERROR_GENERAL;
    }
    ImmReleaseContext(editor, context);
    return ok;
}

[[nodiscard]] bool Save(bool interactive) {
    if (!editor || !document.writable()) return true; // invalid original is read-only
    if (!viewValid) return false;
    if (saving) return false;
    saving = true;
    struct Reset { ~Reset() { saving = false; } } reset;
    Result result = Result::IoError;
    try {
        if (pendingHighSurrogate) result = Result::InvalidText;
        else if (CommitIme() && !pendingHighSurrogate) {
            const auto text = ReadEditor();
            result = document.Save(text);
            if (result == Result::Ok && text != document.text()) Populate();
        }
    } catch (...) { result = Result::IoError; }
    SetStatus(ResultId(result));
    if (result != Result::Ok && interactive) {
        ShowWindow(window, SW_RESTORE);
        ShowSilentMessageDialog(window, localization::Text(L"workspace_memo.title"),
                                localization::Text(ResultId(result)), SoftNoticeKind::Warning);
        SetFocus(editor);
    }
    return result == Result::Ok;
}

void Layout(HWND hwnd) {
    RECT bounds{}; GetClientRect(hwnd, &bounds);
    TEXTMETRICW metrics{};
    HDC dc = GetDC(description);
    if (dc) {
        const auto font = reinterpret_cast<HFONT>(SendMessageW(description, WM_GETFONT, 0, 0));
        HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
        GetTextMetricsW(dc, &metrics);
        if (old) SelectObject(dc, old);
        ReleaseDC(description, dc);
    }
    const int line = std::max(16L, metrics.tmHeight);
    const int margin = std::max(12, line / 2);
    const int descriptionH = line * 2 + margin;
    const int statusH = line * 4 + margin;
    const int buttonH = line * 2 + margin;
    const int width = std::max(1L, bounds.right - 2 * margin);
    const int buttonsY = std::max(descriptionH + margin * 3, static_cast<int>(bounds.bottom) - statusH - buttonH - margin * 3);
    const int saveW = std::min(width / 3, line * 9);
    MoveWindow(description, margin, margin, width, descriptionH, TRUE);
    MoveWindow(editor, margin, descriptionH + margin * 2, width, std::max(1, buttonsY - descriptionH - margin * 3), TRUE);
    MoveWindow(saveButton, margin, buttonsY, saveW, buttonH, TRUE);
    MoveWindow(reloadButton, margin * 2 + saveW, buttonsY, std::max(1, width - saveW - margin), buttonH, TRUE);
    MoveWindow(status, margin, buttonsY + buttonH + margin, width, statusH, TRUE);
}

LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT EditMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == L'S' && (GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000)) {
        const bool saved = Save(true); (void)saved;
        return 0;
    }
    if (msg == WM_CHAR && wp == 0x13) return 0; // nested/modal loop Ctrl+S: no control character/beep
    if (msg == WM_KEYDOWN &&
        ((wp == L'V' && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) ||
         (wp == VK_INSERT && (GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)))) {
        if (document.writable() && !populating) Paste(hwnd);
        return 0; // do not allow native keyboard paste to bypass preflight
    }
    if (msg == WM_CHAR && wp == 0x16) return 0; // Ctrl+V was handled once at keydown
    if (!document.writable()) {
        if (msg == WM_CHAR && (wp >= 32 || wp == 8 || wp == 22 || wp == 24 || wp == 26)) return 0;
        if (msg == WM_KEYDOWN && (wp == VK_DELETE || wp == VK_BACK)) return 0;
        if (msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR || msg == WM_UNDO) return 0;
    }
    if (!populating && document.writable()) {
        if (msg == WM_PASTE) { Paste(hwnd); return 0; }
        if (msg == EM_REPLACESEL || msg == WM_SETTEXT) {
            std::wstring_view insertion;
            if (!BoundedInsertion(reinterpret_cast<const wchar_t*>(lp), insertion)) {
                SetStatus(L"workspace_memo.input_rejected"); return 0;
            }
            const auto normalized = EditorLines(insertion);
            if (!CanReplace(normalized, msg == WM_SETTEXT)) { SetStatus(L"workspace_memo.input_rejected"); return 0; }
            pendingHighSurrogate = 0;
            if (normalized != insertion)
                return DefSubclassProc(hwnd, msg, wp, reinterpret_cast<LPARAM>(normalized.c_str()));
        }
        if (msg == WM_CHAR) {
            if (wp == 8 && pendingHighSurrogate) { pendingHighSurrogate = 0; return 0; }
            if (wp >= 0xD800 && wp <= 0xDBFF) { pendingHighSurrogate = static_cast<wchar_t>(wp); return 0; }
            if (wp >= 32 || wp == 13) {
                std::wstring insertion;
                if (pendingHighSurrogate && wp >= 0xDC00 && wp <= 0xDFFF) insertion += pendingHighSurrogate;
                pendingHighSurrogate = 0;
                insertion += wp == 13 ? L'\r' : static_cast<wchar_t>(wp);
                if (wp == 13) insertion += L'\n';
                if (!CanReplace(insertion)) { SetStatus(L"workspace_memo.input_rejected"); return 0; }
                // Insert a surrogate pair as one edit; never checkpoint half.
                if (insertion.size() == 2 && wp != 13) {
                    SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(insertion.c_str())); return 0;
                }
            }
        }
        if (msg == WM_IME_COMPOSITION && (lp & GCS_RESULTSTR)) {
            HIMC context = ImmGetContext(hwnd);
            if (context) {
                struct Release { HWND hwnd; HIMC context; ~Release() { ImmReleaseContext(hwnd, context); } } release{hwnd, context};
                const LONG size = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                std::wstring result;
                bool valid = size >= 0 && size % sizeof(wchar_t) == 0 &&
                    static_cast<size_t>(size) <= workspace_memo::kMaxEditorUnits * sizeof(wchar_t);
                if (valid && size) {
                    result.resize(static_cast<size_t>(size) / sizeof(wchar_t));
                    valid = ImmGetCompositionStringW(context, GCS_RESULTSTR, result.data(), size) == size;
                }
                if (!valid || !CanReplace(result)) {
                    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                    SetStatus(L"workspace_memo.input_rejected"); return 0;
                }
            }
        }
    }
    if (msg == WM_KEYDOWN) {
        MSG key{}; key.hwnd = hwnd; key.message = msg; key.wParam = wp; key.lParam = lp;
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(key)) return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, EditProc, 1);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    try { return EditMessage(hwnd, msg, wp, lp); }
    catch (...) { SetStatus(L"workspace_memo.save_failed"); return 0; }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
        case WM_CREATE:
            // Control creation/font setup must not checkpoint empty initial
            // controls over the recovered document before Populate completes.
            populating = true;
            description = CreateWindowW(L"STATIC", localization::Text(L"workspace_memo.description").c_str(),
                WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hwnd, nullptr, g_hInst, nullptr);
            editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(kTextId), g_hInst, nullptr);
            saveButton = CreateWindowW(L"BUTTON", localization::Text(L"workspace_memo.save").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(kSaveId), g_hInst, nullptr);
            reloadButton = CreateWindowW(L"BUTTON", localization::Text(L"workspace_memo.reload").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_MULTILINE, 0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(kReloadId), g_hInst, nullptr);
            status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hwnd, nullptr, g_hInst, nullptr);
            if (!editor || !saveButton || !reloadButton || !status || !description) return -1;
            SendMessageW(editor, EM_SETLIMITTEXT, workspace_memo::kMaxEditorUnits, 0);
            if (!SetWindowSubclass(editor, EditProc, 1, 0)) return -1;
            for (HWND child : {description, editor, saveButton, reloadButton, status}) SetUIFont(child);
            Populate(); Layout(hwnd); ApplyThemeToDialog(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kTextId && HIWORD(wp) == EN_CHANGE && !populating && document.writable()) {
                const auto text = ReadEditor();
                if (!workspace_memo::ValidateText(text)) {
                    // Guard unusual/programmatic edit paths too. No prefix of an
                    // invalid edit is accepted; retain the previous valid view.
                    populating = true;
                    viewValid = SetWindowTextW(editor, acceptedText.c_str()) != FALSE;
                    populating = false;
                    SetStatus(L"workspace_memo.input_rejected"); return 0;
                }
                acceptedText = text;
                const auto result = document.Checkpoint(text);
                SetStatus(result == Result::Ok ? L"workspace_memo.editing" : ResultId(result));
                return 0;
            }
            if (LOWORD(wp) == kSaveId && HIWORD(wp) == BN_CLICKED) {
                const bool saved = Save(true); (void)saved; return 0;
            }
            if (LOWORD(wp) == kReloadId && HIWORD(wp) == BN_CLICKED && !saving) {
                if (!CommitIme()) { SetStatus(L"workspace_memo.save_failed"); return 0; }
                const auto result = document.writable() ? document.Reload(ReadEditor()) : document.Load(g_workspaceRoot);
                if (result == Result::Ok || result == Result::Recovered || result == Result::Conflict) Populate();
                SetStatus(result == Result::Ok ? L"workspace_memo.reloaded" : ResultId(result));
                return 0;
            }
            break;
        case WM_SIZE: Layout(hwnd); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {480, 300}; return 0;
        }
        case WM_CLOSE:
            if (Save(true)) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            window = editor = status = saveButton = reloadButton = description = nullptr;
            populating = viewValid = false;
            acceptedText.clear(); pendingHighSurrogate = 0;
            return 0;
        case WM_THEMECHANGED: ApplyThemeToDialog(hwnd); return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: return ThemeCtlColorPanel(reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp));
        case WM_CTLCOLOREDIT: return ThemeCtlColorPanel(reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp));
        }
    } catch (...) { SetStatus(L"workspace_memo.save_failed"); return msg == WM_CREATE ? -1 : 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

std::filesystem::path WorkspaceMemoPath(const std::filesystem::path& root) { return workspace_memo::MemoPath(root); }

void ShowWorkspaceMemoWindow(HWND mainOwner) {
    if (window) { ShowWindow(window, SW_RESTORE); SetForegroundWindow(window); SetFocus(editor); return; }
    try {
        const auto result = document.Load(g_workspaceRoot);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc; wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"PdfNoteWorkspaceMemoWnd";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        // Always owned by the main window, never the search/settings dialog.
        HWND owner = g_hMainWnd ? g_hMainWnd : mainOwner;
        window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName,
            localization::Text(L"workspace_memo.title").c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 680, 490, owner, nullptr, g_hInst, nullptr);
        if (!window) return;
        SetStatus(result == Result::Ok ? L"workspace_memo.ready" : ResultId(result));
        PlaceOwnedPopupAtAppTopLeft(window, owner);
        ShowWindow(window, SW_SHOW); SetFocus(editor);
    } catch (...) {
        ShowSilentMessageDialog(mainOwner, localization::Text(L"workspace_memo.title"),
                                localization::Text(L"workspace_memo.save_failed"), SoftNoticeKind::Warning);
    }
}

bool HandleWorkspaceMemoMessage(const MSG& message) {
    if (!window || GetAncestor(message.hwnd, GA_ROOT) != window) return false;
    if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(message)) return true;
    if (message.message == WM_KEYDOWN && message.wParam == L'S' &&
        (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        const bool saved = Save(true); (void)saved; return true;
    }
    // Do not offer this editor's Ctrl+Z/Ctrl+Y/etc. to the main-note accelerator.
    MSG local = message;
    if (!IsDialogMessageW(window, &local)) { TranslateMessage(&local); DispatchMessageW(&local); }
    return true;
}

bool SaveWorkspaceMemoForExit(bool interactive) { return Save(interactive); }
bool PrepareWorkspaceMemoRootChange() { return Save(true); }
void ResetWorkspaceMemoForRootChange() {
    if (window) DestroyWindow(window); // caller already saved before committing root
    document = workspace_memo::Document{};
}
