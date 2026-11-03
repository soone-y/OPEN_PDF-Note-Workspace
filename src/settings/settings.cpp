// file: settings/settings.cpp
#include "settings/settings.h"
#include "settings/settings_navigation.h"
#include "settings/palette_stock_store.h"
#include "settings/palette_presets_store.h"
#include "settings/palette_active_store.h"
#include "ui/color_gradient.h"

#include "core/app_core.h"
#include "core/atomic_write.h"
#include "core/font_list.h"
#include "core/localization.h"
#include "core/path_safety.h"
#include "diagnostics/bounded_log.h"
#include "diagnostics/artifact_usage.h"
#include "ui/window_corners.h"
#include <atomic>
#include <process.h>
#include "pdf_view/pdf_view.h"
#include "pdf_view/textbox_font_update.h"
#include "note_view/note_view.h"
#include "note/note_presentation.h"
#include "note/note_file_policy.h"
#include "note/note_bottom_space.h"
#include "file_output/file_output.h"
#include "lecture_order/lecture_order.h"
#include "workspace/workspace_memo.h"
#include "workspace/workspace_tree.h"
#include "ui/dialogs/dialogs.h"
#include "ui/dialogs/pending_write_retry.h"
#include "math/math_render.h"
#include "ui/toolbar.h"
#include "ui/combobox_guard.h"
#include "ui/noop_nav_guard.h"
#include "workspace/workspace_config_io.h"

#include <commctrl.h>
#include <commdlg.h>
#include <imm.h>
#include <richedit.h>

#include <iterator>
#include <cwchar>
#include <memory>
#include <filesystem>
#include <fstream>
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
#include "settings/settings_assets.cppinc"
#include "settings/settings_annot.cppinc"
#include "settings/settings_shell.cppinc"
#include "settings/settings_unified.cppinc"
