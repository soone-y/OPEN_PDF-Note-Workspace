"""Static wiring checks complement the executable storage/Win32 UI tests."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class WorkspaceMemoIntegration(unittest.TestCase):
    def test_all_entries_share_manager(self):
        for path in ("src/search/search.cpp", "src/settings/settings_assets.cppinc",
                     "src/app/command_dispatch.cppinc"):
            self.assertIn("ShowWorkspaceMemoWindow(g_hMainWnd)", source(path), path)
        self.assertIn("ID_WORKSPACE_MEMO", source("src/ui/menus/menu_build.cpp"))

    def test_no_old_code_or_locale_ids(self):
        pattern = re.compile(r"\b(?:ShowGlobalMemoWindow|ShowSearchMemoWindow|GlobalMemoCtx|SearchMemoCtx|ID_GLOBAL_MEMOS)\b|global_memos\.json|search_memo\.txt")
        for path in (ROOT / "src").rglob("*"):
            if path.suffix in (".cpp", ".h", ".cppinc"):
                self.assertIsNone(pattern.search(path.read_text(encoding="utf-8-sig")), str(path))
        for language in ("ja", "en"):
            catalog = json.loads(source(f"locales/{language}.json"))
            self.assertTrue(all(not re.search(r"global_memo|search_memo|memo\.unsaved", key) for key in catalog))
            self.assertIn("workspace_memo.conflict", catalog)

    def test_shortcut_precedes_main_and_modal_routing(self):
        text = source("src/app/bootstrap.cppinc")
        self.assertLess(text.index("HandleWorkspaceMemoMessage(msg)"), text.index("TranslateAcceleratorW(hWnd"))
        for name in ("settings_unified", "settings_palette", "settings_general_foundation",
                     "settings_general_schedule", "settings_shortcut_editor"):
            text = source(f"src/settings/{name}.cppinc")
            start = text.index("while (!ctx.done && GetMessageW")
            self.assertLess(text.index("HandleWorkspaceMemoMessage(msg)", start), text.index("IsDialogMessageW", start))

    def test_user_facing_name_and_tools_menu_exception(self):
        for language, name, menu_name, saved, opening, old_name in (
                ("ja", "メモ", "ワークスペース メモ", "メモを保存しました。", "メモを開く", "ワークスペースメモ"),
                ("en", "Memo", "Workspace memo", "Memo saved.", "Open memo", "workspace memo")):
            catalog = json.loads(source(f"locales/{language}.json"))
            for key, expected in {
                    "workspace_memo.title": name,
                    "workspace_memo.saved": saved,
                    "menu.tools.workspace_memo": menu_name + "...",
                    "settings.assets.workspace_memo": name + " (workspace_memo.txt)",
                    "settings.assets.workspace_memo_open": name,
                    "search.ui.workspace_memo": opening}.items():
                self.assertEqual(catalog[key], expected, key)
            for key, value in catalog.items():
                if key == "menu.tools.workspace_memo":
                    continue  # only the Tools menu uses the expanded name
                self.assertNotIn(old_name, value.lower(), key)
                self.assertNotIn(menu_name.lower(), value.lower(), key)
            help_text = source(f"docs/{language}/Help_Reference.md")
            menu_reference = f'ツールの「{menu_name}」' if language == "ja" else f"Tools > {menu_name}"
            self.assertEqual(help_text.count(menu_reference), 1)
            remaining_help = help_text.replace(menu_reference, "")
            self.assertNotIn(old_name, remaining_help.lower())
            self.assertNotIn(menu_name.lower(), remaining_help.lower())
            self.assertIn(f"##{'#' if language == 'ja' else ''} {name}\n", help_text)
            self.assertIn("__resource__/__memo__/workspace_memo.txt", help_text)

    def test_exit_and_root_switch_contract(self):
        text = source("src/ui/core/main_view_layout.cppinc")
        for function in ("HandleAppExit", "HandleManagedAbnormalExit"):
            body = text[text.index(f"static bool {function}(HWND hWnd) {{"):]
            self.assertLess(body.index("SaveWorkspaceMemoForExit()"), body.index("FinalizeAppExitState("))
        body = text[text.index("static void ReloadWorkspaceFromRoot(HWND hWnd,"):]
        self.assertLess(body.index("PrepareWorkspaceMemoRootChange()"), body.index("AcquireWorkspaceWriteLock("))
        self.assertLess(body.index("AcquireWorkspaceWriteLock("), body.index("ResetWorkspaceMemoForRootChange()"))
        self.assertLess(body.index("ResetWorkspaceMemoForRootChange()"), body.index("g_workspaceRoot = root"))
        self.assertIn("SaveWorkspaceMemoForExit(false)", source("src/ui/core/main_window_proc.cppinc"))

    def test_build_and_assets(self):
        manifest = json.loads(source("scripts/build/build_sources.json"))
        for module in ("src/workspace/workspace_memo.cpp", "src/workspace/workspace_memo_store.cpp"):
            self.assertIn(module, manifest["BuildSourceFiles"])
        text = source("src/settings/settings_assets.cppinc")
        for label in ("workspace_memo", "workspace_memo_recovery", "workspace_memo_backups"):
            self.assertIn(f'settings.assets.{label}"', text)
        self.assertIn("WorkspaceMemoPath(root)", text)
        self.assertIn("entry.displayPath", text)
        presets = source("src/workspace/workspace_config_io.cpp")
        self.assertNotIn("workspace_memo", presets)  # user text is not a settings preset

    def test_runtime_suite_and_test_only_hooks(self):
        checks = source("tests/scripts/run_repo_checks.ps1")
        self.assertIn('if (-not $SkipUiAutomation) { $memoArguments += "-AppIntegration" }', checks)
        runner = source("tests/scripts/run_workspace_memo_tests.ps1")
        self.assertIn("-DWORKSPACE_MEMO_TESTING", runner)
        self.assertIn("tests/integration/workspace_memo_app_tests.cpp", runner)
        header = source("src/workspace/workspace_memo_store.h")
        test_api = header.index("namespace testing {")
        self.assertLess(header.index("#ifdef WORKSPACE_MEMO_TESTING"), test_api)
        self.assertLess(test_api, header.index("#endif", test_api))
        for script in ("scripts/build/build_sources.json", "scripts/build/build_workspace.ps1"):
            self.assertNotIn("WORKSPACE_MEMO_TESTING", source(script))


if __name__ == "__main__":
    unittest.main()
