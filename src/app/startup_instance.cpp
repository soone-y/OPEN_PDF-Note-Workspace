#include "app/startup_instance.h"

#include "core/fault_injection.h"

#include <algorithm>
#include <filesystem>
#include <shellapi.h>
#include <utility>

namespace {

constexpr wchar_t kSingleInstanceMutexNameBase[] = L"PdfWorkspaceSingleInstance";
constexpr wchar_t kSingleInstanceReadyEventNameBase[] = L"PdfWorkspaceSingleInstanceReady";
constexpr wchar_t kSingleInstanceShutdownRequestEventNameBase[] =
    L"PdfWorkspaceSingleInstanceShutdownRequest";

int g_uiAutomationExitCode = 0;
std::wstring g_pendingStartupOpenDocumentPath;

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
                    arg == L"--theme-inline" || arg == L"--clrop") && i + 1 < argc) {
            ++i;
        } else if (!IsStartupOptionName(arg) && path.empty()) {
            path = arg;
        }
    }
    LocalFree(argv);
    return AbsoluteOrOriginalPath(path);
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
    std::wstring name = kSingleInstanceMutexNameBase;
    const std::wstring suffix = ReadOptionalInstanceSuffix();
    if (!suffix.empty()) name += suffix;
    return name;
}

std::wstring SingleInstanceReadyEventName() {
    std::wstring name = kSingleInstanceReadyEventNameBase;
    const std::wstring suffix = ReadOptionalInstanceSuffix();
    if (!suffix.empty()) name += suffix;
    return name;
}

std::wstring SingleInstanceShutdownRequestEventName() {
    std::wstring name = kSingleInstanceShutdownRequestEventNameBase;
    const std::wstring suffix = ReadOptionalInstanceSuffix();
    if (!suffix.empty()) name += suffix;
    return name;
}

bool SignalSingleInstanceShutdownRequest() {
    const std::wstring name = SingleInstanceShutdownRequestEventName();
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!event) return false;
    const bool ok = SetEvent(event) != FALSE;
    CloseHandle(event);
    return ok;
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
                                       0,
                                       reinterpret_cast<LPARAM>(&data),
                                       SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                       3000,
                                       &result);
    return sent != 0 && result != 0;
}
