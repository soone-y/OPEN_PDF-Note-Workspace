// file: note_view/note_view.cpp
// NOTE: this module is intentionally split into implementation fragments.
// Keep the include order stable because later fragments depend on earlier shared helpers/state.

#include "core/ui_prompts.h"
#include "core/identity_path_scope.h"
#include "note/note_file_policy.h"
#include "note/note_bottom_space.h"
#include "diagnostics/normal_operations.h"
#include "diagnostics/open_work_counters.h"
#include "bridge/view_bridge.h"
#include "note/note_render_final_win32_adapter.h"
#include "ui/noop_nav_guard.h"
#include "ui/scroll_input_win32.h"
#include "ui/bottom_space_frame.h"
#include "workspace/workspace_write_lock.h"
#include "workspace/file_ops.h"

enum class PaneNavContext {
    LeftPaneList,
    PdfPane,
    NotePane,
};

bool HandlePaneDirectionalNavigation(HWND owner, PaneNavContext context, HWND source, WPARAM vkey);

#include "note_view_shared.cppinc"
#include "note_view_final_render.cppinc"
#include "note_view_note_ops.cppinc"
#include "note_view_bottom_panes.cppinc"
#include "note_view_input.cppinc"
