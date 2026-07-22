#pragma once

#include <windows.h>

#include <string>

std::wstring DebugMenuLabel();
std::wstring DebugArchiveLogsMenuLabel();
std::wstring DebugDeleteLogsMenuLabel();
std::wstring DebugToggleLogsMenuLabel(bool allEnabled, bool anyEnabled);
std::wstring DebugResourceMonitorMenuLabel();
std::wstring BuildDebugResourceMonitorText();
void ShowDebugResourceMonitorWindow(HWND owner, const std::wstring& initialText);
