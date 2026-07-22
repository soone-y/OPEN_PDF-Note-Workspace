#pragma once

#include <windows.h>

#include <string>

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
bool SignalSingleInstanceShutdownRequest();

void CaptureStartupDocumentPathFromCommandLine();
bool HasPendingStartupOpenDocumentPath();
const std::wstring& PeekPendingStartupOpenDocumentPath();
std::wstring ConsumePendingStartupOpenDocumentPath();
void QueueStartupOpenDocumentPath(HWND hWnd, std::wstring path);
bool SendStartupOpenDocumentPath(HWND target, const std::wstring& path);
