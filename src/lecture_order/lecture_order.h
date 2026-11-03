#pragma once
#include <windows.h>
#include <optional>
#include <string>
#include <vector>

void ShowLectureOrderWindow(HWND parent, const std::wstring& selectedPath = {});
void RefreshLectureOrderWindow();
void ReloadExternalLecturePriorities();
[[nodiscard]] std::optional<int> LecturePriorityForPath(const std::wstring& path);
void SortLecturePathsByPriority(std::vector<std::wstring>& paths);
