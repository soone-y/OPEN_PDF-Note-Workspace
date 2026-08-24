#include "ui/dialogs/dialogs.h"
#include "ui/core/main_window_api.h"
#include "core/localization.h"
#include "core/preview_trace.h"
#include "core/secure_memory.h"
#include "ui/noop_nav_guard.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
static constexpr ULONGLONG kSilentMessageDialogRepeatSuppressMs = 10000;

struct UiMessageRepeatState {
    std::wstring title;
    std::wstring message;
    SoftNoticeKind kind = SoftNoticeKind::Info;
    ULONGLONG lastShownTick = 0;
};

static UiMessageRepeatState g_silentMessageDialogRepeatState;

// Keep modal retry failures from reopening the same dialog in a tight loop.
static bool ShouldSuppressRepeatedUiMessage(UiMessageRepeatState& state,
                                            const std::wstring& title,
                                            const std::wstring& message,
                                            SoftNoticeKind kind,
                                            ULONGLONG suppressMs) {
    const ULONGLONG now = GetTickCount64();
    if (state.kind == kind &&
        state.title == title &&
        state.message == message &&
        state.lastShownTick != 0 &&
        now - state.lastShownTick < suppressMs) {
        return true;
    }
    state.title = title;
    state.message = message;
    state.kind = kind;
    state.lastShownTick = now;
    return false;
}
} // namespace

struct NewLectureCtx {
    HWND edit{};
    std::wstring result;
    bool ok = false;
    bool done = false;
    std::wstring title;
    std::wstring label;
};

static void AppendToEdit(HWND edit, const std::wstring& text) {
    if (!edit) return;
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
}

static LRESULT CALLBACK NewLectureDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    NewLectureCtx* ctx = reinterpret_cast<NewLectureCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<NewLectureCtx*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        CreateWindowExW(0, L"STATIC", ctx->label.c_str(),
                        WS_CHILD | WS_VISIBLE,
                        10, 10, 260, 20, hWnd, nullptr, g_hInst, nullptr);
        ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    10, 32, 260, 24, hWnd, reinterpret_cast<HMENU>(101),
                                    g_hInst, nullptr);
        const wchar_t* buttons[] = { L"1", L"2", L"3", L"4", L"5", L"10" };
        int bx = 10, by = 64;
        for (int i = 0; i < 6; ++i) {
            CreateWindowExW(0, L"BUTTON", buttons[i],
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            bx, by, 40, 24, hWnd, reinterpret_cast<HMENU>(200 + i),
                            g_hInst, nullptr);
            bx += 44;
        }
        CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        60, 124, 80, 28, hWnd, reinterpret_cast<HMENU>(IDOK),
                        g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        160, 124, 80, 28, hWnd, reinterpret_cast<HMENU>(IDCANCEL),
                        g_hInst, nullptr);
        SetFocus(ctx->edit);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id >= 200 && id < 206) {
            const wchar_t* nums[] = { L"1", L"2", L"3", L"4", L"5", L"10" };
            AppendToEdit(ctx ? ctx->edit : nullptr, nums[id - 200]);
            return 0;
        }
        if (id == IDOK) {
            wchar_t buf[256]{};
            int len = GetWindowTextW(ctx->edit, buf, 255);
            std::wstring name(buf, buf + len);
            name = TrimWhitespace(name);
            if (name.empty()) {
                ShowSoftNotice(hWnd, localization::Text(L"dialog.input.enter_name"),
                               SoftNoticeKind::Warning);
                return 0;
            }
            ctx->result = name;
            ctx->ok = true;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        } else if (id == IDCANCEL) {
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ctx) ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool RunDialogMessageLoop(HWND dialog, bool* done) {
    if (!dialog || !done) return false;
    MSG msg;
    while (!*done) {
        BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
        if (gm == -1) {
            return false;
        }
        if (gm == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return false;
        }
        if (ShouldSkipImeMessageInLoop(msg)) continue;
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(msg, g_hNoteEdit)) continue;
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

bool PromptNewLectureName(HWND owner, std::wstring& outName) {
    NewLectureCtx ctx;
    const auto& ui = GetUiText();
    ctx.title = ui.dlgNewLectureTitle;
    ctx.label = ui.dlgNewLectureLabel;
    WNDCLASSW wc{};
    wc.lpfnWndProc = NewLectureDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NewLectureDlgClass";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, ui.dlgNewLectureTitle.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 300, 170,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return false;
    PlaceOwnedPopupAtAppTopLeft(w, owner);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RunDialogMessageLoop(w, &ctx.done);
    if (ctx.ok) {
        outName = ctx.result;
        return true;
    }
    return false;
}

bool PromptNewSessionName(HWND owner, std::wstring& outName) {
    NewLectureCtx ctx;
    const auto& ui = GetUiText();
    ctx.title = ui.dlgNewSessionTitle;
    ctx.label = ui.dlgNewSessionLabel;
    WNDCLASSW wc{};
    wc.lpfnWndProc = NewLectureDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NewSessionDlgClass";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, ui.dlgNewSessionTitle.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 300, 170,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return false;
    PlaceOwnedPopupAtAppTopLeft(w, owner);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RunDialogMessageLoop(w, &ctx.done);
    if (ctx.ok) {
        outName = ctx.result;
        return true;
    }
    return false;
}

struct SimpleInputDialog {
    HWND hwnd{};
    HWND edit{};
    std::wstring title;
    std::wstring initial;
    std::wstring result;
    bool ok = false;
    bool done = false;
};

static LRESULT CALLBACK SimpleInputDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SimpleInputDialog* ctx = reinterpret_cast<SimpleInputDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<SimpleInputDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;
        ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                    10, 10, 320, 24, hWnd, reinterpret_cast<HMENU>(101),
                                    cs->hInstance, nullptr);
        if (ctx->edit && !ctx->initial.empty()) {
            SetWindowTextW(ctx->edit, ctx->initial.c_str());
            SendMessageW(ctx->edit, EM_SETSEL, 0, -1);
        }
        CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        70, 50, 80, 26, hWnd, reinterpret_cast<HMENU>(IDOK),
                        cs->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        180, 50, 80, 26, hWnd, reinterpret_cast<HMENU>(IDCANCEL),
                        cs->hInstance, nullptr);
        SetFocus(ctx->edit);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buf[512]{};
            int len = GetWindowTextW(ctx->edit, buf, 511);
            ctx->result.assign(buf, buf + len);
            ctx->ok = true;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        } else if (LOWORD(wParam) == IDCANCEL) {
            ctx->ok = false;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        ctx->ok = false;
        ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool PromptSimpleText(HWND owner, const std::wstring& title,
                      const std::wstring& initial, std::wstring& out) {
    SimpleInputDialog ctx;
    ctx.title = title;
    ctx.initial = initial;
    WNDCLASSW wc{};
    wc.lpfnWndProc = SimpleInputDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SimpleInputDlg";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 360, 130,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return false;
    PlaceOwnedPopupAtAppTopLeft(w, owner);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RunDialogMessageLoop(w, &ctx.done);
    if (ctx.ok) {
        out = ctx.result;
        return true;
    }
    return false;
}

struct SavePathDialog {
    HWND folder{};
    HWND fileName{};
    std::wstring title;
    std::wstring directory;
    std::wstring defaultName;
    std::wstring result;
    SavePathPromptResult action = SavePathPromptResult::Cancel;
    bool done = false;
};

static constexpr int kSavePathDialogOpenSystemId = 102;

static LRESULT CALLBACK SavePathDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SavePathDialog* ctx = reinterpret_cast<SavePathDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<SavePathDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        const std::wstring folderLabel = localization::Text(L"dialog.save_path.folder_label");
        CreateWindowExW(0, L"STATIC", folderLabel.c_str(),
                        WS_CHILD | WS_VISIBLE, 12, 12, 150, 20, hWnd, nullptr, cs->hInstance, nullptr);
        ctx->folder = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctx->directory.c_str(),
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                      12, 34, 716, 24, hWnd, nullptr, cs->hInstance, nullptr);
        const std::wstring fileNameLabel = localization::Text(L"dialog.save_path.file_name_label");
        CreateWindowExW(0, L"STATIC", fileNameLabel.c_str(),
                        WS_CHILD | WS_VISIBLE, 12, 70, 150, 20, hWnd, nullptr, cs->hInstance, nullptr);
        ctx->fileName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctx->defaultName.c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        12, 92, 716, 24, hWnd, reinterpret_cast<HMENU>(101),
                                        cs->hInstance, nullptr);
        const std::wstring systemDialogLabel = localization::Text(L"dialog.save_path.system_dialog");
        CreateWindowExW(0, L"BUTTON", systemDialogLabel.c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 132, 170, 28, hWnd,
                        reinterpret_cast<HMENU>(kSavePathDialogOpenSystemId), cs->hInstance, nullptr);
        const std::wstring saveLabel = localization::Text(L"dialog.save_path.save");
        CreateWindowExW(0, L"BUTTON", saveLabel.c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 512, 132, 100, 28, hWnd,
                        reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
        const std::wstring cancelLabel = localization::Text(L"dialog.save_path.cancel");
        CreateWindowExW(0, L"BUTTON", cancelLabel.c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 628, 132, 100, 28, hWnd,
                        reinterpret_cast<HMENU>(IDCANCEL), cs->hInstance, nullptr);
        if (ctx->folder) SendMessageW(ctx->folder, EM_SETSEL, 0, 0);
        if (ctx->fileName) {
            SendMessageW(ctx->fileName, EM_SETSEL, 0, -1);
            SetFocus(ctx->fileName);
        }
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            const int len = ctx && ctx->fileName ? GetWindowTextLengthW(ctx->fileName) : 0;
            std::wstring name(static_cast<size_t>(std::max(0, len)) + 1, L'\0');
            if (ctx && ctx->fileName && len > 0) {
                const int copied = GetWindowTextW(ctx->fileName, name.data(), len + 1);
                name.resize(static_cast<size_t>(std::max(0, copied)));
            } else {
                name.clear();
            }
            name = TrimWhitespace(name);
            if (name.empty()) {
                ShowSoftNotice(hWnd, localization::Text(L"dialog.input.enter_file_name"),
                               SoftNoticeKind::Warning);
                return 0;
            }
            ctx->result = std::move(name);
            ctx->action = SavePathPromptResult::DirectInput;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == kSavePathDialogOpenSystemId) {
            ctx->action = SavePathPromptResult::OpenSystemDialog;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (ctx) ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

SavePathPromptResult PromptSavePath(HWND owner, const std::wstring& title,
                                    const std::wstring& directory,
                                    const std::wstring& defaultName,
                                    std::wstring& outFileName) {
    SavePathDialog ctx;
    ctx.title = title;
    ctx.directory = directory;
    ctx.defaultName = defaultName;
    WNDCLASSW wc{};
    wc.lpfnWndProc = SavePathDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SavePathDialog";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 756, 210,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return SavePathPromptResult::Cancel;
    PlaceOwnedPopupAtAppTopLeft(w, owner);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RunDialogMessageLoop(w, &ctx.done);
    if (ctx.action == SavePathPromptResult::DirectInput) {
        outFileName = std::move(ctx.result);
    }
    return ctx.action;
}

struct CreateNameDialog {
    HWND hwnd{};
    HWND edit{};
    std::wstring title;
    std::wstring label;
    std::wstring initial;
    std::vector<std::wstring> suggestions;
    bool showExplorerButton = false;
    std::wstring result;
    PromptCreateNameResult action = PromptCreateNameResult::Cancel;
    bool done = false;
};

static void SetCreateNameEditText(HWND edit, const std::wstring& text) {
    if (!edit) return;
    SetWindowTextW(edit, text.c_str());
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SetFocus(edit);
}

static LRESULT CALLBACK CreateNameDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    constexpr int kEditId = 101;
    constexpr int kSuggestionBaseId = 300;
    constexpr int kExplorerId = 410;
    CreateNameDialog* ctx = reinterpret_cast<CreateNameDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<CreateNameDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;
        CreateWindowExW(0, L"STATIC", ctx->label.c_str(),
                        WS_CHILD | WS_VISIBLE,
                        12, 12, 430, 20, hWnd, nullptr, cs->hInstance, nullptr);
        ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    12, 36, 430, 24, hWnd, reinterpret_cast<HMENU>(kEditId),
                                    cs->hInstance, nullptr);
        if (!ctx->initial.empty()) {
            SetWindowTextW(ctx->edit, ctx->initial.c_str());
            SendMessageW(ctx->edit, EM_SETSEL, 0, -1);
        }

        int x = 12;
        int y = 70;
        int shown = 0;
        for (size_t i = 0; i < ctx->suggestions.size(); ++i) {
            const auto& suggestion = ctx->suggestions[i];
            if (suggestion.empty() || shown >= 6) continue;
            int w = std::clamp(56 + static_cast<int>(suggestion.size()) * 8, 64, 168);
            if (x + w > 442) {
                x = 12;
                y += 30;
            }
            CreateWindowExW(0, L"BUTTON", suggestion.c_str(),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            x, y, w, 24, hWnd,
                            reinterpret_cast<HMENU>(kSuggestionBaseId + static_cast<int>(i)),
                            cs->hInstance, nullptr);
            x += w + 6;
            ++shown;
        }
        const int buttonY = shown > 0 ? y + 42 : 76;
        const std::wstring createLabel = localization::Text(L"dialog.create_name.create");
        CreateWindowExW(0, L"BUTTON", createLabel.c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        92, buttonY, 80, 28, hWnd, reinterpret_cast<HMENU>(IDOK),
                        cs->hInstance, nullptr);
        const std::wstring saveDialogLabel = localization::Text(L"dialog.create_name.save_dialog");
        CreateWindowExW(0, L"BUTTON", saveDialogLabel.c_str(),
                        WS_CHILD | (ctx->showExplorerButton ? WS_VISIBLE : 0) | WS_TABSTOP,
                        182, buttonY, 150, 28, hWnd, reinterpret_cast<HMENU>(kExplorerId),
                        cs->hInstance, nullptr);
        const std::wstring cancelLabel = localization::Text(L"dialog.save_path.cancel");
        CreateWindowExW(0, L"BUTTON", cancelLabel.c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        ctx->showExplorerButton ? 342 : 232, buttonY, 90, 28,
                        hWnd, reinterpret_cast<HMENU>(IDCANCEL),
                        cs->hInstance, nullptr);
        SetFocus(ctx->edit);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id >= kSuggestionBaseId && id < kSuggestionBaseId + static_cast<int>(ctx ? ctx->suggestions.size() : 0)) {
            const int idx = id - kSuggestionBaseId;
            if (ctx && idx >= 0 && idx < static_cast<int>(ctx->suggestions.size())) {
                SetCreateNameEditText(ctx->edit, ctx->suggestions[static_cast<size_t>(idx)]);
            }
            return 0;
        }
        if (id == IDOK) {
            wchar_t buf[512]{};
            int len = GetWindowTextW(ctx->edit, buf, 511);
            std::wstring name(buf, buf + len);
            name = TrimWhitespace(name);
            if (name.empty()) {
                ShowSoftNotice(hWnd, localization::Text(L"dialog.input.enter_name"),
                               SoftNoticeKind::Warning);
                return 0;
            }
            ctx->result = name;
            ctx->action = PromptCreateNameResult::Create;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == kExplorerId) {
            ctx->action = PromptCreateNameResult::Explorer;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDCANCEL) {
            ctx->action = PromptCreateNameResult::Cancel;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ctx) {
            ctx->action = PromptCreateNameResult::Cancel;
            ctx->done = true;
        }
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

PromptCreateNameResult PromptCreateName(HWND owner,
                                        const std::wstring& title,
                                        const std::wstring& label,
                                        const std::wstring& initial,
                                        const std::vector<std::wstring>& suggestions,
                                        bool showExplorerButton,
                                        std::wstring& out) {
    CreateNameDialog ctx;
    ctx.title = title;
    ctx.label = label;
    ctx.initial = initial;
    ctx.suggestions = suggestions;
    ctx.showExplorerButton = showExplorerButton;
    WNDCLASSW wc{};
    wc.lpfnWndProc = CreateNameDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CreateNameDlg";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 470, suggestions.empty() ? 150 : 210,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return PromptCreateNameResult::Cancel;
    PlaceOwnedPopupAtAppTopLeft(w, owner);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RunDialogMessageLoop(w, &ctx.done);
    if (ctx.action == PromptCreateNameResult::Create) {
        out = ctx.result;
    }
    return ctx.action;
}

struct PasswordOverlayDisabledControl {
    HWND hwnd{};
    BOOL wasEnabled = FALSE;
};

struct PasswordInputDialog {
    HWND anchor{};
    HWND panel{};
    HWND label{};
    HWND edit{};
    HWND okButton{};
    HWND cancelButton{};
    RECT panelRect{};
    std::wstring title;
    std::wstring message;
    std::wstring result;
    DWORD createdMessageTime = 0;
    ULONGLONG createdTick = 0;
    std::vector<PasswordOverlayDisabledControl> disabledAnnotationControls;
    bool ok = false;
    bool done = false;
};

static int PasswordOverlayScale(HWND hwnd, int pxAt96Dpi) {
    HDC hdc = hwnd ? GetDC(hwnd) : nullptr;
    const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc && hwnd) ReleaseDC(hwnd, hdc);
    return MulDiv(pxAt96Dpi, dpi > 0 ? dpi : 96, 96);
}

static bool IsWindowAndAncestorsEnabled(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    for (HWND current = hwnd; current; current = GetParent(current)) {
        if (!IsWindowEnabled(current)) return false;
    }
    return true;
}

static HWND PasswordOverlayAnchor(HWND owner) {
    HWND candidates[] = {
        ResolveAppDialogAnchor(owner),
        g_hMainWnd,
        owner,
        (g_hPdfView && IsWindow(g_hPdfView) && IsWindowVisible(g_hPdfView)) ? g_hPdfView : nullptr,
    };
    for (HWND candidate : candidates) {
        if (IsWindowAndAncestorsEnabled(candidate)) return candidate;
    }
    for (HWND candidate : candidates) {
        if (candidate && IsWindow(candidate)) return candidate;
    }
    return nullptr;
}

static void TracePasswordOverlay(const PasswordInputDialog* ctx,
                                 const wchar_t* event,
                                 const std::wstring& detail = L"") {
    if (!preview_trace::IsEnabled()) return;
    std::wstring line = L"event=" + std::wstring(event ? event : L"(null)") +
        L" anchor=" + preview_trace::Window(ctx ? ctx->anchor : nullptr) +
        L" panel=" + preview_trace::Window(ctx ? ctx->panel : nullptr) +
        L" edit=" + preview_trace::Window(ctx ? ctx->edit : nullptr) +
        L" focus=" + preview_trace::Window(GetFocus()) +
        L" mainVisible=" + preview_trace::Bool(g_hMainWnd && IsWindowVisible(g_hMainWnd)) +
        L" mainIconic=" + preview_trace::Bool(g_hMainWnd && IsIconic(g_hMainWnd));
    if (!detail.empty()) line += L" " + detail;
    preview_trace::Append(L"PasswordOverlay", line);
}

static RECT PasswordOverlayTargetScreenRect(HWND anchor) {
    RECT target{};
    if (g_hPdfView && IsWindow(g_hPdfView) && IsWindowVisible(g_hPdfView) &&
        GetWindowRect(g_hPdfView, &target)) {
        return target;
    }
    if (anchor && IsWindow(anchor) && GetWindowRect(anchor, &target)) {
        return target;
    }
    return {};
}

static bool AcceptPasswordOverlay(PasswordInputDialog* ctx);

static void CancelPasswordOverlay(PasswordInputDialog* ctx, const wchar_t* reason = L"cancel") {
    if (!ctx) return;
    TracePasswordOverlay(ctx, L"cancel", L"reason=" + std::wstring(reason ? reason : L"(null)"));
    ctx->ok = false;
    ctx->done = true;
}

static LRESULT CALLBACK PasswordOverlayPanelProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PasswordInputDialog* ctx = reinterpret_cast<PasswordInputDialog*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND: {
        const WORD id = LOWORD(wParam);
        if (id == IDOK) {
            AcceptPasswordOverlay(ctx);
            return 0;
        }
        if (id == IDCANCEL) {
            CancelPasswordOverlay(ctx);
            return 0;
        }
        break;
    }
    case WM_SETFOCUS:
        if (ctx && ctx->edit && IsWindow(ctx->edit)) {
            SetFocus(ctx->edit);
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        HBRUSH border = CreateSolidBrush(RGB(210, 214, 220));
        if (border) {
            FrameRect(hdc, &rc, border);
            DeleteObject(border);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(32, 32, 32));
        SetBkColor(hdc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void EnsurePasswordOverlayPanelClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = PasswordOverlayPanelProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = L"PasswordOverlayPanel";
    RegisterClassW(&wc);
    registered = true;
}

static void LayoutPasswordOverlay(PasswordInputDialog* ctx) {
    if (!ctx || !ctx->anchor || !IsWindow(ctx->anchor)) return;
    RECT host = PasswordOverlayTargetScreenRect(ctx->anchor);
    const int clientW = static_cast<int>(host.right - host.left);
    const int clientH = static_cast<int>(host.bottom - host.top);
    if (clientW <= 0 || clientH <= 0) return;
    const int margin = PasswordOverlayScale(ctx->anchor, 10);
    const int panelW = std::min(PasswordOverlayScale(ctx->anchor, 392),
                                std::max(PasswordOverlayScale(ctx->anchor, 260),
                                         clientW - margin * 2));
    const int panelH = PasswordOverlayScale(ctx->anchor, 170);
    const int x = static_cast<int>(host.left) + (clientW - panelW) / 2;
    const int y = static_cast<int>(host.top) + (clientH - panelH) / 2;
    ctx->panelRect = { x, y, x + panelW, y + panelH };

    const int pad = PasswordOverlayScale(ctx->anchor, 10);
    const int editH = PasswordOverlayScale(ctx->anchor, 24);
    const int buttonW = PasswordOverlayScale(ctx->anchor, 80);
    const int buttonH = PasswordOverlayScale(ctx->anchor, 26);
    const int buttonGap = PasswordOverlayScale(ctx->anchor, 20);
    const int contentW = panelW - pad * 2;
    const int buttonY = PasswordOverlayScale(ctx->anchor, 100);
    const int buttonsW = buttonW * 2 + buttonGap;
    const int buttonX = (panelW - buttonsW) / 2;

    if (ctx->panel) {
        SetWindowPos(ctx->panel, HWND_TOP, x, y, panelW, panelH,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (ctx->label) {
        SetWindowPos(ctx->label, HWND_TOP, pad, pad, contentW,
                     PasswordOverlayScale(ctx->anchor, 48), SWP_NOACTIVATE);
    }
    if (ctx->edit) {
        SetWindowPos(ctx->edit, HWND_TOP, pad, PasswordOverlayScale(ctx->anchor, 64),
                     contentW, editH, SWP_NOACTIVATE);
    }
    if (ctx->okButton) {
        SetWindowPos(ctx->okButton, HWND_TOP, buttonX, buttonY, buttonW, buttonH, SWP_NOACTIVATE);
    }
    if (ctx->cancelButton) {
        SetWindowPos(ctx->cancelButton, HWND_TOP, buttonX + buttonW + buttonGap, buttonY,
                     buttonW, buttonH, SWP_NOACTIVATE);
    }
}

static bool IsMouseDownMessage(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
    case WM_NCXBUTTONDOWN:
        return true;
    default:
        return false;
    }
}

static bool IsInitialPasswordOverlayMouseMessage(const PasswordInputDialog* ctx,
                                                 const MSG& msg) {
    if (!ctx || !IsMouseDownMessage(msg.message)) return false;
    if (ctx->createdMessageTime != 0 && msg.time <= ctx->createdMessageTime) {
        return true;
    }
    return ctx->createdTick != 0 && (GetTickCount64() - ctx->createdTick) < 150;
}

static bool IsBlockedPasswordOverlayBackgroundInput(const MSG& msg) {
    switch (msg.message) {
    case WM_COMMAND:
    case WM_SYSCOMMAND:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
        return true;
    default:
        return false;
    }
}

static void ClearPasswordOverlayEdit(PasswordInputDialog* ctx) {
    if (!ctx || !ctx->edit || !IsWindow(ctx->edit)) return;
    SendMessageW(ctx->edit, EM_SETSEL, 0, -1);
    SendMessageW(ctx->edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    SetWindowTextW(ctx->edit, L"");
}

static void DestroyPasswordOverlay(PasswordInputDialog* ctx) {
    if (!ctx) return;
    ClearPasswordOverlayEdit(ctx);
    for (auto it = ctx->disabledAnnotationControls.rbegin();
         it != ctx->disabledAnnotationControls.rend(); ++it) {
        if (it->hwnd && IsWindow(it->hwnd)) {
            EnableWindow(it->hwnd, it->wasEnabled);
            InvalidateRect(it->hwnd, nullptr, FALSE);
        }
    }
    ctx->disabledAnnotationControls.clear();
    HWND controls[] = { ctx->cancelButton, ctx->okButton, ctx->edit, ctx->label, ctx->panel };
    for (HWND control : controls) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    ctx->panel = nullptr;
    ctx->label = nullptr;
    ctx->edit = nullptr;
    ctx->okButton = nullptr;
    ctx->cancelButton = nullptr;
    if (ctx->anchor && IsWindow(ctx->anchor)) {
        InvalidateRect(ctx->anchor, nullptr, FALSE);
    }
    if (g_hPdfView && g_hPdfView != ctx->anchor && IsWindow(g_hPdfView)) {
        InvalidateRect(g_hPdfView, nullptr, FALSE);
    }
    TracePasswordOverlay(ctx, L"destroyed");
}

static bool IsPasswordOverlayControl(const PasswordInputDialog* ctx, HWND hwnd) {
    return ctx && hwnd && (hwnd == ctx->panel || IsChild(ctx->panel, hwnd) ||
                           hwnd == ctx->label ||
                           hwnd == ctx->edit || hwnd == ctx->okButton ||
                           hwnd == ctx->cancelButton);
}

static bool IsPasswordOverlayFocusableControl(const PasswordInputDialog* ctx, HWND hwnd) {
    return ctx && hwnd && (hwnd == ctx->panel || hwnd == ctx->edit ||
                           hwnd == ctx->okButton || hwnd == ctx->cancelButton ||
                           IsChild(ctx->panel, hwnd));
}

static bool IsPasswordOverlayPoint(const PasswordInputDialog* ctx, POINT screenPt) {
    if (!ctx) return false;
    return PtInRect(&ctx->panelRect, screenPt) != FALSE;
}

static bool AcceptPasswordOverlay(PasswordInputDialog* ctx) {
    if (!ctx || !ctx->edit) return false;
    const int textLength = GetWindowTextLengthW(ctx->edit);
    std::wstring value;
    if (textLength > 0) {
        value.resize(static_cast<size_t>(textLength) + 1);
        const int copied = GetWindowTextW(ctx->edit, value.data(), textLength + 1);
        value.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    }
    SecureWideStringScope valueScope(&value);
    if (value.empty()) {
        ShowSoftNotice(ctx->anchor,
                       localization::Text(L"dialog.input.enter_password"),
                       SoftNoticeKind::Warning);
        return false;
    }
    SecureClearString(ctx->result);
    ctx->result = std::move(value);
    ClearPasswordOverlayEdit(ctx);
    ctx->ok = true;
    ctx->done = true;
    TracePasswordOverlay(ctx, L"accept");
    return true;
}

static void DisablePasswordOverlayAnnotationControls(PasswordInputDialog* ctx) {
    if (!ctx) return;
    HWND controls[] = {
        g_hAnnotShow,
        g_hBtnModeSelect,
        g_hBtnModePan,
        g_hBtnModeMagnifier,
        g_hBtnModeMarker,
        g_hBtnModeMarkerFree,
        g_hBtnModeMarkerLine,
        g_hBtnModeMarkerArrow,
        g_hBtnModeMarkerWave,
        g_hBtnModeText,
        g_hBtnModeLine,
        g_hBtnModeArrow,
        g_hBtnModeWave,
        g_hBtnModeFreehand,
        g_hBtnModeShape,
        g_hBtnModeEraser,
        g_hComboFont,
        g_hComboFontSize,
        g_hComboFontSizeAlt,
        g_hRadioFontSizeSlotA,
        g_hRadioFontSizeSlotB,
        g_hChkTextReadableBackground,
        g_hRadioTextReadableBackgroundNormal,
        g_hRadioTextReadableBackgroundInverted,
        g_hComboWidth,
        g_hComboMarkerAlpha,
        g_hComboAnnotMethod,
        g_hComboFreehandCorrection,
        g_hComboMarkerTextStyle,
        g_hComboLineDashStyle,
        g_hComboShapeKind,
        g_hComboShapeGeometry,
        g_hComboShapeDrawMode,
        g_hComboMagnifierShape,
        g_hAnnotSettings,
        g_hAnnotClear,
        g_hAnnotList,
        g_hAnnotSummary,
    };
    for (HWND hwnd : controls) {
        if (!hwnd || !IsWindow(hwnd)) continue;
        ctx->disabledAnnotationControls.push_back({ hwnd, IsWindowEnabled(hwnd) });
        EnableWindow(hwnd, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

static bool CreatePasswordOverlay(PasswordInputDialog* ctx, HWND owner) {
    if (!ctx) return false;
    ctx->createdMessageTime = GetMessageTime();
    ctx->createdTick = GetTickCount64();
    ctx->anchor = PasswordOverlayAnchor(owner);
    if (!ctx->anchor) return false;
    TracePasswordOverlay(ctx, L"create_start", L"owner=" + preview_trace::Window(owner));
    EnsurePasswordOverlayPanelClass();

    ctx->panel = CreateWindowExW(WS_EX_TOOLWINDOW, L"PasswordOverlayPanel", L"",
                                 WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                                 0, 0, 1, 1, ctx->anchor, nullptr, g_hInst, nullptr);
    if (!ctx->panel) return false;
    SetWindowLongPtrW(ctx->panel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    ctx->label = CreateWindowExW(0, L"STATIC", ctx->message.c_str(),
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 1, 1, ctx->panel, nullptr, g_hInst, nullptr);
    ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP,
                                0, 0, 1, 1, ctx->panel, reinterpret_cast<HMENU>(101),
                                g_hInst, nullptr);
    ctx->okButton = CreateWindowExW(0, L"BUTTON", L"OK",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    0, 0, 1, 1, ctx->panel, reinterpret_cast<HMENU>(IDOK),
                                    g_hInst, nullptr);
    ctx->cancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        0, 0, 1, 1, ctx->panel, reinterpret_cast<HMENU>(IDCANCEL),
                                        g_hInst, nullptr);
    if (!ctx->label || !ctx->edit || !ctx->okButton || !ctx->cancelButton) {
        DestroyPasswordOverlay(ctx);
        return false;
    }

    HWND fontControls[] = { ctx->label, ctx->edit, ctx->okButton, ctx->cancelButton };
    for (HWND control : fontControls) {
        if (g_hUIFont) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
    }
    SendMessageW(ctx->edit, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'*'), 0);
    DisablePasswordOverlayAnnotationControls(ctx);
    LayoutPasswordOverlay(ctx);
    SetFocus(ctx->edit);
    TracePasswordOverlay(ctx, L"create_end");
    return true;
}

static POINT CurrentMessageScreenPoint() {
    DWORD pos = GetMessagePos();
    return POINT{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
}

static void RunPasswordOverlayMessageLoop(PasswordInputDialog* ctx) {
    if (!ctx || !ctx->anchor) return;
    MSG msg{};
    while (!ctx->done) {
        const BOOL gotMessage = GetMessageW(&msg, nullptr, 0, 0);
        if (gotMessage == -1) return;
        if (gotMessage == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return;
        }
        if (ShouldSkipImeMessageInLoop(msg)) continue;
        if (msg.hwnd == ctx->anchor && msg.message == WM_SIZE) {
            LayoutPasswordOverlay(ctx);
        }
        if (msg.message == WM_ACTIVATEAPP && msg.wParam == FALSE) {
            CancelPasswordOverlay(ctx, L"activate_app_false");
            continue;
        }
        if (msg.message == WM_KILLFOCUS && IsPasswordOverlayFocusableControl(ctx, msg.hwnd)) {
            HWND nextFocus = reinterpret_cast<HWND>(msg.wParam);
            if (!IsPasswordOverlayFocusableControl(ctx, nextFocus)) {
                CancelPasswordOverlay(ctx, L"focus_left_form");
                continue;
            }
        }
        if (msg.message == WM_COMMAND) {
            HWND commandHwnd = reinterpret_cast<HWND>(msg.lParam);
            if (commandHwnd == ctx->okButton) {
                AcceptPasswordOverlay(ctx);
                continue;
            }
            if (commandHwnd == ctx->cancelButton) {
                CancelPasswordOverlay(ctx, L"cancel_button");
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN && msg.hwnd == ctx->edit) {
                AcceptPasswordOverlay(ctx);
                continue;
            }
            if (msg.wParam == VK_ESCAPE &&
                (msg.hwnd == ctx->anchor || IsPasswordOverlayControl(ctx, msg.hwnd))) {
                CancelPasswordOverlay(ctx, L"escape");
                continue;
            }
            if (msg.wParam == VK_TAB && IsPasswordOverlayControl(ctx, msg.hwnd)) {
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                HWND order[] = { ctx->edit, ctx->okButton, ctx->cancelButton };
                int index = 0;
                for (int i = 0; i < 3; ++i) {
                    if (msg.hwnd == order[i]) {
                        index = i;
                        break;
                    }
                }
                index = shift ? (index + 2) % 3 : (index + 1) % 3;
                SetFocus(order[index]);
                continue;
            }
        }
        if (IsMouseDownMessage(msg.message) &&
            !IsPasswordOverlayControl(ctx, msg.hwnd) &&
            IsPasswordOverlayPoint(ctx, CurrentMessageScreenPoint())) {
            continue;
        }
        if (IsMouseDownMessage(msg.message) &&
            !IsPasswordOverlayControl(ctx, msg.hwnd) &&
            !IsPasswordOverlayPoint(ctx, CurrentMessageScreenPoint())) {
            if (IsInitialPasswordOverlayMouseMessage(ctx, msg)) {
                TracePasswordOverlay(ctx, L"ignore_initial_mouse");
                continue;
            }
            // Outside clicks mean cancel.  Consume the click so it cannot also
            // trigger a file-open command behind the password form.
            CancelPasswordOverlay(ctx, L"outside_mouse");
            continue;
        }
        if (!IsPasswordOverlayControl(ctx, msg.hwnd) &&
            msg.hwnd != ctx->anchor &&
            IsBlockedPasswordOverlayBackgroundInput(msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool PromptPasswordText(HWND owner, const std::wstring& title,
                        const std::wstring& message, std::wstring& out) {
    PasswordInputDialog ctx;
    ctx.title = title;
    ctx.message = message;
    HWND previousFocus = GetFocus();
    if (!CreatePasswordOverlay(&ctx, owner)) return false;
    RunPasswordOverlayMessageLoop(&ctx);
    DestroyPasswordOverlay(&ctx);
    if (previousFocus && IsWindow(previousFocus)) {
        SetFocus(previousFocus);
    }
    if (ctx.ok) {
        SecureClearString(out);
        out = ctx.result;
        SecureClearString(ctx.result);
        return true;
    }
    SecureClearString(ctx.result);
    return false;
}

struct SelectPathDialog {
    HWND hwnd{};
    HWND label{};
    HWND list{};
    std::wstring title;
    std::wstring message;
    std::vector<std::wstring> paths;
    std::wstring initialPath;
    std::wstring result;
    bool ok = false;
    bool done = false;
};

static bool AcceptSelectPathDialogSelection(SelectPathDialog* ctx, HWND hWnd) {
    if (!ctx || !ctx->list) return false;
    int sel = static_cast<int>(SendMessageW(ctx->list, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(ctx->paths.size())) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"dialog.select_path.remove_required"),
                       SoftNoticeKind::Warning);
        return false;
    }
    ctx->result = ctx->paths[static_cast<size_t>(sel)];
    ctx->ok = true;
    ctx->done = true;
    DestroyWindow(hWnd);
    return true;
}

static LRESULT CALLBACK SelectPathDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SelectPathDialog* ctx = reinterpret_cast<SelectPathDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<SelectPathDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;

        const int margin = 10;
        const int width = 620;
        const int labelH = 36;
        const int buttonW = 80;
        const int buttonH = 26;
        const int buttonY = 248;
        const int listY = margin + labelH + 8;
        const int listH = 184;
        ctx->label = CreateWindowExW(0, L"STATIC", ctx->message.c_str(),
                                     WS_CHILD | WS_VISIBLE,
                                     margin, margin, width - margin * 2, labelH,
                                     hWnd, nullptr, cs->hInstance, nullptr);
        ctx->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                    margin, listY, width - margin * 2, listH,
                                    hWnd, reinterpret_cast<HMENU>(101),
                                    cs->hInstance, nullptr);
        for (const auto& path : ctx->paths) {
            SendMessageW(ctx->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
        }
        int initialIndex = 0;
        for (size_t i = 0; i < ctx->paths.size(); ++i) {
            if (ctx->paths[i] == ctx->initialPath) {
                initialIndex = static_cast<int>(i);
                break;
            }
        }
        if (ctx->list && !ctx->paths.empty()) {
            SendMessageW(ctx->list, LB_SETCURSEL, static_cast<WPARAM>(initialIndex), 0);
            SetFocus(ctx->list);
        }

        CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        370, buttonY, buttonW, buttonH, hWnd, reinterpret_cast<HMENU>(IDOK),
                        cs->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        470, buttonY, buttonW, buttonH, hWnd, reinterpret_cast<HMENU>(IDCANCEL),
                        cs->hInstance, nullptr);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND: {
        if (!ctx) break;
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        if (id == IDOK) {
            AcceptSelectPathDialogSelection(ctx, hWnd);
            return 0;
        }
        if (id == IDCANCEL) {
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == 101 && code == LBN_DBLCLK) {
            AcceptSelectPathDialogSelection(ctx, hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ctx) ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool PromptSelectPath(HWND owner, const std::wstring& title,
                      const std::wstring& message,
                      const std::vector<std::wstring>& paths,
                      const std::wstring& initialPath, std::wstring& outPath) {
    if (paths.empty()) return false;

    SelectPathDialog ctx;
    ctx.title = title;
    ctx.message = message;
    ctx.paths = paths;
    ctx.initialPath = initialPath;

    WNDCLASSW wc{};
    wc.lpfnWndProc = SelectPathDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SelectPathDlg";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 620, 320,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return false;
    PlaceOwnedPopupAtAppTopLeft(w, owner);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RunDialogMessageLoop(w, &ctx.done);
    if (ctx.ok) {
        outPath = ctx.result;
        return true;
    }
    return false;
}

struct RestoreBackupDialogCtx {
    HWND hwnd{};
    HWND label{};
    HWND list{};
    std::filesystem::path backupRoot;
    std::vector<std::filesystem::path> metaPaths;
    std::filesystem::path result;
    bool ok = false;
    bool done = false;
};

static std::wstring FormatFileTime(const std::filesystem::file_time_type& ft) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t c_time = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &c_time);
#else
    localtime_r(&c_time, &tm_buf);
#endif
    wchar_t buf[100];
    std::wcsftime(buf, std::size(buf), L"%Y/%m/%d %H:%M:%S", &tm_buf);
    return buf;
}

static std::wstring ExtractDestFromMeta(const std::filesystem::path& metaPath) {
    std::ifstream ifs(metaPath);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.rfind("dest=", 0) == 0) {
            return UTF8ToWide(line.substr(5));
        }
    }
    return L"";
}

static LRESULT CALLBACK RestoreBackupDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RestoreBackupDialogCtx* ctx = reinterpret_cast<RestoreBackupDialogCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<RestoreBackupDialogCtx*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;

        const int margin = 10;
        const int width = 640;
        const int labelH = 50;
        const int btnH = 30;
        const int btnW = 120;
        const int btnY = 310;
        const int listY = margin + labelH + 8;
        const int listH = 220;

        std::wstring msgText = localization::Text(L"dialog.common.eecad8e36e0c");

        ctx->label = CreateWindowExW(0, L"STATIC", msgText.c_str(),
                                     WS_CHILD | WS_VISIBLE,
                                     margin, margin, width - margin * 2, labelH,
                                     hWnd, nullptr, cs->hInstance, nullptr);
        ctx->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                    margin, listY, width - margin * 2, listH,
                                    hWnd, reinterpret_cast<HMENU>(101),
                                    cs->hInstance, nullptr);

        struct BackupEntry {
            std::filesystem::path metaPath;
            std::filesystem::file_time_type ftime;
            std::wstring destPath;
        };
        std::vector<BackupEntry> entries;
        std::error_code ec;
        if (std::filesystem::exists(ctx->backupRoot, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(ctx->backupRoot, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec) || ec) continue;
                if (entry.path().extension() == L".txt" && entry.path().wstring().find(L".meta.txt") != std::wstring::npos) {
                    BackupEntry b;
                    b.metaPath = entry.path();
                    b.ftime = std::filesystem::last_write_time(entry.path(), ec);
                    b.destPath = ExtractDestFromMeta(entry.path());
                    entries.push_back(b);
                }
            }
        }
        std::sort(entries.begin(), entries.end(), [](const BackupEntry& a, const BackupEntry& b) {
            return a.ftime > b.ftime;
        });

        for (const auto& e : entries) {
            std::wstring display = L"[" + FormatFileTime(e.ftime) + L"] ";
            if (!e.destPath.empty()) {
                display += std::filesystem::path(e.destPath).filename().wstring();
            } else {
                display += e.metaPath.filename().wstring();
            }
            ctx->metaPaths.push_back(e.metaPath);
            SendMessageW(ctx->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        }
        if (!entries.empty()) {
            SendMessageW(ctx->list, LB_SETCURSEL, 0, 0);
            SetFocus(ctx->list);
        }

        std::wstring openFolderTxt = localization::Text(L"dialog.common.78f2c08a8845");
        std::wstring restoreTxt = localization::Text(L"dialog.common.575a7e91c663");
        std::wstring cancelTxt = localization::Text(L"dialog.common.3672b0b92134");

        CreateWindowExW(0, L"BUTTON", openFolderTxt.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        margin, btnY, 150, btnH, hWnd, reinterpret_cast<HMENU>(102), cs->hInstance, nullptr);
        
        CreateWindowExW(0, L"BUTTON", restoreTxt.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        width - margin - 2*btnW - 10, btnY, btnW, btnH, hWnd, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
        
        CreateWindowExW(0, L"BUTTON", cancelTxt.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        width - margin - btnW, btnY, btnW, btnH, hWnd, reinterpret_cast<HMENU>(IDCANCEL), cs->hInstance, nullptr);
        
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND: {
        if (!ctx) break;
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        if (id == 102) {
            ShellExecuteW(nullptr, L"open", ctx->backupRoot.wstring().c_str(), nullptr, nullptr, SW_SHOW);
            return 0;
        }
        if (id == IDOK || (id == 101 && code == LBN_DBLCLK)) {
            int sel = static_cast<int>(SendMessageW(ctx->list, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(ctx->metaPaths.size())) {
                ctx->result = ctx->metaPaths[sel];
                ctx->ok = true;
                ctx->done = true;
                DestroyWindow(hWnd);
            }
            return 0;
        }
        if (id == IDCANCEL) {
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ctx) ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool PromptRestoreBackupList(HWND owner, const std::filesystem::path& backupRoot, std::filesystem::path& outPickedMeta) {
    RestoreBackupDialogCtx ctx;
    ctx.backupRoot = backupRoot;

    std::wstring title = localization::Text(L"dialog.common.95d9abd66237");

    WNDCLASSW wc{};
    wc.lpfnWndProc = RestoreBackupDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RestoreBackupDlg";
    RegisterClassW(&wc);

    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 640, 400,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) return false;
    PlaceOwnedPopupAtAppTopLeft(w, owner);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RunDialogMessageLoop(w, &ctx.done);
    if (ctx.ok) {
        outPickedMeta = ctx.result;
        return true;
    }
    return false;
}

namespace {
constexpr int kSilentDialogIdMessage = 5101;
constexpr int kSilentDialogIdButton1 = 5102;
constexpr int kSilentDialogIdButton2 = 5103;
constexpr int kSilentDialogIdButton3 = 5104;
constexpr int kSilentDialogIdButton4 = 5105;
constexpr int kSilentDialogIdPathBase = 5200;
constexpr int kSilentDialogIdCopyPathBase = 5300;
constexpr size_t kSilentDialogMaxPaths = 3;

struct SilentDialogButtonSpec {
    int id = 0;
    SilentDialogResult result = SilentDialogResult::None;
    const wchar_t* label = L"";
};

struct SilentDialogState {
    SilentDialogOptions options;
    HWND hwnd{};
    HWND owner{};
    HWND labelKind{};
    HWND editMessage{};
    HWND button1{};
    HWND button2{};
    HWND button3{};
    HWND button4{};
    std::vector<HWND> pathLabels;
    std::vector<HWND> pathButtons;
    std::vector<HWND> copyPathButtons;
    std::vector<bool> expandedPaths;
    std::wstring defaultOkLabel;
    std::wstring defaultCancelLabel;
    std::wstring defaultYesLabel;
    std::wstring defaultNoLabel;
    SilentDialogButtonSpec buttonSpecs[4]{};
    int buttonCount = 0;
    SilentDialogResult result = SilentDialogResult::None;
    bool done = false;
    bool ownerWasEnabled = false;
};

static std::wstring NormalizeNewlinesForEditControl(const std::wstring& text) {
    // Windows multiline EDIT control uses CRLF as its newline separator.
    // Normalize here so call sites can freely compose messages with '\n'.
    std::wstring out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        if (ch == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            out += L"\r\n";
            continue;
        }
        if (ch == L'\n') {
            out += L"\r\n";
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

static std::wstring NormalizeNewlinesForDrawText(const std::wstring& text) {
    // DrawTextW understands '\n' for line breaks; strip '\r' to keep measurement consistent.
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L'\r') continue;
        out.push_back(ch);
    }
    return out;
}

static std::wstring CompactDiagnosticPath(const std::wstring& path) {
    constexpr size_t kMaxVisibleChars = 72;
    constexpr size_t kPrefixChars = 20;
    constexpr size_t kSuffixChars = 48;
    if (path.size() <= kMaxVisibleChars) return path;
    return path.substr(0, kPrefixChars) + L"…" + path.substr(path.size() - kSuffixChars);
}

static bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    CloseClipboard();
    return true;
}

static void AddPathToolTip(HWND dialog, HWND control, const std::wstring& path) {
    if (!dialog || !control || path.empty()) return;
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   dialog, nullptr, g_hInst, nullptr);
    if (!tooltip) return;
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = dialog;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    tool.lpszText = const_cast<wchar_t*>(path.c_str());
    if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
        DestroyWindow(tooltip);
    }
}

static int DialogScale(HWND hWnd, int pxAt96Dpi) {
    HDC dc = GetDC(hWnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hWnd, dc);
    if (dpi <= 0) dpi = 96;
    return MulDiv(pxAt96Dpi, dpi, 96);
}

static COLORREF BlendDialogColor(COLORREF a, COLORREF b, double t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = static_cast<int>(std::lround(ar + (br - ar) * t));
    int g = static_cast<int>(std::lround(ag + (bg - ag) * t));
    int b2 = static_cast<int>(std::lround(ab + (bb - ab) * t));
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b2, 0, 255));
}

static COLORREF SilentDialogAccentColor(SoftNoticeKind kind) {
    switch (kind) {
    case SoftNoticeKind::Warning:
        return BlendDialogColor(g_theme.accent, RGB(214, 144, 24), 0.75);
    case SoftNoticeKind::Error:
        return BlendDialogColor(g_theme.accent, RGB(196, 64, 64), 0.85);
    case SoftNoticeKind::Info:
    default:
        return g_theme.accent;
    }
}

static std::wstring SilentDialogKindLabel(const SilentDialogOptions& options) {
    if (options.buttons != SilentDialogButtons::Ok) {
        return localization::Text(L"dialog.kind.confirmation");
    }
    switch (options.kind) {
    case SoftNoticeKind::Warning:
        return localization::Text(L"dialog.kind.warning");
    case SoftNoticeKind::Error:
        return localization::Text(L"dialog.kind.error");
    case SoftNoticeKind::Info:
    default:
        return localization::Text(L"dialog.kind.information");
    }
}

static SilentDialogResult SilentDialogDefaultResult(const SilentDialogOptions& options) {
    if (options.defaultResult != SilentDialogResult::None) return options.defaultResult;
    switch (options.buttons) {
    case SilentDialogButtons::Ok:
    case SilentDialogButtons::OkCancel:
        return SilentDialogResult::Ok;
    case SilentDialogButtons::OkYesNoCancel:
    case SilentDialogButtons::YesNo:
    case SilentDialogButtons::YesNoCancel:
        return SilentDialogResult::Yes;
    default:
        return SilentDialogResult::Ok;
    }
}

static SilentDialogResult SilentDialogEscapeResult(const SilentDialogOptions& options) {
    if (options.escapeResult != SilentDialogResult::None) return options.escapeResult;
    switch (options.buttons) {
    case SilentDialogButtons::Ok:
        return SilentDialogResult::Ok;
    case SilentDialogButtons::OkCancel:
    case SilentDialogButtons::OkYesNoCancel:
    case SilentDialogButtons::YesNoCancel:
        return SilentDialogResult::Cancel;
    case SilentDialogButtons::YesNo:
        return SilentDialogResult::No;
    default:
        return SilentDialogResult::Cancel;
    }
}

static int SilentDialogButtonIdForResult(const SilentDialogState* ctx, SilentDialogResult result) {
    if (!ctx) return 0;
    for (int i = 0; i < ctx->buttonCount; ++i) {
        if (ctx->buttonSpecs[i].result == result) return ctx->buttonSpecs[i].id;
    }
    return 0;
}

static void ResolveSilentDialogButtons(SilentDialogState* ctx) {
    if (!ctx) return;
    ctx->defaultOkLabel = localization::Text(L"dialog.button.ok");
    ctx->defaultCancelLabel = localization::Text(L"dialog.button.cancel");
    ctx->defaultYesLabel = localization::Text(L"dialog.button.yes");
    ctx->defaultNoLabel = localization::Text(L"dialog.button.no");
    const wchar_t* ok = ctx->options.okLabel.empty() ? ctx->defaultOkLabel.c_str() : ctx->options.okLabel.c_str();
    const wchar_t* cancel = ctx->options.cancelLabel.empty() ? ctx->defaultCancelLabel.c_str() : ctx->options.cancelLabel.c_str();
    const wchar_t* yes = ctx->options.yesLabel.empty() ? ctx->defaultYesLabel.c_str() : ctx->options.yesLabel.c_str();
    const wchar_t* no = ctx->options.noLabel.empty() ? ctx->defaultNoLabel.c_str() : ctx->options.noLabel.c_str();
    switch (ctx->options.buttons) {
    case SilentDialogButtons::Ok:
        ctx->buttonSpecs[0] = { kSilentDialogIdButton1, SilentDialogResult::Ok, ok };
        ctx->buttonCount = 1;
        break;
    case SilentDialogButtons::OkCancel:
        ctx->buttonSpecs[0] = { kSilentDialogIdButton1, SilentDialogResult::Ok, ok };
        ctx->buttonSpecs[1] = { kSilentDialogIdButton2, SilentDialogResult::Cancel, cancel };
        ctx->buttonCount = 2;
        break;
    case SilentDialogButtons::YesNo:
        ctx->buttonSpecs[0] = { kSilentDialogIdButton1, SilentDialogResult::Yes, yes };
        ctx->buttonSpecs[1] = { kSilentDialogIdButton2, SilentDialogResult::No, no };
        ctx->buttonCount = 2;
        break;
    case SilentDialogButtons::YesNoCancel:
        ctx->buttonSpecs[0] = { kSilentDialogIdButton1, SilentDialogResult::Yes, yes };
        ctx->buttonSpecs[1] = { kSilentDialogIdButton2, SilentDialogResult::No, no };
        ctx->buttonSpecs[2] = { kSilentDialogIdButton3, SilentDialogResult::Cancel, cancel };
        ctx->buttonCount = 3;
        break;
    case SilentDialogButtons::OkYesNoCancel:
        ctx->buttonSpecs[0] = { kSilentDialogIdButton1, SilentDialogResult::Ok, ok };
        ctx->buttonSpecs[1] = { kSilentDialogIdButton2, SilentDialogResult::Yes, yes };
        ctx->buttonSpecs[2] = { kSilentDialogIdButton3, SilentDialogResult::No, no };
        ctx->buttonSpecs[3] = { kSilentDialogIdButton4, SilentDialogResult::Cancel, cancel };
        ctx->buttonCount = 4;
        break;
    }
}

static SIZE MeasureSilentDialogMessage(HWND owner, const std::wstring& message, int widthPx) {
    SIZE size{};
    std::wstring normalized = NormalizeNewlinesForDrawText(message);
    HWND anchor = owner ? owner : GetDesktopWindow();
    HDC hdc = GetDC(anchor);
    HFONT oldFont = nullptr;
    if (hdc && g_hUIFont) oldFont = static_cast<HFONT>(SelectObject(hdc, g_hUIFont));
    RECT rc{ 0, 0, std::max(120, widthPx), 0 };
    DrawTextW(hdc, normalized.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
    if (hdc && oldFont) SelectObject(hdc, oldFont);
    if (hdc) ReleaseDC(anchor, hdc);
    size.cx = rc.right - rc.left;
    size.cy = rc.bottom - rc.top;
    return size;
}

static HWND ResolveSilentDialogOwner(HWND owner) {
    if (!owner) return nullptr;
    HWND root = GetAncestor(owner, GA_ROOT);
    return root ? root : owner;
}

static void PlaceSilentDialogWindow(HWND hwnd, HWND owner, SilentDialogPlacement placement) {
    if (!hwnd) return;

    RECT dialogRect{};
    if (!GetWindowRect(hwnd, &dialogRect)) return;
    const int dialogW = dialogRect.right - dialogRect.left;
    const int dialogH = dialogRect.bottom - dialogRect.top;

    HWND anchor = (owner && IsWindow(owner)) ? owner : GetDesktopWindow();
    RECT anchorRect{};
    if (!GetWindowRect(anchor, &anchorRect)) {
        anchorRect = dialogRect;
    }

    HMONITOR monitor = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT workRect = anchorRect;
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        workRect = mi.rcWork;
    }

    const int offset = DialogScale(hwnd, 24);
    int x = static_cast<int>(anchorRect.left + ((anchorRect.right - anchorRect.left) - dialogW) / 2);
    int y = static_cast<int>(anchorRect.top + ((anchorRect.bottom - anchorRect.top) - dialogH) / 2);
    switch (placement) {
    case SilentDialogPlacement::OwnerLowerLeft:
        x = anchorRect.left + offset;
        y = anchorRect.bottom - dialogH - offset;
        break;
    case SilentDialogPlacement::OwnerUpperLeft:
        x = anchorRect.left + offset;
        y = anchorRect.top + offset;
        break;
    case SilentDialogPlacement::CenterOwner:
    default:
        break;
    }
    const int minX = static_cast<int>(workRect.left);
    const int minY = static_cast<int>(workRect.top);
    const int maxX = static_cast<int>(std::max<LONG>(workRect.left, workRect.right - dialogW));
    const int maxY = static_cast<int>(std::max<LONG>(workRect.top, workRect.bottom - dialogH));
    x = std::clamp(x, minX, maxX);
    y = std::clamp(y, minY, maxY);
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void CloseSilentDialog(SilentDialogState* ctx, SilentDialogResult result) {
    if (!ctx) return;
    ctx->result = result;
    ctx->done = true;
    if (ctx->hwnd) DestroyWindow(ctx->hwnd);
}

static LRESULT CALLBACK SilentDialogEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR idSubclass, DWORD_PTR refData) {
    auto* ctx = reinterpret_cast<SilentDialogState*>(refData);
    if (msg == WM_KEYDOWN) {
        MSG edgeNavMsg{};
        edgeNavMsg.hwnd = hWnd;
        edgeNavMsg.message = msg;
        edgeNavMsg.wParam = wParam;
        edgeNavMsg.lParam = lParam;
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(edgeNavMsg)) return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        if (ctx && ctx->hwnd) {
            int id = SilentDialogButtonIdForResult(ctx, SilentDialogDefaultResult(ctx->options));
            if (id != 0) SendMessageW(ctx->hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (ctx && ctx->hwnd) {
            int id = SilentDialogButtonIdForResult(ctx, SilentDialogEscapeResult(ctx->options));
            if (id != 0) SendMessageW(ctx->hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
        }
        return 0;
    }
    if (msg == WM_CHAR && (wParam == L'\r' || wParam == 27)) {
        return 0;
    }
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && (wParam == L'A' || wParam == L'a')) {
        SendMessageW(hWnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static LRESULT CALLBACK SilentDialogPathProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR idSubclass, DWORD_PTR refData) {
    auto* ctx = reinterpret_cast<SilentDialogState*>(refData);
    if (!ctx || !ctx->hwnd) return DefSubclassProc(hWnd, msg, wParam, lParam);
    const int pathId = kSilentDialogIdPathBase + static_cast<int>(idSubclass);
    if (msg == WM_LBUTTONUP ||
        (msg == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_SPACE))) {
        SendMessageW(ctx->hwnd, WM_COMMAND, MAKEWPARAM(pathId, BN_CLICKED),
                     reinterpret_cast<LPARAM>(hWnd));
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static LRESULT CALLBACK SilentDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SilentDialogState* ctx = reinterpret_cast<SilentDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<SilentDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hwnd = hWnd;
        ResolveSilentDialogButtons(ctx);

        const int margin = DialogScale(hWnd, 12);
        const int labelH = DialogScale(hWnd, 20);
        const int buttonW = DialogScale(hWnd, ctx->buttonCount >= 4 ? 112 : 160);
        const int buttonH = DialogScale(hWnd, 28);
        const int buttonGap = DialogScale(hWnd, 10);
        const int pathLabelH = DialogScale(hWnd, 18);
        const int pathControlH = DialogScale(hWnd, 24);
        const int pathRowH = pathLabelH + pathControlH + DialogScale(hWnd, 8);
        const size_t pathCount = std::min(ctx->options.paths.size(), kSilentDialogMaxPaths);

        RECT client{};
        GetClientRect(hWnd, &client);
        int clientW = client.right - client.left;
        int clientH = client.bottom - client.top;
        int buttonsY = clientH - margin - buttonH;
        int messageTop = margin + labelH + DialogScale(hWnd, 8);
        int pathAreaH = static_cast<int>(pathCount) * pathRowH;
        int messageH = std::max(DialogScale(hWnd, 96),
                                buttonsY - messageTop - pathAreaH - DialogScale(hWnd, 10));

        const std::wstring kindLabel = SilentDialogKindLabel(ctx->options);
        ctx->labelKind = CreateWindowExW(0, L"STATIC", kindLabel.c_str(),
                                         WS_CHILD | WS_VISIBLE,
                                         margin, margin, clientW - margin * 2, labelH,
                                         hWnd, nullptr, cs->hInstance, nullptr);
        ctx->editMessage = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctx->options.message.c_str(),
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                               ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                           margin, messageTop, clientW - margin * 2, messageH,
                                           hWnd, reinterpret_cast<HMENU>(kSilentDialogIdMessage),
                                           cs->hInstance, nullptr);
        if (ctx->editMessage) {
            SetWindowSubclass(ctx->editMessage, SilentDialogEditProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
            SendMessageW(ctx->editMessage, EM_SETSEL, 0, 0);
        }

        ctx->pathLabels.reserve(pathCount);
        ctx->pathButtons.reserve(pathCount);
        ctx->copyPathButtons.reserve(pathCount);
        ctx->expandedPaths.assign(pathCount, false);
        const int pathTop = messageTop + messageH + DialogScale(hWnd, 8);
        const int copyButtonW = DialogScale(hWnd, 76);
        const int pathButtonW = std::max(DialogScale(hWnd, 140),
                                         clientW - margin * 2 - copyButtonW - DialogScale(hWnd, 8));
        for (size_t i = 0; i < pathCount; ++i) {
            const auto& path = ctx->options.paths[i];
            const int rowTop = pathTop + static_cast<int>(i) * pathRowH;
            const std::wstring label = path.label.empty()
                ? localization::Text(L"dialog.path.label")
                : path.label;
            HWND pathLabel = CreateWindowExW(0, L"STATIC", label.c_str(), WS_CHILD | WS_VISIBLE,
                                              margin, rowTop, clientW - margin * 2, pathLabelH,
                                              hWnd, nullptr, cs->hInstance, nullptr);
            HWND pathButton = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", CompactDiagnosticPath(path.value).c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                                               margin, rowTop + pathLabelH, pathButtonW, pathControlH,
                                               hWnd, reinterpret_cast<HMENU>(kSilentDialogIdPathBase + i),
                                               cs->hInstance, nullptr);
            HWND copyButton = CreateWindowExW(0, L"BUTTON", localization::Text(L"dialog.path.copy").c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               margin + pathButtonW + DialogScale(hWnd, 8),
                                               rowTop + pathLabelH, copyButtonW, pathControlH,
                                               hWnd, reinterpret_cast<HMENU>(kSilentDialogIdCopyPathBase + i),
                                               cs->hInstance, nullptr);
            ctx->pathLabels.push_back(pathLabel);
            ctx->pathButtons.push_back(pathButton);
            ctx->copyPathButtons.push_back(copyButton);
            if (pathButton) {
                SetWindowSubclass(pathButton, SilentDialogPathProc, static_cast<UINT_PTR>(i),
                                  reinterpret_cast<DWORD_PTR>(ctx));
            }
            AddPathToolTip(hWnd, pathButton, path.value);
        }

        int totalButtonsW = (buttonW * ctx->buttonCount) + (buttonGap * std::max(0, ctx->buttonCount - 1));
        int startX = clientW - margin - totalButtonsW;
        HWND* handles[] = { &ctx->button1, &ctx->button2, &ctx->button3, &ctx->button4 };
        int defaultId = SilentDialogButtonIdForResult(ctx, SilentDialogDefaultResult(ctx->options));
        for (int i = 0; i < ctx->buttonCount; ++i) {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                          ((ctx->buttonSpecs[i].id == defaultId) ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
            *handles[i] = CreateWindowExW(0, L"BUTTON", ctx->buttonSpecs[i].label,
                                          style,
                                          startX + i * (buttonW + buttonGap), buttonsY, buttonW, buttonH,
                                          hWnd, reinterpret_cast<HMENU>(ctx->buttonSpecs[i].id),
                                          cs->hInstance, nullptr);
        }

        auto applyFont = [&](HWND child) {
            if (child && g_hUIFont) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
        };
        applyFont(ctx->labelKind);
        applyFont(ctx->editMessage);
        applyFont(ctx->button1);
        applyFont(ctx->button2);
        applyFont(ctx->button3);
        applyFont(ctx->button4);
        for (HWND control : ctx->pathLabels) applyFont(control);
        for (HWND control : ctx->pathButtons) applyFont(control);
        for (HWND control : ctx->copyPathButtons) applyFont(control);

        HWND focus = GetDlgItem(hWnd, defaultId);
        if (focus) SetFocus(focus);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        if (ctx && ctl == ctx->labelKind) {
            SetTextColor(hdc, SilentDialogAccentColor(ctx->options.kind));
            SetBkColor(hdc, g_theme.panelBg);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(g_hThemePanelBrush ? g_hThemePanelBrush : GetSysColorBrush(COLOR_WINDOW));
        }
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (!ctx) break;
        if (id >= kSilentDialogIdPathBase &&
            id < kSilentDialogIdPathBase + static_cast<int>(ctx->pathButtons.size())) {
            const size_t index = static_cast<size_t>(id - kSilentDialogIdPathBase);
            ctx->expandedPaths[index] = !ctx->expandedPaths[index];
            const std::wstring& path = ctx->options.paths[index].value;
            SetWindowTextW(ctx->pathButtons[index],
                           (ctx->expandedPaths[index] ? path : CompactDiagnosticPath(path)).c_str());
            return 0;
        }
        if (id >= kSilentDialogIdCopyPathBase &&
            id < kSilentDialogIdCopyPathBase + static_cast<int>(ctx->copyPathButtons.size())) {
            const size_t index = static_cast<size_t>(id - kSilentDialogIdCopyPathBase);
            if (CopyTextToClipboard(hWnd, ctx->options.paths[index].value)) {
                ShowSoftNotice(hWnd, localization::Text(L"dialog.path.copied"), SoftNoticeKind::Info);
            } else {
                ShowSoftNotice(hWnd, localization::Text(L"dialog.path.copy_failed"), SoftNoticeKind::Warning);
            }
            return 0;
        }
        if (id == IDCANCEL) {
            CloseSilentDialog(ctx, SilentDialogEscapeResult(ctx->options));
            return 0;
        }
        if (id == IDOK) {
            CloseSilentDialog(ctx, SilentDialogDefaultResult(ctx->options));
            return 0;
        }
        for (int i = 0; i < ctx->buttonCount; ++i) {
            if (ctx->buttonSpecs[i].id == id) {
                CloseSilentDialog(ctx, ctx->buttonSpecs[i].result);
                return 0;
            }
        }
        break;
    }
    case WM_CLOSE:
        if (ctx) {
            CloseSilentDialog(ctx, SilentDialogEscapeResult(ctx->options));
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
} // namespace

SilentDialogResult ShowSilentDialog(HWND owner, const SilentDialogOptions& options) {
    SilentDialogState ctx;
    ctx.options = options;
    ctx.owner = ResolveSilentDialogOwner(owner);
    if (ctx.options.title.empty()) {
        ctx.options.title = localization::Text(L"dialog.title.notice");
    }
    ctx.options.message = NormalizeNewlinesForEditControl(ctx.options.message);
    ctx.options.paths.erase(
        std::remove_if(ctx.options.paths.begin(), ctx.options.paths.end(),
                       [](const SilentDialogPath& path) { return path.value.empty(); }),
        ctx.options.paths.end());

    const HWND anchor = ctx.owner ? ctx.owner : GetDesktopWindow();
    const int baseWidth = (ctx.options.preferredWidthPx > 0) ? ctx.options.preferredWidthPx : 560;
    const int width = std::clamp(DialogScale(anchor, baseWidth), DialogScale(anchor, 360), DialogScale(anchor, 760));
    const int margin = DialogScale(anchor, 12);
    const int labelH = DialogScale(anchor, 20);
    const int buttonH = DialogScale(anchor, 28);
    const int buttonBandH = buttonH + DialogScale(anchor, 18);
    const int pathRowH = DialogScale(anchor, 50);
    const int pathAreaH = static_cast<int>(std::min(ctx.options.paths.size(), kSilentDialogMaxPaths)) * pathRowH;
    const int textWidth = std::max(DialogScale(anchor, 220), width - margin * 2 - DialogScale(anchor, 8));
    const SIZE measured = MeasureSilentDialogMessage(anchor, ctx.options.message, textWidth);
    const int messageH = std::clamp(static_cast<int>(measured.cy) + DialogScale(anchor, 20),
                                    DialogScale(anchor, 96),
                                    DialogScale(anchor, 280));
    // CreateWindowExW receives an outer-window height.  Reserve the non-client
    // area so the minimum message area and path rows do not overlap the buttons.
    const int nonClientHeight = std::max(0, GetSystemMetrics(SM_CYCAPTION)) +
                                std::max(0, GetSystemMetrics(SM_CYDLGFRAME)) * 2;
    const int height = margin + labelH + DialogScale(anchor, 8) + messageH +
                       DialogScale(anchor, 8) + pathAreaH + buttonBandH + margin + nonClientHeight;

    WNDCLASSW wc{};
    wc.lpfnWndProc = SilentDialogProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SilentDialogClass";
    RegisterClassW(&wc);

    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                             wc.lpszClassName, ctx.options.title.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                             ctx.owner, nullptr, g_hInst, &ctx);
    if (!w) return SilentDialogResult::None;
    PlaceSilentDialogWindow(w, ctx.owner, ctx.options.placement);
    ctx.ownerWasEnabled = ctx.owner && IsWindow(ctx.owner) && IsWindowEnabled(ctx.owner);
    if (ctx.ownerWasEnabled) {
        EnableWindow(ctx.owner, FALSE);
    }
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    SetActiveWindow(w);

    RunDialogMessageLoop(w, &ctx.done);
    if (!ctx.done) {
        ctx.result = SilentDialogEscapeResult(ctx.options);
    }
    if (ctx.ownerWasEnabled && ctx.owner && IsWindow(ctx.owner)) {
        EnableWindow(ctx.owner, TRUE);
        SetActiveWindow(ctx.owner);
    }
    return ctx.result;
}

void ShowSilentMessageDialog(HWND owner, const std::wstring& title, const std::wstring& message,
                             SoftNoticeKind kind, const std::vector<SilentDialogPath>& paths) {
    if (ShouldSuppressRepeatedUiMessage(g_silentMessageDialogRepeatState, title, message, kind,
                                        kSilentMessageDialogRepeatSuppressMs)) {
        return;
    }
    const bool offerAbnormalExit = CanRequestManagedAbnormalExitFromDialog(owner, kind);
    SilentDialogOptions options;
    options.title = title;
    options.message = message;
    options.kind = kind;
    options.paths = paths;
    options.buttons = offerAbnormalExit ? SilentDialogButtons::OkCancel : SilentDialogButtons::Ok;
    options.okLabel = offerAbnormalExit
        ? (localization::Text(L"dialog.common.603bc62f3f34"))
        : std::wstring();
    options.cancelLabel = offerAbnormalExit
        ? (localization::Text(L"dialog.common.fea723590aa2"))
        : std::wstring();
    options.defaultResult = SilentDialogResult::Ok;
    options.escapeResult = SilentDialogResult::Ok;
    const SilentDialogResult result = ShowSilentDialog(owner, options);
    if (offerAbnormalExit && result == SilentDialogResult::Cancel) {
        RequestManagedAbnormalExitFromDialog(owner, title, message);
    }
}

bool TryParseZoomScale(const std::wstring& rawInput, double* outScale) {
    if (!outScale) return false;
    std::wstring s = TrimWhitespace(rawInput);
    if (s.empty()) return false;

    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return std::iswspace(c) != 0; }), s.end());
    if (s.empty()) return false;

    bool isPercent = false;
    if (!s.empty()) {
        wchar_t last = s.back();
        if (last == L'%' || last == L'％') {
            isPercent = true;
            s.pop_back();
        }
    }
    if (!s.empty() && s.back() == L'倍') s.pop_back();
    if (!s.empty()) {
        wchar_t last = s.back();
        if (last == L'x' || last == L'X' || last == L'ｘ' || last == L'Ｘ') {
            s.pop_back();
        }
    }
    if (s.empty()) return false;

    wchar_t* end = nullptr;
    double val = std::wcstod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    while (end && *end && std::iswspace(*end)) ++end;
    if (end && *end) return false;
    if (!std::isfinite(val)) return false;

    double scale = val;
    if (isPercent || val > 10.0) {
        scale = val / 100.0;
    }

    if (!std::isfinite(scale) || scale <= 0.0) return false;
    scale = std::clamp(scale, kMinScale, kMaxScale);
    *outScale = scale;
    return true;
}

LRESULT CALLBACK ToolbarHostProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM: {
        HWND parent = GetParent(hWnd);
        if (parent) return SendMessageW(parent, msg, wParam, lParam);
        break;
    }
    case WM_ERASEBKGND:
        // Prevent default erase to avoid flicker; we paint background in WM_PAINT.
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeToolbarBrush ? g_hThemeToolbarBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
