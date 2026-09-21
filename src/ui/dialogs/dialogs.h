#pragma once

#include "core/app_core.h"
#include "core/ui_prompts.h"

LRESULT CALLBACK ToolbarHostProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool ShouldSkipImeMessageInLoop(const MSG& msg);

// Top-level dialogs opt in to one of these roles.  On an application exit
// request, transient notices are dismissed while settings/output dialogs keep
// the exit flow from discarding work that has not been applied or executed.
void RegisterAppExitDismissibleDialog(HWND hWnd);
void RegisterAppExitBlockingDialog(HWND hWnd);
void UnregisterAppExitDialog(HWND hWnd);
void DismissAppExitDismissibleDialogs();
bool HasAppExitBlockingDialog();
