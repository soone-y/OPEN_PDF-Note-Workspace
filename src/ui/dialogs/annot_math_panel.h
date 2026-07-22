#pragma once

#include <windows.h>

void UpdateAnnotPanelSummary();
void RequestAnnotPanelRevealLatest();
void RefreshAnnotPanel();
bool SyncTextBoxToolFontFromAnnotationIndex(int index);
void JumpToSelectedAnnot();
bool ShowAnnotPanelContextMenu(HWND owner, POINT screenPt);
void ShowAnnotationInspector(HWND owner, int annotationIndex, bool editMode);
