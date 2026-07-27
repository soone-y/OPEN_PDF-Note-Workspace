// file: note_view/note_view.cpp
// NOTE: this module is intentionally split into implementation fragments.
// Keep the include order stable because later fragments depend on earlier shared helpers/state.

#include "core/ui_prompts.h"
#include "ui/noop_nav_guard.h"

enum class PaneNavContext {
    LeftPaneList,
    PdfPane,
    NotePane,
};

bool HandlePaneDirectionalNavigation(HWND owner, PaneNavContext context, HWND source, WPARAM vkey);

#include "note_view_shared.cppinc"
#include "note_view_note_ops.cppinc"
#include "note_view_bottom_panes.cppinc"
#include "note_view_input.cppinc"
