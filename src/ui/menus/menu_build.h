#pragma once

#include "ui/menus/main_menu_snapshot.h"

#include <windows.h>

HMENU BuildMenuBar();
HMENU BuildMenuBarForState(const MainMenuStateSnapshot& menuState);
bool UpdateEditMenuUndoRedoState(HMENU menu);
// The menu bar is rebuilt only on UI refresh boundaries, while note typing can
// make work saveable earlier. TranslateAccelerator drops Ctrl+S silently when
// the matching item is grayed, so WM_INITMENU resyncs it before that check.
bool IsSaveAllMenuAvailable();
bool UpdateSaveAllMenuState(HMENU menu);
