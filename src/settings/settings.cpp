// file: settings/settings.cpp
#include "settings/settings.h"

#include "core/app_core.h"
#include "core/font_list.h"
#include "pdf_view/pdf_view.h"
#include "note_view/note_view.h"
#include "file_output/file_output.h"
#include "schedule/schedule.h"
#include "ui/dialogs/dialogs.h"
#include "math/math_render.h"
#include "ui/toolbar.h"
#include "ui/combobox_guard.h"
#include "ui/noop_nav_guard.h"

#include <commctrl.h>
#include <commdlg.h>
#include <colordlg.h>
#include <imm.h>
#include <richedit.h>

#include <iterator>
#include <memory>
#include <vector>

static void LogPanelEvent(const wchar_t* panel,
                          const wchar_t* tag,
                          int id,
                          int code,
                          HWND from);

#include "settings/settings_common.cppinc"
#include "settings/settings_general.cppinc"
#include "settings/settings_shortcut_editor.cppinc"
#include "settings/settings_markup.cppinc"
#include "settings/settings_palette.cppinc"
#include "settings/settings_annot.cppinc"
#include "settings/settings_unified.cppinc"
