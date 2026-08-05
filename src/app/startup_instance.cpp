#include "app/startup_instance.h"

#include "core/fault_injection.h"

#include <algorithm>
#include <filesystem>
#include <shellapi.h>
#include <shobjidl.h>
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

bool CommandLineHasOption(const wchar_t* option) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && option && wcscmp(argv[i], option) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

std::wstring ParseStartupWorkspaceRoot() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};
    std::wstring root;
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && wcscmp(argv[i], L"--workspace") == 0) {
            root = argv[++i] ? argv[i] : L"";
        }
    }
    LocalFree(argv);
    return AbsoluteOrOriginalPath(root);
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

bool IsNewInstanceLaunchRequested() {
    return CommandLineHasOption(L"--new-instance");
}

bool ShouldChooseStartupWorkspace() {
    return CommandLineHasOption(L"--choose-workspace");
}

bool TryGetStartupWorkspaceRoot(std::wstring* out) {
    if (!out) return false;
    *out = ParseStartupWorkspaceRoot();
    return !out->empty();
}

bool LaunchNewMainWindow(const std::wstring& workspaceRoot) {
    if (workspaceRoot.empty()) return false;
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return false;
    std::wstring commandLine = L"\"" + executable + L"\" --new-instance --workspace \"" + workspaceRoot + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void RegisterNewWindowJumpListTask() {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return;
    ICustomDestinationList* list = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&list))) || !list) return;
    UINT slots = 0;
    IObjectArray* removed = nullptr;
    if (FAILED(list->BeginList(&slots, IID_PPV_ARGS(&removed)))) {
        if (removed) removed->Release();
        list->Release();
        return;
    }
    if (removed) removed->Release();
    IObjectCollection* tasks = nullptr;
    IShellLinkW* task = nullptr;
    IObjectArray* taskArray = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&tasks));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&task));
    if (SUCCEEDED(hr)) hr = task->SetPath(executable.c_str());
    if (SUCCEEDED(hr)) hr = task->SetArguments(L"--new-instance --choose-workspace");
    if (SUCCEEDED(hr)) hr = task->SetDescription(L"新しいウィンドウ...");
    if (SUCCEEDED(hr)) hr = tasks->AddObject(task);
    if (SUCCEEDED(hr)) hr = tasks->QueryInterface(IID_PPV_ARGS(&taskArray));
    if (SUCCEEDED(hr)) hr = list->AddUserTasks(taskArray);
    if (SUCCEEDED(hr)) list->CommitList(); else list->AbortList();
    if (taskArray) taskArray->Release();
    if (task) task->Release();
    if (tasks) tasks->Release();
    list->Release();
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
