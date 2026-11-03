"""Source contracts; these do not replace the native isolated UI probes."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class AnnotationShortcutContractTests(unittest.TestCase):
    def test_ctrl_digits_name_stable_tools_and_leave_zoom_zero(self):
        core = source("src/core/app_core.cpp")
        rows = re.findall(r'\{ "Ctrl\+([1-9])", AnnotToolShortcutTargetKind::Detail, '
                          r'AnnotToolFamily::\w+, ToolMode::(\w+) \}', core)
        self.assertEqual(rows, list(zip("123456789", (
            "Select", "Pan", "Magnifier", "TextBox", "MarkerText", "MarkerFree",
            "Freehand", "Line", "Shape"))))
        self.assertNotIn('{ "Ctrl+0",', core)

    def test_direct_dispatch_and_toolbar_ignore_only_bar_visibility(self):
        main = source("src/main.cpp")
        policy = main.split("static bool ToolModeAvailableForCurrentPdfView(", 1)[1].split(
            "static bool ToolModeEnabledForCurrentPdfView", 1)[0]
        self.assertNotIn("AnnotToolModeUiStateFor", policy)
        for restriction in ("DocKind::Image", "IsSaveTransactionRunning", "IsPdfPreviewReadOnlyActive"):
            self.assertIn(restriction, policy)
        self.assertIn("SelectAnnotToolMode(owner, binding.mode, false, false, true)", main)
        self.assertIn("ShouldSuppressAnnotToolShortcutForFocus(msg)", main)
        self.assertIn("if (!ToolModeAvailableForCurrentPdfView(g_toolMode))",
                      source("src/ui/menus/main_toolbar_ui.cppinc"))
        dispatch = source("src/app/command_dispatch.cppinc")
        self.assertIn("direct ? ToolModeAvailableForCurrentPdfView(mode)", dispatch)
        self.assertIn("if (!direct && AnnotToolFamilyForMode(mode)", dispatch)

    def test_editor_overview_and_target_order_are_independent(self):
        editor = source("src/settings/settings_shortcut_editor.cppinc")
        for identifier in ("kShortcutEditorFixed", "kShortcutEditorUpdate", "kShortcutEditorDigits",
                           "settings.shortcut.duplicate", "IsReservedAnnotToolShortcut"):
            self.assertIn(identifier, editor)
        self.assertIn("value < kToolModeCount", editor)
        self.assertNotIn("AnnotToolModeUiOrder()", editor)
        self.assertNotIn("AnnotToolFamilyUiOrder()", editor)
        self.assertIn("if (!taken) ctx->bindings.push_back(binding)", editor)
        self.assertIn("binding.targetKind = AnnotToolShortcutTargetKind::Detail", editor)

    def test_persistence_keeps_empty_lists_and_commits_runtime_after_disk(self):
        core = source("src/core/app_core.cpp")
        sync = core.split("void SyncUserToolShortcutsToRuntime()", 1)[1].split(
            "static const char* AnnotToolUiStateKey", 1)[0]
        self.assertNotIn("loaded.push_back", sync)
        self.assertIn("return entries.empty() || !out.empty()", core)
        replace = core.split("bool ReplaceAnnotToolShortcutBindings(", 1)[1].split(
            "const char* AnnotToolFamilyKey", 1)[0]
        self.assertNotIn("if (normalized.empty())", replace)
        self.assertIn("ParseAnnotToolShortcutKeyForUser(binding.key, parsed)", replace)
        self.assertIn("IsReservedAnnotToolShortcut(binding)", replace)
        self.assertLess(replace.index("AtomicWriteUtf8WithWorkspaceDirs"),
                        replace.index("g_annotToolShortcuts ="))
        self.assertIn("json.size() > kMaxUserToolShortcutBytes", replace)

    def test_editor_selects_keys_and_validates_drafts_without_replacing_runtime(self):
        editor = source("src/settings/settings_shortcut_editor.cppinc")
        self.assertIn('ctx->key = control(0, L"COMBOBOX"', editor)
        self.assertNotIn("ShortcutEditorText", editor)
        self.assertNotIn("EM_SETLIMITTEXT", editor)
        for control in ("kShortcutEditorCtrl", "kShortcutEditorShift", "kShortcutEditorAlt"):
            self.assertIn(control, editor)
        self.assertEqual(editor.count("BS_AUTOCHECKBOX"), 3)
        validation = editor.split("static bool ReadShortcutEditorChord(", 1)[1].split(
            "static void RefreshShortcutEditorDraft", 1)[0]
        for check in ("ParseAnnotToolShortcutKeyForUser", "IsReservedAnnotToolShortcut",
                      "settings.shortcut.duplicate", "ShortcutEditorTargetLabel(other.targetKind",
                      "settings.shortcut.overrides_action", "common->mainLabel"):
            self.assertIn(check, validation)
        self.assertNotIn("ReplaceAnnotToolShortcutBindings", validation)
        self.assertEqual(editor.count("ReplaceAnnotToolShortcutBindings("), 1)
        self.assertIn("!other.alt && !other.shift", editor.split("if (id == kShortcutEditorAdd)", 1)[1])

    def test_capture_precedes_dialog_and_workspace_actions_and_drains_releases(self):
        editor = source("src/settings/settings_shortcut_editor.cppinc")
        modal_loop = editor.split("MSG msg{};", 1)[1]
        self.assertLess(modal_loop.index("FilterShortcutEditorCapture"), modal_loop.index("HandleWorkspaceMemoMessage"))
        self.assertLess(modal_loop.index("FilterShortcutEditorCapture"), modal_loop.index("IsDialogMessageW"))
        capture = editor.split("static bool FilterShortcutEditorCapture(", 1)[1].split(
            "static void ShowAnnotToolShortcutEditor", 1)[0]
        for check in ("IsChild(window, msg.hwnd)", "VK_ESCAPE", "ctx.drainingKeys = true",
                      "ctx.drainingKeys = false", "WM_SYSKEYDOWN", "WM_SYSKEYUP", "WM_SYSCHAR",
                      "VK_RMENU", "VK_LWIN", "VK_RWIN", "1LL << 30", "SetShortcutEditorChord"):
            self.assertIn(check, capture)
        for unsafe in ("ReplaceAnnotToolShortcutBindings", "TranslateMessage", "DispatchMessage", "WM_COMMAND"):
            self.assertNotIn(unsafe, capture)
        self.assertIn("LOWORD(wParam) == WA_INACTIVE", editor)
        self.assertIn("if (result <= 0)", modal_loop)
        self.assertIn("PostQuitMessage", modal_loop)

    def test_native_selector_and_ui_probes_cover_parser_and_capture_boundaries(self):
        model = source("src/settings/shortcut_key_model.h")
        for name in ("digits", "letters", "function", "numpad", "navigation", "symbols"):
            self.assertIn("settings.shortcut.group." + name, model)
        for vk in set(re.findall(r'outVk = (VK_\w+);', source("src/core/app_core.cpp"))):
            self.assertIn(vk, model, f"selector must expose parser key {vk}")
        probe = source("src/features/automation/annotation_shortcut_tests.cppinc")
        for check in ("mask < 8", "shortcut_key_model::Find(key.vk) != &key",
                      "ParseAnnotToolShortcutKeyForUser(chord, parsed)", "parsed.vk != key.vk"):
            self.assertIn(check, probe)
        ui_probe = source("tests/integration/settings_workflow_app_tests.cpp")
        for check in ("CBS_DROPDOWNLIST", "MAKEWPARAM(5202, CBN_SELCHANGE)", "VK_ESCAPE", "VK_RETURN",
                      "recorded Enter cannot execute", "Esc cancels recording only"):
            self.assertIn(check, ui_probe)
        self.assertIn("annotation_shortcuts:selector_ok cases=688", source("tests/scripts/run_ui_automation_fault_tests.ps1"))

    def test_native_probe_is_wired_to_both_pdf_and_full_suites(self):
        automation = source("src/features/automation/main_ui_automation.cppinc")
        self.assertEqual(automation.count("RunUiAutomationAnnotationShortcutScenario(owner, outError)"), 2)
        probe = source("src/features/automation/annotation_shortcut_tests.cppinc")
        for check in ("AnnotToolUiState::Hidden", "AnnotToolUiState::Disabled", "std::reverse",
                      "DocKind::Image", "ReplaceAnnotToolShortcutBindings({})", "SyncUserToolShortcutsToRuntime()",
                      "FILE_SHARE_READ", "before != after", "Ctrl+0", "duplicates"):
            self.assertIn(check, probe)
        self.assertIn("annotation_shortcuts:independent_ok cases=34",
                      source("tests/scripts/run_ui_automation_fault_tests.ps1"))


if __name__ == "__main__":
    unittest.main()
