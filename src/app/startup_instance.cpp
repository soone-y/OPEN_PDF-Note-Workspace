#include "app/startup_instance.h"
#include "app/main_window_recovery_notice.h"

#include "core/fault_injection.h"
#include "core/constants.h"
#include "ui/window_corners.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <shellapi.h>
#include <tlhelp32.h>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kSingleInstanceMutexNameBase[] = L"PdfWorkspaceSingleInstance";
constexpr wchar_t kSingleInstanceReadyEventNameBase[] = L"PdfWorkspaceSingleInstanceReady";
constexpr wchar_t kSingleInstanceShutdownRequestEventNameBase[] =
    L"PdfWorkspaceSingleInstanceShutdownRequest";

int g_uiAutomationExitCode = 0;
std::wstring g_pendingStartupOpenDocumentPath;
std::atomic<HWND> g_selfMainWindow{nullptr};

bool IsStartupOptionName(const std::wstring& value) {
    return value.rfind(L"--", 0) == 0 || value.rfind(L"/", 0) == 0;
}

std::wstring ParseStartupDocumentPathFromCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};

    std::wstring path;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i] ? argv[i] : L"";
        if ((arg == L"--pdf" || arg == L"--open") && i + 1 < argc) {
            path = argv[++i] ? argv[i] : L"";
        } else if ((arg == L"--page" || arg == L"--theme" || arg == L"--theme-id" ||
                    arg == L"--theme-inline" || arg == L"--clrop" ||
                    arg == L"--workspace") && i + 1 < argc) {
            ++i;
        } else if (!IsStartupOptionName(arg) && path.empty()) {
            path = arg;
        }
    }
    LocalFree(argv);
    return AbsoluteOrOriginalPath(path);
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring CanonicalPackageKeyForExecutablePath(const std::wstring& executable) {
    if (executable.empty()) return {};

    std::error_code ec;
    std::filesystem::path packageSetup = std::filesystem::path(executable).parent_path() /
                                        L"pdf_note_workspace_setup.json";
    std::filesystem::path normalized = std::filesystem::weakly_canonical(packageSetup, ec);
    if (ec || normalized.empty()) {
        ec.clear();
        normalized = std::filesystem::absolute(packageSetup, ec);
    }
    if (ec || normalized.empty()) return {};

    std::wstring key = normalized.lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    });
    return key;
}

std::wstring CanonicalPackageKey() {
    return CanonicalPackageKeyForExecutablePath(CurrentExecutablePath());
}

std::wstring ExecutablePathForProcess(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};

    std::vector<wchar_t> buffer(512);
    std::wstring path;
    for (;;) {
        DWORD length = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) {
            path.assign(buffer.data(), length);
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) break;
        buffer.resize(buffer.size() * 2);
    }
    CloseHandle(process);
    return path;
}

std::uint64_t PackageKeyHash(const std::wstring& value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : value) {
        const std::uint16_t codeUnit = static_cast<std::uint16_t>(ch);
        hash ^= static_cast<std::uint8_t>(codeUnit & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<std::uint8_t>(codeUnit >> 8u);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring PackageInstanceSuffix() {
    const std::wstring key = CanonicalPackageKey();
    if (key.empty()) return L"_fallback";
    wchar_t hash[17]{};
    swprintf_s(hash, L"%016llx", static_cast<unsigned long long>(PackageKeyHash(key)));
    return L"_" + std::wstring(hash);
}

std::wstring PackageInstanceSuffixForExecutablePath(const std::wstring& executable) {
    const std::wstring key = CanonicalPackageKeyForExecutablePath(executable);
    if (key.empty()) return {};
    wchar_t hash[17]{};
    swprintf_s(hash, L"%016llx", static_cast<unsigned long long>(PackageKeyHash(key)));
    return L"_" + std::wstring(hash);
}

struct ProcessWindowState { bool mainVisible = false; bool auxiliaryVisible = false; };

std::optional<ProcessWindowState> InspectProcessWindows(DWORD processId) {
    struct Search { DWORD processId; ProcessWindowState state; } search{processId, {}};
    const BOOL enumerated = EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* search = reinterpret_cast<Search*>(parameter);
        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(window, &windowProcessId);
        if (windowProcessId == search->processId) {
            wchar_t name[128]{};
            // Hidden helper windows are not evidence of a usable main UI.
            // A visible dialog, however, may own unsaved work: protect it.
            if (IsWindowVisible(window)) {
                if (GetClassNameW(window, name, 128) && wcscmp(name, kMainClass) == 0)
                    search->state.mainVisible = true;
                else
                    search->state.auxiliaryVisible = true;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return enumerated ? std::optional<ProcessWindowState>(search.state) : std::nullopt;
}

std::wstring ProcessEndpointName(DWORD processId, const std::wstring& executable) {
    const auto suffix = PackageInstanceSuffixForExecutablePath(executable);
    return suffix.empty() ? std::wstring{} :
        std::wstring(kSingleInstanceShutdownRequestEventNameBase) + suffix + L"_pid_" + std::to_wstring(processId);
}

bool IsOtherPackageHeadlessMainProcess(DWORD processId, std::wstring* outExecutablePath,
                                      FILETIME* outCreationTime = nullptr, bool* outCanRequest = nullptr) {
    if (processId == 0 || processId == GetCurrentProcessId()) return false;
    const std::wstring executable = ExecutablePathForProcess(processId);
    const std::wstring currentExecutable = CurrentExecutablePath();
    if (executable.empty() || currentExecutable.empty() ||
        _wcsicmp(std::filesystem::path(executable).filename().c_str(),
                 std::filesystem::path(currentExecutable).filename().c_str()) != 0) return false;
    const std::wstring suffix = PackageInstanceSuffixForExecutablePath(executable);
    if (suffix.empty() || suffix == PackageInstanceSuffix()) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
    if (!process) return false;
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    GetSystemTimeAsFileTime(&now);
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    const bool times = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    ULARGE_INTEGER createdTicks{}, nowTicks{};
    createdTicks.LowPart = created.dwLowDateTime; createdTicks.HighPart = created.dwHighDateTime;
    nowTicks.LowPart = now.dwLowDateTime; nowTicks.HighPart = now.dwHighDateTime;
    // Do not classify a concurrently starting application as orphaned.
    if (!alive || !times || nowTicks.QuadPart < createdTicks.QuadPart ||
        nowTicks.QuadPart - createdTicks.QuadPart < 15000ull * 10000ull) return false;
    const auto windows = InspectProcessWindows(processId);
    if (!windows || windows->mainVisible || windows->auxiliaryVisible) return false;
    const std::wstring eventName = ProcessEndpointName(processId, executable);
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    const bool canRequest = event != nullptr;
    if (event) {
        CloseHandle(event);
    } else {
        // Legacy package endpoints permit a diagnostic, not a PID-scoped
        // request: an isolated sibling may share this executable path.
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, (kSingleInstanceMutexNameBase + suffix).c_str());
        if (!mutex) return false;
        CloseHandle(mutex);
        HANDLE legacy = OpenEventW(SYNCHRONIZE, FALSE, (kSingleInstanceShutdownRequestEventNameBase + suffix).c_str());
        if (!legacy) return false;
        CloseHandle(legacy);
    }
    if (outExecutablePath) *outExecutablePath = executable;
    if (outCreationTime) *outCreationTime = created;
    if (outCanRequest) *outCanRequest = canRequest;
    return true;
}

std::wstring ReadOptionalInstanceSuffix() {
    std::wstring suffix;
    if (!ReadMainEnvVar(L"PDF_NOTE_SMALL_INSTANCE_SUFFIX", &suffix)) {
        suffix.clear();
    }
    return suffix;
}

} // namespace

bool ReadMainEnvVar(const wchar_t* name, std::wstring* out) {
    if (!name || !out) return false;
    return fault_injection::ReadEnvVar(name, out);
}

bool IsUiAutomationEnabled() {
    std::wstring value;
    if (!ReadMainEnvVar(L"PDF_NOTE_SMALL_UI_AUTOMATION", &value)) return false;
    return value == L"1" || value == L"true" || value == L"TRUE" || value == L"on";
}

bool IsBackgroundUiAutomationEnabled() {
    if (!IsUiAutomationEnabled()) return false;
    // Match RunUiAutomationScenarios' mode flags. Mixed modes must never opt
    // out of UI behavior: the scenario validator reports that input as failure.
    constexpr const wchar_t* modes[] = {
        L"PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_LOG_CONTRACT_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_RECOVERY_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_UNKNOWN_FIELD_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_PDFIUM_RAW_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_HELP_VISIBILITY_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_DIALOG_OWNER_VISIBILITY_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_OUTPUT_EXPORT_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_TARGET_SESSION_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_PDF_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_MATH_RENDER_ONLY",
    };
    int selected = 0;
    bool background = false;
    for (size_t i = 0; i < std::size(modes); ++i) {
        std::wstring value;
        if (ReadMainEnvVar(modes[i], &value) && (value == L"1" || value == L"true")) {
            ++selected;
            background = i < 5;
        }
    }
    return selected == 1 && background;
}

bool TryGetUiAutomationWorkspaceRoot(std::wstring* out) {
    if (!out) return false;
    out->clear();
    return ReadMainEnvVar(L"PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", out) && !out->empty();
}

int UiAutomationExitCode() {
    return g_uiAutomationExitCode;
}

void SetUiAutomationExitCode(int code) {
    g_uiAutomationExitCode = code;
}

std::wstring AbsoluteOrOriginalPath(const std::wstring& path) {
    if (path.empty()) return {};
    std::error_code ec;
    auto abs = std::filesystem::absolute(std::filesystem::path(path), ec);
    return ec ? path : abs.wstring();
}

std::wstring SingleInstanceMutexName() {
    std::wstring name = kSingleInstanceMutexNameBase + PackageInstanceSuffix();
    const std::wstring suffix = ReadOptionalInstanceSuffix();
    if (!suffix.empty()) name += suffix;
    return name;
}

std::wstring SingleInstanceReadyEventName() {
    std::wstring name = kSingleInstanceReadyEventNameBase + PackageInstanceSuffix();
    const std::wstring suffix = ReadOptionalInstanceSuffix();
    if (!suffix.empty()) name += suffix;
    return name;
}

std::wstring SingleInstanceShutdownRequestEventName() {
    std::wstring name = kSingleInstanceShutdownRequestEventNameBase + PackageInstanceSuffix();
    const std::wstring suffix = ReadOptionalInstanceSuffix();
    if (!suffix.empty()) name += suffix;
    return name;
}

bool IsProcessInCurrentMainPackage(DWORD processId) {
    const std::wstring currentPackageKey = CanonicalPackageKey();
    if (currentPackageKey.empty()) return false;
    const std::wstring processPath = ExecutablePathForProcess(processId);
    return !processPath.empty() &&
           CanonicalPackageKeyForExecutablePath(processPath) == currentPackageKey;
}

bool SignalSingleInstanceShutdownRequest() {
    const std::wstring name = SingleInstanceShutdownRequestEventName();
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!event) return false;
    const bool ok = SetEvent(event) != FALSE;
    CloseHandle(event);
    return ok;
}

std::wstring ProcessShutdownRequestEventName() {
    return ProcessEndpointName(GetCurrentProcessId(), CurrentExecutablePath());
}

void PublishSelfMainWindow(HWND window) noexcept {
    g_selfMainWindow.store(window, std::memory_order_release);
}

bool IsSelfMainWindowVisible() noexcept {
    const HWND window = g_selfMainWindow.load(std::memory_order_acquire);
    DWORD processId = 0;
    wchar_t name[128]{};
    return window && GetWindowThreadProcessId(window, &processId) &&
        processId == GetCurrentProcessId() && GetClassNameW(window, name, 128) &&
        wcscmp(name, kMainClass) == 0 && IsWindowVisible(window);
}

bool RestoreSelfMainWindowOnUiThread() noexcept {
    const HWND window = g_selfMainWindow.load(std::memory_order_acquire);
    DWORD processId = 0;
    const DWORD thread = window ? GetWindowThreadProcessId(window, &processId) : 0;
    wchar_t name[128]{};
    if (thread != GetCurrentThreadId() || processId != GetCurrentProcessId() ||
        !GetClassNameW(window, name, 128) || wcscmp(name, kMainClass) != 0) return false;
    if (!IsWindowVisible(window)) ShowWindow(window, SW_SHOWNOACTIVATE);
    return IsSelfMainWindowVisible();
}

bool PostSelfMainWindowRecovery(UINT message) noexcept {
    const HWND window = g_selfMainWindow.load(std::memory_order_acquire);
    DWORD processId = 0;
    wchar_t name[128]{};
    return window && GetWindowThreadProcessId(window, &processId) &&
        processId == GetCurrentProcessId() && GetClassNameW(window, name, 128) &&
        wcscmp(name, kMainClass) == 0 && PostMessageW(window, message, 0, 0);
}

namespace main_window_liveness {
namespace {
// RecoveryNotice HWND-local registry: only this stop timer uses ID 1.
constexpr UINT_PTR kNoticeStopTimerId = 1;
UINT NoticeDpi(HWND window) noexcept {
    using GetDpi = UINT(WINAPI*)(HWND);
    const auto get = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = get ? get(window) : 96;
    return dpi ? dpi : 96;
}
}
RecoveryNotice::RecoveryNotice(NoticeText text, HANDLE stop) : text_(std::move(text)), stop_(stop) {}
RecoveryNotice::~RecoveryNotice() { Reset(); }
void RecoveryNotice::Reset() noexcept {
    if (window_) DestroyWindow(window_);
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    controls_.fill(nullptr);
    dismissed_ = false;
    command_ = NoticeCommand::None;
}
void RecoveryNotice::UpdateFont() noexcept {
    dpi_ = NoticeDpi(window_);
    LOGFONTW description{};
    description.lfHeight = -MulDiv(14, static_cast<int>(dpi_), 96);
    description.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(description.lfFaceName, L"Segoe UI");
    HFONT replacement = CreateFontIndirectW(&description);
    if (!replacement) return;
    for (HWND control : controls_) if (control)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
    if (font_) DeleteObject(font_);
    font_ = replacement;
}
void RecoveryNotice::Layout() noexcept {
    RECT rect{};
    if (!GetClientRect(window_, &rect)) return;
    const int margin = MulDiv(16, static_cast<int>(dpi_), 96);
    const int gap = MulDiv(8, static_cast<int>(dpi_), 96);
    const int buttonHeight = MulDiv(34, static_cast<int>(dpi_), 96);
    const int statusHeight = MulDiv(56, static_cast<int>(dpi_), 96);
    const int width = std::max(1, static_cast<int>(rect.right) - 2 * margin);
    const int buttonY = std::max(margin, static_cast<int>(rect.bottom) - margin - buttonHeight);
    const int statusY = std::max(margin, buttonY - gap - statusHeight);
    MoveWindow(controls_[0], margin, margin, width, std::max(1, statusY - margin - gap), TRUE);
    MoveWindow(controls_[1], margin, statusY, width, std::max(1, buttonY - gap - statusY), TRUE);
    const int buttonWidth = std::max(1, (width - 2 * gap) / 3);
    for (int i = 0; i < 3; ++i)
        MoveWindow(controls_[i + 2], margin + i * (buttonWidth + gap), buttonY, buttonWidth, buttonHeight, TRUE);
}
LRESULT CALLBACK RecoveryNotice::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<RecoveryNotice*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<RecoveryNotice*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE: {
        const wchar_t* labels[] = {self->text_.message.c_str(), L"", self->text_.retry.c_str(),
                                  self->text_.normalClose.c_str(), self->text_.wait.c_str()};
        for (int i = 0; i < 5; ++i) {
            self->controls_[i] = CreateWindowExW(0, i < 2 ? L"STATIC" : L"BUTTON", labels[i],
                WS_CHILD | WS_VISIBLE | (i < 2 ? SS_LEFT : WS_TABSTOP |
                    (i == 4 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON)),
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(i + 1)),
                GetModuleHandleW(nullptr), nullptr);
            if (!self->controls_[i]) return -1;
        }
        self->UpdateFont(); self->Layout();
        return 0;
    }
    case WM_SIZE: self->Layout(); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) SetFocus(self->controls_[4]);
        return 0;
    case WM_TIMER:
        // Private notice HWND timer 1: active only while the notice exists.
        // Also observes stop during native move/menu loops so join cannot be
        // held hostage by a notification being dragged during app shutdown.
        if (wParam == kNoticeStopTimerId && self->stop_ && WaitForSingleObject(self->stop_, 0) == WAIT_OBJECT_0) {
            SendMessageW(window, WM_CANCELMODE, 0, 0);
            DestroyWindow(window);
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOACTIVATE | SWP_NOZORDER);
        self->UpdateFont(); self->Layout(); return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) return 0;
        if (LOWORD(wParam) == 3) self->command_ = NoticeCommand::RetryDisplay;
        else if (LOWORD(wParam) == 4) self->command_ = NoticeCommand::NormalClose;
        else if (LOWORD(wParam) == 5 || LOWORD(wParam) == IDCANCEL) {
            self->command_ = NoticeCommand::None;
            self->dismissed_ = true; DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        self->command_ = NoticeCommand::None;
        self->dismissed_ = true; DestroyWindow(window); return 0;
    case WM_CHAR: return 0; // Unsupported input is silent, not a warning beep.
    case WM_NCDESTROY:
        self->window_ = nullptr;
        self->controls_.fill(nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
bool RecoveryNotice::Show() {
    if (!stop_ || WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) return false;
    if (window_ || dismissed_) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kRecoveryNoticeClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    POINT point{};
    GetCursorPos(&point);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), &monitor)) return false;
    HWND created = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kRecoveryNoticeClass,
        text_.title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        monitor.rcWork.left, monitor.rcWork.top, 600, 340, nullptr, nullptr, wc.hInstance, this);
    if (!created) return false;
    // The recovery notice owns a different UI thread from the main hook.
    const HRESULT cornerResult = ui::ApplyAuxiliaryWindowCorners(created);
    (void)cornerResult;
    if (!stop_ || !SetTimer(window_, kNoticeStopTimerId, 500, nullptr)) {
        DestroyWindow(window_);
        return false;
    }
    const int width = std::min(MulDiv(600, static_cast<int>(dpi_), 96),
                               static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    // Measure the immutable localized body once, not in paint/mouse/timer paths.
    RECT outer{}, client{};
    GetWindowRect(window_, &outer); GetClientRect(window_, &client);
    const int nonClientHeight = (outer.bottom - outer.top) - client.bottom;
    const int nonClientWidth = (outer.right - outer.left) - client.right;
    RECT measured{0, 0, std::max(1, width - nonClientWidth - MulDiv(32, static_cast<int>(dpi_), 96)), 0};
    HDC dc = GetDC(window_);
    if (!dc) { DestroyWindow(window_); return false; }
    HGDIOBJ previous = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, text_.message.c_str(), -1, &measured, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous); ReleaseDC(window_, dc);
    const int requiredHeight = nonClientHeight + measured.bottom + MulDiv(146, static_cast<int>(dpi_), 96);
    const int height = std::min(std::max(MulDiv(340, static_cast<int>(dpi_), 96), requiredHeight),
                                static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    SetWindowPos(window_, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
        monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
        width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    // Visible taskbar entry; never owned/disabled by the unavailable main HWND.
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    return IsWindowVisible(window_) != FALSE;
}
NoticeCommand RecoveryNotice::PumpCommand() {
    MSG message{};
    // Only this private thread's queue. A bounded batch preserves stop checks.
    for (unsigned count = 0; count < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
        if (!window_ || !IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    return std::exchange(command_, NoticeCommand::None);
}
void RecoveryNotice::ReportRequest(NoticeCommand command, bool posted) noexcept {
    if (!controls_[1]) return;
    const std::wstring& status = !posted ? text_.requestFailed :
        command == NoticeCommand::NormalClose ? text_.closePending : text_.retryPending;
    SetWindowTextW(controls_[1], status.c_str());
}
} // namespace main_window_liveness

std::vector<OtherPackageHeadlessMainProcess> FindOtherPackageHeadlessMainProcesses() {
    std::vector<OtherPackageHeadlessMainProcess> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const auto currentName = std::filesystem::path(CurrentExecutablePath()).filename().wstring();
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (currentName.empty() || _wcsicmp(entry.szExeFile, currentName.c_str()) != 0) continue;
            std::wstring executable;
            FILETIME created{};
            bool canRequest = false;
            if (IsOtherPackageHeadlessMainProcess(entry.th32ProcessID, &executable, &created, &canRequest)) {
                result.push_back({entry.th32ProcessID, std::filesystem::path(executable).parent_path().wstring(), created, canRequest});
                if (result.size() >= 16) break;
            }
            entry.dwSize = sizeof(entry);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

ProcessShutdownObservation::ProcessShutdownObservation(HANDLE process, ULONGLONG start) noexcept
    : process_(process), start_(start) {}
ProcessShutdownObservation::~ProcessShutdownObservation() {
    if (process_) CloseHandle(process_);
}
ProcessShutdownObservation::ProcessShutdownObservation(ProcessShutdownObservation&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)), start_(other.start_) {}
ProcessShutdownObservation& ProcessShutdownObservation::operator=(ProcessShutdownObservation&& other) noexcept {
    if (this != &other) {
        if (process_) CloseHandle(process_);
        process_ = std::exchange(other.process_, nullptr);
        start_ = other.start_;
    }
    return *this;
}
ProcessShutdownStatus ProcessShutdownObservation::Poll(DWORD timeoutMs) const noexcept {
    if (!process_) return ProcessShutdownStatus::Failed;
    const DWORD wait = WaitForSingleObject(process_, 0);
    if (wait == WAIT_OBJECT_0) return ProcessShutdownStatus::Exited;
    if (wait != WAIT_TIMEOUT) return ProcessShutdownStatus::Failed;
    return GetTickCount64() - start_ >= timeoutMs ? ProcessShutdownStatus::TimedOut : ProcessShutdownStatus::Pending;
}

std::optional<ProcessShutdownObservation>
RequestOtherPackageHeadlessMainProcessShutdown(const OtherPackageHeadlessMainProcess& target) {
    if (!target.canRequestShutdown) return std::nullopt;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, target.processId);
    if (!process) return std::nullopt;
    ProcessShutdownObservation observation(process, GetTickCount64());
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user) ||
        CompareFileTime(&created, &target.creationTime) != 0) return std::nullopt;
    if (observation.Poll() == ProcessShutdownStatus::Exited) return observation;
    std::wstring executable;
    if (!IsOtherPackageHeadlessMainProcess(target.processId, &executable)) return std::nullopt;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, ProcessEndpointName(target.processId, executable).c_str());
    if (!event) return std::nullopt;
    const bool requested = SetEvent(event) != FALSE;
    CloseHandle(event);
    if (!requested) return std::nullopt;
    return observation;
}

void CaptureStartupDocumentPathFromCommandLine() {
    g_pendingStartupOpenDocumentPath = ParseStartupDocumentPathFromCommandLine();
}

bool HasPendingStartupOpenDocumentPath() {
    return !g_pendingStartupOpenDocumentPath.empty();
}

const std::wstring& PeekPendingStartupOpenDocumentPath() {
    return g_pendingStartupOpenDocumentPath;
}

std::wstring ConsumePendingStartupOpenDocumentPath() {
    std::wstring path = std::move(g_pendingStartupOpenDocumentPath);
    g_pendingStartupOpenDocumentPath.clear();
    return path;
}

void QueueStartupOpenDocumentPath(HWND hWnd, std::wstring path) {
    if (path.empty()) return;
    g_pendingStartupOpenDocumentPath = AbsoluteOrOriginalPath(path);
    if (hWnd && IsWindow(hWnd)) {
        PostMessageW(hWnd, kMsgOpenStartupDocument, 0, 0);
    }
}

bool SendStartupOpenDocumentPath(HWND target, const std::wstring& path) {
    if (!target || path.empty()) return false;
    COPYDATASTRUCT data{};
    data.dwData = kCopyDataOpenDocumentPath;
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(path.c_str());
    DWORD_PTR result = 0;
    LRESULT sent = SendMessageTimeoutW(target,
                                       WM_COPYDATA,
                                       static_cast<WPARAM>(GetCurrentProcessId()),
                                       reinterpret_cast<LPARAM>(&data),
                                       SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                       3000,
                                       &result);
    return sent != 0 && result != 0;
}
