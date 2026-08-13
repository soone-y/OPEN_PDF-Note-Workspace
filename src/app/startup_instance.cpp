#include "app/startup_instance.h"

#include "core/fault_injection.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <shellapi.h>
#include <utility>
#include <vector>

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
                                        L"pdf_workspace_setup.json";
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
