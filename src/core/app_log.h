// file: core/app_log.h
#pragma once

#include <filesystem>
#include <string>

enum class AppLogKind {
    PreviewTrace,
    SwitchTiming,
    Crash,
    StartupWatchdog
};

bool IsAppLogEnabled(AppLogKind kind);
void AppendAppLogLine(AppLogKind kind, const std::wstring& line);
