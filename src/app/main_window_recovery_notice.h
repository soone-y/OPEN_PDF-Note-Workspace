#pragma once

#include <windows.h>
#include <array>
#include <string>

namespace main_window_liveness {
inline constexpr wchar_t kRecoveryNoticeClass[] = L"PdfWorkspaceMainRecoveryNotice";
enum class NoticeCommand { None, RetryDisplay, NormalClose };
struct NoticeText {
    std::wstring title, message, retry, normalClose, wait;
    std::wstring retryPending, closePending, requestFailed;
};

// Owned exclusively by the monitor thread. No owner HWND, shared app fonts,
// application state, file access, force-exit rights or calls into the UI thread.
// Dismissal suppresses repeats until visibility recovers; stop/recovery destroys
// this thread's window. A queued command is not a confirmed recovery/save/exit.
class RecoveryNotice {
public:
    explicit RecoveryNotice(NoticeText text, HANDLE stop);
    ~RecoveryNotice();
    RecoveryNotice(const RecoveryNotice&) = delete;
    RecoveryNotice& operator=(const RecoveryNotice&) = delete;
    [[nodiscard]] bool Show();
    [[nodiscard]] NoticeCommand PumpCommand();
    void ReportRequest(NoticeCommand command, bool posted) noexcept;
    void Reset() noexcept;
private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Layout() noexcept;
    void UpdateFont() noexcept;
    NoticeText text_;
    HANDLE stop_ = nullptr; // Borrowed until the monitor is joined.
    HWND window_ = nullptr;
    std::array<HWND, 5> controls_{};
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    bool dismissed_ = false;
    NoticeCommand command_ = NoticeCommand::None;
};
} // namespace main_window_liveness
