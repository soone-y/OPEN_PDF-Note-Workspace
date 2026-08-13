// file: help.h
#pragma once

#include <windows.h>

void ShowHelpDialog(HWND owner);
// Opens the bundled Markdown guide in the locally bundled read-only viewer.
// The guide and viewer are resolved only relative to this executable.
void OpenBundledHelpGuide(HWND owner);
void ShowPdfInfoDialog(HWND owner);
