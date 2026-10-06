#pragma once

#include <windows.h>

#include <string>
#include <optional>
#include <vector>

inline constexpr UINT kMsgOpenStartupDocument = WM_APP + 211;
inline constexpr ULONG_PTR kCopyDataOpenDocumentPath = 0x50445731; // "PDW1"

bool ReadMainEnvVar(const wchar_t* name, std::wstring* out);
bool IsUiAutomationEnabled();
bool TryGetUiAutomationWorkspaceRoot(std::wstring* out);
int UiAutomationExitCode();
void SetUiAutomationExitCode(int code);

std::wstring AbsoluteOrOriginalPath(const std::wstring& path);
std::wstring SingleInstanceMutexName();
std::wstring SingleInstanceReadyEventName();
std::wstring SingleInstanceShutdownRequestEventName();
// True only when processId belongs to an executable packaged with this executable's setup file.
bool IsProcessInCurrentMainPackage(DWORD processId);
bool SignalSingleInstanceShutdownRequest();
// A PID-specific cooperative endpoint prevents a request from reaching a
// different instance in the same package (including isolated test instances).
[[nodiscard]] std::wstring ProcessShutdownRequestEventName();
void PublishSelfMainWindow(HWND window) noexcept;
[[nodiscard]] bool IsSelfMainWindowVisible() noexcept;
// UI-owner-only recovery. Does not activate another process or change data.
[[nodiscard]] bool RestoreSelfMainWindowOnUiThread() noexcept;
[[nodiscard]] bool PostSelfMainWindowRecovery(UINT message) noexcept;

struct OtherPackageHeadlessMainProcess {
    DWORD processId = 0;
    std::wstring packageDirectory;
    FILETIME creationTime{};
    bool canRequestShutdown = false;
};
[[nodiscard]] std::vector<OtherPackageHeadlessMainProcess> FindOtherPackageHeadlessMainProcesses();
enum class ProcessShutdownStatus { Pending, Exited, TimedOut, Failed };
// Owns only a query/synchronize handle, never PROCESS_TERMINATE rights.
class ProcessShutdownObservation {
public:
    ProcessShutdownObservation(HANDLE process, ULONGLONG start) noexcept;
    ~ProcessShutdownObservation();
    ProcessShutdownObservation(ProcessShutdownObservation&& other) noexcept;
    ProcessShutdownObservation& operator=(ProcessShutdownObservation&& other) noexcept;
    ProcessShutdownObservation(const ProcessShutdownObservation&) = delete;
    ProcessShutdownObservation& operator=(const ProcessShutdownObservation&) = delete;
    [[nodiscard]] ProcessShutdownStatus Poll(DWORD timeoutMs = 10000) const noexcept;
private:
    HANDLE process_ = nullptr;
    ULONGLONG start_ = 0;
};
[[nodiscard]] std::optional<ProcessShutdownObservation>
RequestOtherPackageHeadlessMainProcessShutdown(const OtherPackageHeadlessMainProcess& target);

void CaptureStartupDocumentPathFromCommandLine();
bool HasPendingStartupOpenDocumentPath();
const std::wstring& PeekPendingStartupOpenDocumentPath();
std::wstring ConsumePendingStartupOpenDocumentPath();
void QueueStartupOpenDocumentPath(HWND hWnd, std::wstring path);
bool SendStartupOpenDocumentPath(HWND target, const std::wstring& path);
