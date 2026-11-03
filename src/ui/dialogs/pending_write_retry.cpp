#include "ui/dialogs/pending_write_retry.h"

#include "core/command_ids.h"
#include "app/startup_instance.h"
#include "core/localization.h"
#include "core/ui_notify.h"
#include "ui/window_corners.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace pending_write_retry {
namespace {

constexpr int kList = 1;
constexpr int kDetails = 2;
constexpr int kRetry = 3;
constexpr int kHide = 4;
constexpr int kCopyPath = 5;
constexpr int kCopyContent = 6;
constexpr wchar_t kClassName[] = L"PdfNotePendingWriteRetry";

std::vector<Attempt> g_attempts;
HWND g_window = nullptr;
HWND g_owner = nullptr;
HWND g_list = nullptr;
HWND g_details = nullptr;
HWND g_retry = nullptr;
bool g_retrying = false;
unsigned long long g_nextAttemptId = 0;

std::wstring Label(const wchar_t* id) { return localization::Text(id); }

void ShowRetryGuidance(HWND owner, const std::wstring& operation) {
    ShowSoftNotice(owner, localization::Format(L"write_retry.notice_guidance",
        {{L"OPERATION", operation}}), SoftNoticeKind::Warning);
}

void UpdateMenu() {
    if (!g_owner || !IsWindow(g_owner)) return;
    HMENU menu = GetMenu(g_owner);
    if (!menu) return;
    EnableMenuItem(menu, ID_FILE_PENDING_WRITES,
                   MF_BYCOMMAND | (g_attempts.empty() ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, ID_FILE_RETRY_TEMP_EXTERNAL,
                   MF_BYCOMMAND | (HasPendingTemporaryPaths() ? MF_ENABLED : MF_GRAYED));
    DrawMenuBar(g_owner);
}

int SelectedIndex() {
    if (!g_list) return -1;
    const auto index = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    return index >= 0 && static_cast<size_t>(index) < g_attempts.size()
        ? static_cast<int>(index) : -1;
}

void RefreshDetails() {
    const int index = SelectedIndex();
    const bool selected = index >= 0;
    EnableWindow(g_retry, selected && !g_retrying && !g_attempts[index].inFlight && g_attempts[index].retryable);
    EnableWindow(GetDlgItem(g_window, kCopyPath), selected && !g_attempts[index].targetPath.empty());
    EnableWindow(GetDlgItem(g_window, kCopyContent), selected && !g_attempts[index].content.empty());
    if (!selected) {
        SetWindowTextW(g_details, L"");
        return;
    }
    const Attempt& attempt = g_attempts[index];
    std::wstring display = attempt.operation + L"\r\n";
    if (!attempt.targetPath.empty()) {
        display += Label(L"write_retry.target") + L": " +
            std::filesystem::path(attempt.targetPath).filename().wstring() + L"\r\n";
    }
    if (!attempt.error.empty()) display += Label(L"write_retry.reason") + L": " + attempt.error + L"\r\n";
    display += L"\r\n" + Label(L"write_retry.content") + L":\r\n";
    constexpr size_t kPreviewLimit = 8192;
    const auto& preview = attempt.preview.empty() ? attempt.content : attempt.preview;
    display += preview.substr(0, kPreviewLimit);
    if (preview.size() > kPreviewLimit) display += L"\r\n…";
    SetWindowTextW(g_details, display.c_str());
}

void RefreshList(int preferred = -1) {
    if (!g_list) return;
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& attempt : g_attempts) {
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(attempt.operation.c_str()));
    }
    if (!g_attempts.empty()) {
        const int index = std::clamp(preferred, 0, static_cast<int>(g_attempts.size()) - 1);
        SendMessageW(g_list, LB_SETCURSEL, index, 0);
    }
    RefreshDetails();
    UpdateMenu();
}

void LayoutWindow(HWND window) {
    RECT area{};
    GetClientRect(window, &area);
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    const int margin = 12;
    const int inner = std::max(0, width - 2 * margin);
    const int listHeight = std::min(82, std::max(50, height / 4));
    const int buttonY = std::max(margin, height - margin - 32);
    const int detailY = margin + listHeight + 10;
    MoveWindow(g_list, margin, margin, inner, listHeight, TRUE);
    MoveWindow(g_details, margin, detailY, inner,
               std::max(40, buttonY - detailY - 10), TRUE);
    const int gap = 8;
    const int buttonWidth = std::max(84, (inner - 3 * gap) / 4);
    const int ids[] = {kRetry, kCopyContent, kCopyPath, kHide};
    for (int i = 0; i < 4; ++i) {
        if (HWND button = GetDlgItem(window, ids[i])) {
            MoveWindow(button, margin + i * (buttonWidth + gap), buttonY,
                       buttonWidth, 32, TRUE);
        }
    }
}

bool CopyText(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!data) return false;
    void* memory = GlobalLock(data);
    if (!memory) { GlobalFree(data); return false; }
    memcpy(memory, text.c_str(), bytes);
    GlobalUnlock(data);
    if (!OpenClipboard(owner)) { GlobalFree(data); return false; }
    EmptyClipboard();
    const bool copied = SetClipboardData(CF_UNICODETEXT, data) != nullptr;
    CloseClipboard();
    if (!copied) GlobalFree(data);
    return copied;
}

void FinishRetry(const std::wstring& key, Result result, std::wstring error) {
    auto it = std::find_if(g_attempts.begin(), g_attempts.end(),
        [&](const Attempt& attempt) { return attempt.key == key; });
    if (it == g_attempts.end()) return;
    const auto operation = it->operation;
    const int selected = static_cast<int>(it - g_attempts.begin());
    it->inFlight = false;
    if (result == Result::Saved) g_attempts.erase(it);
    else {
        it->error = error.empty() ? Label(L"write_retry.failed") : std::move(error);
        if (result == Result::Conflict) it->retryable = false;
    }
    RefreshList(selected);
    if (result == Result::Failed) ShowRetryGuidance(g_owner, operation);
    if (g_attempts.empty() && g_window) ShowWindow(g_window, SW_HIDE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
            12, 12, 530, 82, window, reinterpret_cast<HMENU>(kList), instance, nullptr);
        g_details = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            12, 104, 530, 178, window, reinterpret_cast<HMENU>(kDetails), instance, nullptr);
        g_retry = CreateWindowExW(0, L"BUTTON", Label(L"write_retry.retry").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            12, 294, 118, 32, window, reinterpret_cast<HMENU>(kRetry), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", Label(L"write_retry.copy_content").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 138, 294, 126, 32,
            window, reinterpret_cast<HMENU>(kCopyContent), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", Label(L"write_retry.copy_path").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 272, 294, 126, 32,
            window, reinterpret_cast<HMENU>(kCopyPath), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", Label(L"write_retry.later").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 406, 294, 136, 32,
            window, reinterpret_cast<HMENU>(kHide), instance, nullptr);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (int id : {kList, kDetails, kRetry, kHide, kCopyPath, kCopyContent}) {
            if (HWND child = GetDlgItem(window, id))
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        (void)ui::ApplyAuxiliaryWindowCorners(window);
        LayoutWindow(window);
        RefreshList();
        return 0;
    }
    case WM_SIZE:
        if (g_list && g_details) LayoutWindow(window);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        if (limits) {
            limits->ptMinTrackSize.x = 470;
            limits->ptMinTrackSize.y = 300;
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == kList && HIWORD(wp) == LBN_SELCHANGE) { RefreshDetails(); return 0; }
        if (id == kHide) { ShowWindow(window, SW_HIDE); return 0; }
        const int index = SelectedIndex();
        if (index < 0) return 0;
        if (id == kCopyPath) { (void)CopyText(window, g_attempts[index].targetPath); return 0; }
        if (id == kCopyContent) { (void)CopyText(window, g_attempts[index].content); return 0; }
        if (id == kRetry && !g_retrying && !g_attempts[index].inFlight && g_attempts[index].retryable) {
            g_retrying = true;
            EnableWindow(g_retry, FALSE);
            const std::wstring key = g_attempts[index].key;
            auto callback = g_attempts[index].retry;
            auto asyncCallback = g_attempts[index].retryAsync;
            g_attempts[index].inFlight = true;
            try {
                if (asyncCallback) {
                    asyncCallback([key](Result result, std::wstring error) {
                        FinishRetry(key, result, std::move(error));
                    });
                } else {
                    std::wstring error;
                    const Result result = callback ? callback(error) : Result::Conflict;
                    FinishRetry(key, result, std::move(error));
                }
            } catch (...) {
                FinishRetry(key, Result::Failed, Label(L"write_retry.failed"));
            }
            g_retrying = false;
            RefreshDetails();
            UpdateMenu();
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_NCDESTROY:
        g_window = g_list = g_details = g_retry = nullptr;
        return 0;
    default:
        return DefWindowProcW(window, message, wp, lp);
    }
}

void EnsureWindow(HWND owner) {
    if (g_window && IsWindow(g_window)) return;
    g_owner = owner && IsWindow(owner) ? GetAncestor(owner, GA_ROOTOWNER) : nullptr;
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = kClassName;
    RegisterClassW(&cls);
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, Label(L"write_retry.title").c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 570, 380, g_owner, nullptr, cls.hInstance, nullptr);
}

} // namespace

void Remember(HWND owner, Attempt attempt) {
    if (attempt.key.empty() || (!attempt.retry && !attempt.retryAsync)) return;
    // An asynchronous request owns its immutable completion key until done.
    // Coalescing is reserved for synchronous auto-save drafts.
    if (attempt.retryAsync) attempt.coalesce = false;
    if (attempt.coalesce) {
        const auto it = std::find_if(g_attempts.begin(), g_attempts.end(),
            [&](const Attempt& current) { return current.coalesce && !current.inFlight && current.key == attempt.key; });
        if (it != g_attempts.end()) {
            const int selected = static_cast<int>(it - g_attempts.begin());
            *it = std::move(attempt);
            RefreshList(selected);
            Show(owner);
            if (g_attempts[selected].retryable) ShowRetryGuidance(owner, g_attempts[selected].operation);
            return;
        }
    }
    // Each explicit failed save owns its own immutable request, even when the
    // same target is tried again before an older request is resolved.
    if (!attempt.coalesce) attempt.key += L"#" + std::to_wstring(++g_nextAttemptId);
    const int selected = static_cast<int>(g_attempts.size());
    g_attempts.push_back(std::move(attempt));
    EnsureWindow(owner);
    RefreshList(selected);
    Show(owner);
    if (g_attempts[selected].retryable) ShowRetryGuidance(owner, g_attempts[selected].operation);
}

void Forget(const std::wstring& key) {
    const auto it = std::find_if(g_attempts.begin(), g_attempts.end(),
        [&](const Attempt& current) { return current.coalesce && current.key == key; });
    if (it == g_attempts.end()) return;
    g_attempts.erase(it);
    RefreshList();
    UpdateMenu();
    if (g_attempts.empty() && g_window) ShowWindow(g_window, SW_HIDE);
}

void Show(HWND owner) {
    if (g_attempts.empty()) return;
    EnsureWindow(owner);
    if (!g_window) return;
    if (IsUiAutomationEnabled()) {
        ShowWindow(g_window, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(g_window, SW_SHOWNORMAL);
        SetForegroundWindow(g_window);
    }
}

bool HasPending() { return !g_attempts.empty(); }

bool HasPendingTemporaryPaths() {
    return std::any_of(g_attempts.begin(), g_attempts.end(), [](const Attempt& attempt) {
        return attempt.category == Category::TemporaryPaths && attempt.retryable && !attempt.inFlight;
    });
}

void RetryTemporaryPaths(HWND owner) {
    if (g_retrying) return;
    int selected = -1;
    size_t count = 0;
    for (size_t i = 0; i < g_attempts.size(); ++i) {
        if (g_attempts[i].category != Category::TemporaryPaths || !g_attempts[i].retryable || g_attempts[i].inFlight) continue;
        if (selected < 0) selected = static_cast<int>(i);
        ++count;
    }
    if (selected < 0) return;
    EnsureWindow(owner);
    if (!g_window) return;
    RefreshList(selected);
    // Keep the existing guarded retry callback as the sole execution path.
    if (count == 1) {
        const std::wstring key = g_attempts[selected].key;
        SendMessageW(g_window, WM_COMMAND, MAKEWPARAM(kRetry, BN_CLICKED), 0);
        if (std::any_of(g_attempts.begin(), g_attempts.end(), [&](const Attempt& attempt) {
                return attempt.key == key;
            })) Show(owner);
    } else Show(owner);
}

bool ConfirmExit(HWND owner) {
    if (g_attempts.empty()) return true;
    SilentDialogOptions options;
    options.title = Label(L"write_retry.title");
    options.message = Label(L"write_retry.exit_message");
    options.kind = SoftNoticeKind::Warning;
    options.buttons = SilentDialogButtons::YesNo;
    options.defaultResult = SilentDialogResult::No;
    options.escapeResult = SilentDialogResult::No;
    return ShowSilentDialog(owner, options) == SilentDialogResult::Yes;
}

} // namespace pending_write_retry
