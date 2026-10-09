#pragma once
#include <windows.h>
void ShowWriteChecksDialog(HWND owner);
[[nodiscard]] bool HandleWriteChecksMessage(const MSG& message);
