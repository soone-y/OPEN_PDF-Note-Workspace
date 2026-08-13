#pragma once

#include "core/app_core.h"
#include "core/ui_prompts.h"

LRESULT CALLBACK ToolbarHostProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool ShouldSkipImeMessageInLoop(const MSG& msg);
