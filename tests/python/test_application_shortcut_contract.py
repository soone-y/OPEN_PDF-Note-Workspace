"""Static shortcut integration checks. Native execution is a separate gate."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


class ApplicationShortcutContractTests(unittest.TestCase):
    def test_closing_uses_shift_q_with_no_plain_q_or_old_w_alias(self):
        header = read("src/ui/application_shortcuts.h")
        self.assertIn("{Action::CloseFile, 'Q', false, true, 3", header)
        self.assertIn("{Action::CloseAll, 'Q', true, true, 3", header)
        self.assertNotIn('L"Ctrl+Q"', header)
        self.assertNotIn('L"Ctrl+Alt+Q"', header)
        self.assertNotIn('L"Ctrl+W"', header)
        self.assertNotIn('L"Ctrl+Shift+W"', header)
        reader = read("src/readonly_viewer/main.cpp")
        route = reader.split("bool HandleReaderKey(const MSG& message) {", 1)[1].split("LRESULT CALLBACK WndProc", 1)[0]
        self.assertNotIn("message.wParam == 'W'", route)
        for locale in ("ja", "en"):
            welcome = json.loads(read(f"locales/{locale}.json"))["reader_next.welcome_keys"]
            self.assertIn("Ctrl+Shift+Q", welcome)
            self.assertNotIn("Ctrl+W", welcome)
        self.assertIn("application_shortcuts:close_guard_ok cases=8", read("tests/scripts/run_ui_automation_fault_tests.ps1"))

    def test_required_actions_have_unique_shared_chords_and_localized_labels(self):
        header = read("src/ui/application_shortcuts.h")
        rows = re.findall(r'\{Action::(\w+), (.*?), (true|false), (true|false), ([13]), '
                          r'(true|false), (true|false), L"([^"]+)", L"([^"]+)", (nullptr|L"[^"]+")\}', header)
        self.assertEqual(len(rows), 13)
        self.assertEqual({r[0] for r in rows}, {"JumpPage", "FitPage", "OpenFile", "CloseFile", "CloseAll",
            "ToggleAnnotations", "RotateLeft", "RotateRight", "ResetRotation", "Memo", "Export", "QuickPdf", "QuickNote"})
        self.assertEqual(len({r[7] for r in rows}), 13)
        fit = next(row for row in rows if row[0] == "FitPage")
        self.assertEqual(fit[1:8], ("'E'", "false", "false", "3", "true", "false", "Ctrl+E"))
        for locale in ("ja", "en"):
            catalog = json.loads(read(f"locales/{locale}.json"))
            for row in rows:
                self.assertIn(row[8], catalog)
                if row[9] != "nullptr": self.assertIn(row[9][2:-1], catalog)
        for action in ("Export", "QuickPdf", "QuickNote"):
            self.assertTrue(next(row for row in rows if row[0] == action)[7].startswith("Ctrl+Alt+"))
        self.assertIn("static_assert(RegistryValid()", header)

    def test_main_reuses_guarded_commands_and_preserves_custom_chords(self):
        main = read("src/main.cpp")
        route = main.split("bool HandleMainApplicationShortcutInLoop(HWND owner, const MSG& msg) {", 1)[1]
        for token in ("IsWindowEnabled(owner)", "MainLoopShortcutMessageBelongsToOwner", "VK_RMENU",
                      "ResolveAnnotToolShortcut(msg, &custom)", "MayRun", "IsNoteImeComposing()", "g_pdf.imeComposing",
                      "IsExplicitImeTargetWindow", "MF_GRAYED", "SendMessageW(owner, WM_COMMAND"):
            self.assertIn(token, route)
        for command in ("ID_VIEW_JUMP_PAGE", "ID_VIEW_FIT_PAGE", "ID_FILE_IMPORT_FILE", "ID_FILE_CLOSE_FOCUSED", "ID_TOOL_TOGGLE_ANNOTS",
                        "ID_VIEW_ROTATE_PAGE_LEFT", "ID_VIEW_ROTATE_PAGE_RIGHT", "ID_WORKSPACE_MEMO",
                        "ID_FILE_EXPORT_COMBINED", "ID_FILE_EXPORT_PDF_QUICK", "ID_FILE_EXPORT_NOTE_TEXT_QUICK"):
            self.assertIn(command, route)
        self.assertLess(route.index("MayRun("), route.index("msg.lParam & 0x40000000"))
        bootstrap = read("src/app/bootstrap.cppinc")
        self.assertLess(bootstrap.index("HandleMainApplicationShortcutInLoop(hWnd, msg)"),
                        bootstrap.index("HandleMainPdfZoomShortcutInLoop(hWnd, msg)"))

    def test_reader_uses_shared_keys_without_adding_main_only_writes(self):
        reader = read("src/readonly_viewer/main.cpp")
        route = reader.split("bool HandleReaderKey(const MSG& message) {", 1)[1].split("LRESULT CALLBACK WndProc", 1)[0]
        for token in ("WM_SYSKEYDOWN", "Surface::Reader", "HasActivePdfPreview()", "MayRun", "VK_RMENU", "!command && ctrl && !alt"):
            self.assertIn(token, route)
        for command in ("kCmdPdfJumpPage", "kCmdPdfFitPage", "kCmdPdfToggleAnnots", "kCmdPdfExportAnnotatedPdf", "kCmdPdfRotateLeft", "kCmdPdfRotateRight"):
            self.assertIn(command, route)
        self.assertNotIn("QuickPdf", route)
        self.assertNotIn("Action::Memo", route)
        self.assertIn("ReopenLastClosedTab()", route)
        self.assertIn("g_readerShortcutInput.Suppress(GetFocus())", route)
        self.assertLess(route.index("MayRun("), route.index("message.lParam & 0x40000000"))

    def test_jump_reuses_validated_prompt_and_compact_range_navigation(self):
        panel = read("src/readonly_viewer/pdf_preview_panel.cpp")
        jump = panel.split("void PdfPreviewPanel_PromptJumpPage(HWND hwnd) {", 1)[1].split("bool PdfPreviewPanel_AreAnnotationsVisible", 1)[0]
        self.assertIn("PromptPageRange(hwnd, g_state.pdf->pageCount, page, unusedLast, true)", jump)
        self.assertIn("PdfPreviewPanel_JumpToPdfLocation(hwnd, page - 1, 0.0, false)", jump)
        self.assertIn("end > static_cast<UINT>(state->pageCount)", panel)
        prompt = panel.split("bool PromptPageRange(", 1)[1].split("void ChoosePageRange", 1)[0]
        self.assertIn("GetAncestor(owner, GA_ROOT)", prompt)
        self.assertIn("if (!state.accepted) return false", prompt)
        self.assertLess(prompt.index("if (!state.accepted) return false"), prompt.index("firstPage = state.firstPage"))
        self.assertIn("ApplyAuxiliaryWindowCorners(hwnd)", panel)
        self.assertIn("ui::ParsePageNumber(input, g_pdf.pageCount, page)", read("src/app/command_dispatch.cppinc"))
        self.assertIn("ui::ParsePageNumber(text, maximum, page)", panel)

    def test_search_ime_observer_is_fail_closed_and_removed_with_the_control(self):
        guard = read("src/ui/shortcut_input_guard.h")
        for token in ("SetWindowSubclass", "RemoveWindowSubclass", "WM_IME_STARTCOMPOSITION",
                      "WM_IME_ENDCOMPOSITION", "WM_KILLFOCUS", "WM_NCDESTROY", "!ready_ || composing_", "DefSubclassProc"):
            self.assertIn(token, guard)
        reader = read("src/readonly_viewer/main.cpp")
        self.assertIn("g_readerShortcutInput.Attach(search)", reader)

    def test_overviews_and_menus_use_registry_not_parallel_key_lists(self):
        main_help = read("src/ui/shortcut_help.cpp")
        self.assertIn("ApplicationRows(bindings)", main_help)
        self.assertIn("IsOverridden(binding, bindings)", main_help)
        self.assertIn("shortcut.overridden", main_help)
        self.assertIn("CommonOverview(ctx->bindings)", read("src/settings/settings_shortcut_editor.cppinc"))
        self.assertIn("shortcut_help::MainMenuLabel", read("src/ui/menus/menu_build.cpp"))
        reference = read("src/readonly_viewer/shortcut_reference.h")
        for token in ("application_shortcuts::bindings", "Surface::Reader", "ES_READONLY", "ApplyAuxiliaryWindowCorners",
                      "GetMessageW", "PostQuitMessage", "DestroyWindow(window)", "common.close"):
            self.assertIn(token, reference)

    def test_native_policy_cases_are_required_by_the_runner(self):
        native = read("src/features/automation/annotation_shortcut_tests.cppinc")
        self.assertIn("application_shortcuts::Resolve", native)
        self.assertIn("application_shortcuts::MayRun", native)
        self.assertIn("application_shortcuts:policy_ok cases=26", read("tests/scripts/run_ui_automation_fault_tests.ps1"))
        self.assertIn("application_shortcuts:page_number_ok cases=9", read("tests/scripts/run_ui_automation_fault_tests.ps1"))


if __name__ == "__main__": unittest.main()
