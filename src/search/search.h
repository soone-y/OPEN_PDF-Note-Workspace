// file: search.h
#pragma once

#include <windows.h>
#include <string>

void ShowSearchWindow(HWND parent);
void ToggleSearchWindow(HWND parent);

// Opens (or focuses) the search window and applies a small preset intended for
// Keyboard software workflows (e.g. "/").
// - rangeIndex: 0=Open lecture, 1=Open session, 2=Whole workspace, 3=Whole workspace + temporary paths, -1=leave unchanged
// - targetMask: bit0=Note, bit1=PDF, bit2=Annotations, 0=leave unchanged
// - initialQuery: optional; empty string keeps existing text when the window already exists
void ShowSearchWindowWithPreset(HWND parent, int rangeIndex, unsigned targetMask,
                                const std::wstring& initialQuery);
