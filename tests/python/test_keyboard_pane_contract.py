"""Source contracts for keyboard pane movement; native UI probes cover runtime focus."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def between(text: str, start: str, end: str) -> str:
    return text.split(start, 1)[1].split(end, 1)[0]


class KeyboardPaneContractTests(unittest.TestCase):
    def test_ctrl_hjk_name_the_same_destination_from_every_pane(self):
        nav = between(source("src/main.cpp"),
                      "bool HandlePaneDirectionalNavigation(HWND owner, PaneNavContext context",
                      "// Context-menu \"close\" commands")
        list_pane = between(nav, "case PaneNavContext::LeftPaneList:", "case PaneNavContext::PdfPane:")
        pdf_pane = between(nav, "case PaneNavContext::PdfPane:", "case PaneNavContext::NotePane:")
        note_pane = nav.split("case PaneNavContext::NotePane:", 1)[1]
        for pane in (list_pane, pdf_pane, note_pane):
            for key in ("'H'", "'J'", "'K'"):
                self.assertIn(f"case {key}:", pane)
        self.assertIn("FocusPdfPaneForPaneNav", between(list_pane, "case 'K':", "case 'L':"))
        self.assertIn("FocusNotePaneForPaneNav", between(pdf_pane, "case 'J':", "case 'K':"))
        self.assertIn("return true;", between(pdf_pane, "case 'K':", "default:"))
        self.assertIn("FocusPdfPaneForPaneNav", between(note_pane, "case 'K':", "default:"))

    def test_main_window_pane_keys_do_not_require_vim_normal_mode(self):
        proc = source("src/ui/core/main_window_proc.cppinc")
        guard = between(proc, "if (!IsNoteCmdlineActive() && ctrlDown && !altDown && !shiftDown) {",
                        "PaneNavContext::NotePane")
        self.assertNotIn("g_noteNormalMode", guard)

    def test_insert_ctrl_hjkl_keeps_caret_movement(self):
        # Pane movement starts from the operation state; typing keeps Ctrl+h/j/k/l
        # as caret movement, including IME candidate navigation.
        proc = source("src/note_view/note_view_input_proc.cppinc")
        self.assertIn("if (!g_noteNormalMode) {", proc)
        self.assertIn("ResolveCtrlInsertNavKey(hWnd, wParam, &navKey, &suppressedChar)", proc)

    def test_esc_keeps_pdf_and_lists_in_place(self):
        loop = source("src/app/bootstrap.cppinc")
        esc = between(loop, "bool isEscKey = (msg.message == WM_KEYDOWN", "HandleMainApplicationShortcutInLoop")
        pdf_branch = between(esc, "IsChild(g_hPdfView, escFocus)", "escFocus == g_hNoteList")
        self.assertIn("ClearPdfTextSelection();", pdf_branch)
        self.assertIn("continue;", pdf_branch)
        self.assertNotIn("EnterNoteNormalMode", pdf_branch)
        self.assertLess(esc.index("IsChild(g_hPdfView, escFocus)"), esc.index("EnterNoteNormalMode(hWnd)"))
        textbox = between(source("src/pdf_view/input.cppinc"), "if (wParam == VK_ESCAPE) {\n", "return 0;")
        self.assertIn("CommitTextEditing(hwnd, true);", textbox)
        self.assertNotIn("kMsgEnterNoteNormalMode", textbox)
        probe = source("src/features/automation/main_ui_automation.cppinc")
        self.assertIn("Esc did not keep the PDF pane after textbox editing.", probe)

    def test_pdf_held_scroll_uses_hjkl_and_shift_keeps_page_keys(self):
        motion = between(source("src/pdf_view/keyboard_scroll.h"), "static int KeyIndex(", "default:")
        self.assertEqual([key for key in "KHJL" if f"case '{key}':" in motion], list("KHJL"))
        for old in "WASD":
            self.assertNotIn(f"case '{old}':", motion)
        keydown = source("src/pdf_view/input.cppinc")
        self.assertIn("!g_pdf.editingText && !ctrlDown && !altDown && !shiftDown &&", keydown)

    def test_pdf_operation_characters_use_menu_commands(self):
        pdf = source("src/pdf_view/input.cppinc")
        handler = between(pdf, "static bool HandlePdfOperationChar(", "\n}\n")
        for char, command in (("/", "ID_SEARCH"), ("J", "ID_VIEW_NEXT_PAGE"), ("K", "ID_VIEW_PREV_PAGE"),
                              ("G", "ID_VIEW_LAST_PAGE"), ("0", "ID_VIEW_FIT_PAGE")):
            self.assertIn(f"case L'{char}': command = {command};", handler)
        self.assertIn("command = ID_VIEW_FIRST_PAGE;", between(handler, "case L'g':", "default:"))
        self.assertIn("WM_COMMAND", handler)
        char_route = between(pdf, "case WM_CHAR: {", "if (IsPdfPreviewReadOnlyActive())")
        self.assertIn("!g_pdf.editingText", between(char_route, "IsCtrlPaneNavCharPdf(wParam)) {", "HandlePdfOperationChar"))
        self.assertNotIn("ScrollPdfByKeyboard(hwnd, 0.0, step)", pdf)
        probe = source("src/features/automation/main_ui_automation.cppinc")
        self.assertIn("SendMessageW(g_hPdfView, WM_CHAR, L'J', 1);", probe)
        self.assertIn("for (unsigned key : {'K', 'H', 'J', 'L'})", probe)

    def test_single_key_controls_default_on_and_gate_pdf_letters(self):
        self.assertIn("bool noteVimModeEnabled = true;", source("src/core/workspace_config.h"))
        core = source("src/core/app_core.cpp")
        self.assertIn("bool g_noteVimModeEnabled = true;", core)
        self.assertIn("cfg.noteVimModeEnabled = true;", core)
        # Saved configs keep their explicit value.
        self.assertIn('ParseJsonBoolField(json, "noteVimModeEnabled")', core)
        pdf = source("src/pdf_view/input.cppinc")
        self.assertIn("if (movementKey && g_noteVimModeEnabled && !g_pdf.editingText", pdf)
        self.assertIn("if (g_noteVimModeEnabled && !g_pdf.editingText && (GetKeyState(VK_CONTROL) & 0x8000) == 0", pdf)
        probe = source("src/features/automation/main_ui_automation.cppinc")
        self.assertIn("With single-key controls off, PDF letters neither scroll nor turn pages.", probe)

    def test_status_reports_pdf_and_list_operation_state(self):
        status = between(source("src/note_view/note_view_shared.cppinc"),
                         'if (isFocusIn(g_hNoteCmdlineEdit)) modeId = L"note.assist.input.search";', "return {")
        self.assertIn('note.assist.input.textbox', status)
        self.assertIn('note.assist.input.operation', status)
        for loc in ("ja", "en"):
            text = source(f"locales/{loc}.json")
            for key in ("note.assist.input.operation", "note.assist.input.textbox"):
                self.assertIn(f'"{key}"', text)


if __name__ == "__main__":
    unittest.main()
