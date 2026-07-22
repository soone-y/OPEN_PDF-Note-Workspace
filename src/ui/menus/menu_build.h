#pragma once

#include "ui/menus/main_menu_snapshot.h"

#include <windows.h>

HMENU BuildMenuBar();
HMENU BuildMenuBarForState(const MainMenuStateSnapshot& menuState);
bool UpdateEditMenuUndoRedoState(HMENU menu);
