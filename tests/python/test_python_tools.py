from __future__ import annotations

import importlib.util
import io
import csv
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from contextlib import contextmanager
from pathlib import Path
from unittest import mock
import uuid


REPO_ROOT = Path(__file__).resolve().parents[2]
TEST_TMP_ROOT = Path(tempfile.gettempdir()) / "pdf_note_workspace_py_tmp"


def load_module(name: str, rel_path: str):
    path = REPO_ROOT / rel_path
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ReaderMigrationTests = load_module(
    "test_readonly_viewer_migration", "tests/python/test_readonly_viewer_migration.py"
).ReaderMigrationTests
SearchCancelTests = load_module(
    "test_search_cancel", "tests/python/test_search_cancel.py"
).SearchCancelTests
WritingGuideTests = load_module(
    "test_note_writing_guide", "tests/python/test_note_writing_guide.py"
).WritingGuideTests
PrototypeLicenseTests = load_module(
    "test_readonly_viewer_next_licenses", "tests/python/test_readonly_viewer_next_licenses.py"
).PrototypeLicenseTests
AnnotationShortcutContractTests = load_module(
    "test_annotation_shortcut_contract", "tests/python/test_annotation_shortcut_contract.py"
).AnnotationShortcutContractTests
ApplicationShortcutContractTests = load_module(
    "test_application_shortcut_contract", "tests/python/test_application_shortcut_contract.py"
).ApplicationShortcutContractTests
ImagePdfOrientationTests = load_module(
    "test_image_pdf_orientation", "tests/python/test_image_pdf_orientation.py"
).ImagePdfOrientationTests


migrate_clrop_v1 = load_module("migrate_clrop_v1", "tools/migration/migrate_clrop_v1.py")
analyze_build_logs = load_module("analyze_build_logs", "tools/metrics/analyze_build_logs.py")
distribution_size_report = load_module(
    "distribution_size_report", "tools/metrics/distribution_size_report.py"
)
analyze_document_language = load_module("analyze_document_language", "tools/metrics/analyze_document_language.py")
analyze_repo = load_module("analyze_repo", "tools/metrics/code_metrics/analyze_repo.py")
code_metrics_gui = load_module("code_metrics_gui", "tools/metrics/code_metrics/gui.py")
text_integrity_gate = load_module("text_integrity_gate", "tools/release_checks/text_integrity_gate.py")
libreoffice_reduce = load_module("libreoffice_reduce", "tools/libreoffice/libreoffice_reduce.py")
libreoffice_build_env_check = load_module("libreoffice_build_env_check", "tools/libreoffice/libreoffice_build_env_check.py")
libreoffice_smoke_test = load_module("libreoffice_smoke_test", "tools/libreoffice/libreoffice_smoke_test.py")
libreoffice_conversion_quality_test = load_module(
    "libreoffice_conversion_quality_test", "tools/libreoffice/libreoffice_conversion_quality_test.py"
)
# Include custom build-input regressions in the ordinary Python tools suite.
LibreOfficeCustomPatchTests = load_module(
    "libreoffice_custom_patch_tests", "tests/python/test_libreoffice_custom_patches.py"
).LibreOfficeCustomPatchTests
LibreOfficeUpstreamConversionTests = load_module(
    "libreoffice_upstream_conversion_tests", "tests/python/test_libreoffice_upstream_conversion.py"
).LibreOfficeUpstreamConversionTests
LibreOfficeConversionCorpusTests = load_module(
    "libreoffice_conversion_corpus_tests", "tests/python/test_libreoffice_conversion_corpus.py"
).LibreOfficeConversionCorpusTests
LibreOfficeConversionExpectationsTests = load_module(
    "libreoffice_conversion_expectations_tests", "tests/python/test_libreoffice_conversion_expectations.py"
).ConversionExpectationsTests
render_human_docs = load_module("render_human_docs", "site/github/scripts/render_human_docs.py")
build_public_site = load_module("build_public_site", "site/github/scripts/build_public_site.py")
validate_public_site = load_module("validate_public_site", "site/github/scripts/validate_public_site.py")
validate_introduction_site = load_module(
    "validate_introduction_site", "site/cloudflare/scripts/validate_introduction_site.py"
)
SiteValidationRegressionTests = load_module(
    "site_validation_regressions", "tests/python/test_site_validation.py"
).SiteValidationRegressionTests
release_license_gate = load_module("release_license_gate", "tools/release_checks/release_license_gate.py")
release_text_gate = load_module("release_text_gate", "tools/release_checks/release_text_gate.py")
release_locale_content_gate = load_module(
    "release_locale_content_gate", "tools/release_checks/release_locale_content_gate.py"
)
validate_locale_usage = load_module(
    "validate_locale_usage", "tools/localization/validate_locale_usage.py"
)
public_snapshot_content_gate = load_module(
    "public_snapshot_content_gate", "tools/release_checks/public_snapshot_content_gate.py"
)
release_set_integrity_gate = load_module("release_set_integrity_gate", "tools/release_checks/release_set_integrity_gate.py")
release_startup_smoke_gate = load_module("release_startup_smoke_gate", "tools/release_checks/release_startup_smoke_gate.py")
repo_hygiene_gate = load_module("repo_hygiene_gate", "tools/release_checks/repo_hygiene_gate.py")
sync_publication_inputs = load_module(
    "sync_publication_inputs", "tools/dev/sync_publication_inputs.py"
)
cpp_include_visualizer = load_module("cpp_include_visualizer", "tools/metrics/cpp_include_visualizer.py")
md_structure_scanner = load_module("md_structure_scanner", "tools/dev/md_structure_scanner.py")
persistence_index = load_module("persistence_index", "tools/dev/persistence_index.py")
change_impact = load_module("change_impact", "tools/dev/change_impact.py")
export_public_snapshot = load_module("export_public_snapshot", "tools/dev/export_public_snapshot.py")
binary_scan = load_module("binary_scan", "tools/release_checks/binary_scan.py")
libreoffice_runtime_analyzer = load_module(
    "libreoffice_runtime_analyzer", "tools/libreoffice/libreoffice_runtime_analyzer.py"
)
libreoffice_runtime_dynamic_probe = load_module(
    "libreoffice_runtime_dynamic_probe", "tools/libreoffice/libreoffice_runtime_dynamic_probe.py"
)
libreoffice_runtime_removal_trial = load_module(
    "libreoffice_runtime_removal_trial", "tools/libreoffice/libreoffice_runtime_removal_trial.py"
)
libreoffice_runtime_gate = load_module("libreoffice_runtime_gate", "tools/release_checks/libreoffice_runtime_gate.py")
sanitize_libreoffice_runtime_release = load_module(
    "sanitize_libreoffice_runtime_release", "tools/release_checks/sanitize_libreoffice_runtime_release.py"
)
validate_codebase = load_module("validate_codebase", "tests/python/validate_codebase.py")
pe_fixtures = load_module("pe_fixtures", "tests/python/pe_fixtures.py")
# Keep failure-propagation regressions in the ordinary Python tools gate.
CheckFailureContractTests = load_module(
    "check_failure_contracts", "tests/python/test_check_failure_contracts.py"
).CheckFailureContractTests
PublicStatisticsTests = load_module(
    "public_statistics_tests", "tests/python/test_public_statistics.py"
).PublicStatisticsTests
PreparedStatisticsTests = sys.modules["public_statistics_tests"].PreparedStatisticsTests
HistoricalStatisticsTests = sys.modules["public_statistics_tests"].HistoricalStatisticsTests
StatisticsPipelineTests = sys.modules["public_statistics_tests"].StatisticsPipelineTests
# Release/build regressions must run in the ordinary gate, not only on demand.
ReleaseEntrypointTests = load_module(
    "release_entrypoint_tests", "tests/python/test_release_entrypoints.py"
).ReleaseEntrypointTests
BuildVersionRefreshTests = load_module(
    "build_version_refresh_tests", "tests/python/test_build_version_refresh.py"
).BuildVersionRefreshTests
OfficeWorkerStaticContractTests = load_module(
    "office_worker_static_contract_tests", "tests/python/test_office_worker_static_contract.py"
).OfficeWorkerStaticContractTests
BackgroundAppTests = load_module(
    "background_app_tests", "tests/python/test_background_app.py"
).BackgroundAppTests
IsolatedAppTests = load_module(
    "isolated_app_tests", "tests/python/test_isolated_app_test.py"
).IsolatedAppTests
IsolatedAppWindowsTests = sys.modules["isolated_app_tests"].IsolatedAppWindowsTests

# Run executable JSON persistence regressions in the ordinary repository gate.
SetupJsonRoundTripTests = load_module(
    "setup_json_roundtrip", "tests/python/test_setup_json_roundtrip.py"
).SetupJsonRoundTripTests


def _shortcut_chord(key: str) -> str:
    aliases = {
        "CONTROL": "CTRL", "MENU": "ALT",
        "ARROWLEFT": "LEFT", "ARROWRIGHT": "RIGHT",
        "ARROWUP": "UP", "ARROWDOWN": "DOWN",
    }
    tokens = {aliases.get(part.strip().upper(), part.strip().upper()) for part in key.split("+") if part.strip()}
    main = next((token for token in tokens if token not in {"CTRL", "ALT", "SHIFT"}), "")
    modifiers = tuple(sorted(token for token in tokens if token in {"CTRL", "ALT", "SHIFT"}))
    return "+".join((*modifiers, main))


def _validate_annotation_shortcuts(entries: list[dict]) -> None:
    seen: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("key"), str):
            raise ValueError("shortcut entry must contain a string key")
        has_tool = isinstance(entry.get("tool"), str)
        has_category = isinstance(entry.get("category"), str)
        if has_tool == has_category:
            raise ValueError("shortcut entry must contain exactly one target")
        chord = _shortcut_chord(entry["key"])
        if not chord or chord in seen:
            raise ValueError("shortcut key is empty or duplicated")
        if chord in {"ALT+CTRL+LEFT", "ALT+CTRL+RIGHT", "ALT+CTRL+UP", "ALT+CTRL+DOWN"}:
            raise ValueError("fixed annotation navigation shortcut is reserved")
        seen.add(chord)


class WriteChecksParentFallbackContractTests(unittest.TestCase):
    def test_only_missing_session_children_can_use_the_existing_parent_probe(self) -> None:
        source = (REPO_ROOT / "src/diagnostics/write_checks.cpp").read_text(encoding="utf-8")
        probe = source[source.index("Result ProbeSessionDestination("):source.index("Result ReadProbeFile(")]
        for guard in ("result.outcome != Outcome::Unavailable", "result.step != Step::Open",
                      "result.error != ERROR_FILE_NOT_FOUND", "result.error != ERROR_PATH_NOT_FOUND",
                      "kind != Kind::PdfWrite && kind != Kind::NoteWrite", "!IsSafeLocalPath(session)",
                      "SameTarget(target.wstring(), (session / leaf).wstring())",
                      "parentAttributes & FILE_ATTRIBUTE_DIRECTORY", "parentAttributes & FILE_ATTRIBUTE_REPARSE_POINT",
                      "childError != ERROR_FILE_NOT_FOUND", "childError != ERROR_PATH_NOT_FOUND"):
            self.assertIn(guard, probe)
        self.assertLess(probe.index("parentAttributes"), probe.index("ProbeDirectory(session, true)"))
        self.assertIn("result.operationPath = session.wstring()", probe)
        self.assertNotIn("CreateDirectory", probe)
        self.assertNotIn("EnsureDirectory", probe)

    def test_display_uses_persisted_evidence_without_filesystem_queries(self) -> None:
        source = (REPO_ROOT / "src/diagnostics/write_checks.cpp").read_text(encoding="utf-8")
        classify = source[source.index("bool IsParentDestinationProbe("):source.index("Result ProbeSessionDestination(")]
        self.assertIn("record.result.source != Source::Manual", classify)
        self.assertIn("SameTarget(record.target", classify)
        for forbidden in ("IsSafeLocalPath", "GetFileAttributes", "ProbeDirectory"):
            self.assertNotIn(forbidden, classify)
        ui = (REPO_ROOT / "src/ui/dialogs/write_checks_dialog.cpp").read_text(encoding="utf-8")
        self.assertIn("auto status = ResultText(record, ctx.loadOk)", ui)
        self.assertIn('T(L"dialog.details.status") + L"\\n" + ResultText(record, ctx.loadOk)', ui)
        self.assertIn('L"checks.description.parent_probe" : L"checks.description.destination"', ui)
        self.assertIn("Execute(kind, target, session, ctx.cancel)", ui)
        self.assertIn("std::filesystem::path(ctx.snapshotSession).lexically_normal()", ui)
        self.assertIn("ctx.targets[8] != CurrentPdfDirectory().lexically_normal()", ui)
        self.assertIn("ctx.targets[9] != CurrentNoteDirectory().lexically_normal()", ui)
        self.assertIn('L"checks.operation_path"), record->result.operationPath', ui)
        # The current ledger already preserves manual operationPath; no format migration.
        self.assertIn('const std::string header = "PDF_NOTE_WRITE_CHECKS_2\\n"', source)
        self.assertIn("std::quoted(Encode(row.result.operationPath))", source)
        self.assertIn("Decode(operation, row.result.operationPath)", source)

    def test_short_results_and_scope_explanation_exist_in_both_locales(self) -> None:
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertEqual(catalog["checks.parent_result"].count("{RESULT}"), 1)
            self.assertTrue(catalog["checks.unavailable_short"])
            self.assertTrue(catalog["checks.description.parent_probe"])
        native = (REPO_ROOT / "tests/unit/write_checks_tests.cpp").read_text(encoding="utf-8")
        for case in ("absentParent", "wrongLeaf", "wrongKind", "canceledDestination", "IsParentDestinationProbe(parentRows.front())"):
            self.assertIn(case, native)


class BottomPanePersistenceContractTests(unittest.TestCase):
    def test_dedicated_settings_page_and_menu_keep_existing_page_ids(self) -> None:
        nav = (REPO_ROOT / "src/settings/settings_navigation.h").read_text(encoding="utf-8")
        self.assertIn("QuickNote = 14, BottomPane = 15, Count = 16", nav)
        self.assertIn('{BottomPane, EnvironmentGroup, L"settings.page.bottom_pane"}', nav)
        self.assertIn("sidebarOrder = {Environment, BottomPane, Palette", nav)
        menu = (REPO_ROOT / "src/ui/menus/menu_build.cpp").read_text(encoding="utf-8")
        self.assertIn('ID_SETTINGS_BOTTOM_PANE, text(L"menu.settings.bottom_pane")', menu)
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        self.assertIn("case ID_SETTINGS_BOTTOM_PANE:\n        ShowBottomPaneSettingsDialog(hWnd)", dispatch)
        dialog = (REPO_ROOT / "src/settings/settings_general_dialog.cppinc").read_text(encoding="utf-8")
        own = dialog[dialog.index("if (ctx->pageMode == GeneralSettingsPageMode::BottomPane)"):dialog.index("if (ctx->pageMode == GeneralSettingsPageMode::Basic)")]
        self.assertIn("ctx->cbBottomPane = addCombo", own)
        self.assertIn("settings.bottom_pane.persistence_hint", own)
        self.assertEqual(dialog.count("ctx->cbBottomPane = addCombo"), 1)

    def test_view_is_runtime_only_and_settings_commit_before_runtime_activation(self) -> None:
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        start = main.index("static void SetBottomPaneRuntimeMode(BottomPanePin pin, BottomNoteMode noteMode) {")
        runtime = main[start:main.index("HACCEL BuildAccelerators()", start)]
        self.assertIn("g_bottomPanePin = pin", runtime)
        self.assertNotIn("g_config", runtime)
        self.assertNotIn("Persist", runtime)
        core = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        persist = core[core.index("bool TryPersistConfig() {"):core.index("void PersistConfig() {")]
        self.assertNotIn("g_config.bottomPanePin =", persist)
        self.assertNotIn("g_config.bottomNoteMode =", persist)
        self.assertIn('WideToUTF8(cfg.bottomPanePin.empty() ? L"note" : cfg.bottomPanePin)', core)
        apply = (REPO_ROOT / "src/settings/settings_general_controls.cppinc").read_text(encoding="utf-8")
        start = apply.index("if (ctx->cbBottomPane) {")
        self.assertLess(apply.index("g_config.bottomPanePin = BottomPanePinToString(wantPin)", start), apply.index("const bool saved = TryPersistConfig()"))
        self.assertLess(apply.index("const bool saved = TryPersistConfig()"), apply.index("g_bottomPanePin = ParseBottomPanePin(g_config.bottomPanePin)"))
        self.assertNotIn("configuredPin != wantPin", apply)
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        self.assertIn("kSettingsNavBottomPane};", shell)
        self.assertIn("ctx->currentCategory == page", shell)
        self.assertIn("!UnifiedPageDirty(ctx, page) && !applyBottomPane", shell)

    def test_failed_save_preserves_temporary_mode_and_native_regressions_are_present(self) -> None:
        common = (REPO_ROOT / "src/settings/settings_common.cppinc").read_text(encoding="utf-8")
        restore = common[common.index("static void RestoreSettingsRuntimeAfterFailedSave"):common.index("static void RequestLayout(HWND owner) {")]
        self.assertLess(restore.index("const auto bottomPin = g_bottomPanePin"), restore.index("ApplyConfigToUI(owner)"))
        self.assertLess(restore.index("ApplyConfigToUI(owner)"), restore.index("g_bottomPanePin = bottomPin"))
        self.assertIn("g_bottomNoteMode = bottomMode", restore)
        native = (REPO_ROOT / "tests/integration/settings_workflow_app_tests.cpp").read_text(encoding="utf-8")
        for case in ("applying an unchanged saved choice overrides the temporary View mode",
                     "saving unrelated settings preserves the temporary bottom-pane mode",
                     "failed bottom-pane save preserves disk, temporary mode and retryable dialog draft",
                     "restart restores the dialog default rather than the final temporary mode"):
            self.assertIn(case, native)
        self.assertIn('BottomPanePersistenceMatrix(executable, root / L"bottom_pane")', native)
        runner = (REPO_ROOT / "tests/scripts/run_settings_workflow_tests.ps1").read_text(encoding="utf-8")
        self.assertIn("[PASS] bottom-pane persistent default / temporary View / dialog precedence / failure / restart", runner)
        for locale, temporary in (("ja", "一時"), ("en", "temporary")):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            for key in ("ui.menu_bottom_pane", "menu.view.readability"):
                self.assertIn(temporary, catalog[key])
            self.assertTrue(catalog["menu.settings.bottom_pane"])


class PdfRotationPersistenceContractTests(unittest.TestCase):
    def test_rotation_restores_before_page_geometry_fit_and_anchor(self) -> None:
        document = (REPO_ROOT / "src/pdf_view/document_io.cppinc").read_text(encoding="utf-8")
        start = document.index("bool LoadPdfDocument(")
        pdf = document[start:]
        self.assertLess(pdf.index("RestorePendingPdfPageRotations()"), pdf.index("BuildPageCaches(pdfWnd)"))
        self.assertNotIn("RestorePendingPdfPageRotations()", document[:start])
        state = (REPO_ROOT / "src/pdf_view/view_state.cppinc").read_text(encoding="utf-8")
        queue = state[state.index("static void QueuePendingPdfRestoreForPath"):state.index("static void RestorePendingPdfPageRotations")]
        self.assertIn("g_restorePdfOpeningAnchor =", queue)
        self.assertIn("ReadPdfViewRecordForPath", queue)
        self.assertNotIn('L"page")) return', queue)
        self.assertIn("turns.size() == static_cast<size_t>(g_pdf.pageCount)", state)
        zoom = (REPO_ROOT / "src/pdf_view/zoom_flow.cppinc").read_text(encoding="utf-8")
        fit = zoom[zoom.index("static void CompleteInitialPdfFit"):zoom.index("static std::optional<double> PreferredInitialPdfScale")]
        self.assertLess(fit.index("ApplyPdfScaleDirect"), fit.index("ApplyPendingPdfRestore"))

    def test_rotation_is_captured_written_and_protects_unreadable_records(self) -> None:
        state = (REPO_ROOT / "src/pdf_view/view_state.cppinc").read_text(encoding="utf-8")
        self.assertIn("out.pageRotations = g_pdf.pageViewRotations", state)
        self.assertIn("view_rotation_record::Decode(rotations, rec.pageRotations)", state)
        self.assertIn("view_rotation_record::Encode(rec.pageRotations)", state)
        save = state[state.index("void SaveLastPdfViewInfo()"):state.index("bool MovePdfViewInfoPath")]
        self.assertLess(save.index("if (!complete)"), save.index("entries[record.path] = record"))
        self.assertIn('L"pdf.view_state.save_failed"', save)
        source = (REPO_ROOT / "src/pdf_view/pdf_view.cpp").read_text(encoding="utf-8")
        rotate = source[source.index("static bool RotatePdfPageViews"):source.index("bool RotateCurrentPdfPageView")]
        self.assertIn("SaveLastPdfViewInfo()", rotate)
        codec = (REPO_ROOT / "src/pdf_view/view_rotation_record.h").read_text(encoding="utf-8")
        self.assertIn("text[i] < '0' || text[i] > '3'", codec)
        self.assertIn("kMaxPages", codec)
        self.assertLess(codec.index("std::vector<int> parsed"), codec.index("out = std::move(parsed)"))

    def test_persistent_menu_and_native_regressions_are_defined(self) -> None:
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertNotIn("一時", catalog["menu.view.rotation"])
            self.assertNotIn("temporary", catalog["menu.view.rotation"])
            self.assertTrue(catalog["pdf.view_state.save_failed"])
        native = (REPO_ROOT / "src/pdf_view/page_rotation_tests.cppinc").read_text(encoding="utf-8")
        self.assertIn("stored.pageRotations != persistedTurns", native)
        self.assertIn('{L"width", L"page", L"previous"}', native)
        unit = (REPO_ROOT / "tests/unit/page_rotation_tests.cpp").read_text(encoding="utf-8")
        self.assertIn("invalid suffix preserves output", unit)


class DialogActionLabelTests(unittest.TestCase):
    def test_help_groups_checks_and_document_information_without_changing_routes(self) -> None:
        menu = (REPO_ROOT / "src/ui/menus/menu_build.cpp").read_text(encoding="utf-8")
        start = menu.index("HMENU checks = CreatePopupMenu();")
        end = menu.index('text(L"menu.help.checks").c_str());', start)
        submenu = menu[start:end]
        commands = ("ID_WRITE_CHECKS", "ID_SETTINGS_ASSETS", "ID_HELP_PDF_INFO", "ID_HELP_NOTE_INFO")
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        for command in commands:
            self.assertIn(f"AppendMenuW(checks, MF_STRING, {command},", submenu)
            self.assertNotIn(f"AppendMenuW(help, MF_STRING, {command},", menu)
            self.assertEqual(menu.count(f"MF_STRING, {command},"), 1)
            self.assertIn(f"case {command}:", dispatch)
        self.assertIn("AppendMenuW(help, MF_POPUP, reinterpret_cast<UINT_PTR>(checks),", submenu)
        self.assertIn("AppendMenuW(help, MF_STRING, ID_HELP_SHOW_LOG_PATH,", menu)
        for locale, label in (("ja", "確認"), ("en", "Checks")):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertEqual(catalog["menu.help.checks"], label)

    def test_preset_restore_dialog_uses_the_output_dialog_theme_contract(self) -> None:
        ja = json.loads((REPO_ROOT / "locales/ja.json").read_text(encoding="utf-8"))
        en = json.loads((REPO_ROOT / "locales/en.json").read_text(encoding="utf-8"))
        self.assertEqual(ja["settings.workspace_tools.title"], "設定プリセットと復元")
        self.assertEqual(en["settings.workspace_tools.title"], "Settings Presets and Restore")
        self.assertEqual(ja["settings.preset.close"], "やめる")
        source = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")
        dialog = source[source.index("static LRESULT CALLBACK SettingsPresetDialogProc"):
                        source.index("void ShowSettingsPresetDialog")]
        output = (REPO_ROOT / "src/ui/dialogs/export_dialog.cpp").read_text(encoding="utf-8")
        for marker in ("case WM_THEMECHANGED:", "case WM_ERASEBKGND:", "case WM_CTLCOLORSTATIC:",
                       "case WM_DRAWITEM:", "ThemeCtlColorPanel(", "DrawThemeButton(",
                       "WS_CAPTION | WS_POPUPWINDOW", "RegisterAppExitBlockingDialog("):
            self.assertIn(marker, dialog)
            self.assertIn(marker, output)
        helpers = source[source.index("static void SetPresetDialogFont"):source.index("static void ShowWorkspaceToolsPage")]
        self.assertNotIn("DEFAULT_GUI_FONT", helpers)
        self.assertIn("if (LOWORD(wParam) == IDCANCEL)", dialog)
        self.assertIn("return ctx ? DefWindowProcW(hWnd, message, wParam, lParam) : FALSE;", dialog)
        self.assertIn("kWorkspaceToolsPresetTab = 7101", source)
        self.assertIn("kWorkspaceToolsRestoreTab = 7102", source)

    def test_settings_persistence_uses_save_without_mislabeling_color_selection(self) -> None:
        for locale, label in (("ja", "保存"), ("en", "Save")):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertEqual(catalog["dialog.action.save"], label)
            self.assertIn(label, catalog["settings.navigation.save_close"])
            self.assertIn(label, catalog["settings.navigation.save"])
            self.assertNotEqual(catalog["settings.navigation.save_close"], catalog["settings.palette.use_color"])
        shortcuts = (REPO_ROOT / "src/settings/settings_shortcut_editor.cppinc").read_text(encoding="utf-8")
        self.assertIn('localization::Text(L"settings.shortcut.save_hint")', shortcuts)
        palette = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        self.assertIn('selecting ? L"settings.palette.use_color" : L"settings.navigation.save_close"', shell)
        self.assertNotIn('L"BUTTON", UiApplyLabel().c_str()', palette)

    def test_common_choices_have_distinct_short_labels_in_both_locales(self) -> None:
        for locale, labels in (("ja", ("する", "しない", "やめる")),
                               ("en", ("Do", "Don't", "Exit"))):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertEqual(catalog["dialog.button.yes"], labels[0])
            self.assertEqual(catalog["dialog.button.ok"], labels[0])
            self.assertEqual(catalog["dialog.button.keep_open"], labels[1])
            self.assertEqual(catalog["dialog.button.no"], labels[2])
            self.assertEqual(catalog["dialog.button.cancel"], labels[2])
            self.assertFalse(set(catalog.values()) & {"OK", "Cancel", "キャンセル", "Yes", "No", "はい", "いいえ"})

    def test_app_buttons_do_not_bypass_localized_action_labels(self) -> None:
        opaque_button = re.compile(r'L"BUTTON",\s*L"(?:OK|Cancel|キャンセル|はい|いいえ)"')
        for path in (REPO_ROOT / "src").rglob("*"):
            if path.suffix in {".cpp", ".cppinc", ".h"}:
                self.assertIsNone(opaque_button.search(path.read_text(encoding="utf-8")), str(path))
        core = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        self.assertNotIn("ChooseColorW(", core)
        self.assertIn("return SelectColorInPaletteSettings(owner, initial, outColor);", core)

    def test_keep_open_is_not_returned_as_a_closed_dialog_result(self) -> None:
        source = (REPO_ROOT / "src/ui/dialogs/dialogs.cpp").read_text(encoding="utf-8")
        self.assertIn("if (ctx->buttonSpecs[i].result == SilentDialogResult::None) return 0;", source)
        self.assertIn("ctx->options.yesLabel.empty() && ctx->options.noLabel.empty()", source)
        automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        self.assertIn("SendMessageW(keep, BM_CLICK, 0, 0)", automation)
        self.assertIn("automation:silent_choices_keep_open_execute_exit_ok", automation)

    def test_file_picker_actions_use_operation_labels(self) -> None:
        ja = json.loads((REPO_ROOT / "locales/ja.json").read_text(encoding="utf-8"))
        en = json.loads((REPO_ROOT / "locales/en.json").read_text(encoding="utf-8"))
        self.assertEqual(ja["dialog.action.import"], "取り込む")
        self.assertEqual(ja["dialog.action.convert"], "変換")
        self.assertEqual(ja["dialog.action.restore"], "復元")
        self.assertEqual(ja["dialog.action.delete"], "削除")
        self.assertEqual(en["dialog.action.import"], "Import")
        self.assertEqual(en["dialog.action.convert"], "Convert")
        self.assertEqual(en["dialog.action.restore"], "Restore")
        self.assertEqual(en["dialog.action.delete"], "Delete")
        actions = (REPO_ROOT / "src/workspace/workspace_actions.cpp").read_text(encoding="utf-8")
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        browser = (REPO_ROOT / "src/ui/lists/main_local_path_browser.cppinc").read_text(encoding="utf-8")
        self.assertIn('L"dialog.action.import"', actions)
        self.assertIn('L"dialog.action.convert"', actions)
        self.assertIn('L"dialog.action.delete"', dispatch)
        self.assertIn("SetOkButtonLabel(action.c_str())", browser)
        self.assertIn("state.confirmLabel", browser)


class PaletteGradientAutoSaveContractTests(unittest.TestCase):
    def test_drag_repaints_local_controls_without_erasing_the_palette_page(self) -> None:
        page = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        edit = page[page.index("static void SetPaletteSettingsColor"):page.index("static void UpdatePaletteSettingsFromRgb")]
        self.assertNotIn("InvalidateRect(hWnd", edit)
        self.assertIn("InvalidatePaletteSlot(hWnd, idx)", edit)
        self.assertIn("InvalidatePaletteSlot(hWnd, previousChangedSlot)", edit)
        self.assertIn("InvalidateRect(ctx->selectedPreview, nullptr, FALSE)", page)
        self.assertIn("if (current == text) return", page)
        self.assertIn("SetPaletteControlText(edit, std::to_wstring(value))", page)
        control = (REPO_ROOT / "src/ui/color_gradient.h").read_text(encoding="utf-8")
        self.assertIn("state.color->h != previous.h", control)
        self.assertIn("PaintBuffered(window, *state, dc, rc)", control)
        self.assertLess(control.index("Paint(window, state, surface.dc, rc)"), control.index("BitBlt(dc"))
        self.assertIn("SelectObject(dc, oldBitmap)", control)
        self.assertIn("DeleteObject(bitmap)", control)
        self.assertIn("DeleteDC(dc)", control)
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        start = shell.index("shell->childPalette = CreateWindowExW")
        create = shell[start:shell.index("return shell->childPalette", start)]
        self.assertIn("WS_CLIPCHILDREN", create)

    def test_removed_palette_footer_notice_is_not_replaced_or_catalogued(self) -> None:
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        self.assertNotIn("settings.palette.auto.footer", shell)
        self.assertIn('L"settings.navigation.palette_autosave"', shell)
        self.assertIn("SetWindowTextW(ctx->labelApplyHint, hint.c_str())", shell)
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertNotIn("settings.palette.auto.footer", catalog)

    def test_gradient_is_inline_bounded_and_keyboard_accessible(self) -> None:
        control = (REPO_ROOT / "src/ui/color_gradient.h").read_text(encoding="utf-8")
        page = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        self.assertIn("std::min(256", control)
        self.assertIn("std::min(128", control)
        self.assertIn("DLGC_WANTARROWS", control)
        self.assertIn("case WM_CAPTURECHANGED: Cancel", control)
        self.assertIn("VK_ESCAPE", control)
        self.assertIn("gradientInitialColor", page)
        self.assertIn("ui::color_gradient::Create", page)
        self.assertNotIn("SaveDocument", control)
        self.assertIn("source != ctx->hue && source != ctx->sv", page)

    def test_auto_save_validates_preserves_snapshot_and_excludes_picker(self) -> None:
        page = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        start = page.index("static bool FlushPaletteAutoSave")
        commit = page[start:page.index("static void SchedulePaletteAutoSave", start)]
        self.assertLess(commit.index("if (ctx->colorPickerPurpose) return true"), commit.index("palette_active::Save"))
        self.assertLess(commit.index("ReadPaletteEditorColor"), commit.index("palette_active::Save"))
        self.assertLess(commit.index("palette_active::Save"), commit.index("ctx->runtimePending = true"))
        self.assertIn("SetTimer(hWnd, kPaletteAutoSaveTimer, 500", page)
        self.assertIn("KillTimer(hWnd, kPaletteAutoSaveTimer)", page)
        self.assertIn("options.defaultResult = SilentDialogResult::No", page)
        self.assertIn("options.escapeResult = SilentDialogResult::No", page)
        wrapper = (REPO_ROOT / "src/settings/palette_active_store.h").read_text(encoding="utf-8")
        self.assertIn("LoadDocument", wrapper)
        self.assertIn("SaveDocument", wrapper)
        self.assertIn('L"user_palette.json"', wrapper)

    def test_palette_footer_is_close_only_and_other_tabs_retain_save(self) -> None:
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        self.assertIn("ctx->currentCategory == kSettingsNavPalette && !ctx->dirty", shell)
        for control in ("btnApply", "btnRevert", "btnOk"):
            self.assertIn(f"ShowWindow(ctx->{control}, selecting || paletteOnly ? SW_HIDE : SW_SHOW)", shell)
        self.assertIn('button(IDCANCEL, selecting ? L"settings.common.603bc62f3f34" : L"settings.navigation.close"', shell)
        self.assertIn("IDC_PALETTE_USE_COLOR", (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8"))
        self.assertNotIn("SaveUserPaletteColorsForSettings", shell)
        self.assertIn("ApplyGeneralSettings(ctx->generalPages[page].get())", shell)
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            for suffix in ("saved", "pending", "incomplete", "failed", "runtime_failed", "unavailable", "discard_confirm"):
                self.assertTrue(catalog[f"settings.palette.auto.{suffix}"])

    def test_native_gradient_and_auto_save_failures_are_defined(self) -> None:
        source = (REPO_ROOT / "tests/integration/workspace_memo_app_tests.cpp").read_text(encoding="utf-8")
        for label in ("HSV RGB round trip within one channel step", "gradient Escape restores exact color without saving",
            "gradient release auto-saves the chosen color", "gradient arrows select and save colors",
            "incomplete numeric input never auto-saves", "palette footer contains the current exit action only",
            "stale active palette cannot overwrite newer colors", "corrupt active palette cannot be auto-overwritten",
            "active palette write failure preserves file and snapshot", "active auto-save retains previous palette bytes",
            "stale UI auto-save preserves competing bytes and edited color",
            "palette file success and related config failure are reported separately",
            "runtime retry does not rewrite the already saved palette",
            "default Close confirmation retains unsaved edit", "explicit discard closes without overwriting corrupt original"):
            self.assertIn(label, source)


class PaletteStockContractTests(unittest.TestCase):
    def test_all_eight_slots_are_editable_and_not_described_as_fixed(self) -> None:
        source = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        self.assertIn("ctx->colors[idx] = color", source)
        self.assertIn("IsPaletteSlotId(id) && code == BN_CLICKED", source)
        self.assertIn("IDC_PALETTE_SLOT_COUNT = 8", source)
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertNotIn("固定", catalog["settings.palette.be911f831213"])
            self.assertNotIn("Fixed", catalog["settings.palette.be911f831213"])

    def test_stock_is_separate_bounded_and_transactional(self) -> None:
        model = (REPO_ROOT / "src/settings/palette_stock.h").read_text(encoding="utf-8")
        store = (REPO_ROOT / "src/settings/palette_stock_store.h").read_text(encoding="utf-8")
        self.assertIn("kMaxColors = 128", model)
        self.assertIn("kMaxBytes = 64 * 1024", model)
        self.assertIn('key != "colors"', model)
        self.assertIn("out = std::move(colors)", model)
        self.assertIn('L"color_stock.json"', store)
        self.assertNotIn("g_workspaceRoot", store)
        self.assertIn("current.bytes != state.bytes", store)
        self.assertIn("WaitForSingleObject(mutex_, 0)", store)
        self.assertIn("ReadFileBytesNoFollowLimitedWin32", store)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", store)
        self.assertIn('L"__escape__"', store)
        self.assertLess(store.index("state.bytes, temp, escape, &error"),
                        store.index("FilePath(state.root, fileName), bytes, temp, escape, &error"))
        self.assertGreater(store.index("state = std::move(next)"),
                           store.index("FilePath(state.root, fileName), bytes, temp, escape, &error"))

    def test_palette_offers_explicit_stock_operations_and_scroll_access(self) -> None:
        source = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        for name in ("IDC_PALETTE_STOCK_ADD", "IDC_PALETTE_STOCK_USE", "IDC_PALETTE_STOCK_REMOVE", "IDC_PALETTE_STOCK_RELOAD"):
            self.assertIn(name, source)
        self.assertIn("ValidatePaletteSettingsEditor(hWnd, ctx)", source)
        self.assertIn("palette_stock::Save(ctx->stock, colors)", source)
        self.assertIn("ctx->stockRoot != std::filesystem::path(g_workspaceRoot)", source)
        self.assertIn("LBS_OWNERDRAWFIXED", source)
        self.assertIn("LBN_DBLCLK", source)
        self.assertIn("ScrollPaletteSettingsPage", source)
        self.assertIn("case WM_VSCROLL", source)

    def test_stock_is_in_assets_and_both_preset_directions(self) -> None:
        assets = (REPO_ROOT / "src/settings/settings_assets.cppinc").read_text(encoding="utf-8")
        presets = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")
        self.assertIn('L"settings.assets.color_stock"', assets)
        self.assertIn('palette_stock::FilePath(root)', assets)
        self.assertIn('{L"color_stock.json", settings / L"color_stock.json"}', presets)
        self.assertIn('if (name == L"color_stock.json") return settings / L"color_stock.json"', presets)
        self.assertIn("palette_stock::Parse(bytes, colors)", presets)
        self.assertIn("palette_stock::Parse(kv.second, colors)", presets)

    def test_stock_runtime_regressions_are_defined(self) -> None:
        integration = (REPO_ROOT / "tests/integration/workspace_memo_app_tests.cpp").read_text(encoding="utf-8")
        automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        self.assertIn('{L"color_stock.json", settings / L"color_stock.json"}', automation)
        self.assertIn("settings_bundle_invalid_stock.pnssettings", automation)
        self.assertIn("malformed, duplicate, unknown, oversized, invalidStock", automation)
        for expected in ("stock RGB/HEX round trip", "stock accepts all 128 colors", "stale stock cannot overwrite newer colors",
                         "failed stock save retains file and snapshot", "corrupt stock is never overwritten with an empty default",
                         "color stock survives Exit and is shared by palette entry points", "stock removal leaves palette slots unchanged",
                         "explicit stock reload recovers without recreating the dialog"):
            self.assertIn(expected, integration)
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertTrue(catalog["settings.palette.stock.hint"])
            self.assertTrue(catalog["settings.palette.stock.save_failed"])


class PalettePresetContractTests(unittest.TestCase):
    def test_format_is_one_immutable_eight_color_set_and_five_user_slots(self) -> None:
        model = (REPO_ROOT / "src/settings/palette_presets.h").read_text(encoding="utf-8")
        defaults = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        self.assertIn("kSlotCount = 8", model)
        self.assertIn("kUserCount = 5", model)
        self.assertIn("std::array<std::optional<Colors>, kUserCount> users", model)
        self.assertIn("colors != kInitial", model)
        self.assertIn("appendColors(kInitial)", model)
        self.assertIn("out = std::move(next)", model)
        self.assertIn("kMaxBytes = 64 * 1024", model)
        # The compiled initial set must agree with the existing application's first eight defaults.
        default_block = defaults[defaults.index("constexpr COLORREF defaults[]"):
                                 defaults.index("// Build the runtime palette")]
        default_colors = re.findall(r"RGB\(\s*(\d+),\s*(\d+),\s*(\d+)\)", default_block)[:8]
        initial_block = model[model.index("inline constexpr Colors kInitial"):model.index("struct Document")]
        self.assertEqual(default_colors, re.findall(r"RGB\(\s*(\d+),\s*(\d+),\s*(\d+)\)", initial_block))

    def test_preset_storage_reuses_safe_local_bounded_atomic_store(self) -> None:
        wrapper = (REPO_ROOT / "src/settings/palette_presets_store.h").read_text(encoding="utf-8")
        store = (REPO_ROOT / "src/settings/palette_stock_store.h").read_text(encoding="utf-8")
        self.assertIn('L"palette_presets.json"', wrapper)
        self.assertIn("LoadDocument", wrapper)
        self.assertIn("SaveDocument", wrapper)
        self.assertNotIn("g_workspaceRoot", wrapper)
        self.assertIn("current.bytes != state.bytes", store)
        self.assertIn("WaitForSingleObject(mutex_, 0)", store)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", store)
        self.assertIn("ReadFileBytesNoFollowLimitedWin32", store)
        self.assertIn("std::wstring_view(fileName)", store)
        self.assertLess(store.index("state.bytes, temp, escape, &error"),
                        store.index("FilePath(state.root, fileName), bytes, temp, escape, &error"))

    def test_selection_previews_and_apply_commits_except_in_single_color_picker(self) -> None:
        source = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        command = source[source.index("static void HandlePaletteSetCommand"):
                         source.index("static void ScrollPaletteSettingsPage")]
        self.assertIn("CBN_SELCHANGE", source)
        self.assertIn("Selection is preview only", source)
        self.assertIn("SelectedPaletteSet(ctx)", command)
        self.assertIn("std::copy(colors->begin(), colors->end(), ctx->colors)", command)
        self.assertNotIn("SaveUserPaletteColorsForSettings", command)
        self.assertIn("FlushPaletteAutoSave(hWnd, ctx)", command)
        self.assertIn("if (ctx->colorPickerPurpose) return true", source)
        self.assertNotIn("palette_stock::Save", command)
        self.assertIn("IDC_PALETTE_SETS_PREVIEW", source)
        self.assertIn("ValidatePaletteSettingsEditor(hWnd, ctx)", command)

    def test_user_mutations_are_guarded_and_confirm_destructive_changes(self) -> None:
        source = (REPO_ROOT / "src/settings/settings_palette.cppinc").read_text(encoding="utf-8")
        self.assertIn("selected < 1 || selected > static_cast<int>(palette_presets::kUserCount)", source)
        self.assertIn("palette_presets::Save(ctx->sets, doc)", source)
        self.assertIn("ctx->stockRoot != std::filesystem::path(g_workspaceRoot)", source)
        for key in ("apply_confirm", "overwrite_confirm", "clear_confirm"):
            self.assertIn(f'L"settings.palette.sets.{key}"', source)
        self.assertIn("options.defaultResult = SilentDialogResult::No", source)
        self.assertIn("options.escapeResult = SilentDialogResult::No", source)
        self.assertIn("ctx->sets.ready = false", source)
        self.assertIn("selected == 0) return &palette_presets::kInitial", source)

    def test_assets_and_workspace_preset_transfer_validate_palette_collections(self) -> None:
        source = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")
        assets = (REPO_ROOT / "src/settings/settings_assets.cppinc").read_text(encoding="utf-8")
        self.assertIn('{L"palette_presets.json", settings / L"palette_presets.json"}', source)
        self.assertIn('if (name == L"palette_presets.json")', source)
        self.assertIn("palette_presets::Parse(bytes, doc)", source)
        self.assertIn("palette_presets::Parse(kv.second, doc)", source)
        self.assertIn('name == L"palette_presets.json" && importedEntry == entries.end()) continue', source)
        self.assertEqual(source.count("palette_stock::WriteLock paletteLock"), 2)
        self.assertIn("palette_presets::FilePath(root)", assets)
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            for suffix in ("hint", "hint_picker", "initial", "save_failed", "load_failed"):
                self.assertTrue(catalog[f"settings.palette.sets.{suffix}"])

    def test_native_success_failure_and_cancellation_regressions_are_defined(self) -> None:
        source = (REPO_ROOT / "tests/integration/workspace_memo_app_tests.cpp").read_text(encoding="utf-8")
        for label in ("six palette sets round trip with repeated colors",
                      "invalid palette collection preserves parsed users and immutable initial set",
                      "stale palette collection cannot overwrite newer presets",
                      "failed palette preset save retains file and snapshot",
                      "initial palette set cannot be overwritten through a direct command",
                      "cancelling preset overwrite preserves saved sets and editor colors",
                      "palette preset and active application survive Close",
                      "initial palette set remains usable when the user file is invalid",
                      "explicit preset reload restores fifth user slot access"):
            self.assertIn(label, source)
        automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        self.assertIn('{L"palette_presets.json", settings / L"palette_presets.json"}', automation)
        self.assertIn("settings_bundle_invalid_palette_sets.pnssettings", automation)
        self.assertIn("settings_bundle_legacy_without_palette_sets.pnssettings", automation)
        self.assertIn("invalidStock, invalidPaletteSets", automation)

    def test_post_replacement_rollback_observes_different_incoming_bytes(self) -> None:
        automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        io_source = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")
        self.assertIn('name == L"palette_presets.json") fault_injection::MaybeThrow(L"settings_import_after_palette_presets_write")', io_source)
        self.assertIn("mutatedSetsJson == paletteSetsJson", automation)
        self.assertIn("else if (bytes == probe->expected) probe->sawReplacement = true", automation)
        self.assertIn("afterPalette && !probe.sawReplacement", automation)
        self.assertIn("err.find(point) == std::wstring::npos", automation)
        self.assertIn("!UiAutomationSettingsBytesEqual(beforeFault, afterFault)", automation)
        self.assertIn("write_checks::normalSink.store(previous_)", automation)
        self.assertIn("if (!observation.armed())", automation)
        self.assertIn("backupBytes == mutatedSetsJson", automation)
        self.assertIn("recoveryBackup != previousFaultBackup", automation)
        self.assertIn("if (std::filesystem::create_directory(backupDir, ec)) { created = true; break; }", io_source)
        self.assertIn("if (!created || backupDir.empty()", io_source)

    def test_cross_process_and_backup_denial_tests_are_owned_and_bounded(self) -> None:
        helper = (REPO_ROOT / "tests/integration/palette_presets_test_support.h").read_text(encoding="utf-8")
        native = (REPO_ROOT / "tests/integration/workspace_memo_app_tests.cpp").read_text(encoding="utf-8")
        self.assertIn("CREATE_NO_WINDOW", helper)
        self.assertIn("startup.wShowWindow = SW_HIDE", helper)
        self.assertIn("WaitForSingleObject(release, 10000)", helper)
        self.assertIn("TerminateProcess(process_.hProcess, 79)", helper)
        self.assertIn("CloseHandle(process_.hProcess)", helper)
        self.assertIn("DACL_SECURITY_INFORMATION, security_.data()", helper)
        self.assertIn("scope restricted to out/tests", native)
        self.assertIn("native write probe confirms backup denial is active", native)
        self.assertIn("backup creation failure preserves preset file and all five user slots", native)
        self.assertIn("cross-process lock refuses both palette writers without changing files or snapshots", native)
        self.assertIn("preset saving resumes after the competing process releases its lock", native)

    def test_complete_ui_round_trip_and_real_restart_are_defined(self) -> None:
        native = (REPO_ROOT / "tests/integration/workspace_memo_app_tests.cpp").read_text(encoding="utf-8")
        for label in ("UI saves each user set without altering other sets or rewriting the saved active palette",
                      "confirmed UI overwrite replaces all eight colors in only the chosen user set",
                      "UI applies every color from all five user sets",
                      "automatic save persists all eight colors of the applied preset",
                      "applying a preset preserves every hidden legacy palette slot",
                      "restart isolated app with its original valid workspace setup",
                      "applied preset colors survive actual process restart in all eight slots",
                      "all five user presets and committed palette survive restart unchanged",
                      "keyboard preset selection changes preview only",
                      "palette preset and stock controls do not overlap"):
            self.assertIn(label, native)

    def test_import_preflight_distinguishes_missing_from_unreadable(self) -> None:
        source = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")
        capture = source[source.index("[[nodiscard]] static bool CaptureSettingsFileSnapshot"):
                         source.index("static std::filesystem::path CreateSettingsImportBackupDir")]
        self.assertIn("TryGetPathAttributesWin32(path, &attrs)", capture)
        self.assertIn("error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND", capture)
        self.assertIn("!ReadFileBytesNoFollowLimitedWin32(path, kMaxSnapshotFileBytes, bytes)", capture)
        self.assertIn("kMaxSnapshotFileBytes = 512 * 1024", capture)
        self.assertIn("could not capture existing settings file", capture)
        self.assertIn("if (!capture(entry.name, entry.path)) return false", capture)
        self.assertIn("if (!capture(kv.first, path)) return false", capture)
        self.assertIn("*out = std::move(snapshot)", capture)
        caller = source[source.index("bool ImportSettingsPresetFromFile("):]
        self.assertLess(caller.index("if (!CaptureSettingsFileSnapshot(entries, &snapshot, outErr)) return false"),
                        caller.index("CreateSettingsImportBackupDir(snapshot, outErr)"))

    def test_snapshot_observation_fails_closed_and_read_denial_is_real(self) -> None:
        source = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        capture = source[source.index("[[nodiscard]] static bool CaptureUiAutomationSettingsBytes"):
                         source.index("static bool UiAutomationSettingsBytesEqual")]
        self.assertIn("error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND", capture)
        self.assertIn("!ReadFileBytesNoFollowLimitedWin32(item.second, kMaxSnapshotFileBytes, bytes)) return false", capture)
        self.assertIn("*out = std::move(next)", capture)
        self.assertNotIn("out->clear()", capture)
        self.assertNotRegex(source, r"(?m)^\s*CaptureUiAutomationSettingsBytes\(")
        for text in ("GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_DELETE", "probeError != ERROR_SHARING_VIOLATION",
                     "failedCaptureOutput = beforeDenied", "!deniedRecovery.empty()",
                     "UiAutomationSettingsBytesEqual(beforeDenied, failedCaptureOutput)",
                     "Unreadable settings were treated as missing or changed during import"):
            self.assertIn(text, source)

    def test_manual_acceptance_is_not_claimed_by_static_or_simulated_dpi(self) -> None:
        procedure = (REPO_ROOT / "docs/internal/test_配色プリセット検査手順_2026-10-10.md").read_text(encoding="utf-8")
        for text in ("ビルド禁止中", "コンパイル・実行とも未実施", "JA・EN", "100%、125%、150%、200%",
                     "WM_DPICHANGED", "Windows警告音", "ディスクフル", "実際の電源断"):
            self.assertIn(text, procedure)


class AnnotationToolPolicyTests(unittest.TestCase):
    def test_cached_note_syntax_reuse_checks_canonical_content_and_owner(self) -> None:
        ops = (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(encoding="utf-8")
        load = ops[ops.index("bool LoadNoteFile("):ops.index("void ClearNoteEditorSilently(")]
        self.assertIn("GetEditTextForIndexing(targetEdit) == indexedText", load)
        self.assertIn("cachedKernel->text_core().note_id() == loadedSnapshot.note_id", load)
        self.assertIn("cachedKernel->content_kind() == CurrentNoteContentKind()", load)
        self.assertIn("cachedKernel->text_core().MatchesRaw(indexedText)", load)
        self.assertIn("cachedKernel->text_core().content_revision() == loadedSnapshot.content_revision", load)
        self.assertIn("cachedKernel->text_core().SetPersistenceRevision(loadedSnapshot.persistence_revision)", load)
        self.assertIn("if (keepCachedSyntax) ClearLegacyRenderCache();", load)
        self.assertIn("else ClearLineCache();", load)
        self.assertIn("TextSnapshotReadPolicy::DiskAndStage", load)
        self.assertIn("std::move(storageSnapshot.diskBytes)", load)

    def test_initial_pdf_cache_work_is_after_final_fit_and_restore(self) -> None:
        zoom = (REPO_ROOT / "src/pdf_view/zoom_flow.cppinc").read_text(encoding="utf-8")
        cache = (REPO_ROOT / "src/pdf_view/layout_cache.cppinc").read_text(encoding="utf-8")
        probe = (REPO_ROOT / "src/pdf_view/initial_viewport_work_tests.cppinc").read_text(encoding="utf-8")
        complete = zoom[zoom.index("static void CompleteInitialPdfFit"):zoom.index("static bool TryApplyInitialFitWidth")]
        self.assertIn("PdfInitialViewportUpdateGuard viewportGuard", complete)
        self.assertLess(complete.index("ApplyPendingPdfRestore"), complete.index("UpdatePdfVirtualScrollCaches"))
        self.assertEqual(complete.count("UpdatePdfVirtualScrollCaches"), 1)
        update = cache[cache.index("static PdfVirtualUpdateResult UpdatePdfVirtualScrollCaches"):]
        self.assertLess(update.index("g_pdfInitialViewportUpdateDepth != 0"), update.index("ComputePdfVirtualViewport"))
        self.assertIn("updates != 1 || premature != 0", probe)
        self.assertIn("cases == 44", probe)
        self.assertIn("reloaded.pageIndex != targetPage", probe)

    def test_pending_pdf_queue_refreshes_without_preempting_active_render(self) -> None:
        cache = (REPO_ROOT / "src/pdf_view/layout_cache.cppinc").read_text(encoding="utf-8")
        update = cache[cache.index("static PdfVirtualUpdateResult UpdatePdfVirtualScrollCaches"):]
        self.assertLess(update.index("ComputePdfVirtualViewport"), update.index("RefreshPdfVirtualJobPriorities"))
        self.assertIn("std::lock_guard<std::mutex> lock(g_pdfVirtualWorkerMutex);", update)
        worker = cache[cache.index("static void PdfVirtualRenderWorkerMain"):cache.index("static bool EnsurePdfVirtualRenderWorker")]
        self.assertIn("pdf_render_queue::ChooseNext(g_pdfVirtualJobs, g_pdfVirtualVisibleBurst)", worker)
        self.assertIn("pdf_render_queue::AfterDispatch", worker)
        refresh = cache[cache.index("static void RefreshPdfVirtualJobPriorities"):cache.index("static PdfVirtualUpdateResult UpdatePdfVirtualScrollCaches")]
        self.assertNotIn("g_pdfVirtualWorkerActiveJob", refresh)
        self.assertIn("job.generation != generation", refresh)
        identity = cache[cache.index("static bool IsSamePdfVirtualJob"):cache.index("static void PdfVirtualRenderWorkerMain")]
        self.assertNotIn(".visible", identity)
        self.assertNotIn(".distance", identity)

    def test_pdf_worker_probe_observes_real_render_and_owns_bounded_gates(self) -> None:
        cache = (REPO_ROOT / "src/pdf_view/layout_cache.cppinc").read_text(encoding="utf-8")
        probe = (REPO_ROOT / "src/pdf_view/worker_concurrency_tests.cppinc").read_text(encoding="utf-8")
        worker = cache[cache.index("static void PdfVirtualRenderWorkerMain"):cache.index("static bool EnsurePdfVirtualRenderWorker")]
        self.assertLess(worker.index("ObservePdfWorkerProbeLocked(lock, job);"), worker.index("RenderPdfPageBitmapOnWorker"))
        self.assertLess(worker.index("RenderPdfPageBitmapOnWorker"), worker.index("ObservePdfWorkerProbeLocked(lock, job, &result)"))
        self.assertIn("std::chrono::seconds(5)", cache)
        self.assertIn("if (!probe.enabled) return;", cache)
        self.assertIn("g_pdfVirtualWorkerStop || !probe.enabled", cache)
        for required in ("probe.nonWhite != expected.size()", "staleRejected != 1", "stoppedResults != 1",
                         "g_pdfVirtualWorkerProbe.enabled = false", "g_pdfVirtualWorkerCv.notify_all()",
                         "ShutdownPdfVirtualRenderWorker()", "LoadPdfDocument(pdfWnd, alternatePdf)"):
            self.assertIn(required, probe)
        runner = (REPO_ROOT / "tests/scripts/run_ui_automation_fault_tests.ps1").read_text(encoding="utf-8")
        self.assertIn("[string]$ArtifactName", runner)
        self.assertIn("[string]$ExecutableName", runner)
        self.assertIn("Refusing to remove an automation workspace", runner)
        # Focused-mode membership has one authority: mode additions must not
        # silently change full-suite, fixture, or completion-marker behavior.
        self.assertIn("$focusedModeSwitches = @(\n", runner)
        self.assertIn("$fullSuite = $selectedFocusedModes.Count -eq 0", runner)
        self.assertIn("$requiresSessionFixture = -not", runner)

    def test_auto_open_dispatch_is_before_note_and_deferred_for_unstable_geometry(self) -> None:
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        start = main.index("static void StartAutoOpenedPdfBeforeNote() {")
        end = main.index("static bool FocusAutoOpenedSoleNoteForEditing() {", start)
        route = main[start:end]
        self.assertIn("g_bottomPanePin != BottomPanePin::Math", route)
        self.assertLess(route.index("autoOpenNotePath ="), route.index("OpenPdfWithAnnotations"))
        self.assertIn("const bool noteFirst = g_bottomPanePin == BottomPanePin::Math", route)
        self.assertLess(route.index("if (noteFirst) openNote();"), route.index("OpenPdfWithAnnotations"))
        self.assertLess(route.index("StartAutoOpenedPdfBeforeNote();"), route.index("if (!noteFirst) openNote();"))
        self.assertIn("RestorePdfListSelectionToCurrent();", route)
        self.assertIn("SessionAutoOpenMode::Off", route)
        self.assertIn("SyncBottomPaneAfterNoteLoad(hWnd)", route)
        self.assertIn("FocusAutoOpenedSoleNoteForEditing()", route)
        self.assertNotIn("PeekMessage", route)
        zoom = (REPO_ROOT / "src/pdf_view/zoom_flow.cppinc").read_text(encoding="utf-8")
        public_start = zoom[zoom.index("bool StartPendingPdfInitialRender"):zoom.index("void SetPdfFlowMode")]
        self.assertIn("IsWindowVisible(pdfWnd)", public_start)
        self.assertIn("g_pdfInitialViewportUpdateDepth != 0", public_start)

    def test_note_pdf_selection_have_one_recovery_boundary(self) -> None:
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        for name, next_name, opening in (
            ("OnNoteSelChange", "OnPdfSelChange", "LoadNoteFile(hWnd, f.path)"),
            ("OnPdfSelChange", "OnPdfDblClk", "OpenPdfWithAnnotations(hWnd, f.path)"),
        ):
            start = main.index(f"static void {name}(HWND hWnd) {{")
            end = main.index(f"static void {next_name}(HWND hWnd)", start + 1)
            route = main[start:end]
            self.assertEqual(route.count("file_output::PrepareStagedDiffsForSwitch(hWnd)"), 1)
            self.assertNotIn("if (!SaveNoteIfDirty(hWnd))", route)
            self.assertLess(route.index("file_output::PrepareStagedDiffsForSwitch(hWnd)"),
                            route.index("VerifyWorkspaceWritableForEditing(hWnd)"))
            self.assertLess(route.index("VerifyWorkspaceWritableForEditing(hWnd)"), route.index(opening))

    def test_open_work_probe_observes_production_routes_without_text_reads(self) -> None:
        probe = (REPO_ROOT / "src/features/automation/main_ui_open_work_tests.cppinc").read_text(encoding="utf-8")
        counter = (REPO_ROOT / "src/diagnostics/open_work_counters.h").read_text(encoding="utf-8")
        runner = (REPO_ROOT / "tests/scripts/run_ui_automation_fault_tests.ps1").read_text(encoding="utf-8")
        measured = probe[probe.index("open_work::Observation observation;"):probe.index("counts = observation.Read();")]
        self.assertIn("SetListboxSelAndNotify(list, index)", measured)
        self.assertNotIn("GetNoteAutomationIndexingText", measured)
        self.assertNotIn("ReadUiAutomationFileBytes", measured)
        self.assertIn("checks != expectedChecks || reads != expectedChecks", probe)
        self.assertIn("SendUiAutomationNoteUndoChangedText", probe)
        self.assertIn("SendUiAutomationNoteRedoChangedText", probe)
        self.assertIn("finalBytes != originalBytes", probe)
        self.assertIn("thread_local", counter)
        self.assertIn("observationDepth != 0", counter)
        self.assertIn("open_work:content_stage_undo_redo_ok", runner)
        self.assertIn("open_work:paired_auto_open_ok", runner)
        self.assertIn("open_work:paired_pdf_failure_ok", runner)
        for marker in ("paired_note_failure_ok", "paired_long_complex_ok", "pdf_worker_concurrency_ok", "startup_restore_ok"):
            self.assertIn("open_work:" + marker, runner)
        self.assertIn("automation:open_work_ok", runner)
        self.assertIn('"PDF_NOTE_SMALL_UI_AUTOMATION_OPEN_WORK_ONLY" =', runner)

    def test_annotation_panel_settings_open_the_active_tool_section(self) -> None:
        unified = (REPO_ROOT / "src/settings/settings_unified.cppinc").read_text(encoding="utf-8")
        annot = (REPO_ROOT / "src/settings/settings_annot.cppinc").read_text(encoding="utf-8")
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        self.assertIn("ShowAnnotationSettingsForTool", unified)
        self.assertIn("AnnotSettingsSectionForTool", unified)
        self.assertIn("case ToolMode::TextBox", unified)
        self.assertIn("return kAnnotSectionStroke", unified)
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        self.assertIn("shell->annotCtx->initialSection = shell->initialAnnotSection", shell)
        self.assertIn("JumpAnnotSettingsToSection(hWnd, ctx, initialSection)", annot)
        self.assertIn("ShowAnnotationSettingsForTool(hWnd, g_toolMode)", dispatch)

    def test_palette_color_creation_is_centralized_in_settings(self) -> None:
        palette = (REPO_ROOT / "src" / "settings" / "settings_palette.cppinc").read_text(encoding="utf-8")
        dispatch = (REPO_ROOT / "src" / "app" / "command_dispatch.cppinc").read_text(encoding="utf-8")
        core = (REPO_ROOT / "src" / "core" / "app_core.cpp").read_text(encoding="utf-8")
        unified = (REPO_ROOT / "src/settings/settings_unified.cppinc").read_text(encoding="utf-8")
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        self.assertNotIn("ChooseColorW", palette)
        self.assertIn("IDC_PALETTE_EDITOR_HEX", palette)
        self.assertIn("UpdatePaletteSettingsFromHex", palette)
        self.assertNotIn("PaletteColorEditorDlgClass", palette)
        self.assertNotIn("ShowPaletteColorEditorDialogImpl", palette)
        self.assertNotIn("IDC_PALETTE_EDITOR_SV", palette)
        self.assertIn("SamplePaletteColorAtCursor", palette)
        self.assertIn("IDC_PALETTE_EDITOR_EYEDROPPER", palette)
        self.assertIn("ShowAnnotationPaletteSettingsDialog(hWnd)", dispatch)
        self.assertIn("ShowUnifiedSettingsDialog(owner, kSettingsNavPalette, -1, &request)", unified)
        self.assertIn("palette->selectedSlot = kLastOkColorSlotIndex", shell)
        self.assertIn("if (request.accepted) *outColor = request.selectedColor", unified)
        self.assertIn("GetAncestor(owner, GA_ROOT)", unified)
        self.assertIn("if (!ctx || IsUnifiedColorSelection(ctx)) return false", shell)
        self.assertIn("if (ctx && !ctx->embedded) RestoreOwnerAfterModal(ctx->owner)", palette)
        self.assertNotIn("g_paletteDialogCustomColor", core)
        self.assertIn("Legacy Windows picker slots remain in JSON only", core)

    def test_color_selection_regressions_cover_shared_page_and_cancellation(self) -> None:
        automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        integration = (REPO_ROOT / "tests/integration/workspace_memo_app_tests.cpp").read_text(encoding="utf-8")
        self.assertNotIn('L"PaletteColorEditorDlgClass"', automation)
        self.assertIn('L"settings.palette.use_color"', automation)
        self.assertIn("g_palette != originalPalette", automation)
        self.assertIn("open annotation palette through full settings", integration)
        self.assertIn("annotation palette Close preserves immediately saved colors", integration)
        self.assertIn("saved annotation palette survives Exit", integration)

    def test_annotation_automation_covers_freehand_correction_and_history(self) -> None:
        automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        pdf_view = (REPO_ROOT / "src/pdf_view/pdf_view.cpp").read_text(encoding="utf-8")
        self.assertIn("RunUiAutomationFreehandAndAnnotationHistoryScenario", automation)
        self.assertIn("TryCorrectFreehandAnnotation(raw, &corrected)", automation)
        self.assertIn("ExecutePdfUndoRedoFromFocus(owner, true)", automation)
        self.assertIn("ExecutePdfUndoRedoFromFocus(owner, false)", automation)
        self.assertIn("DuplicateAnnotationAtIndex(owner, 0)", automation)
        self.assertIn("automation:freehand_annotation_history_ok", automation)
        self.assertIn("bool TryCorrectFreehandAnnotation", pdf_view)

    def test_shortcut_schema_accepts_category_and_detail_targets(self) -> None:
        _validate_annotation_shortcuts([
            {"key": "Ctrl+Alt+5", "category": "marker"},
            {"key": "Ctrl+Alt+U", "tool": "marker_text_underline"},
        ])

    def test_shortcut_schema_rejects_duplicate_or_ambiguous_targets(self) -> None:
        with self.assertRaises(ValueError):
            _validate_annotation_shortcuts([
                {"key": "Ctrl+Alt+5", "category": "marker"},
                {"key": "Alt+Ctrl+5", "tool": "marker_text"},
            ])
        with self.assertRaises(ValueError):
            _validate_annotation_shortcuts([
                {"key": "Ctrl+Alt+5", "tool": "marker_text", "category": "marker"},
            ])
        with self.assertRaises(ValueError):
            _validate_annotation_shortcuts([
                {"key": "Ctrl+Alt+Left", "category": "marker"},
            ])
        with self.assertRaises(ValueError):
            _validate_annotation_shortcuts([
                {"key": "Control+Menu+ArrowDown", "category": "marker"},
            ])

    def test_default_shortcuts_cover_categories_and_details(self) -> None:
        source = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        self.assertIn('"Ctrl+Alt+1", AnnotToolShortcutTargetKind::Category', source)
        self.assertIn('"Ctrl+Alt+8", AnnotToolShortcutTargetKind::Category', source)
        self.assertIn('"Ctrl+Alt+9", AnnotToolShortcutTargetKind::Detail', source)
        self.assertIn('"Ctrl+Alt+0", AnnotToolShortcutTargetKind::Detail', source)

    def test_workspace_detail_keys_and_legacy_migration_are_present(self) -> None:
        source = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        for key in (
            "annotLastMarkerDetail", "annotLastPenDetail", "shapeDetail", "annotLastShapePresentation",
            "annotLastShapeGeometry", "annotLastShapeDetail",
        ):
            self.assertIn(f'"{key}"', source)
        for legacy in ("annotLastMarkerMode", "annotLastPenMode", "annotLastShapeMode"):
            self.assertIn(f'"{legacy}"', source)
        self.assertIn("legacyDetailKey", source)

    def test_shape_selection_uses_structured_order_and_compatibility_mapping(self) -> None:
        core = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/core/app_core.h").read_text(encoding="utf-8")
        self.assertIn("enum class ShapeDetail", header)
        self.assertIn("g_shapeDetail", header)
        self.assertIn("ToolModeForShapeDetail", core)
        self.assertIn("ShapeDetailForLegacyState", core)
        self.assertIn("OrderedShapeDetails", dispatch)
        self.assertIn("OrderedShapeDetails", main)
        self.assertIn("IsFixedAnnotToolNavigationShortcut", core)

    def test_toolbar_fallback_resynchronizes_shape_selection(self) -> None:
        toolbar = (REPO_ROOT / "src/ui/menus/main_toolbar_ui.cppinc").read_text(encoding="utf-8")
        self.assertIn("SyncLegacyShapeStateFromDetail();", toolbar)

    def test_toolbar_controls_receive_a_centralized_common_ui_font_fallback(self) -> None:
        """New toolbar controls must receive the common font and be checked at runtime."""
        layout = (REPO_ROOT / "src/ui/core/main_view_layout.cppinc").read_text(encoding="utf-8")
        automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8")
        self.assertIn("static BOOL CALLBACK ApplyToolbarChildUIFont", layout)
        self.assertIn("EnumChildWindows(g_hPdfToolbar, ApplyToolbarChildUIFont, 0);", layout)
        self.assertIn("ApplyToolbarChildUIFonts();", layout)
        toolbar_control_creations = [
            match.start()
            for match in re.finditer(
                r"CreateWindowExW\((?:(?!;).)*?\bg_hPdfToolbar,", layout, re.DOTALL
            )
        ]
        self.assertTrue(toolbar_control_creations, "toolbar controls must be created in the main layout")
        self.assertGreater(
            layout.rindex("ApplyToolbarChildUIFonts();"),
            max(toolbar_control_creations),
            "the common-font fallback must run after every toolbar control is created",
        )
        self.assertIn("RunUiAutomationToolbarChildFontScenario", automation)
        self.assertIn("VerifyToolbarChildUIFontForAutomation", automation)
        self.assertIn("SendMessageW(child, WM_GETFONT, 0, 0)", automation)
        self.assertIn("automation:toolbar_fonts_ok", automation)
        ui_automation_script = (REPO_ROOT / "tests/scripts/run_ui_automation_fault_tests.ps1").read_text(encoding="utf-8")
        self.assertIn("automation:toolbar_fonts_ok", ui_automation_script)

    def test_annotation_color_defaults_are_orange(self) -> None:
        core = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        config = (REPO_ROOT / "src/core/workspace_config.h").read_text(encoding="utf-8")
        for key in (
            "g_textColor", "g_lineColor", "g_arrowColor", "g_waveColor",
            "g_freehandColor", "g_markerFreeColor", "g_markerTextColor", "g_shapeColor",
        ):
            self.assertIn(f"{key} = RGB(255, 140, 0)", core)
        for key in (
            "textColor", "lineColor", "arrowColor", "waveColor", "freehandColor",
            "markerFreeColor", "markerTextColor", "shapeColor",
        ):
            self.assertIn(f"{key} = RGB(255, 140, 0)", config)

    def test_quick_output_settings_round_trip_and_presets_include_them(self) -> None:
        """New quick-output controls must not silently disappear from workspace settings or presets."""
        config = (REPO_ROOT / "src/core/workspace_config.h").read_text(encoding="utf-8")
        core = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        export_dialog = (REPO_ROOT / "src/ui/dialogs/export_dialog.cpp").read_text(encoding="utf-8")
        presets = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")

        fields = {
            "quickPdfScalePercent": "ParseJsonIntField",
            "quickPdfStandardTextAnnots": "ParseJsonBoolField",
            "quickPdfMatchPdfPaneTextLayout": "ParseJsonBoolField",
            "quickPdfKeepPageRotation": "ParseJsonBoolField",
            "quickNoteStripMarkup": "ParseJsonBoolField",
            "quickNoteIncludeComments": "ParseJsonBoolField",
            "quickNoteMathPlaceholder": "ParseJsonBoolField",
            "quickNoteMathPlaceholderText": "ParseJsonStringField",
        }
        for field, parser in fields.items():
            self.assertIn(field, config)
            self.assertIn(f'{parser}(json, "{field}")', core)
            self.assertIn(f'"{field}"', core)
            self.assertIn(f'\\"{field}\\"', core)
            self.assertIn(field, export_dialog)

        self.assertIn("ExportSettingsPresetToFile", presets)
        self.assertIn('"workspace.json", root / L"workspace.json"', presets)
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        self.assertIn("g_config.quickNoteMathPlaceholderText", dispatch)
        self.assertIn("EscapeJsonStringValue", core)

    def test_main_menu_keeps_the_requested_action_routes(self) -> None:
        """The public menu map is an action map, not a collection of disabled placeholders."""
        menu = (REPO_ROOT / "src/ui/menus/menu_build.cpp").read_text(encoding="utf-8")
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        required_routes = {
            "ID_FILE_NEW_CLRO": "menu.file.create_note",
            "ID_FILE_NEW_SESSION": "menu.file.create_session",
            "ID_FILE_NEW_LECTURE": "menu.file.create_lecture",
            "ID_FILE_IMPORT_FILE": "menu.file.import_file",
            "ID_FILE_IMPORT_DIR_AS_SESSION": "menu.file.import_session",
            "ID_FILE_IMPORT_DIR_AS_LECTURE": "menu.file.import_lecture",
            "ID_FILE_OPEN_WORKSPACE_DIR": "menu.file.open_root",
            "ID_FILE_OPEN_LECTURE_DIR": "menu.file.open_lecture",
            "ID_FILE_OPEN_SESSION_DIR": "menu.file.open_session",
            "ID_FILE_ADD_TEMP_EXTERNAL_LECTURE": "menu.file.add_external_folder",
            "ID_FILE_REMOVE_TEMP_EXTERNAL_LECTURE": "menu.common.delete",
            "ID_OP_RENAME_PDF": "menu.common.pdf",
            "ID_OP_RENAME_NOTE": "menu.common.note",
            "ID_OP_MOVE_PDF": "menu.common.pdf",
            "ID_OP_MOVE_NOTE": "menu.common.note",
            "ID_VIEW_CLOSE_PDF": "menu.common.pdf",
            "ID_VIEW_CLOSE_NOTE": "menu.common.note",
            "ID_FILE_CLOSE": "menu.file.close",
            "ID_VIEW_PDF_SINGLE_PAGE_MODE": "menu.scroll.page_display",
            "ID_FILE_SAVE_ALL": "menu.save.work",
            "ID_OP_STAGE_MANAGE": "menu.save.review_diffs",
            "ID_FILE_RESTORE_BACKUP": "menu.common.restore",
            "ID_FILE_DELETE_BACKUP": "menu.common.delete",
            "ID_FILE_EXPORT_PDF_QUICK": "menu.export.quick_pdf",
            "ID_FILE_EXPORT_NOTE_TEXT_QUICK": "menu.export.quick_note",
            "ID_FILE_EXPORT_COMBINED": "menu.export.dialog",
            "ID_OP_OPEN_READONLY_VIEWER_FILE": "menu.viewer.open_file",
            "ID_OP_OPEN_READONLY_VIEWER": "menu.tools.open_in_viewer",
            "ID_SETTINGS_GENERAL": "menu.settings.dialog",
            "ID_HELP_GUIDE": "menu.help.dialog",
            "ID_HELP_NOTE_INFO": "menu.help.note_info",
        }
        for command, label in required_routes.items():
            self.assertIn(command, menu)
            self.assertIn(label, menu)
            self.assertTrue(
                f"case {command}" in dispatch or f"case {command}" in main,
                f"{command} has a menu item but no command dispatch route",
            )

        self.assertNotIn("appendPending", menu)
        self.assertNotIn("menu.pending.", menu)
        navigation = (REPO_ROOT / "src/settings/settings_navigation.h").read_text(encoding="utf-8")
        shell = (REPO_ROOT / "src/settings/settings_shell.cppinc").read_text(encoding="utf-8")
        self.assertIn("BottomPane, Palette, Files", navigation)
        self.assertIn("page == kSettingsNavPalette) target = EnsureUnifiedPalettePage", shell)

        self.assertIn("OpenReadOnlyViewerCurrentOrPickFile", main)
        self.assertIn("LaunchReadOnlyViewerForFile(owner, *selected)", main)
        self.assertIn("LaunchReadOnlyViewerForPdf(owner, *selected)", main)

        menu_commands = set(re.findall(r"\bID_[A-Z0-9_]+\b", menu))
        non_action_commands = {"ID_STATUS_DISPLAY"}
        for command in menu_commands - non_action_commands:
            self.assertTrue(
                f"case {command}" in dispatch or f"case {command}" in main,
                f"{command} is present in the menu but has no command dispatch route",
            )

    def test_exported_markdown_opens_in_the_bundled_readonly_viewer(self) -> None:
        """Markdown results must not depend on a Windows file association."""
        export_dialog = (REPO_ROOT / "src/ui/dialogs/export_dialog.cpp").read_text(encoding="utf-8")
        app_core = (REPO_ROOT / "src/core/app_core.h").read_text(encoding="utf-8")
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")

        self.assertIn('return extension == L".md" || extension == L".markdown";', export_dialog)
        self.assertIn("IsReadOnlyViewerOutputPath", export_dialog)
        self.assertIn("LaunchReadOnlyViewerForFile(owner, path)", export_dialog)
        self.assertIn("bool LaunchReadOnlyViewerForFile(HWND owner, const std::wstring& filePath);", app_core)
        self.assertIn("bool LaunchReadOnlyViewerForFile(HWND owner, const std::wstring& filePath) {", main)

    def test_stage_manager_stages_current_work_before_listing_diffs(self) -> None:
        """The review dialog must include edits made since the last auto-stage checkpoint."""
        stage_manager = (REPO_ROOT / "src/workspace/file_ops_stage_manager.cppinc").read_text(encoding="utf-8")

        show_dialog = stage_manager.index("void ShowStageManagerDialog(HWND owner)")
        create_window = stage_manager.index("HWND w = CreateWindowExW", show_dialog)
        review_setup = stage_manager[show_dialog:create_window]
        self.assertIn("file_output::SaveNoteIfDirty(owner)", review_setup)
        self.assertIn("file_output::SaveAnnotationsIfDirty(owner)", review_setup)
        self.assertIn("never write the original note or .clrop", review_setup)

    def test_settings_presets_are_the_single_settings_transfer_route(self) -> None:
        """Preset save/load must use the full protected bundle; no parallel migration UI remains."""
        menu = (REPO_ROOT / "src/ui/menus/menu_build.cpp").read_text(encoding="utf-8")
        dispatch = (REPO_ROOT / "src/app/command_dispatch.cppinc").read_text(encoding="utf-8")
        config_io = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")
        assets = (REPO_ROOT / "src/settings/settings_assets.cppinc").read_text(encoding="utf-8")
        ids = (REPO_ROOT / "src/core/command_ids.h").read_text(encoding="utf-8")

        self.assertIn('AppendMenuW(settings, MF_STRING, ID_SETTINGS_PRESETS, text(L"menu.settings.presets").c_str());', menu)
        self.assertNotIn("HMENU presets", menu)
        self.assertNotIn("ID_SETTINGS_PRESET_SAVE", menu)
        self.assertNotIn("ID_SETTINGS_PRESET_LOAD", menu)
        self.assertIn("case ID_SETTINGS_PRESETS:\n        ShowSettingsPresetDialog(hWnd);", dispatch)
        self.assertIn("case ID_SETTINGS_PRESET_SAVE", dispatch)
        self.assertIn("case ID_SETTINGS_PRESET_LOAD", dispatch)
        self.assertIn("PickSettingsPresetSavePath", config_io)
        self.assertIn("PickSettingsPresetOpenPath", config_io)
        self.assertIn("ExportSettingsPresetToFile", config_io)
        self.assertIn("ImportSettingsPresetFromFile", config_io)
        self.assertIn("workspace.config_io.0d32b1c7e154", config_io)
        self.assertNotIn("IDC_SETTINGS_ASSETS_PRESET_SAVE", assets)
        self.assertNotIn("IDC_SETTINGS_ASSETS_PRESET_LOAD", assets)

        combined = menu + dispatch + config_io + assets + ids
        for obsolete in (
            "ID_SETTINGS_BUNDLE_",
            "ExportAllUserSettings",
            "ImportAllUserSettings",
            "PickSettingsBundle",
            "IDC_SETTINGS_ASSETS_EXPORT",
            "IDC_SETTINGS_ASSETS_IMPORT",
            "menu.settings.migration",
        ):
            self.assertNotIn(obsolete, combined)

    def test_organize_notice_reports_updated_file_locations(self) -> None:
        layout = (REPO_ROOT / "src/ui/core/main_view_layout.cppinc").read_text(encoding="utf-8")
        ja = json.loads((REPO_ROOT / "locales/ja.json").read_text(encoding="utf-8"))
        en = json.loads((REPO_ROOT / "locales/en.json").read_text(encoding="utf-8"))
        key = "ui.layout.6212da2d3a1c"
        self.assertIn(key, layout)
        self.assertEqual(ja[key], "\nファイル位置を更新しました。")
        self.assertEqual(en[key], "\nUpdated file locations.")
        self.assertIn("ファイル位置を更新しました", ja["ui.layout.3b1d1c31a5eb"])

    def test_annotation_inspector_acknowledges_apply_without_a_sound(self) -> None:
        source = (REPO_ROOT / "src/ui/dialogs/annot_math_panel.cpp").read_text(encoding="utf-8")
        ja = json.loads((REPO_ROOT / "locales/ja.json").read_text(encoding="utf-8"))
        en = json.loads((REPO_ROOT / "locales/en.json").read_text(encoding="utf-8"))

        self.assertIn("kAnnotInspectorApplyFeedbackTimer", source)
        self.assertIn("ShowAnnotInspectorApplied", source)
        self.assertIn("SetTimer(hWnd, kAnnotInspectorApplyFeedbackTimer, 1000", source)
        self.assertIn("case WM_TIMER", source)
        self.assertIn("KillTimer(hWnd, kAnnotInspectorApplyFeedbackTimer)", source)
        self.assertIn('localization::Text(L"ui.annot_math.e96aa12267b4")', source)
        self.assertIn("const std::wstring title = AnnotListLabel", source)
        self.assertIn("SetAnnotInspectorTitle", source)
        self.assertIn("RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME)", source)
        self.assertIn('localization::Text(L"ui.annot_math.63c3859d7f0b")', source)
        self.assertIn("FindOpenAnnotInspector(annotation.id)", source)
        self.assertIn("EnumThreadWindows(GetCurrentThreadId(), FindOpenAnnotInspectorProc", source)
        self.assertNotIn("ui.annot_math.6e745bc1653d", ja)
        self.assertNotIn("ui.annot_math.6e745bc1653d", en)
        self.assertEqual(ja["ui.annot_math.63c3859d7f0b"], "閲覧")
        self.assertEqual(en["ui.annot_math.63c3859d7f0b"], "View")
        self.assertEqual(ja["ui.annot_math.e96aa12267b4"], "適用しました")
        self.assertEqual(en["ui.annot_math.e96aa12267b4"], "Applied")

    def test_japanese_early_design_clrop_matches_its_paired_pdf(self) -> None:
        session = REPO_ROOT / "release_assets/sample_workspace/ja/01_講義サンプル/第03回_最初期構想"
        pdf = session / "PDF学習ワークスペース統合画面構成および基本仕様書.pdf"
        clrop = session / "PDF学習ワークスペース統合画面構成および基本仕様書.clrop"
        data = json.loads(clrop.read_text(encoding="utf-8"))
        self.assertEqual(data["version"], 1)
        self.assertEqual(data["pdf_id"]["path"], pdf.name)
        self.assertEqual(data["pdf_id"]["size"], pdf.stat().st_size)
        self.assertEqual(data["pdf_id"]["sha256"], hashlib.sha256(pdf.read_bytes()).hexdigest())
        text_item = next(item for item in data["pages"][0]["items"] if item["id"] == "sample-text")
        self.assertEqual(text_item["font"], "Meiryo")
        self.assertEqual(text_item["content"], "注釈は .clrop（JSON）で管理します。")
        self.assertEqual(text_item["lines"], [text_item["content"]])
        self.assertEqual(text_item["bbox"], [147.749896103, 605.333315878, 253.25, 33.375])
        red_text = next(item for item in data["pages"][0]["items"] if item["id"] == "amrwvyqqz_ncg_8")
        self.assertEqual(red_text["font"], "Yu Mincho")
        wave = next(item for item in data["pages"][0]["items"] if item["type"] == "wave")
        self.assertEqual(wave["id"], "amrwvvir3_ncg_4")
        self.assertEqual(wave["color"], "#FF8C00")
        self.assertEqual(wave["alpha"], 1)
        self.assertEqual(wave["width"], 2)
        self.assertEqual(wave["p1"], [149.087732612, 579.204292878])
        self.assertEqual(wave["p2"], [394.249014612, 578.441943878])

    def test_magnifier_options_are_persistent_and_dpi_aware(self) -> None:
        config = (REPO_ROOT / "src/core/workspace_config.h").read_text(encoding="utf-8")
        core = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
        overlay = (REPO_ROOT / "src/pdf_view/interaction_overlay.cppinc").read_text(encoding="utf-8")
        settings = (REPO_ROOT / "src/settings/settings_annot.cppinc").read_text(encoding="utf-8")
        catalog = json.loads((REPO_ROOT / "locales/ja.json").read_text(encoding="utf-8"))
        self.assertIn("Horizontal", config + core + overlay + settings)
        self.assertIn("magnifierZoom", config)
        self.assertIn("magnifierSizeDip", config)
        self.assertIn("magnifierPosition", config)
        self.assertIn('ParseJsonDoubleField(json, "magnifierZoom")', core)
        self.assertIn('ParseJsonIntField(json, "magnifierSizeDip")', core)
        self.assertIn('ParseJsonStringField(json, "magnifierPosition")', core)
        self.assertIn("GetDeviceCaps(hdc, LOGPIXELSX)", overlay)
        self.assertIn("cursorGap", overlay)
        self.assertIn("magnifier_shape.horizontal", settings)
        self.assertIn("settings.annot.magnifier_zoom", catalog)
        self.assertIn("settings.annot.magnifier_size", catalog)
        self.assertIn("settings.annot.magnifier_position", catalog)

    def test_annotation_input_warns_when_annotations_are_hidden(self) -> None:
        source = (REPO_ROOT / "src/pdf_view/input.cppinc").read_text(encoding="utf-8")
        catalog = json.loads((REPO_ROOT / "locales/ja.json").read_text(encoding="utf-8"))
        self.assertIn("NotifyAnnotationInputWhileHidden", source)
        self.assertIn('localization::Text(L"pdf.annotation_input_hidden")', source)
        self.assertIn("注釈表示がOFFです。入力した注釈は保存されますが", catalog["pdf.annotation_input_hidden"])
        self.assertIn("if (g_showAnnots) return;", source)


@contextmanager
def repo_tempdir():
    TEST_TMP_ROOT.mkdir(parents=True, exist_ok=True)
    path = TEST_TMP_ROOT / f"case_{uuid.uuid4().hex}"
    path.mkdir(parents=True, exist_ok=False)
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


class MigrateClropV1Tests(unittest.TestCase):
    def test_newer_clrop_versions_are_preserved_even_with_force(self) -> None:
        for version in (2, 3):
            for force in (False, True):
                with repo_tempdir() as root:
                    path = root / "newer.clrop"
                    original = json.dumps({"version": version, "pdf_id": {"size": 1},
                        "pages": [{"page": 0, "items": [{"type": "marker-text", "marker_display_mode": "legacy"}]}]})
                    path.write_text(original, encoding="utf-8")
                    status, detail = migrate_clrop_v1.migrate_one(path, force=force, dry_run=False)
                    self.assertEqual(status, "skipped" if version == 2 else "failed")
                    self.assertEqual(path.read_text(encoding="utf-8"), original)
                    self.assertEqual(list(root.iterdir()), [path])

    def test_current_v1_file_is_skipped_without_changes(self) -> None:
        with repo_tempdir() as root:
            path = root / "sample.clrop"
            doc = {
                "version": 1,
                "pdf_id": {"path": "x.pdf", "size": 0, "sha256": ""},
                "pages": [],
            }
            original = json.dumps(doc, ensure_ascii=False)
            path.write_text(original, encoding="utf-8")

            status, detail = migrate_clrop_v1.migrate_one(path, force=False, dry_run=False)

            self.assertEqual(status, "skipped")
            self.assertEqual(detail, "already-current")
            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_legacy_clrop_dry_run_reports_would_migrate(self) -> None:
        with repo_tempdir() as root:
            path = root / "legacy.clrop"
            doc = {
                "annots": [
                    {
                        "page": 0,
                        "type": 2,
                        "text": "hello",
                        "bbox": [10, 20, 30, 40],
                    }
                ]
            }
            original = json.dumps(doc, ensure_ascii=False)
            path.write_text(original, encoding="utf-8")

            status, detail = migrate_clrop_v1.migrate_one(path, force=False, dry_run=True)

            self.assertEqual(status, "would-migrate")
            self.assertIn("dry-run", detail)
            self.assertEqual(path.read_text(encoding="utf-8"), original)
            self.assertFalse(any(root.glob("legacy.legacy_*.clrop")))

    def test_write_failure_restores_original_legacy_source(self) -> None:
        with repo_tempdir() as root:
            path = root / "broken.clrop"
            doc = {
                "annots": [
                    {
                        "page": 0,
                        "type": 2,
                        "text": "hello",
                        "bbox": [1, 2, 3, 4],
                    }
                ]
            }
            original = json.dumps(doc, ensure_ascii=False)
            path.write_text(original, encoding="utf-8")

            with mock.patch.object(migrate_clrop_v1, "write_atomic", side_effect=OSError("disk full")):
                status, detail = migrate_clrop_v1.migrate_one(path, force=False, dry_run=False)

            self.assertEqual(status, "failed")
            self.assertIn("restored backup", detail)
            self.assertTrue(path.exists())
            self.assertEqual(path.read_text(encoding="utf-8"), original)
            self.assertFalse(any(root.glob("broken.legacy_*.clrop")))

    def test_successful_migration_renames_old_file_and_reuses_original_name(self) -> None:
        with repo_tempdir() as root:
            path = root / "sample.clrop"
            doc = {
                "annots": [
                    {
                        "page": 0,
                        "type": 2,
                        "text": "hello",
                        "bbox": [1, 2, 30, 40],
                    }
                ]
            }
            original = json.dumps(doc, ensure_ascii=False)
            path.write_text(original, encoding="utf-8")

            status, detail = migrate_clrop_v1.migrate_one(path, force=False, dry_run=False)

            self.assertEqual(status, "migrated")
            self.assertIn("backup=", detail)
            self.assertTrue(path.exists())
            backups = list(root.glob("sample.legacy_*.clrop"))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_text(encoding="utf-8"), original)

            migrated = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(migrated["version"], 1)
            self.assertEqual(migrated["pages"][0]["items"][0]["id"].startswith("migrated-p1-i1-"), True)

    def test_iter_clrop_files_excludes_resource_tree(self) -> None:
        with repo_tempdir() as root:
            keep = root / "lecture" / "sample.clrop"
            keep.parent.mkdir(parents=True, exist_ok=True)
            keep.write_text("{}", encoding="utf-8")

            ignored = root / "__pdf_note_workspace__" / "__tmp__" / "__stage__" / "clrop" / "staged.clrop"
            ignored.parent.mkdir(parents=True, exist_ok=True)
            ignored.write_text("{}", encoding="utf-8")

            found = sorted(p.relative_to(root).as_posix() for p in migrate_clrop_v1.iter_clrop_files(root))

            self.assertEqual(found, ["lecture/sample.clrop"])

    def test_setup_json_workspace_root_is_resolved(self) -> None:
        with repo_tempdir() as root:
            workspace = root / "workspace"
            workspace.mkdir()
            setup = root / "pdf_workspace_setup.json"
            setup.write_text('{\n  "workspaceRoot": "workspace"\n}\n', encoding="utf-8")

            resolved = migrate_clrop_v1.resolve_scan_roots(None, str(setup))

            self.assertEqual(len(resolved), 1)
            self.assertEqual(resolved[0].resolve(), workspace.resolve())

    def test_setup_json_temp_external_dirs_are_included_even_with_loose_json(self) -> None:
        with repo_tempdir() as root:
            workspace = root / "workspace"
            workspace.mkdir()
            ext1 = root / "外科学"
            ext2 = root / "内科学"
            ext1.mkdir()
            ext2.mkdir()
            setup = root / "pdf_workspace_setup.json"
            setup.write_text(
                '{\n'
                '  "workspaceRoot": "workspace",\n'
                '  "tempExternalLectureDirs": ["外科学", "内科学"]\n'
                '}\n',
                encoding="utf-8",
            )

            roots = migrate_clrop_v1.resolve_scan_roots(None, str(setup))

            self.assertEqual([p.resolve() for p in roots], [workspace.resolve(), ext1.resolve(), ext2.resolve()])

    def test_main_counts_invalid_json_as_failed(self) -> None:
        with repo_tempdir() as root:
            (root / "bad.clrop").write_text("{ invalid json", encoding="utf-8")

            out = io.StringIO()
            with redirect_stdout(out):
                code = migrate_clrop_v1.main(["--root", str(root)])

            self.assertEqual(code, 1)
            text = out.getvalue()
            self.assertIn("[failed]", text)
            self.assertIn("[SUMMARY]", text)

    def test_main_writes_report_json(self) -> None:
        with repo_tempdir() as root:
            (root / "bad.clrop").write_text("{ invalid json", encoding="utf-8")
            report = root / "report.json"

            out = io.StringIO()
            with redirect_stdout(out):
                code = migrate_clrop_v1.main(["--root", str(root), "--report", str(report)])

            self.assertEqual(code, 1)
            self.assertTrue(report.exists())
            payload = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(payload["tool"], "migrate_clrop_v1")
            self.assertEqual(payload["report_version"], 1)
            self.assertEqual(payload["workspace_root"], str(root))
            self.assertEqual(payload["scan_roots"], [str(root)])
            self.assertEqual(payload["counts"]["failed"], 1)
            self.assertEqual(payload["results"][0]["status"], "failed")


class AnalyzeRepoTests(unittest.TestCase):
    def test_collect_files_and_summarize_small_tree(self) -> None:
        with repo_tempdir() as root:
            (root / "tools").mkdir()
            (root / "tools" / "sample.py").write_text(
                "import os\n\n\ndef hello():\n    return 1\n\nunused_var = 1\n", encoding="utf-8"
            )

            files = analyze_repo.collect_files(
                root=root,
                scope="all",
                include_roots=[],
                extra_excludes=[],
            )
            data = analyze_repo.summarize(files)

            self.assertEqual(data["summary"]["files"], 1)
            self.assertGreaterEqual(data["summary"]["lines"], 4)
            self.assertGreaterEqual(data["summary"]["functions"], 1)
            self.assertGreaterEqual(data["summary"]["approx_variable_decls"], 1)

    def test_analyze_repository_own_scope_respects_include_and_exclude(self) -> None:
        with repo_tempdir() as root:
            (root / "tools").mkdir()
            (root / "docs").mkdir()
            (root / "tools" / "keep.py").write_text("def kept():\n    return 1\n", encoding="utf-8")
            (root / "docs" / "drop.md").write_text("# ignored\n", encoding="utf-8")

            data = analyze_repo.analyze_repository(
                root=root,
                scope="own",
                include=["tools"],
                exclude=["docs"],
            )

            self.assertEqual(data["summary"]["files"], 1)
            self.assertEqual(data["meta"]["scope"], "own")

    def test_render_text_report_includes_unused_sections(self) -> None:
        with repo_tempdir() as root:
            (root / "tools").mkdir()
            (root / "tools" / "sample.py").write_text(
                "def used():\n    return 1\n\nused()\nvalue = 1\n", encoding="utf-8"
            )

            data = analyze_repo.analyze_repository(root=root, scope="all")
            rendered = analyze_repo.render_text_report(data, top_files=5, top_dirs=5, max_tree_depth=3)

            self.assertIn("Approx Unused", rendered)
            self.assertIn("Summary", rendered)

    def test_render_json_report_is_ascii_safe_for_windows_shell_redirection(self) -> None:
        data = {"path": "tests/fixtures/日本語\\記号を含む.pptx"}

        rendered = analyze_repo.render_json_report(data)

        self.assertTrue(rendered.isascii())
        self.assertEqual(json.loads(rendered), data)


    def test_main_defaults_to_stdout_and_refuses_to_overwrite_report_files(self) -> None:
        with repo_tempdir() as root:
            (root / "tools").mkdir()
            (root / "tools" / "sample.py").write_text("def hello():\n    return 1\n", encoding="utf-8")

            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = analyze_repo.main(["--root", str(root), "--scope", "all"])

            self.assertEqual(code, 0)
            self.assertIn("Summary", stdout.getvalue())
            self.assertFalse((root / "out" / "repo_report.txt").exists())

            report = root / "out" / "repo_report.txt"
            with redirect_stdout(stdout):
                code = analyze_repo.main(["--root", str(root), "--scope", "all", "--report", str(report)])

            self.assertEqual(code, 0)
            self.assertTrue(report.exists())
            self.assertIn("Summary", report.read_text(encoding="utf-8"))

            stderr = io.StringIO()
            with redirect_stderr(stderr):
                code = analyze_repo.main(["--root", str(root), "--scope", "all", "--report", str(report)])

            self.assertEqual(code, 2)
            self.assertIn("already exists", stderr.getvalue())

    def test_main_supports_report_stdout_without_creating_file(self) -> None:
        with repo_tempdir() as root:
            (root / "tools").mkdir()
            (root / "tools" / "sample.py").write_text("def hello():\n    return 1\n", encoding="utf-8")

            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = analyze_repo.main(["--root", str(root), "--scope", "all", "--report-stdout"])

            self.assertEqual(code, 0)
            self.assertIn("Summary", stdout.getvalue())
            self.assertFalse((root / "report_stdout.txt").exists())


class TextIntegrityGateTests(unittest.TestCase):
    def test_deleted_index_entry_is_not_a_text_read_error(self) -> None:
        with repo_tempdir() as root:
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            deleted = root / "removed.txt"
            deleted.write_text("removed\n", encoding="utf-8")
            kept = root / "kept.json"
            kept.write_text('{"valid":true}', encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            deleted.unlink()
            paths = text_integrity_gate.git_visible_paths(root)
            self.assertEqual([Path("kept.json")], paths)
            self.assertEqual([], text_integrity_gate.audit_paths(root, paths)["errors"])
            kept.unlink()  # disappearing after enumeration must still fail closed
            report = text_integrity_gate.audit_paths(root, paths)
            self.assertEqual("read-error", report["errors"][0]["kind"])

    def test_git_visible_paths_includes_nonignored_untracked_files(self) -> None:
        with repo_tempdir() as root:
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            (root / ".gitignore").write_text("ignored.txt\n", encoding="utf-8")
            (root / "tracked.txt").write_text("tracked\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", ".gitignore", "tracked.txt"], check=True)
            (root / "untracked.json").write_text('{"pending":true}', encoding="utf-8")
            (root / "ignored.txt").write_text("ignored\n", encoding="utf-8")

            paths = {path.as_posix() for path in text_integrity_gate.git_visible_paths(root)}

            self.assertIn("tracked.txt", paths)
            self.assertIn("untracked.json", paths)
            self.assertNotIn("ignored.txt", paths)

    def test_audit_rejects_utf16_invalid_utf8_and_invalid_json(self) -> None:
        with repo_tempdir() as root:
            (root / "valid.json").write_text('{"name":"日本語"}', encoding="utf-8")
            (root / "invalid.json").write_bytes(b'{"value":"\\q"}')
            (root / "utf16.cpp").write_bytes(b"\xff\xfei\x00n\x00t\x00")
            (root / "invalid.txt").write_bytes(b"\x80")

            report = text_integrity_gate.audit_paths(
                root,
                [Path("valid.json"), Path("invalid.json"), Path("utf16.cpp"), Path("invalid.txt")],
            )

            kinds = {entry["kind"] for entry in report["errors"]}
            self.assertEqual(report["summary"]["text_files_checked"], 4)
            self.assertIn("invalid-json", kinds)
            self.assertIn("non-utf8-bom", kinds)
            self.assertIn("invalid-utf8", kinds)

    def test_audit_allows_only_the_pinned_legacy_exception(self) -> None:
        with repo_tempdir() as root:
            path = root / "third_party/pdfium/licenses/freetype.txt"
            path.parent.mkdir(parents=True)
            data = b"\x93FreeType\x94"
            path.write_bytes(data)
            original = text_integrity_gate.LEGACY_TEXT_EXCEPTIONS[
                "third_party/pdfium/licenses/freetype.txt"
            ]
            text_integrity_gate.LEGACY_TEXT_EXCEPTIONS["third_party/pdfium/licenses/freetype.txt"] = {
                **original,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            try:
                report = text_integrity_gate.audit_paths(root, [path.relative_to(root)])
            finally:
                text_integrity_gate.LEGACY_TEXT_EXCEPTIONS["third_party/pdfium/licenses/freetype.txt"] = original

            self.assertEqual(report["summary"]["errors"], 0)
            self.assertEqual(report["summary"]["legacy_exceptions"], 1)

    def test_text_report_escapes_non_ascii_paths_for_console_safety(self) -> None:
        report = {
            "summary": {
                "text_files_checked": 1,
                "structured_files_checked": 0,
                "legacy_exceptions": 0,
                "errors": 1,
                "warnings": 0,
            },
            "errors": [{"path": "docs/日本語.json", "kind": "invalid-json", "detail": "test"}],
            "warnings": [],
        }

        rendered = text_integrity_gate.render_text(report)

        self.assertTrue(rendered.isascii())
        self.assertIn(r"docs/\u65e5\u672c\u8a9e.json", rendered)


class AnalyzeBuildLogsTests(unittest.TestCase):
    def test_analyze_log_directory_collects_durations_and_findings(self) -> None:
        with repo_tempdir() as root:
            logs = root / "out" / "logs"
            logs.mkdir(parents=True)
            (logs / "build_end_time.log").write_text(
                "2026-07-06T01:00:00+09:00\telapsed_sec=12.500\n"
                "2026-07-06T01:10:00+09:00\telapsed_sec=7.500\n",
                encoding="utf-8",
            )
            (logs / "build_readonly_viewer_end_time.log").write_text(
                "2026-07-06T01:05:00+09:00\telapsed_sec=3.250\n",
                encoding="utf-8",
            )
            (logs / "build_detail_20260706_010000.log").write_text(
                "== Build ==\n"
                "started: 2026-07-06T01:00:00+09:00\n"
                "configuration: Release\n\n"
                "src/main.cpp:10:3: warning: sample warning\n"
                "src/main.cpp:11:4: error: sample error\n",
                encoding="utf-8",
            )
            (logs / "build_readonly_viewer_detail_20260706_010500.log").write_text(
                "== Read-Only Viewer Build ==\n"
                "started: 2026-07-06T01:05:00+09:00\n"
                "configuration: Release\n\n"
                "src/readonly_viewer/main.cpp:20:5: warning: viewer warning\n",
                encoding="utf-8",
            )

            data = analyze_build_logs.analyze_log_directory(logs, root, top=5)

            self.assertEqual(data["summary"]["detail_log_count"], 2)
            self.assertEqual(data["summary"]["warning_total"], 2)
            self.assertEqual(data["summary"]["error_total"], 1)
            self.assertEqual(data["detail_stats"]["app_or_all"]["failed"], 1)
            self.assertEqual(data["detail_stats"]["readonly_viewer"]["ok"], 1)
            self.assertEqual(data["duration_stats"]["combined"]["count"], 3.0)
            self.assertEqual(data["warnings"]["top_files"][0]["value"], "src/main.cpp")
            self.assertEqual(data["errors"]["top_messages"][0]["value"], "sample error")

    def test_main_writes_markdown_report_when_requested(self) -> None:
        with repo_tempdir() as root:
            logs = root / "out" / "logs"
            logs.mkdir(parents=True)
            (logs / "build_end_time.log").write_text(
                "2026-07-06T01:00:00+09:00\telapsed_sec=1.000\n",
                encoding="utf-8",
            )
            (logs / "build_detail_20260706_010000.log").write_text(
                "== Build ==\n"
                "started: 2026-07-06T01:00:00+09:00\n"
                "configuration: Release\n",
                encoding="utf-8",
            )
            report = root / "out" / "reports" / "build_log_analysis.md"
            stdout = io.StringIO()

            with redirect_stdout(stdout):
                code = analyze_build_logs.main(
                    [
                        "--root",
                        str(root),
                        "--format",
                        "md",
                        "--report",
                        str(report),
                    ]
                )

            self.assertEqual(code, 0)
            self.assertTrue(report.exists())
            self.assertIn("# Build Log Analysis", report.read_text(encoding="utf-8"))
            self.assertIn("## Summary", stdout.getvalue())


class DistributionSizeReportTests(unittest.TestCase):
    def test_zip_report_uses_compressed_member_sizes_and_accounts_for_overhead(self) -> None:
        with repo_tempdir() as root:
            archive_path = root / "distribution.zip"
            with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_STORED) as archive:
                archive.writestr("app/PDFNote.exe", b"x" * 100)
                archive.writestr("app/assets/icon.PNG", b"y" * 50)
                archive.writestr("app/docs/guide.pdf", b"z" * 25)
                archive.writestr("app/notes/lesson.clro", b"n" * 40)
                archive.writestr("app/annotations/lesson.clrop", b"a" * 30)
                archive.writestr("app/src/main.cpp", b"c" * 20)
                archive.writestr("app/fonts/text.ttf", b"f" * 10)
                archive.writestr("app/README.txt", b"t" * 5)

            report = distribution_size_report.analyze_distribution(archive_path)
            categories = {row["category"]: row for row in report["categories"]}

            self.assertEqual(report["basis"], "ZIP圧縮後サイズ")
            self.assertEqual(report["total_size_bytes"], archive_path.stat().st_size)
            self.assertEqual(categories["実行ファイル・ライブラリ"]["size_bytes"], 100)
            self.assertEqual(categories["PNG画像"]["size_bytes"], 50)
            self.assertEqual(categories["PDF"]["size_bytes"], 25)
            self.assertEqual(categories["ノート (.clro)"]["size_bytes"], 40)
            self.assertEqual(categories["注釈データ (.clrop)"]["size_bytes"], 30)
            self.assertEqual(categories["ソースコード・パッチ"]["size_bytes"], 20)
            self.assertEqual(categories["フォント"]["size_bytes"], 10)
            self.assertEqual(categories["テキスト・文書"]["size_bytes"], 5)
            self.assertEqual(
                categories["ZIP管理情報"]["size_bytes"],
                archive_path.stat().st_size - 280,
            )
            self.assertEqual(sum(row["size_bytes"] for row in report["categories"]), report["total_size_bytes"])
            self.assertAlmostEqual(sum(row["percent"] for row in report["categories"]), 100.0)

    def test_directory_report_groups_file_sizes_without_modifying_files(self) -> None:
        with repo_tempdir() as root:
            distribution = root / "distribution"
            distribution.mkdir()
            (distribution / "app.dll").write_bytes(b"a" * 20)
            (distribution / "photo.webp").write_bytes(b"b" * 30)
            (distribution / "unknown.xyz").write_bytes(b"c" * 10)

            report = distribution_size_report.analyze_distribution(distribution)
            categories = {row["category"]: row for row in report["categories"]}

            self.assertEqual(report["basis"], "展開後ファイルサイズ")
            self.assertEqual(report["total_size_bytes"], 60)
            self.assertEqual(categories["実行ファイル・ライブラリ"]["percent"], 100 * 20 / 60)
            self.assertEqual(categories["その他の画像"]["size_bytes"], 30)
            self.assertEqual(categories["その他"]["size_bytes"], 10)
            self.assertTrue((distribution / "app.dll").exists())


class AnalyzeDocumentLanguageTests(unittest.TestCase):
    def test_analyze_documents_keeps_groups_separate_and_applies_filters(self) -> None:
        with repo_tempdir() as root:
            (root / "docs" / "internal").mkdir(parents=True)
            (root / "docs" / "public").mkdir(parents=True)
            (root / "docs" / "internal" / "guide.md").write_text(
                "PDF 注釈 PDF 注釈 の です\n", encoding="utf-8"
            )
            (root / "docs" / "internal" / "LICENSE.md").write_text(
                "ignored license words\n", encoding="utf-8"
            )
            (root / "docs" / "public" / "guide.md").write_text(
                "保存 復元 保存\n", encoding="utf-8"
            )

            data = analyze_document_language.analyze_documents(
                root,
                [("internal", Path("docs/internal")), ("public", Path("docs/public"))],
                {".md"},
                list(analyze_document_language.DEFAULT_EXCLUDES),
                set(analyze_document_language.DEFAULT_STOP_WORDS),
                top=5,
            )

            internal, public = data["groups"]
            self.assertEqual(internal["file_count"], 1)
            self.assertEqual(public["file_count"], 1)
            self.assertEqual(internal["unigrams"][0]["term"], "pdf")
            self.assertEqual(public["unigrams"][0]["term"], "保存")
            self.assertEqual(internal["unigrams"][0]["source_files"], ["docs/internal/guide.md"])

    def test_main_refuses_to_overwrite_report(self) -> None:
        with repo_tempdir() as root:
            (root / "docs" / "public").mkdir(parents=True)
            (root / "docs" / "public" / "guide.md").write_text("PDF 注釈\n", encoding="utf-8")
            report = root / "out" / "report.md"
            report.parent.mkdir()
            report.write_text("keep this report", encoding="utf-8")

            with redirect_stdout(io.StringIO()):
                code = analyze_document_language.main(
                    ["--root", str(root), "--group", "public=docs/public", "--report", str(report)]
                )

            self.assertEqual(code, 2)
            self.assertEqual(report.read_text(encoding="utf-8"), "keep this report")

    def test_theme_candidates_use_document_coverage_headings_and_locations(self) -> None:
        with repo_tempdir() as root:
            internal = root / "docs" / "internal"
            public = root / "docs" / "public"
            internal.mkdir(parents=True)
            public.mkdir(parents=True)
            (internal / "a.md").write_text("# 保存 復元\n本文\n", encoding="utf-8")
            (internal / "b.md").write_text("# 保存 復元\n本文\n", encoding="utf-8")
            (public / "guide.md").write_text("# 保存 復元\n> 引用 固有語\n```\nコード 固有語\n```\n", encoding="utf-8")

            data = analyze_document_language.analyze_documents(
                root,
                [("internal", Path("docs/internal")), ("public", Path("docs/public"))],
                {".md"}, [], set(), top=5, theme_top=5, theme_min_docs=2,
            )

            candidate = next(row for row in data["theme_candidates"] if row["term"] == "保存復元")
            self.assertEqual(candidate["document_count"], 3)
            self.assertEqual(candidate["heading_document_count"], 3)
            self.assertEqual(candidate["groups"], ["internal", "public"])
            self.assertEqual(candidate["source_locations"][0], {"file": "docs/internal/a.md", "line": 1})
            all_terms = {row["term"] for row in data["groups"][1]["unigrams"]}
            self.assertNotIn("引用", all_terms)
            self.assertNotIn("コード", all_terms)


class CodeMetricsGuiTests(unittest.TestCase):
    def test_parse_csv_field_trims_and_drops_empty_items(self) -> None:
        app = object.__new__(code_metrics_gui.CodeMetricsApp)
        parsed = app._parse_csv_field(" tools, docs ,, tests ")
        self.assertEqual(parsed, ["tools", "docs", "tests"])

    def test_get_positive_int_uses_fallback_for_invalid_values(self) -> None:
        app = object.__new__(code_metrics_gui.CodeMetricsApp)
        self.assertEqual(app._get_positive_int("7", 3), 7)
        self.assertEqual(app._get_positive_int("0", 3), 3)
        self.assertEqual(app._get_positive_int("bad", 3), 3)


class ValidateCodebaseTests(unittest.TestCase):
    def test_final_render_boundary_accepts_current_runtime_route(self) -> None:
        self.assertEqual(validate_codebase.find_final_render_publication_boundary_regressions(), [])
        self.assertEqual(validate_codebase.find_note_render_incremental_regressions(), [])

    def test_final_render_boundary_rejects_missing_proof_or_mixed_paint(self) -> None:
        final_path = validate_codebase.REPO_ROOT / "src/note_view/note_view_final_render.cppinc"
        input_path = validate_codebase.REPO_ROOT / "src/note_view/note_view_input_proc.cppinc"
        read_text = Path.read_text
        cases = (
            (final_path, "bool FinalFrameCanUseCustomPaint(", "bool FinalRenderRouteEnabled(",
             "if (!context.finalRenderState.has_frame()) return false;", "",
             "require a published frame"),
            (final_path, "bool FinalFrameCanUseCustomPaint(", "bool FinalRenderRouteEnabled(",
             "return IsCurrentNoteTextCoreSynchronizedWithEditor(hWnd);", "return true;",
             "require the current editor binding outside IME"),
            (final_path, "bool FinalFrameCanUseCustomPaint(", "bool FinalRenderRouteEnabled(",
             "context.finalImePreedit.source_identity() ==", "true ||",
             "compare the IME source identity"),
            (final_path, "bool FinalFrameCanUseCustomPaint(", "bool FinalRenderRouteEnabled(",
             "if (!context.finalRenderState.has_frame())", "GetEditTextForIndexing(hWnd); if (!context.finalRenderState.has_frame())",
             "must not materialize complete editor text"),
            (final_path, "static bool TryPaintFinalNoteRenderFrame(", "static bool TryGetFinalNoteRenderHit(",
             "WithdrawFinalNoteRenderFrame(hWnd);", "(void)hWnd;",
             "a final paint failure must withdraw its publication"),
            (input_path, "if (!finalPainted)", "if (savedOverlayDc != 0)",
             "paintNoteBackground(paintDC);", "",
             "native fallback after final paint failure must erase partial final pixels first"),
            (input_path, "const bool finalPainted =", "case WM_IME_REQUEST",
             "TryPaintFinalNoteRenderFrame(hWnd, paintDC);", "TryPaintFinalNoteRenderFrame(hWnd, paintDC); RefreshFinalNoteRenderFrame(hWnd);",
             "paint must consume a publication without rebuilding or copying the document"),
        )
        for path, start, end, original, replacement, expected in cases:
            with self.subTest(contract=expected):
                source = read_text(path, encoding="utf-8")
                begin = source.index(start)
                finish = source.index(end, begin)
                section = source[begin:finish]
                self.assertEqual(section.count(original), 1)
                changed = source[:begin] + section.replace(original, replacement, 1) + source[finish:]

                def mutated_read_text(candidate: Path, *args, **kwargs):
                    if candidate == path:
                        return changed
                    return read_text(candidate, *args, **kwargs)

                with mock.patch.object(Path, "read_text", new=mutated_read_text):
                    problems = validate_codebase.find_final_render_publication_boundary_regressions()
                self.assertTrue(any(expected in item for item in problems), problems)

    def test_owner_plan_rejects_missing_binding_or_partition_proof(self) -> None:
        path = validate_codebase.REPO_ROOT / "src/note/note_presentation_owner_plan.cpp"
        read_text = Path.read_text
        source = read_text(path, encoding="utf-8")
        cases = (
            ("!input.editor_text_core_current", "false",
             "owner planning must reject an unbound editor"),
            ("input.ime_preedit && !input.ime_preedit_can_reuse_committed_layout", "false",
             "owner planning must reject an unproven preedit layout"),
            ("if (!IsExactOwnerPartition(candidate->owner_ranges_, input.visible_lines))", "if (false)",
             "published line owners must form an exact visible partition"),
            ("NotePresentationOwnerPlanBuildResult::InvariantViolation", "NotePresentationOwnerPlanBuildResult::Built",
             "an inconsistent owner partition must reject publication"),
        )
        for original, replacement, expected in cases:
            with self.subTest(contract=expected):
                self.assertEqual(source.count(original), 1)
                changed = source.replace(original, replacement, 1)

                def mutated_read_text(candidate: Path, *args, **kwargs):
                    if candidate == path:
                        return changed
                    return read_text(candidate, *args, **kwargs)

                with mock.patch.object(Path, "read_text", new=mutated_read_text):
                    problems = validate_codebase.find_note_render_incremental_regressions()
                self.assertTrue(any(expected in item for item in problems), problems)

    def test_windows_powershell_requires_bom_for_non_ascii_source(self) -> None:
        with repo_tempdir() as root:
            script = root / "tests" / "scripts" / "sample.ps1"
            script.parent.mkdir(parents=True)
            script.write_text('Write-Host "日本語"\n', encoding="utf-8")

            with mock.patch.object(validate_codebase, "REPO_ROOT", root):
                problems = validate_codebase.find_windows_powershell_encoding_violations()

            self.assertEqual(len(problems), 1)
            self.assertIn("UTF-8 BOM", problems[0])

            script.write_bytes(b"\xef\xbb\xbf" + script.read_bytes())
            with mock.patch.object(validate_codebase, "REPO_ROOT", root):
                problems = validate_codebase.find_windows_powershell_encoding_violations()

            self.assertEqual(problems, [])

    def test_command_id_validation_rejects_palette_range_collision(self) -> None:
        with repo_tempdir() as root:
            header = root / "src" / "core" / "command_ids.h"
            header.parent.mkdir(parents=True)
            header.write_text(
                "inline constexpr int kToolPaletteCommandSlotCapacity = 16;\n"
                "enum CommandId : int {\n"
                "    ID_TOOL_COLOR_BASE = 3100,\n"
                "    ID_TOOL_FONT = 3110\n"
                "};\n",
                encoding="utf-8",
            )

            with mock.patch.object(validate_codebase, "REPO_ROOT", root):
                problems = validate_codebase.find_command_id_collisions()

            self.assertTrue(any("ID_TOOL_FONT=3110" in item for item in problems))
            self.assertTrue(any("3100..3115" in item for item in problems))

    def test_command_id_validation_accepts_reserved_palette_range(self) -> None:
        with repo_tempdir() as root:
            header = root / "src" / "core" / "command_ids.h"
            header.parent.mkdir(parents=True)
            header.write_text(
                "inline constexpr int kToolPaletteCommandSlotCapacity = 16;\n"
                "enum CommandId : int {\n"
                "    ID_TOOL_COLOR_BASE = 3050,\n"
                "    ID_TOOL_FONT = 3110\n"
                "};\n",
                encoding="utf-8",
            )

            with mock.patch.object(validate_codebase, "REPO_ROOT", root):
                problems = validate_codebase.find_command_id_collisions()

            self.assertEqual(problems, [])


class MdStructureScannerTests(unittest.TestCase):
    def test_extract_headings_ignores_html_comments_closed_with_end_bang(self) -> None:
        text = "# Before\n<!--\n# Hidden\n--!>\n# After\n"

        headings = md_structure_scanner.extract_headings_from_text(text)

        self.assertEqual([(h.level, h.text) for h in headings], [(1, "Before"), (1, "After")])

    def test_extract_headings_ignores_front_matter_and_fenced_code(self) -> None:
        text = (
            "---\n"
            "# metadata only\n"
            "---\n"
            "# Document\n"
            "```md\n"
            "# not a heading\n"
            "```\n"
            "Section\n"
            "-------\n"
        )

        headings = md_structure_scanner.extract_headings_from_text(text)

        self.assertEqual([(h.level, h.text) for h in headings], [(1, "Document"), (2, "Section")])

    def test_repository_defaults_exclude_generated_local_and_third_party_trees(self) -> None:
        with repo_tempdir() as root:
            keep = root / "docs" / "keep.md"
            local = root / ".local" / "private.md"
            third_party = root / "third_party" / "vendor.md"
            generated = root / "out" / "report.md"
            for path in (keep, local, third_party, generated):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("# Heading\n", encoding="utf-8")

            found = md_structure_scanner.iter_markdown_files(
                root,
                md_structure_scanner.DEFAULT_MD_EXTENSIONS,
                md_structure_scanner.DEFAULT_EXCLUDE_DIRS,
                md_structure_scanner.DEFAULT_EXCLUDE_FILES,
            )

            self.assertEqual([path.relative_to(root).as_posix() for path in found], ["docs/keep.md"])

    def test_main_is_read_only_by_default_and_explicit_reports_link_to_sources(self) -> None:
        with repo_tempdir() as root:
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = md_structure_scanner.main([str(root)])

            self.assertEqual(code, 0)
            self.assertIn("outputs: none (summary only;", stdout.getvalue())
            self.assertIn("--index out/md_structure_index.tsv", stdout.getvalue())
            self.assertFalse((root / "md_structure.json").exists())
            self.assertFalse((root / "MD_STRUCTURE_TOC.md").exists())

            report = root / "out" / "toc.md"
            data = root / "out" / "structure.json"
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = md_structure_scanner.main(
                    [str(root), "--toc", str(report), "--json", str(data)]
                )

            self.assertEqual(code, 0)
            self.assertIn(f"json: {data.resolve()}", stdout.getvalue())
            self.assertIn(f"toc: {report.resolve()}", stdout.getvalue())
            self.assertIn("(../README.md#project)", report.read_text(encoding="utf-8"))
            self.assertEqual(json.loads(data.read_text(encoding="utf-8"))["file_count"], 1)

    def test_main_writes_compact_search_index_for_source_lookup(self) -> None:
        with repo_tempdir() as root:
            (root / "docs").mkdir()
            (root / "docs" / "design.md").write_text(
                "# 保存設計\n\n## Stage\t保存\n", encoding="utf-8"
            )
            index = root / "out" / "md_structure_index.tsv"
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = md_structure_scanner.main([str(root), "--index", str(index)])

            self.assertEqual(code, 0)
            index_text = index.read_text(encoding="utf-8")
            self.assertIn("# Generated locator only;", index_text)
            self.assertIn("docs/design.md\t1\tH1\t保存設計", index_text)
            self.assertIn("docs/design.md\t3\tH2\tStage\\t保存", index_text)
            self.assertIn(f"index: {index.resolve()}", stdout.getvalue())

    def test_stdout_toc_reports_written_paths_on_stderr(self) -> None:
        with repo_tempdir() as root:
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            report = root / "out" / "toc.md"
            stdout = io.StringIO()
            stderr = io.StringIO()
            with redirect_stdout(stdout), mock.patch.object(sys, "stderr", stderr):
                code = md_structure_scanner.main([str(root), "--toc", str(report), "--stdout"])

            self.assertEqual(code, 0)
            self.assertTrue(stdout.getvalue().startswith("# Markdown Structure TOC"))
            self.assertNotIn("outputs:", stdout.getvalue())
            self.assertIn(f"toc: {report.resolve()}", stderr.getvalue())

    def test_main_refuses_toc_that_would_be_scanned_as_source_markdown(self) -> None:
        with repo_tempdir() as root:
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            report = root / "docs" / "generated_toc.md"
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = md_structure_scanner.main([str(root), "--toc", str(report)])

            self.assertEqual(code, 2)
            self.assertIn("--toc must be outside scanned Markdown inputs", stderr.getvalue())
            self.assertFalse(report.exists())

    def test_main_refuses_json_output_that_would_replace_source_markdown(self) -> None:
        with repo_tempdir() as root:
            source = root / "README.md"
            original = "# Project\n"
            source.write_text(original, encoding="utf-8")
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = md_structure_scanner.main([str(root), "--json", str(source)])

            self.assertEqual(code, 2)
            self.assertIn("--json must be outside scanned Markdown inputs", stderr.getvalue())
            self.assertEqual(source.read_text(encoding="utf-8"), original)

    def test_main_refuses_index_output_that_would_replace_source_markdown(self) -> None:
        with repo_tempdir() as root:
            source = root / "README.md"
            original = "# Project\n"
            source.write_text(original, encoding="utf-8")
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = md_structure_scanner.main([str(root), "--index", str(source)])

            self.assertEqual(code, 2)
            self.assertIn("--index must be outside scanned Markdown inputs", stderr.getvalue())
            self.assertEqual(source.read_text(encoding="utf-8"), original)

    def test_main_refuses_same_path_for_generated_outputs(self) -> None:
        with repo_tempdir() as root:
            report = root / "out" / "report.md"
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = md_structure_scanner.main(
                    [str(root), "--index", str(report), "--toc", str(report)]
                )

            self.assertEqual(code, 2)
            self.assertIn("output paths must be different", stderr.getvalue())
            self.assertFalse(report.exists())

    def test_console_output_escapes_characters_not_supported_by_cp932(self) -> None:
        raw = io.BytesIO()
        output = io.TextIOWrapper(raw, encoding="cp932")

        md_structure_scanner.print_console("A \u2194 B", stream=output)
        output.flush()

        self.assertEqual(raw.getvalue().decode("cp932").splitlines(), ["A \\u2194 B"])


class CppIncludeVisualizerTests(unittest.TestCase):
    def test_root_beneath_ignored_directory_still_scans_its_sources(self) -> None:
        with repo_tempdir() as temp:
            root = temp / "out" / "project"
            (root / "src").mkdir(parents=True)
            (root / "src" / "main.cpp").write_text('#include "local.h"\n', encoding="utf-8")
            (root / "src" / "local.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "build").mkdir()
            (root / "build" / "generated.cpp").write_text("", encoding="utf-8")

            result = cpp_include_visualizer.analyze_project(
                root, cpp_include_visualizer.DEFAULT_IGNORE_DIRS
            )

            self.assertEqual(result.files, ["src/local.h", "src/main.cpp"])
            self.assertEqual([(edge.src, edge.dst) for edge in result.edges],
                             [("src/main.cpp", "src/local.h")])

    def test_empty_scan_fails_without_writing_a_complete_report(self) -> None:
        with repo_tempdir() as root:
            report = root / "report.md"
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                code = cpp_include_visualizer.main([str(root), "--report", str(report)])

            self.assertEqual(code, 2)
            self.assertIn("no C/C++ source files", stderr.getvalue())
            self.assertFalse(report.exists())

    def test_cppinc_files_are_scanned_and_resolved(self) -> None:
        with repo_tempdir() as root:
            (root / "src" / "main").mkdir(parents=True)
            (root / "src" / "core").mkdir(parents=True)
            (root / "src" / "main.cpp").write_text(
                '#include "main/bootstrap.cppinc"\n', encoding="utf-8"
            )
            (root / "src" / "main" / "bootstrap.cppinc").write_text(
                '#include "core/app_core.h"\n', encoding="utf-8"
            )
            (root / "src" / "core" / "app_core.h").write_text("#pragma once\n", encoding="utf-8")

            result = cpp_include_visualizer.analyze_project(
                root,
                ignore_dirs=set(),
                include_roots=[Path("src")],
            )

            edges = {(edge.src, edge.dst) for edge in result.edges}
            self.assertIn(("src/main.cpp", "src/main/bootstrap.cppinc"), edges)
            self.assertIn(("src/main/bootstrap.cppinc", "src/core/app_core.h"), edges)
            self.assertIn("src/main/bootstrap.cppinc", result.files)
            self.assertEqual(result.unresolved, {})

    def test_ambiguous_basename_include_stays_unresolved(self) -> None:
        with repo_tempdir() as root:
            (root / "a").mkdir()
            (root / "b").mkdir()
            (root / "src").mkdir()
            (root / "a" / "shared.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "b" / "shared.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "src" / "main.cpp").write_text('#include "shared.h"\n', encoding="utf-8")

            result = cpp_include_visualizer.analyze_project(root, ignore_dirs=set())

            self.assertEqual(result.edges, [])
            self.assertEqual(result.unresolved, {"src/main.cpp": [("shared.h", 1)]})

    def test_main_writes_graph_and_report_with_include_root(self) -> None:
        with repo_tempdir() as root:
            (root / "src" / "core").mkdir(parents=True)
            (root / "src" / "feature").mkdir(parents=True)
            (root / "src" / "feature" / "feature.cpp").write_text(
                '#include "core/app_core.h"\n', encoding="utf-8"
            )
            (root / "src" / "core" / "app_core.h").write_text("#pragma once\n", encoding="utf-8")
            graph = root / "deps.mmd"
            report = root / "report.md"

            out = io.StringIO()
            with redirect_stdout(out):
                code = cpp_include_visualizer.main(
                    [
                        str(root),
                        "--include-root",
                        "src",
                        "--out",
                        str(graph),
                        "--report",
                        str(report),
                    ]
                )

            self.assertEqual(code, 0)
            self.assertIn("resolved edges: 1", out.getvalue())
            self.assertIn("src_feature_feature_cpp", graph.read_text(encoding="utf-8"))
            self.assertIn("No cycles detected.", report.read_text(encoding="utf-8"))
            self.assertTrue(report.read_text(encoding="utf-8").rstrip().endswith("ANALYSIS_COMPLETE"))

    def test_main_refuses_to_overwrite_existing_report(self) -> None:
        with repo_tempdir() as root:
            (root / "main.cpp").write_text("", encoding="utf-8")
            report = root / "report.md"
            original = "previous report must remain unchanged\n"
            report.write_text(original, encoding="utf-8")
            stderr = io.StringIO()

            with mock.patch.object(sys, "stderr", stderr):
                code = cpp_include_visualizer.main([str(root), "--report", str(report)])

            self.assertEqual(code, 2)
            self.assertEqual(report.read_text(encoding="utf-8"), original)
            self.assertIn("refusing to overwrite", stderr.getvalue())

    def test_main_prints_report_to_stdout_without_creating_file(self) -> None:
        with repo_tempdir() as root:
            (root / "main.cpp").write_text('#include "local.h"\n', encoding="utf-8")
            (root / "local.h").write_text("#pragma once\n", encoding="utf-8")
            stdout = io.StringIO()
            stderr = io.StringIO()

            with redirect_stdout(stdout), redirect_stderr(stderr):
                code = cpp_include_visualizer.main([str(root), "--report-stdout"])

            self.assertEqual(code, 0)
            self.assertIn("C/C++ Local Include Dependency Report", stdout.getvalue())
            self.assertTrue(stdout.getvalue().rstrip().endswith("ANALYSIS_COMPLETE"))
            self.assertIn("resolved edges: 1", stderr.getvalue())
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["local.h", "main.cpp"])

    def test_report_records_provenance_and_prints_visible_todos(self) -> None:
        with repo_tempdir() as root:
            (root / "main.cpp").write_text("", encoding="utf-8")
            report = root / "report.md"
            stderr = io.StringIO()

            with redirect_stderr(stderr):
                code = cpp_include_visualizer.main([str(root), "--report", str(report)])

            self.assertEqual(code, 0)
            report_text = report.read_text(encoding="utf-8")
            self.assertIn("## Reproducibility", report_text)
            self.assertIn("Git commit", report_text)
            self.assertIn("Analyzer SHA-256", report_text)
            self.assertIn("Include resolution order", report_text)
            self.assertIn("## Interpretation", report_text)
            self.assertIn("## TODO (not analyzed)", report_text)
            self.assertIn("Shared-state ownership", report_text)
            self.assertIn("TODO (not analyzed):", stderr.getvalue())

    def test_provenance_handles_non_ascii_git_status_bytes(self) -> None:
        with repo_tempdir() as root:
            mocked_results = [
                mock.Mock(stdout="C:/repo\n"),
                mock.Mock(stdout="deadbeef\n"),
                mock.Mock(stdout=" M docs/\u65e5\u672c\u8a9e.md\n".encode("utf-8")),
            ]
            with mock.patch.object(cpp_include_visualizer.subprocess, "run", side_effect=mocked_results):
                provenance = cpp_include_visualizer.collect_provenance(root, [], set(), None)

            self.assertEqual(provenance["git_commit"], "deadbeef")
            self.assertEqual(provenance["git_worktree_clean"], "false")

    def test_cycle_and_layer_violation_are_reported(self) -> None:
        with repo_tempdir() as root:
            (root / "src" / "ui").mkdir(parents=True)
            (root / "src" / "core").mkdir(parents=True)
            (root / "src" / "ui" / "ui.cpp").write_text(
                '#include "core/core.h"\n', encoding="utf-8"
            )
            (root / "src" / "core" / "core.h").write_text(
                '#include "ui/ui.cpp"\n', encoding="utf-8"
            )
            layers = root / "layers.json"
            layers.write_text(
                json.dumps(
                    {
                        "layers": [
                            {"name": "ui", "paths": ["src/ui/"], "may_depend_on": []},
                            {"name": "core", "paths": ["src/core/"], "may_depend_on": []},
                        ]
                    }
                ),
                encoding="utf-8",
            )
            report = root / "report.md"
            stdout = io.StringIO()
            stderr = io.StringIO()

            with redirect_stdout(stdout), redirect_stderr(stderr):
                code = cpp_include_visualizer.main(
                    [str(root), "--include-root", "src", "--layers", str(layers), "--report", str(report)]
                )

            self.assertEqual(code, 0)
            report_text = report.read_text(encoding="utf-8")
            self.assertIn("src/ui/ui.cpp", report_text)
            self.assertIn("src/core/core.h", report_text)
            self.assertIn("Cycle Detection", report_text)
            self.assertIn("layer violations: 2", stdout.getvalue())

    def test_main_writes_compact_index_without_printing_graph(self) -> None:
        with repo_tempdir() as root:
            (root / "src").mkdir()
            (root / "src" / "main.cpp").write_text(
                '#include "missing.h"\n#include "local.h"\n', encoding="utf-8"
            )
            (root / "src" / "local.h").write_text("#pragma once\n", encoding="utf-8")
            index = root / "out" / "includes.tsv"

            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = cpp_include_visualizer.main([str(root), "--index", str(index)])

            self.assertEqual(code, 0)
            self.assertNotIn("graph TD", stdout.getvalue())
            text = index.read_text(encoding="utf-8")
            self.assertIn("resolved\tsrc/main.cpp\t2\tsrc/local.h", text)
            self.assertIn("unresolved\tsrc/main.cpp\t1\tmissing.h", text)

    def test_main_refuses_index_that_replaces_scanned_source_file(self) -> None:
        with repo_tempdir() as root:
            source = root / "main.cpp"
            original = '#include "local.h"\n'
            source.write_text(original, encoding="utf-8")
            (root / "local.h").write_text("#pragma once\n", encoding="utf-8")

            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = cpp_include_visualizer.main([str(root), "--index", str(source)])

            self.assertEqual(code, 2)
            self.assertIn("must not replace a scanned source file", stderr.getvalue())
            self.assertEqual(source.read_text(encoding="utf-8"), original)


class PersistenceIndexTests(unittest.TestCase):
    def test_collect_findings_classifies_persistence_operations(self) -> None:
        with repo_tempdir() as root:
            source = root / "src" / "save.inc"
            source.parent.mkdir()
            source.write_text(
                "bool SaveNote() {\n"
                "  atomic_write::AtomicWriteUtf8(path, data, tmp, escape, &err);\n"
                "  std::filesystem::remove(stage, ec);\n"
                "  std::ofstream output(path, std::ios::binary);\n"
                "  WriteFile(handle, data, size, &written, nullptr);\n"
                "  SaveDC(hdc);\n"
                "  return true;\n"
                "}\n",
                encoding="utf-8",
            )

            findings = persistence_index.collect_findings(root, ["src"])

            rows = {(finding.category, finding.symbol) for finding in findings}
            self.assertIn(("persistence_symbol", "SaveNote"), rows)
            self.assertIn(("atomic_write", "AtomicWriteUtf8"), rows)
            self.assertIn(("filesystem_mutation", "remove"), rows)
            self.assertIn(("stream_write", "ofstream"), rows)
            self.assertIn(("win32_mutation", "WriteFile"), rows)
            self.assertNotIn(("persistence_symbol", "SaveDC"), rows)

    def test_main_writes_locator_and_refuses_source_replacement(self) -> None:
        with repo_tempdir() as root:
            source = root / "src" / "save.cpp"
            source.parent.mkdir()
            source.write_text("void SaveData() {}\n", encoding="utf-8")
            index = root / "out" / "persistence.tsv"
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = persistence_index.main(["--root", str(root), "--out", str(index)])

            self.assertEqual(code, 0)
            self.assertIn("src/save.cpp\t1\tpersistence_symbol\tSaveData", index.read_text(encoding="utf-8"))
            self.assertIn(f"index: {index.resolve()}", stdout.getvalue())

            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = persistence_index.main(["--root", str(root), "--out", str(source)])

            self.assertEqual(code, 2)
            self.assertIn("must not replace a scanned source file", stderr.getvalue())
            self.assertEqual(source.read_text(encoding="utf-8"), "void SaveData() {}\n")


class ChangeImpactTests(unittest.TestCase):
    def test_storage_and_tool_changes_propose_focused_checks(self) -> None:
        report = change_impact.recommendations_for_paths(
            ["src/file_output/file_output_stage.cpp", "tools/dev/persistence_index.py"]
        )

        self.assertIn("docs/internal/persistence_保存系現行実装整理方針_2026-04-29.md", report["read"])
        self.assertIn("python tools/dev/persistence_index.py --out out/persistence_index.tsv", report["inspect"])
        self.assertIn("python -m unittest tests/python/test_python_tools.py", report["run"])
        self.assertIn(
            "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_atomic_write_tests.ps1",
            report["run"],
        )

    def test_app_core_and_workspace_changes_propose_persistence_checks(self) -> None:
        report = change_impact.recommendations_for_paths(
            ["src/core/app_core.cpp", "src/main/workspace_actions.cppinc"]
        )

        self.assertIn("python tools/dev/persistence_index.py --out out/persistence_index.tsv", report["inspect"])
        self.assertIn(
            "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_fault_injection_tests.ps1",
            report["run"],
        )

    def test_timer_diff_proposes_timer_registry_inspection(self) -> None:
        report = change_impact.recommendations_for_paths(
            ["src/main/main_window_proc.cpp"], "+ SetTimer(hwnd, kTimerId, 10, nullptr);"
        )

        self.assertIn('rg -n "TimerId|SetTimer|WM_TIMER|KillTimer" src', report["inspect"])

    def test_markdown_timer_rule_text_does_not_trigger_timer_code_inspection(self) -> None:
        report = change_impact.recommendations_for_paths(
            ["AGENTS.md"], "+ Confirm SetTimer and WM_TIMER ownership."
        )

        self.assertIn("python tools/dev/md_structure_scanner.py . --index out/md_structure_index.tsv", report["inspect"])
        self.assertNotIn('rg -n "TimerId|SetTimer|WM_TIMER|KillTimer" src', report["inspect"])
        self.assertNotIn(
            "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_repo_checks.ps1",
            report["run"],
        )

    def test_libreoffice_changes_propose_runtime_gate(self) -> None:
        report = change_impact.recommendations_for_paths(
            ["third_party/libreoffice/custom_build/communication_free_options.input"]
        )

        self.assertIn(
            "python tools/release_checks/libreoffice_runtime_gate.py --image third_party/libreoffice/custom_runtime/instdir",
            report["run"],
        )
        self.assertIn(
            "python tools/release_checks/binary_scan.py --include third_party/libreoffice/custom_runtime/instdir/program",
            report["inspect"],
        )
        self.assertIn("python tools/release_checks/dependency_security_gate.py", report["run"])

    def test_parser_changes_propose_deterministic_fuzz_regression(self) -> None:
        report = change_impact.recommendations_for_paths(
            ["src/pdf_view/annotation_store.cppinc", "third_party/pdfium/VERSION"]
        )

        self.assertIn(
            "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_input_fuzz_regression_tests.ps1",
            report["run"],
        )
        self.assertIn("python tools/release_checks/dependency_security_gate.py", report["run"])

    def test_release_packaging_changes_propose_libreoffice_runtime_gate(self) -> None:
        report = change_impact.recommendations_for_paths(["scripts/release/pack_release.ps1"])

        self.assertIn(
            "python tools/release_checks/libreoffice_runtime_gate.py --image third_party/libreoffice/custom_runtime/instdir",
            report["run"],
        )


class ExportPublicSnapshotTests(unittest.TestCase):
    def test_default_policy_paths_resolve_to_current_internal_docs(self) -> None:
        args = export_public_snapshot.parse_args(["--dest", "unused"])
        for policy_path in (args.allowlist, args.gitignore_template):
            with self.subTest(policy=policy_path.name):
                self.assertEqual(policy_path.parent, REPO_ROOT / "docs" / "internal")
                self.assertTrue(policy_path.is_file(), f"Missing default policy: {policy_path}")

    def test_main_uses_relocated_default_gitignore_template(self) -> None:
        with repo_tempdir() as root:
            internal = root / "docs" / "internal"
            internal.mkdir(parents=True)
            allowlist = internal / "allowlist.txt"
            allowlist.write_text("README.md\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            self.track_current_files(root)
            dest = root.parent / f"{root.name}_public"

            with redirect_stdout(io.StringIO()):
                code = export_public_snapshot.main([
                    "--root", str(root), "--allowlist", str(allowlist), "--dest", str(dest),
                ])

            self.assertEqual(code, 0)
            self.assertEqual(
                (dest / ".gitignore").read_bytes(),
                export_public_snapshot.DEFAULT_GITIGNORE_TEMPLATE.read_bytes(),
            )
            self.assertEqual((dest / "README.md").read_text(encoding="utf-8"), "# Project\n")

    @staticmethod
    def track_current_files(root: Path) -> None:
        subprocess.run(["git", "init", "-q", str(root)], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(["git", "-C", str(root), "add", "--all"], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_main_uses_gui_destination_selection_by_default(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            with mock.patch.object(export_public_snapshot, "select_destination_via_gui", return_value=dest) as chooser:
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 0)
            chooser.assert_called_once_with()
            self.assertTrue((dest / "README.md").exists())

    def test_main_uses_cui_destination_selection_when_requested(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            with mock.patch.object(export_public_snapshot, "select_destination_via_cui", return_value=dest) as chooser:
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--select-dest", "cui",
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 0)
            chooser.assert_called_once_with()
            self.assertTrue((dest / "README.md").exists())

    def test_main_copies_allowlisted_files_and_generates_public_gitignore(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\nsrc/\ndocs/public/\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")

            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            (root / "src").mkdir()
            (root / "src" / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            (root / "src" / "__pycache__").mkdir()
            (root / "src" / "__pycache__" / "main.cpython-312.pyc").write_bytes(b"pyc")
            (root / "docs" / "public").mkdir(parents=True)
            (root / "docs" / "public" / "README.md").write_text("# Public\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--dest", str(dest),
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 0)
            self.assertEqual((dest / ".gitignore").read_text(encoding="utf-8"), "out/\n")
            self.assertEqual((dest / "README.md").read_text(encoding="utf-8"), "# Project\n")
            self.assertEqual((dest / "src" / "main.cpp").read_text(encoding="utf-8"), "int main() { return 0; }\n")
            self.assertEqual((dest / "docs" / "public" / "README.md").read_text(encoding="utf-8"), "# Public\n")
            self.assertFalse((dest / "src" / "__pycache__").exists())
            self.assertIn("files: 3", stdout.getvalue())

    def test_main_applies_repo_version_to_version_tracked_documents_only_in_snapshot(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("REPO_VERSION.txt\nREADME.md\nLICENSE.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "REPO_VERSION.txt").write_text("0.8.48\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n\n同梱リポジトリ版: (ZIP配布物ではここにバージョンが記載されます)\n", encoding="utf-8")
            (root / "LICENSE.md").write_text("# License\nVersion 1\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            code = export_public_snapshot.main(
                [
                    "--root", str(root),
                    "--dest", str(dest),
                    "--allowlist", str(allowlist),
                    "--gitignore-template", str(gitignore_template),
                ]
            )

            self.assertEqual(code, 0)
            self.assertIn("同梱リポジトリ版: (ZIP配布物ではここにバージョンが記載されます)", (dest / "README.md").read_text(encoding="utf-8"))
            self.assertIn("同梱リポジトリ版: (ZIP配布物ではここにバージョンが記載されます)", (root / "README.md").read_text(encoding="utf-8"))
            self.assertEqual((dest / "LICENSE.md").read_text(encoding="utf-8"), "# License\nVersion 1\n")

    def test_main_excludes_working_copy_artifacts_from_allowlisted_directory(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("third_party/\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            third_party = root / "third_party"
            third_party.mkdir()
            (third_party / "LICENSE.txt").write_text("license\n", encoding="utf-8")
            (third_party / "header.h.orig").write_text("backup\n", encoding="utf-8")
            (third_party / "patch.rej").write_text("reject\n", encoding="utf-8")
            (third_party / "copy.bak").write_text("backup\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            code = export_public_snapshot.main(
                [
                    "--root", str(root),
                    "--dest", str(dest),
                    "--allowlist", str(allowlist),
                    "--gitignore-template", str(gitignore_template),
                ]
            )

            self.assertEqual(code, 0)
            self.assertTrue((dest / "third_party" / "LICENSE.txt").exists())
            self.assertFalse((dest / "third_party" / "header.h.orig").exists())
            self.assertFalse((dest / "third_party" / "patch.rej").exists())
            self.assertFalse((dest / "third_party" / "copy.bak").exists())

    def test_main_refuses_destination_inside_repository_root(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            self.track_current_files(root)

            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--dest", str(root / "public"),
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 2)
            self.assertIn("destination must be outside the repository root", stderr.getvalue())
            self.assertFalse((root / "public").exists())

    def test_main_fails_before_writing_when_allowlist_entry_is_missing(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\nmissing.txt\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--dest", str(dest),
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 2)
            self.assertIn("allowlist path not found: missing.txt", stderr.getvalue())
            self.assertFalse(dest.exists())

    def test_main_refuses_non_empty_destination(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            dest.mkdir()
            (dest / "keep.txt").write_text("keep\n", encoding="utf-8")

            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--dest", str(dest),
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 2)
            self.assertIn("destination directory must be empty", stderr.getvalue())
            self.assertEqual((dest / "keep.txt").read_text(encoding="utf-8"), "keep\n")

    def test_main_rejects_dest_and_select_dest_combination(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")
            self.track_current_files(root)

            dest = root.parent / f"{root.name}_public"
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--dest", str(dest),
                        "--select-dest", "gui",
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 2)
            self.assertIn("use either --dest or --select-dest, not both", stderr.getvalue())
            self.assertFalse(dest.exists())

    def test_main_rejects_untracked_file_below_allowlisted_directory(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("src/\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            source = root / "src"
            source.mkdir()
            (source / "tracked.cpp").write_text("int tracked;\n", encoding="utf-8")
            self.track_current_files(root)
            (source / "private-note.txt").write_text("do not publish\n", encoding="utf-8")

            dest = root.parent / f"{root.name}_public"
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--dest", str(dest),
                        "--allowlist", str(allowlist),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 2)
            self.assertIn("not tracked by Git", stderr.getvalue())
            self.assertIn("src/private-note.txt", stderr.getvalue())
            self.assertFalse(dest.exists())

    def test_main_allows_only_hash_pinned_untracked_vendor_artifact(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            artifact_manifest = ops / "artifacts.tsv"
            allowlist.write_text("third_party/vendor/\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            vendor = root / "third_party" / "vendor"
            vendor.mkdir(parents=True)
            self.track_current_files(root)
            artifact = vendor / "runtime.bin"
            artifact.write_bytes(b"reviewed vendor runtime")
            artifact_manifest.write_text(
                f"{export_public_snapshot.sha256_file(artifact)}\tthird_party/vendor/runtime.bin\n",
                encoding="utf-8",
            )

            dest = root.parent / f"{root.name}_public"
            code = export_public_snapshot.main(
                [
                    "--root", str(root),
                    "--dest", str(dest),
                    "--allowlist", str(allowlist),
                    "--artifact-manifest", str(artifact_manifest),
                    "--gitignore-template", str(gitignore_template),
                ]
            )

            self.assertEqual(code, 0)
            self.assertEqual((dest / "third_party" / "vendor" / "runtime.bin").read_bytes(), artifact.read_bytes())

    def test_main_rejects_vendor_artifact_hash_change(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            artifact_manifest = ops / "artifacts.tsv"
            allowlist.write_text("third_party/vendor/\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            vendor = root / "third_party" / "vendor"
            vendor.mkdir(parents=True)
            self.track_current_files(root)
            artifact = vendor / "runtime.bin"
            artifact.write_bytes(b"changed vendor runtime")
            artifact_manifest.write_text(
                f"{'0' * 64}\tthird_party/vendor/runtime.bin\n",
                encoding="utf-8",
            )

            dest = root.parent / f"{root.name}_public"
            stderr = io.StringIO()
            with mock.patch.object(sys, "stderr", stderr):
                code = export_public_snapshot.main(
                    [
                        "--root", str(root),
                        "--dest", str(dest),
                        "--allowlist", str(allowlist),
                        "--artifact-manifest", str(artifact_manifest),
                        "--gitignore-template", str(gitignore_template),
                    ]
                )

            self.assertEqual(code, 2)
            self.assertIn("artifact hash mismatch", stderr.getvalue())
            self.assertFalse(dest.exists())


class BinaryScanTests(unittest.TestCase):
    def test_strict_pe_mode_rejects_malformed_executable(self) -> None:
        with repo_tempdir() as root:
            binary = root / "truncated.exe"
            binary.write_bytes(b"MZ")

            output = io.StringIO()
            with redirect_stdout(output):
                code = binary_scan.main(
                    [
                        "--root", str(root),
                        "--include", "truncated.exe",
                        "--imports-only",
                        "--fail-on-unparseable-pe",
                    ]
                )

            self.assertEqual(code, 1)
            self.assertIn("Invalid PE images", output.getvalue())

    def test_strict_pe_mode_allows_legacy_com_file(self) -> None:
        with repo_tempdir() as root:
            binary = root / "legacy.com"
            binary.write_bytes(b"not a PE image")

            with redirect_stdout(io.StringIO()):
                code = binary_scan.main(
                    [
                        "--root", str(root),
                        "--include", "legacy.com",
                        "--imports-only",
                        "--fail-on-unparseable-pe",
                    ]
                )

            self.assertEqual(code, 0)

    def test_imports_only_does_not_match_embedded_network_text(self) -> None:
        with repo_tempdir() as root:
            binary = root / "sample.exe"
            binary.write_bytes(b"http://example.invalid winhttp")

            finding = binary_scan.scan_file(binary, root, [], min_string=5, max_strings=10)

            self.assertEqual(finding.matched_strings, [])
            self.assertEqual(finding.matched_imports, [])

    def test_fail_on_import_returns_failure_for_prohibited_dll(self) -> None:
        with repo_tempdir() as root:
            binary = root / "sample.exe"
            binary.write_bytes(b"MZ")
            prohibited = binary_scan.BinaryFinding(
                path="sample.exe",
                size_bytes=2,
                import_dlls=["winhttp.dll"],
                import_symbols=[],
                matched_strings=[],
                matched_imports=[],
            )

            with mock.patch.object(binary_scan, "scan_file", return_value=prohibited):
                with redirect_stdout(io.StringIO()):
                    code = binary_scan.main(
                        [
                            "--root",
                            str(root),
                            "--include",
                            "sample.exe",
                            "--imports-only",
                            "--imported-dll",
                            "winhttp.dll",
                            "--fail-on-import",
                        ]
                    )

            self.assertEqual(code, 1)

    def test_strings_format_prints_recovered_strings_only(self) -> None:
        with repo_tempdir() as root:
            binary = root / "sample.exe"
            binary.write_bytes(b"visible-ascii\0hidden\0")
            output = io.StringIO()

            with redirect_stdout(output):
                code = binary_scan.main(
                    [
                        "--root",
                        str(root),
                        "--include",
                        "sample.exe",
                        "--all-strings",
                        "--format",
                        "strings",
                    ]
                )

            self.assertEqual(code, 0)
            self.assertEqual(output.getvalue().splitlines(), ["visible-ascii", "hidden"])


class LibreOfficeBuildEnvironmentTests(unittest.TestCase):
    def test_missing_empty_or_malformed_checksum_cannot_report_ready(self) -> None:
        with repo_tempdir() as root:
            archive = root / "source.tar.xz"
            archive.write_bytes(b"source")
            sidecar = root / "source.tar.xz.sha256"
            for content in (None, "", "not-a-checksum"):
                with self.subTest(content=content):
                    if content is not None:
                        sidecar.write_text(content, encoding="utf-8")
                    with mock.patch.object(libreoffice_build_env_check, "COMMANDS", []):
                        result = libreoffice_build_env_check.build_result(root, [archive.name])
                    self.assertFalse(result["summary"]["ready_for_first_build"])
                    self.assertEqual(result["summary"]["archive_failures"], [archive.name])

    def test_explicit_external_archive_and_verified_checksum(self) -> None:
        with repo_tempdir() as root:
            repo = root / "repo"
            repo.mkdir()
            archive = root / "source.tar.xz"
            archive.write_bytes(b"source")
            digest = hashlib.sha256(b"source").hexdigest()
            sidecar = root / "source.tar.xz.sha256"
            sidecar.write_text(f"{digest}  source.tar.xz\n", encoding="utf-8")
            with mock.patch.object(libreoffice_build_env_check, "COMMANDS", []):
                result = libreoffice_build_env_check.build_result(repo, [str(archive)])
            self.assertTrue(result["summary"]["ready_for_first_build"])
            self.assertEqual(result["source_archives"][0]["sha256_file"], str(sidecar))
            sidecar.write_text("0" * 64, encoding="utf-8")
            self.assertFalse(libreoffice_build_env_check.check_archive(repo, str(archive)).sha256_matches)

    def test_empty_source_selection_is_rejected(self) -> None:
        with repo_tempdir() as root, self.assertRaises(ValueError):
            libreoffice_build_env_check.build_result(root, [])


class LibreOfficeRuntimeGateTests(unittest.TestCase):
    def test_minimal_converter_without_prohibited_indicator_passes(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            (program / "soffice.com").write_bytes(pe_fixtures.make_pe())

            violations = libreoffice_runtime_gate.collect_violations(root / "image")

            self.assertEqual(violations, [])

    def test_known_communication_runtime_file_is_rejected(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            (program / "soffice.com").write_bytes(pe_fixtures.make_pe())
            (program / "libcurl.dll").write_bytes(pe_fixtures.make_pe())

            violations = libreoffice_runtime_gate.collect_violations(root / "image")

            self.assertTrue(
                any(item.kind == "prohibited-path" and item.path == "program/libcurl.dll" for item in violations)
            )

    def test_network_import_in_conversion_binary_is_rejected(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            soffice = program / "soffice.com"
            merged = program / "mergedlo.dll"
            soffice.write_bytes(pe_fixtures.make_pe())
            merged.write_bytes(pe_fixtures.make_pe())

            def fake_scan(path, _root, _queries, min_string, max_strings):
                imports = ["WINHTTP.dll"] if path == merged else []
                return binary_scan.BinaryFinding(
                    path=path.name,
                    size_bytes=path.stat().st_size,
                    import_dlls=imports,
                    import_symbols=[],
                    matched_strings=[],
                    matched_imports=[],
                    pe_status="valid",
                )

            with mock.patch.object(libreoffice_runtime_gate.binary_scan, "scan_file", side_effect=fake_scan):
                violations = libreoffice_runtime_gate.collect_violations(root / "image")

            self.assertTrue(
                any(item.kind == "prohibited-import" and item.evidence == "WINHTTP.dll" for item in violations)
            )

    def test_winmm_without_sound_api_symbol_is_allowed(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            soffice = program / "soffice.com"
            merged = program / "mergedlo.dll"
            soffice.write_bytes(pe_fixtures.make_pe())
            merged.write_bytes(pe_fixtures.make_pe())

            def fake_scan(path, _root, _queries, min_string, max_strings):
                imports = ["winmm.dll"] if path == merged else []
                return binary_scan.BinaryFinding(
                    path=path.name,
                    size_bytes=path.stat().st_size,
                    import_dlls=imports,
                    import_symbols=[],
                    matched_strings=[],
                    matched_imports=[],
                    pe_status="valid",
                )

            with mock.patch.object(libreoffice_runtime_gate.binary_scan, "scan_file", side_effect=fake_scan):
                violations = libreoffice_runtime_gate.collect_violations(root / "image")

            self.assertFalse(any(item.evidence == "winmm.dll" for item in violations))

    def test_sound_api_marker_is_rejected(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            soffice = program / "soffice.com"
            merged = program / "mergedlo.dll"
            soffice.write_bytes(pe_fixtures.make_pe())
            merged.write_bytes(pe_fixtures.make_pe())

            def fake_scan(path, _root, _queries, min_string, max_strings):
                markers = ["PlaySoundW"] if path == merged else []
                return binary_scan.BinaryFinding(
                    path=path.name,
                    size_bytes=path.stat().st_size,
                    import_dlls=[],
                    import_symbols=[],
                    matched_strings=[],
                    matched_imports=markers,
                    pe_status="valid",
                )

            with mock.patch.object(libreoffice_runtime_gate.binary_scan, "scan_file", side_effect=fake_scan):
                violations = libreoffice_runtime_gate.collect_violations(root / "image")

            self.assertTrue(
                any(item.kind == "prohibited-marker" and item.evidence == "PlaySoundW" for item in violations)
            )

    def test_online_update_channel_marker_is_rejected(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            (program / "soffice.com").write_bytes(pe_fixtures.make_pe())
            (program / "version.ini").write_text("UpdateChannel=LOOnlineUpdater\n", encoding="utf-8")

            violations = libreoffice_runtime_gate.collect_violations(root / "image")

            self.assertTrue(
                any(item.kind == "prohibited-marker" and item.path == "program/version.ini" for item in violations)
            )


class LibreOfficeReleaseRuntimeSanitizerTests(unittest.TestCase):
    def test_release_manifest_preserves_document_conversion_dependencies(self) -> None:
        manifest = json.loads(
            sanitize_libreoffice_runtime_release.DEFAULT_REDUCTION_MANIFEST.read_text(encoding="utf-8")
        )
        removed_paths = set(manifest["paths"])
        required_paths = {
            "program/analysislo.dll",
            "program/datelo.dll",
            "program/orcus-parser.dll",
            "program/orcus.dll",
            "program/pricinglo.dll",
            "program/scdlo.dll",
            "program/scfiltlo.dll",
            "program/sclo.dll",
            "program/scnlo.dll",
            "program/scuilo.dll",
            "program/smlo.dll",
            "program/storagefdlo.dll",
            "program/ucptdoc1lo.dll",
            "share/calc",
            "share/config/soffice.cfg/modules/scalc",
            "share/registry/calc.xcd",
        }

        self.assertTrue(required_paths.isdisjoint(removed_paths))
        self.assertTrue({"program/scuilo.dll", "share/config/soffice.cfg/modules/scalc", "share/xslt"} <= set(manifest["protected_paths"]))
        self.assertNotIn("share/config/soffice.cfg/modules/scalc/ui", removed_paths)
        self.assertNotIn("share/xslt", removed_paths)

    def test_removes_sdk_and_rewrites_local_build_paths(self) -> None:
        with repo_tempdir() as root:
            image = root / "image"
            program = image / "program"
            sdk = image / "sdk" / "lib"
            program.mkdir(parents=True)
            sdk.mkdir(parents=True)
            dll = program / "sample.dll"
            marker = b"C:/Users/localuser/lo/src/libreoffice-26.2.3.2/workdir/sample.cxx"
            version = program / "version.ini"
            dll.write_bytes(b"before\0" + marker + b"\0after")
            version.write_text(
                "ExtensionUpdateURL=https://updates.example.invalid/check\n"
                "UpdateURL=https://updates.example.invalid/app\n"
                "UpdateChannel=LOOnlineUpdater\n"
                "Vendor=localuser\n",
                encoding="utf-8",
            )
            (sdk / "unused.exp").write_bytes(b"C:\\Users\\localuser\\lo\\src\\libreoffice-26.2.3.2")

            result = sanitize_libreoffice_runtime_release.sanitize(image)

            self.assertFalse((image / "sdk").exists())
            self.assertGreaterEqual(result.files_changed, 2)
            self.assertGreaterEqual(result.replacements, 5)
            updated = dll.read_bytes()
            self.assertNotIn(b"C:/Users", updated)
            self.assertNotIn(b"localuser", updated.lower())
            self.assertIn(b"/workdir/sample.cxx", updated)
            self.assertEqual(
                version.read_text(encoding="utf-8"),
                "ExtensionUpdateURL=\nUpdateURL=\nUpdateChannel=\nVendor=PDF Note Workspace\n",
            )

    def test_remaining_sensitive_path_fails(self) -> None:
        with repo_tempdir() as root:
            image = root / "image"
            program = image / "program"
            program.mkdir(parents=True)
            (program / "sample.dll").write_bytes(b"C:/Users/localuser/other/path")

            with self.assertRaises(RuntimeError):
                sanitize_libreoffice_runtime_release.sanitize(image)

    def test_manifest_removes_explicit_paths_and_globs(self) -> None:
        with repo_tempdir() as root:
            image = root / "image"
            (image / "program").mkdir(parents=True)
            (image / "share" / "config").mkdir(parents=True)
            (image / "program" / "soffice.com").write_bytes(b"keep")
            (image / "sdk" / "lib").mkdir(parents=True)
            (image / "sdk" / "lib" / "unused.lib").write_bytes(b"sdk")
            (image / "share" / "config" / "images_test.zip").write_bytes(b"icons")
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "paths": ["sdk"],
                        "globs": ["share/config/images_*.zip"],
                        "protected_paths": ["program/soffice.com"],
                    }
                ),
                encoding="utf-8",
            )

            result = sanitize_libreoffice_runtime_release.sanitize(image, manifest_path=manifest)

            self.assertEqual(result.removed_bytes, 8)
            self.assertFalse((image / "sdk").exists())
            self.assertFalse((image / "share" / "config" / "images_test.zip").exists())
            self.assertTrue((image / "program" / "soffice.com").exists())

    def test_manifest_cannot_remove_parent_of_protected_path(self) -> None:
        with repo_tempdir() as root:
            image = root / "image"
            (image / "program").mkdir(parents=True)
            (image / "program" / "soffice.com").write_bytes(b"keep")
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "paths": ["program"],
                        "globs": [],
                        "protected_paths": ["program/soffice.com"],
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaises(ValueError):
                sanitize_libreoffice_runtime_release.sanitize(image, manifest_path=manifest)

            self.assertTrue((image / "program" / "soffice.com").exists())


    def test_manifest_cannot_remove_descendant_of_protected_directory(self) -> None:
        for field in ("paths", "globs"):
            with self.subTest(field=field), repo_tempdir() as root:
                image = root / "image"
                protected_dir = image / "share/calc"
                protected_dir.mkdir(parents=True)
                data = protected_dir / "needed.xml"
                data.write_bytes(b"keep")
                manifest = root / "manifest.json"
                payload = {"version": 1, "paths": [], "globs": [], "protected_paths": ["share/calc"]}
                payload[field] = ["share/calc/needed.xml" if field == "paths" else "share/calc/*.xml"]
                manifest.write_text(json.dumps(payload), encoding="utf-8")
                with self.assertRaises(ValueError):
                    sanitize_libreoffice_runtime_release.sanitize(image, manifest_path=manifest)
                self.assertEqual(data.read_bytes(), b"keep")


class LibreOfficeReduceToolTests(unittest.TestCase):
    def collect(self, image_root: Path, **overrides):
        kwargs = {
            "include_phase1": False,
            "include_cache": False,
            "include_conversion_only": False,
            "include_headless_only": False,
            "include_templates": False,
            "include_authoring_data": False,
            "include_ui_locales_ja_en": False,
            "include_stale_registry": False,
            "include_dictionaries_ja_en": False,
            "include_program_resources_ja_en": False,
            "include_scripting_runtime": False,
            "include_ui_icon_themes": False,
            "include_database_java": False,
            "include_calc": False,
            "include_nonconversion_leftovers": False,
        }
        kwargs.update(overrides)
        return libreoffice_reduce.collect_items(image_root, **kwargs)

    def test_calc_removal_option_is_rejected(self) -> None:
        with mock.patch.object(sys, "argv", ["libreoffice_reduce.py", "--calc"]):
            with self.assertRaises(SystemExit) as raised:
                libreoffice_reduce.parse_args()

        self.assertEqual(raised.exception.code, 2)

    def test_phase1_removes_root_msi_and_update_send_entries(self) -> None:
        with repo_tempdir() as root:
            image = root / "image"
            (image / "program").mkdir(parents=True)
            (image / "share" / "registry").mkdir(parents=True)
            (image / "LibreOffice_26.2.3_Win_x86-64.msi").write_text("msi", encoding="utf-8")
            (image / "program" / "updater.exe").write_text("updater", encoding="utf-8")
            (image / "program" / "senddoc.exe").write_text("senddoc", encoding="utf-8")
            (image / "update-settings.ini").write_text("update", encoding="utf-8")
            (image / "share" / "registry" / "onlineupdate.xcd").write_text("online", encoding="utf-8")

            rels = {item.rel for item in self.collect(image, include_phase1=True)}

            self.assertIn("LibreOffice_26.2.3_Win_x86-64.msi", rels)
            self.assertIn("program/updater.exe", rels)
            self.assertIn("program/senddoc.exe", rels)
            self.assertIn("update-settings.ini", rels)
            self.assertIn("share/registry/onlineupdate.xcd", rels)

    def test_scripting_runtime_removes_versioned_python_core_directory(self) -> None:
        with repo_tempdir() as root:
            image = root / "image"
            (image / "program" / "python-core-3.12.13").mkdir(parents=True)
            (image / "program" / "python-core-3.12.13" / "python.exe").write_text("py", encoding="utf-8")

            rels = {item.rel for item in self.collect(image, include_scripting_runtime=True)}

            self.assertEqual(rels, {"program/python-core-3.12.13"})

    def test_parent_removal_suppresses_child_removal(self) -> None:
        with repo_tempdir() as root:
            image = root / "image"
            (image / "program" / "classes").mkdir(parents=True)
            (image / "program" / "classes" / "java_websocket.jar").write_text("jar", encoding="utf-8")

            rels = {item.rel for item in self.collect(image, include_phase1=True, include_database_java=True)}

            self.assertEqual(rels, {"program/classes"})


class LibreOfficeRuntimeAnalyzerTests(unittest.TestCase):
    def create_runtime(self, root: Path) -> Path:
        runtime = root / "runtime"
        program = runtime / "program"
        program.mkdir(parents=True)
        for name in libreoffice_runtime_analyzer.DEFAULT_REQUIRED_PATHS:
            path = runtime / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(pe_fixtures.make_pe() if path.suffix.lower() in libreoffice_runtime_analyzer.DEPENDENCY_BINARY_SUFFIXES
                             else (name + "\n").encode("utf-8"))
        (runtime / "share" / "template").mkdir(parents=True)
        (runtime / "share" / "template" / "sample.ott").write_bytes(b"template")
        return runtime

    def test_analyze_runtime_reports_capacity_and_required_paths(self) -> None:
        with repo_tempdir() as root:
            runtime = self.create_runtime(root)

            report = libreoffice_runtime_analyzer.analyze_runtime(runtime, hashes=True, largest=3)

            self.assertEqual(report["integrity"]["status"], "ok")
            self.assertEqual(report["inventory"]["summary"]["files"], 6)
            self.assertEqual(len(report["inventory"]["largest_files"]), 3)
            self.assertTrue(all("sha256" in item for item in report["inventory"]["files"]))

    def test_dependency_graph_traverses_soffice_bin_imports(self) -> None:
        with repo_tempdir() as root:
            runtime = self.create_runtime(root)
            program = runtime / "program"
            (program / "soffice.bin").write_bytes(b"entry")
            (program / "sal3.dll").write_bytes(b"dependency")
            (program / "unused.dll").write_bytes(b"unused")

            def fake_parse_pe_imports(data: bytes) -> tuple[list[str], list[str]]:
                return (["sal3.dll"], []) if data == b"entry" else ([], [])

            with mock.patch.object(
                libreoffice_runtime_analyzer.binary_scan,
                "parse_pe_imports",
                side_effect=fake_parse_pe_imports,
            ):
                report = libreoffice_runtime_analyzer.analyze_runtime(runtime)

            dependencies = report["dependencies"]
            self.assertEqual(dependencies["summary"]["binaries"], 4)
            self.assertIn("program/sal3.dll", dependencies["reachable_paths"])
            self.assertIn(
                {"from": "program/soffice.bin", "to": "program/sal3.dll", "import": "sal3.dll"},
                dependencies["local_edges"],
            )
            self.assertEqual(dependencies["static_unreachable_candidates"], ["program/unused.dll"])

    def test_analyze_runtime_fails_when_required_entry_is_missing(self) -> None:
        with repo_tempdir() as root:
            runtime = self.create_runtime(root)
            (runtime / "program" / "soffice.com").unlink()

            report = libreoffice_runtime_analyzer.analyze_runtime(runtime)

            self.assertEqual(report["integrity"]["status"], "failed")
            self.assertIn(
                "missing-required-path",
                {item["kind"] for item in report["integrity"]["errors"]},
            )

    def test_compare_with_hashes_detects_same_size_content_change(self) -> None:
        with repo_tempdir() as root:
            baseline = self.create_runtime(root)
            candidate = root / "candidate"
            shutil.copytree(baseline, candidate)
            target = candidate / "share" / "template" / "sample.ott"
            target.write_bytes(b"Template")

            report = libreoffice_runtime_analyzer.compare_runtimes(baseline, candidate, hashes=True)

            self.assertEqual(report["summary"]["bytes_delta"], 0)
            self.assertEqual(report["summary"]["changed_files"], 1)
            self.assertEqual(report["changed"][0]["path"], "share/template/sample.ott")

    def test_compare_reports_reduction_bytes_and_percent(self) -> None:
        with repo_tempdir() as root:
            baseline = self.create_runtime(root)
            candidate = root / "candidate"
            shutil.copytree(baseline, candidate)
            (candidate / "share" / "template" / "sample.ott").unlink()

            report = libreoffice_runtime_analyzer.compare_runtimes(baseline, candidate)
            summary = report["summary"]

            self.assertEqual(summary["reduction_bytes"], len(b"template"))
            self.assertAlmostEqual(
                summary["reduction_percent"],
                summary["reduction_bytes"] * 100 / summary["baseline_bytes"],
            )

    def test_report_writer_refuses_to_modify_analyzed_runtime(self) -> None:
        with repo_tempdir() as root:
            runtime = self.create_runtime(root)

            with self.assertRaises(ValueError):
                libreoffice_runtime_analyzer.write_json_atomic(
                    runtime / "analysis.json",
                    {"ok": True},
                    [runtime],
                )


class LibreOfficeRuntimeDynamicProbeTests(unittest.TestCase):
    def test_inventory_contains_only_runtime_binaries(self) -> None:
        with repo_tempdir() as root:
            (root / "program").mkdir()
            (root / "program" / "writer.dll").write_bytes(b"dll")
            (root / "program" / "soffice.com").write_bytes(b"com")
            (root / "program" / "readme.txt").write_text("text", encoding="utf-8")

            inventory = libreoffice_runtime_dynamic_probe.runtime_binary_inventory(root)

            self.assertEqual(
                inventory,
                {"program/writer.dll": 3, "program/soffice.com": 3},
            )

    def test_is_inside_accepts_child_and_rejects_sibling(self) -> None:
        with repo_tempdir() as root:
            runtime = root / "runtime"
            runtime.mkdir()

            self.assertTrue(
                libreoffice_runtime_dynamic_probe.is_inside(runtime / "program" / "x.dll", runtime)
            )
            self.assertFalse(
                libreoffice_runtime_dynamic_probe.is_inside(root / "runtime-other" / "x.dll", runtime)
            )


class LibreOfficeRuntimeRemovalTrialTests(unittest.TestCase):
    def test_collect_removals_normalizes_and_deduplicates(self) -> None:
        removals = libreoffice_runtime_removal_trial.collect_removals(
            ["program\\unused.dll", "program/unused.dll", "# comment", ""],
            None,
        )

        self.assertEqual(removals, ["program/unused.dll"])

    def test_collect_removals_rejects_escape_and_protected_parent(self) -> None:
        with self.assertRaises(ValueError):
            libreoffice_runtime_removal_trial.collect_removals(["../outside.dll"], None)
        with self.assertRaises(ValueError):
            libreoffice_runtime_removal_trial.collect_removals(["program"], None)

    def test_compare_quality_detects_page_metric_change(self) -> None:
        baseline = {
            "summary": {},
            "results": [
                {
                    "relative_office_file": "sample.docx",
                    "pages": [{"difference_ratio": 0.0}],
                }
            ],
        }
        candidate = json.loads(json.dumps(baseline))
        candidate["results"][0]["pages"][0]["difference_ratio"] = 0.01

        differences = libreoffice_runtime_removal_trial.compare_quality(baseline, candidate)

        self.assertEqual(len(differences), 1)
        self.assertEqual(differences[0]["field"], "difference_ratio")

    def test_compare_quality_detects_current_page_dimension_change(self) -> None:
        baseline = {
            "summary": {},
            "results": [
                {
                    "relative_office_file": "sample.docx",
                    "pages": [{"candidate_pixels": [100, 200]}],
                }
            ],
        }
        candidate = json.loads(json.dumps(baseline))
        candidate["results"][0]["pages"][0]["candidate_pixels"] = [101, 200]

        differences = libreoffice_runtime_removal_trial.compare_quality(baseline, candidate)

        self.assertEqual(len(differences), 1)
        self.assertEqual(differences[0]["field"], "candidate_pixels")


class LibreOfficeSmokeTestToolTests(unittest.TestCase):
    def test_docx_space_token_protection_keeps_space_and_joins_following_token(self) -> None:
        text = "神経診断学実習　テーマ２小脳機能　小テスト"

        protected = libreoffice_smoke_test.transform_docx_text_for_space_protection(
            text, "word-joiner-token-after-space"
        )

        self.assertIn("実習　テ\u2060ー\u2060マ\u2060２\u2060小\u2060脳\u2060機\u2060能", protected)
        self.assertIn("能　小\u2060テ\u2060ス\u2060ト", protected)
        self.assertNotIn("\u00a0", protected)

    def test_docx_space_after_space_mode_does_not_change_token_internals(self) -> None:
        text = "機能　小テスト"

        protected = libreoffice_smoke_test.transform_docx_text_for_space_protection(
            text, "word-joiner-after-space"
        )

        self.assertEqual(protected, "機能　\u2060小テスト")

    def test_docx_space_protection_writes_only_staged_copy(self) -> None:
        with repo_tempdir() as root:
            source = root / "sample.docx"
            staged = root / "staged.docx"
            document = (
                '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                "<w:body><w:p><w:r><w:t>神経診断学実習　小テスト</w:t></w:r></w:p></w:body>"
                "</w:document>"
            )
            with zipfile.ZipFile(source, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("word/document.xml", document.encode("utf-8"))

            libreoffice_smoke_test.transform_docx_for_space_protection(
                source, staged, "word-joiner-token-after-space"
            )

            with zipfile.ZipFile(source, "r") as archive:
                original_document = archive.read("word/document.xml").decode("utf-8")
            with zipfile.ZipFile(staged, "r") as archive:
                staged_document = archive.read("word/document.xml").decode("utf-8")

            self.assertNotIn("\u2060", original_document)
            self.assertIn("神経診断学実習　小\u2060テ\u2060ス\u2060ト", staged_document)


class LibreOfficeConversionQualityToolTests(unittest.TestCase):
    def test_discover_pairs_matches_same_stem_and_reports_missing_reference(self) -> None:
        with repo_tempdir() as root:
            paired = root / "paired.docx"
            paired.write_bytes(b"docx")
            (root / "paired.pdf").write_bytes(b"pdf")
            missing = root / "missing.pptx"
            missing.write_bytes(b"pptx")

            pairs, missing_references = libreoffice_conversion_quality_test.discover_pairs(root)

            self.assertEqual(pairs, [(paired, root / "paired.pdf")])
            self.assertEqual(missing_references, [missing])

    def test_difference_metrics_detects_identical_and_changed_pixels(self) -> None:
        image_type = libreoffice_conversion_quality_test.Image
        reference = image_type.new("RGB", (2, 2), "white")
        candidate = reference.copy()

        identical = libreoffice_conversion_quality_test.difference_metrics(reference, candidate, threshold=8)
        candidate.putpixel((1, 1), (0, 0, 0))
        changed = libreoffice_conversion_quality_test.difference_metrics(reference, candidate, threshold=8)

        self.assertEqual(identical["difference_ratio"], 0.0)
        self.assertEqual(changed["different_pixels"], 1)
        self.assertEqual(changed["difference_ratio"], 0.25)

    def test_difference_metrics_pads_one_pixel_size_difference(self) -> None:
        image_type = libreoffice_conversion_quality_test.Image
        reference = image_type.new("RGB", (2, 2), "white")
        candidate = image_type.new("RGB", (3, 2), "white")

        result = libreoffice_conversion_quality_test.difference_metrics(reference, candidate, threshold=8)

        self.assertFalse(result["same_dimensions"])
        self.assertEqual(result["comparison_pixels"], [3, 2])
        self.assertEqual(result["difference_ratio"], 0.0)

    def test_pdf_subset_prefix_is_removed_before_font_comparison(self) -> None:
        self.assertEqual(
            libreoffice_conversion_quality_test.normalize_pdf_font_name("BCDEEE+YuGothic-Regular"),
            "YuGothic-Regular",
        )

    def test_rendered_image_count_ignores_unused_shared_resources(self) -> None:
        class PageWithSharedResources:
            def get_image_info(self, *, xrefs: bool):
                self.requested_xrefs = xrefs
                return [{"xref": 10}, {"xref": 20}]

            def get_images(self, *, full: bool):
                raise AssertionError("resource dictionary entries must not be counted")

        page = PageWithSharedResources()

        self.assertEqual(libreoffice_conversion_quality_test.count_rendered_images(page), 2)
        self.assertTrue(page.requested_xrefs)

    def test_output_inside_fixture_directory_is_rejected(self) -> None:
        with repo_tempdir() as root:
            fixtures = root / "fixtures"
            fixtures.mkdir()

            with self.assertRaises(ValueError):
                libreoffice_conversion_quality_test.ensure_output_outside_inputs(fixtures / "output", fixtures)


class RenderHumanDocsTests(unittest.TestCase):
    def test_fenced_examples_keep_nested_shorter_fences_literal(self) -> None:
        rendered = render_human_docs.simple_markdown_to_html(
            "````markdown\n```text\n**literal**\n```\n````\n"
            "~~~text\n```\n[example](missing.md)\n~~~\n"
            "## After examples\n", ".",
        )
        self.assertIn("<pre><code>```text\n**literal**\n```</code></pre>", rendered)
        self.assertIn("<pre><code>```\n[example](missing.md)</code></pre>", rendered)
        self.assertIn('<h2 id="after-examples">After examples</h2>', rendered)
        self.assertNotIn("<a ", rendered)
        self.assertEqual(
            render_human_docs.simple_markdown_to_html("```text\n<unclosed>\n", "."),
            "<pre><code>&lt;unclosed&gt;</code></pre>",
        )

    def test_table_delimiters_do_not_remove_syntax_examples(self) -> None:
        rendered = render_human_docs.simple_markdown_to_html(
            "| Syntax | Example |\n| :--- | ---: |\n"
            "| Rule | `---` |\n| Table | Header then `---` |\n"
            "| Dashes | ---- |\n",
            ".",
        )
        self.assertEqual(rendered.count("<tr>"), 4)
        self.assertIn("<td>Rule</td><td><code>---</code></td>", rendered)
        self.assertIn("<td>Table</td><td>Header then <code>---</code></td>", rendered)
        self.assertIn("<td>Dashes</td><td>----</td>", rendered)

    def test_inline_code_preserves_markdown_and_backtick_examples(self) -> None:
        examples = {
            "`**bold** [guide](guide.md) ![image](image.png) <u>&amp;</u>`":
                "<code>**bold** [guide](guide.md) ![image](image.png) &lt;u&gt;&amp;amp;&lt;/u&gt;</code>",
            "`` `code` ``": "<code>`code`</code>",
            "`` ```text `` and `` ``` ``": "<code>```text</code> and <code>```</code>",
            "``a`b``": "<code>a`b</code>",
            "`  `": "<code>  </code>",
            "`unclosed": "`unclosed",
            "[`.clro` Note Format](guide.md)": '<a href="guide.html"><code>.clro</code> Note Format</a>',
            "**Bold `**literal**`**": '<strong>Bold <code>**literal**</code></strong>',
            "![`code` <label>](image.png)":
                '<img src="image.png" alt="code &lt;label&gt;"><br><em>code &lt;label&gt;</em>',
            "\x00code0\x00 `literal`": "\x00code0\x00 <code>literal</code>",
            "**Bold** `**literal**` [guide](guide.md)":
                '<strong>Bold</strong> <code>**literal**</code> <a href="guide.html">guide</a>',
        }
        for source, expected in examples.items():
            with self.subTest(source=source):
                self.assertEqual(render_human_docs.format_inline(source, "."), expected)

    def test_renders_numbered_steps_and_standalone_captions(self) -> None:
        rendered = render_human_docs.simple_markdown_to_html(
            "# Guide\n\n*（画面の説明）*\n\n1. Open `file*name.pdf`.\n\n"
            "2. **Save** the work.\n- Read the result.\n"
            "3. Continue.\n```\n4. Keep this as code.\n```\n", "."
        )
        self.assertIn("<p><em>（画面の説明）</em></p>", rendered)
        self.assertIn("<ol>\n<li>Open <code>file*name.pdf</code>.</li>\n<li><strong>Save</strong> the work.</li>\n</ol>", rendered)
        self.assertIn("<ul>\n<li>Read the result.</li>\n</ul>", rendered)
        self.assertIn('<ol start="3">\n<li>Continue.</li>\n</ol>', rendered)
        self.assertIn("<pre><code>4. Keep this as code.</code></pre>", rendered)
        unsupported_number = "9" * 5000 + ". Keep the source text."
        self.assertEqual(
            render_human_docs.simple_markdown_to_html(unsupported_number, "."),
            f"<p>{unsupported_number}</p>",
        )

    def test_mermaid_labels_only_allow_exact_line_break_tags(self) -> None:
        rendered = render_human_docs.render_mermaid_flowchart(
            [
                "flowchart LR",
                "A[One<BR />Two] --> B[<br onclick=alert(1)>]",
                "B -. optional label .-> C[End]",
            ]
        )

        self.assertIsNotNone(rendered)
        self.assertIn("One<br>Two", rendered)
        self.assertIn("&lt;br onclick=alert(1)&gt;", rendered)
        self.assertIn('class="flowchart-branch">optional label</span>', rendered)
        self.assertIn(">End</span>", rendered)

    def test_renders_source_readable_note_and_details_containers(self) -> None:
        rendered = render_human_docs.simple_markdown_to_html(
            "::: note\nKeep this visible.\n:::\n\n"
            "::: details Optional explanation\nOpen this only when needed.\n:::\n\n"
            "```text\n::: note\nThis remains code.\n:::\n```\n",
            ".",
        )
        self.assertIn('<aside class="doc-note" role="note">\n<p>Keep this visible.</p>\n</aside>', rendered)
        self.assertIn('<details class="doc-details"><summary>Optional explanation</summary>\n<p>Open this only when needed.</p>\n</details>', rendered)
        self.assertIn('<pre><code>::: note\nThis remains code.\n:::</code></pre>', rendered)

    def test_wraps_tables_for_horizontal_scrolling_without_splitting_code_words(self) -> None:
        rendered = render_human_docs.simple_markdown_to_html(
            "| Application | Role |\n| --- | --- |\n| `PDF Note Workspace` | Main app |\n",
            ".",
        )
        self.assertIn('<div class="table-scroll"><table>', rendered)
        self.assertIn('<td><code>PDF Note Workspace</code></td>', rendered)
        self.assertTrue(rendered.endswith("</table></div>"))

    def test_collapses_only_the_long_lookup_references_and_keeps_heading_ids(self) -> None:
        body = '<h1 id="help">Help</h1>\n<h2 id="start">Start</h2>\n<p>First.</p>\n<h2 id="output">Output</h2>\n<p>Second.</p>'
        rendered = render_human_docs.collapse_reference_sections(
            body, Path("docs/ja/Help_Reference.md")
        )
        self.assertIn('<details class="doc-reference-section" open><summary id="start" role="heading" aria-level="2">Start</summary>', rendered)
        self.assertIn('<details class="doc-reference-section"><summary id="output" role="heading" aria-level="2">Output</summary>', rendered)
        self.assertNotIn('<h2 id="output">', rendered)
        self.assertEqual(
            render_human_docs.collapse_reference_sections(body, Path("docs/ja/Using_the_App.md")),
            body,
        )

    def test_generates_html_for_language_docs_including_introduction(self) -> None:
        with repo_tempdir() as site_dir:
            readme = site_dir / "README.md"
            readme.write_text("# Project README\n\nSome text.", encoding="utf-8")

            doc_dir = site_dir / "docs" / "ja"
            doc_dir.mkdir(parents=True)
            use_doc = doc_dir / "How_to_Use.md"
            use_doc.write_text(
                "# How to Use\n\n## Section One\n\nUsage steps.\n\n"
                "```mermaid\nflowchart LR\nA[Download] --> B[Extract]\nB --> C[Start]\n```\n\n## Section One",
                encoding="utf-8",
            )
            english_doc = site_dir / "docs" / "en" / "How_to_Use.md"
            english_doc.parent.mkdir(parents=True)
            english_doc.write_text("# How to Use\n\nUsage steps.", encoding="utf-8")

            introduction = site_dir / "introduction" / "index.md"
            introduction.parent.mkdir()
            introduction.write_text("# Introduction\n\nPortal rules.", encoding="utf-8")

            code = render_human_docs.main([str(site_dir)])
            self.assertEqual(code, 0)

            # 人間用ドキュメントの .html が作られていること
            self.assertTrue((site_dir / "README.html").exists())
            self.assertTrue((site_dir / "docs" / "ja" / "How_to_Use.html").exists())
            human_html = (site_dir / "docs" / "ja" / "How_to_Use.html").read_text(encoding="utf-8")
            self.assertIn('class="site-menu"', human_html)
            self.assertIn('class="contrast-toggle"', human_html)
            self.assertIn('pdf-note-workspace-high-contrast', human_html)
            self.assertIn('GitHub リポジトリ', human_html)
            self.assertIn('導入・操作・保存・トラブル対処', human_html)
            self.assertIn('aria-current="page"', human_html)
            self.assertIn('class="menu-current-label">（現在の文書）</span>', human_html)
            self.assertIn('<h1 id="how-to-use">How to Use</h1>', human_html)
            self.assertIn('<h2 id="section-one">Section One</h2>', human_html)
            self.assertIn('<h2 id="section-one-1">Section One</h2>', human_html)
            self.assertIn('<details class="document-outline"><summary>この文書の項目</summary>', human_html)
            self.assertIn('class="flowchart-diagram"', human_html)
            self.assertIn('>Download</span>', human_html)
            self.assertIn('>Extract</span>', human_html)
            self.assertNotIn('>B</span>', human_html)
            self.assertNotIn('flowchart LR', human_html)
            self.assertIn('@media (max-width: 560px)', human_html)
            self.assertIn('class="menu-outside"', human_html)
            self.assertIn('content: "↗"', human_html)
            self.assertIn('class="menu-icon" aria-hidden="true"><span class="menu-icon-bar"></span><span class="menu-icon-bar"></span><span class="menu-icon-bar"></span>', human_html)
            self.assertIn('.menu-icon-bar {\n      display: block;\n      width: 100%;\n      height: 2px;', human_html)
            self.assertLess(human_html.index('class="header-tools"'), human_html.index('class="site-menu"'))
            self.assertIn('目的別の入口へ戻る', human_html)
            self.assertLess(human_html.index('文書ポータルのトップ'), human_html.index('プロジェクトの概要'))
            self.assertLess(human_html.index('プロジェクトの概要'), human_html.index('日本語の文書'))
            self.assertIn('class="language-switch" href="../../docs/en/How_to_Use.html"', human_html)
            self.assertIn('>English</a>', human_html)
            self.assertNotIn('日本語</span><span aria-hidden="true">/</span>', human_html)
            self.assertNotIn('☰', human_html)
            # 生の .md も残っていること
            self.assertTrue(readme.exists())
            self.assertTrue(use_doc.exists())
            self.assertTrue(english_doc.exists())
            # 追加の説明資料もブラウザ用HTMLを生成し、生の .md は残すこと
            self.assertTrue((site_dir / "introduction" / "index.html").exists())
            self.assertTrue(introduction.exists())
            introduction_html = (site_dir / "introduction" / "index.html").read_text(encoding="utf-8")
            self.assertIn('class="site-menu"', introduction_html)
            self.assertNotIn('Raw Markdown', introduction_html)
            self.assertIn('class="menu-current-label">（現在の文書）</span>', introduction_html)
            self.assertIn('背景・設計・確認資料', introduction_html)
            self.assertIn('<h1 id="introduction">Introduction</h1>', introduction_html)
            self.assertNotIn('📄', introduction_html)


class PublicDocumentationStructureTests(unittest.TestCase):
    def test_introduction_tree_is_in_the_public_snapshot_allowlist(self) -> None:
        allowlist = (
            REPO_ROOT / "docs/internal/public_repo_release_allowlist_2026-08-24.txt"
        ).read_text(encoding="utf-8-sig")

        self.assertIn("\nintroduction/\n", f"\n{allowlist}")
        self.assertNotIn("\nllms.txt\n", f"\n{allowlist}")

    def test_locale_build_inputs_are_in_the_public_snapshot_allowlist(self) -> None:
        allowlist = (
            REPO_ROOT / "docs/internal/public_repo_release_allowlist_2026-08-24.txt"
        ).read_text(encoding="utf-8-sig")

        for required in ("locales/", "tools/localization/", "docs/ja/", "docs/en/"):
            self.assertIn(f"\n{required}\n", f"\n{allowlist}")

    def test_introduction_index_links_every_human_readable_detail(self) -> None:
        index = (REPO_ROOT / "introduction" / "index.md").read_text(encoding="utf-8-sig")
        details = [REPO_ROOT / "introduction" / "project_overview.md"]
        details.extend(sorted((REPO_ROOT / "introduction" / "core").glob("*.md")))

        for document in details:
            relative = document.relative_to(REPO_ROOT / "introduction").as_posix()
            self.assertIn(f"]({relative})", index, document.name)


class PublicSnapshotContentGateTests(unittest.TestCase):
    def test_accepts_reviewed_public_source_tree(self) -> None:
        with repo_tempdir() as root:
            (root / "README.md").write_text("# Public snapshot\n", encoding="utf-8")
            source = root / "src"
            source.mkdir()
            (source / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")

            self.assertEqual(public_snapshot_content_gate.collect_violations(root), [])

    def test_rejects_private_paths_secrets_logs_and_internal_files(self) -> None:
        with repo_tempdir() as root:
            (root / "README.md").write_text(
                "<!-- DEVELOPMENT_" + "HISTORY:START -->\n開発側の累積" + "コミット数: 1\n",
                encoding="utf-8",
            )
            build = root / "third_party" / "libreoffice" / "custom_build"
            build.mkdir(parents=True)
            (build / "autogen.input").write_text(
                "PYTHON=C:/" + "Users/private-person/Python/python.exe\n"
                "WORK=D:/" + "global_develop/lo\n",
                encoding="utf-8",
            )
            internal = root / "docs" / "internal"
            internal.mkdir(parents=True)
            (internal / "notes.md").write_text("internal\n", encoding="utf-8")
            (root / "build.log").write_text("log\n", encoding="utf-8")
            (root / "token.txt").write_text("github_pat_" + "A" * 30, encoding="utf-8")

            violations = public_snapshot_content_gate.collect_violations(root)
            kinds = {item.kind for item in violations}
            self.assertIn("private-development-history", kinds)
            self.assertIn("private-user-path", kinds)
            self.assertIn("private-workspace-path", kinds)
            self.assertIn("forbidden-path", kinds)
            self.assertIn("forbidden-file-type", kinds)
            self.assertIn("github-token", kinds)

    def test_allows_explicit_test_user_placeholder(self) -> None:
        with repo_tempdir() as root:
            (root / "fixture.txt").write_text(
                "C:/Users/localuser/example/input.pdf\n",
                encoding="utf-8",
            )

            self.assertEqual(public_snapshot_content_gate.collect_violations(root), [])

    def test_rejects_python_runtime_cache(self) -> None:
        with repo_tempdir() as root:
            cache = root / "site" / "github" / "scripts" / "__pycache__"
            cache.mkdir(parents=True)
            (cache / "render_human_docs.cpython-312.pyc").write_bytes(b"cache")

            violations = public_snapshot_content_gate.collect_violations(root)
            self.assertIn(
                public_snapshot_content_gate.Violation(
                    "forbidden-file-type",
                    "site/github/scripts/__pycache__/render_human_docs.cpython-312.pyc",
                ),
                violations,
            )


class PublicSiteValidationTests(unittest.TestCase):
    def test_documentation_portal_source_maps_user_guides(self) -> None:
        portal = (REPO_ROOT / "site/github/index.html").read_text(encoding="utf-8-sig")
        self.assertIn("初めて使う・配布物を選ぶ", portal)
        self.assertIn("起動や保存で困っている", portal)
        for group in ("導入・更新", "資料を読む・記録する", "保存・復元・問題への対処", "ファイルと記法"):
            self.assertIn(f"<h2>{group}</h2>", portal)
        self.assertIn('class="site-menu"', portal)
        self.assertIn('aria-current="page"><span class="menu-link-title">文書ポータルのトップ', portal)
        self.assertIn('class="menu-outside"', portal)
        for guide in (
            "docs/ja/Getting_Started.html",
            "docs/ja/Using_the_App.html",
            "docs/ja/Save_and_Recovery.html",
            "docs/ja/File_Formats.html",
            "docs/ja/CLRO_Note_Format.html",
            "docs/ja/CLROP_Annotation_Format.html",
            "docs/ja/Troubleshooting.html",
            "docs/ja/Help_Reference.html",
            "DOCUMENTATION.html",
        ):
            self.assertIn(f'href="{guide}"', portal)
        self.assertIn('href="en/index.html"', portal)
        english_portal = (REPO_ROOT / "site/github/en/index.html").read_text(encoding="utf-8-sig")
        self.assertIn('href="../index.html" lang="ja">日本語</a>', english_portal)
        self.assertIn("Set up and open the app", english_portal)
        self.assertIn("Resolve a problem", english_portal)
        for group in ("Start using the app", "Read, annotate, and take notes", "Protect and restore your work", "Understand the files"):
            self.assertIn(f"<h2>{group}</h2>", english_portal)
        self.assertIn('class="site-menu"', english_portal)
        self.assertIn('aria-current="page"><span class="menu-link-title">Documentation portal', english_portal)
        for guide in (
            "../docs/en/Getting_Started.html",
            "../docs/en/Using_the_App.html",
            "../docs/en/Save_and_Recovery.html",
            "../docs/en/File_Formats.html",
            "../docs/en/CLRO_Note_Format.html",
            "../docs/en/CLROP_Annotation_Format.html",
            "../docs/en/Troubleshooting.html",
            "../docs/en/Help_Reference.html",
            "../DOCUMENTATION.html",
        ):
            self.assertIn(f'href="{guide}"', english_portal)

    def test_public_site_sources_include_persistent_high_contrast_controls(self) -> None:
        github_portal = (REPO_ROOT / "site/github/index.html").read_text(encoding="utf-8-sig")
        cloudflare_intro = (REPO_ROOT / "site/cloudflare/public/index.html").read_text(encoding="utf-8-sig")
        for source in (github_portal, cloudflare_intro):
            self.assertIn("contrast-toggle", source)
            self.assertIn("pdf-note-workspace-high-contrast", source)

    @staticmethod
    def write_minimal_site(root: Path) -> None:
        for relative, text in {
            "index.html": (
                '<html><meta name="ai-agent-entrypoint" content="introduction/index.html">'
                '<a href="introduction/index.html">Document index</a>'
                '<a href="README.html">Project overview</a>'
                '<a href="https://pdf-note-workspace.soone-y.com/">Product site</a>'
                '<a href="https://github.com/soone-y/OPEN_PDF-Note-Workspace">GitHub repository</a>'
                '</html>'
            ),
            "README.md": "# README",
            "README.html": "<html></html>",
            "introduction/project_overview.md": "# AI context",
            "introduction/project_overview.html": "<html></html>",
            "introduction/index.md": "# Documentation index",
            "introduction/index.html": '<html><a href="project_overview.html">Project overview</a></html>',
        }.items():
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")

    def test_rejects_document_more_than_two_visible_clicks_from_portal(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "introduction" / "project_overview.html").write_text(
                '<html><a href="deep_reference.html">Deep reference</a></html>',
                encoding="utf-8",
            )
            (root / "introduction" / "deep_reference.html").write_text("<html></html>", encoding="utf-8")

            errors = validate_public_site.validate_site(root)

            self.assertIn(
                "document requires more than 2 visible clicks from portal: "
                "introduction/deep_reference.html (3)",
                errors,
            )

    def test_rejects_document_reachable_only_through_hamburger_menu(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "introduction" / "index.html").write_text(
                '<html><details class="site-menu"><a href="project_overview.html">Project overview</a></details></html>',
                encoding="utf-8",
            )

            errors = validate_public_site.validate_site(root)

            self.assertIn(
                "document is not reachable from the portal without opening a hamburger menu: "
                "introduction/project_overview.html",
                errors,
            )

    def test_rejects_development_only_reference(self) -> None:
        with repo_tempdir() as root:
            for relative, text in {
                "index.html": "<html></html>",
                "README.md": "# README\n\ndocs/internal/ is private.",
                "introduction/index.md": "# Documentation index",
            }.items():
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("development-only reference" in error for error in errors))

    def test_rejects_link_that_escapes_generated_site(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "index.html").write_text('<html><a href="../private.md">private</a></html>', encoding="utf-8")

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("escapes public site" in error for error in errors))

    def test_rejects_missing_rendered_html(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "README.html").unlink()

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("rendered HTML is missing" in error for error in errors))

    def test_rejects_missing_portal_machine_readable_metadata(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "index.html").write_text("<html></html>", encoding="utf-8")

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("ai-agent-entrypoint" in error for error in errors))

    def test_rejects_missing_visible_common_entry_link(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "index.html").write_text(
                '<html><meta name="ai-agent-entrypoint" content="introduction/index.html">'
                '<a href="README.html">README</a></html>',
                encoding="utf-8",
            )

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("must visibly link to common entry document" in error for error in errors))

    def test_allowlist_coverage_rejects_missing_and_unallowlisted_output(self) -> None:
        with repo_tempdir() as root:
            source = root / "source"
            site = root / "site"
            (source / "docs" / "ja").mkdir(parents=True)
            (source / "README.md").write_text("# README", encoding="utf-8")
            (source / "docs" / "ja" / "Guide.md").write_text("# Guide", encoding="utf-8")
            allowlist = source / "allowlist.json"
            allowlist.write_text(json.dumps({
                "schema_version": 1,
                "documentation_portal": {
                    "files": [{"source": "README.md", "destination": "README.md"}],
                    "trees": [{"source": "docs/ja", "destination": "docs/ja"}],
                },
            }), encoding="utf-8")
            for relative in ("README.md", "README.html", "docs/ja/Guide.md", "unexpected.txt"):
                target = site / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("content", encoding="utf-8")

            errors: list[str] = []
            validate_public_site.validate_allowlist_coverage(site, source, allowlist, errors)

            self.assertIn("allowlisted portal file is missing from generated site: docs/ja/Guide.html", errors)
            self.assertIn("generated site contains file outside documentation portal allowlist: unexpected.txt", errors)

    def test_documentation_map_rejects_missing_and_unallowlisted_documents(self) -> None:
        with repo_tempdir() as root:
            (root / "docs" / "ja").mkdir(parents=True)
            (root / "README.md").write_text("# README", encoding="utf-8")
            (root / "docs" / "ja" / "Guide.md").write_text("# Guide", encoding="utf-8")
            (root / "private.md").write_text("# Private", encoding="utf-8")
            (root / "DOCUMENTATION.md").write_text(
                "[README](README.md)\n[Private](private.md)\n", encoding="utf-8"
            )
            allowlist = root / "allowlist.json"
            allowlist.write_text(json.dumps({
                "schema_version": 1,
                "documentation_portal": {
                    "files": [
                        {"source": "README.md", "destination": "README.md"},
                        {"source": "DOCUMENTATION.md", "destination": "DOCUMENTATION.md"},
                    ],
                    "trees": [{"source": "docs/ja", "destination": "docs/ja"}],
                },
            }), encoding="utf-8")

            errors: list[str] = []
            validate_public_site.validate_documentation_map(root, allowlist, errors)

            self.assertIn("DOCUMENTATION.md does not list allowlisted Markdown document: docs/ja/Guide.md", errors)
            self.assertIn("DOCUMENTATION.md does not list allowlisted Markdown document: DOCUMENTATION.md", errors)
            self.assertIn("DOCUMENTATION.md lists Markdown document outside documentation portal allowlist: private.md", errors)

class IntroductionSiteValidationTests(unittest.TestCase):
    @staticmethod
    def write_minimal_site(root: Path) -> None:
        for relative, content in {
            "index.html": (
                '<a href="https://soone-y.github.io/OPEN_PDF-Note-Workspace/introduction/index.html">AI</a>'
                '<a href="https://github.com/soone-y/OPEN_PDF-Note-Workspace/releases">Release</a>'
                '<a href="https://github.com/soone-y/OPEN_PDF-Note-Workspace">Repository</a>'
                '<a href="https://soone-y.github.io/OPEN_PDF-Note-Workspace/">Docs</a>'
                '<img src="assets/app_overview.png">'
            ),
            "robots.txt": (
                "User-agent: *\nAllow: /\n"
                "Content-signal: search=yes, ai-input=yes, ai-train=no, use=reference\n"
                "Sitemap: https://pdf-note-workspace.soone-y.com/sitemap.xml\n"
            ),
            "sitemap.xml": (
                '<?xml version="1.0"?><urlset><url><loc>'
                'https://pdf-note-workspace.soone-y.com/</loc></url></urlset>'
            ),
            "_headers": "/*\n  Content-Signal: search=yes, ai-input=yes, ai-train=no, use=reference\n",
        }.items():
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        asset = root / "assets" / "app_overview.png"
        asset.parent.mkdir(parents=True, exist_ok=True)
        asset.write_bytes(b"image")

    def test_rejects_unapproved_file_and_escaping_link(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "draft.txt").write_text("not public", encoding="utf-8")
            (root / "index.html").write_text('<a href="../private.html">private</a>', encoding="utf-8")

            errors = validate_introduction_site.validate_site(root)

            self.assertTrue(any("unapproved public file" in error for error in errors))
            self.assertTrue(any("escapes introduction site" in error for error in errors))


class ReleaseLicenseGateTests(unittest.TestCase):
    @staticmethod
    def write_release(release_dir: Path, locale: str = "ja") -> None:
        for relative_path in release_license_gate.required_license_files(locale):
            target = release_dir / relative_path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"License material: {relative_path}\n", encoding="utf-8")

    def test_rejects_missing_license_in_zip(self) -> None:
        with repo_tempdir() as root:
            release_dir = root / "release_1.0.0"
            self.write_release(release_dir)
            zip_path = root / "release_1.0.0.zip"
            with zipfile.ZipFile(zip_path, "w") as archive:
                for path in release_dir.rglob("*"):
                    if path.is_file() and path.relative_to(release_dir).as_posix() != "licenses/zlib/LICENSE":
                        archive.write(path, Path(release_dir.name) / path.relative_to(release_dir))

            errors = release_license_gate.validate_release_zip(release_dir, zip_path)

            self.assertTrue(any("licenses/zlib/LICENSE" in error for error in errors))

    def test_rejects_zip_license_with_different_contents(self) -> None:
        with repo_tempdir() as root:
            release_dir = root / "release_1.0.0"
            self.write_release(release_dir)
            zip_path = root / "release_1.0.0.zip"
            with zipfile.ZipFile(zip_path, "w") as archive:
                for path in release_dir.rglob("*"):
                    if not path.is_file():
                        continue
                    arcname = Path(release_dir.name) / path.relative_to(release_dir)
                    archive.writestr(arcname.as_posix(), "tampered" if path.name == "LICENSE.md" else path.read_bytes())

            errors = release_license_gate.validate_release_zip(release_dir, zip_path)

            self.assertTrue(any("differs from unpacked release" in error for error in errors))

    def test_requires_the_common_top_level_license_material(self) -> None:
        with repo_tempdir() as root:
            release_dir = root / "release_1.0.0"
            self.write_release(release_dir, "en")

            self.assertEqual(release_license_gate.validate_release_directory(release_dir, "en"), [])
            self.assertTrue((release_dir / "LICENSE.md").exists())
            self.assertFalse((release_dir / "LICENSE.en.md").exists())
            self.assertFalse((release_dir / "LICENSE.ja.md").exists())


class ReleaseTextGateTests(unittest.TestCase):
    def test_rejects_invalid_utf8_and_replacement_character(self) -> None:
        with repo_tempdir() as root:
            docs = root / "docs"
            docs.mkdir()
            (docs / "invalid.md").write_bytes(b"\xff\xfe")
            (docs / "replacement.md").write_text("broken \ufffd text", encoding="utf-8")

            errors = release_text_gate.validate_release_directory(root)

            self.assertTrue(any("invalid UTF-8: docs/invalid.md" in error for error in errors))
            self.assertTrue(any("replacement character" in error for error in errors))

    def test_rejects_reversible_windows_1252_mojibake(self) -> None:
        with repo_tempdir() as root:
            docs = root / "docs"
            docs.mkdir()
            (docs / "garbled.md").write_text("Ã©", encoding="utf-8")

            errors = release_text_gate.validate_release_directory(root)

            self.assertTrue(any("likely Windows-1252/UTF-8 mojibake" in error for error in errors))


class LocaleUsageValidationTests(unittest.TestCase):
    def test_accepts_catalog_backed_references_and_english_text(self) -> None:
        with repo_tempdir() as root:
            source = root / "src"
            source.mkdir()
            (source / "screen.cpp").write_text(
                'auto text = localization::Text(L"screen.title");\n', encoding="utf-8"
            )
            ja = root / "ja.json"
            en = root / "en.json"
            ja.write_text(json.dumps({"screen.title": "画面"}), encoding="utf-8")
            en.write_text(json.dumps({"screen.title": "Screen"}), encoding="utf-8")

            self.assertEqual(validate_locale_usage.validate(source, ja, en), [])

    def test_rejects_unknown_id_and_japanese_in_english_catalog(self) -> None:
        with repo_tempdir() as root:
            source = root / "src"
            source.mkdir()
            (source / "screen.cpp").write_text(
                'auto text = localization::Format(L"screen.missing", {});\n', encoding="utf-8"
            )
            ja = root / "ja.json"
            en = root / "en.json"
            ja.write_text(json.dumps({"screen.title": "画面"}), encoding="utf-8")
            en.write_text(json.dumps({"screen.title": "画面"}), encoding="utf-8")

            errors = validate_locale_usage.validate(source, ja, en)

            self.assertTrue(any("absent from Japanese catalog: screen.missing" in error for error in errors))
            self.assertTrue(any("English catalog contains Japanese text: screen.title" in error for error in errors))

    def test_rejects_english_sentence_in_japanese_catalog(self) -> None:
        with repo_tempdir() as root:
            source = root / "src"
            source.mkdir()
            ja = root / "ja.json"
            en = root / "en.json"
            ja.write_text(json.dumps({"screen.error": "Operation transaction requires an open session.",
                                      "screen.mixed": "保存失敗: Operation transaction requires an open session.",
                                      "screen.placeholder": "{FILE} を確認してください。",
                                      "ui.app_title": "PDF Note Workspace",
                                      "screen.short": "Error"}), encoding="utf-8")
            en.write_text(json.dumps({"screen.error": "Operation transaction requires an open session.",
                                      "screen.mixed": "Operation transaction requires an open session.",
                                      "screen.placeholder": "Check {FILE}.",
                                      "ui.app_title": "PDF Note Workspace",
                                      "screen.short": "Error"}), encoding="utf-8")

            errors = validate_locale_usage.validate(source, ja, en)
            self.assertIn("Japanese catalog contains English prose: screen.error", errors)
            self.assertIn("Japanese catalog contains English prose: screen.mixed", errors)
            self.assertIn("Japanese catalog has unapproved ASCII-only text: screen.short", errors)
            self.assertFalse(any("ui.app_title" in error for error in errors))

    def test_rejects_direct_ui_literals_even_when_japanese(self) -> None:
        with repo_tempdir() as root:
            source = root / "src"
            source.mkdir()
            (source / "screen.cpp").write_text(
                'ShowSoftNotice(owner, L"保存できません");\n'
                'ShowSilentMessageDialog(owner, L"Note", detail, kind);\n'
                'ShowSilentMessageDialog(owner, title, L"Failed to save", kind);\n'
                'ShowSoftNotice(owner, localization::Text(L"screen.error"));\n',
                encoding="utf-8",
            )
            ja = root / "ja.json"
            en = root / "en.json"
            ja.write_text(json.dumps({"screen.error": "保存できません"}), encoding="utf-8")
            en.write_text(json.dumps({"screen.error": "Could not save"}), encoding="utf-8")

            errors = validate_locale_usage.validate(source, ja, en)
            self.assertEqual(len(errors), 3)
            self.assertTrue(all("direct UI literal" in error for error in errors))

    def test_rejects_raw_file_operation_error_but_skips_automation_fixture(self) -> None:
        with repo_tempdir() as root:
            source = root / "src"
            operation = source / "workspace" / "file_ops_move_rename.cppinc"
            operation.parent.mkdir(parents=True)
            operation.write_text(
                'if (outErr) *outErr = L"Could not open file";\n'
                'bool RunUiAutomationOperationTransactionRecoveryScenario() {\n'
                '  if (outErr) *outErr = L"Fixture failed";\n}\n',
                encoding="utf-8",
            )
            ja = root / "ja.json"
            en = root / "en.json"
            ja.write_text("{}", encoding="utf-8")
            en.write_text("{}", encoding="utf-8")

            errors = validate_locale_usage.validate(source, ja, en)
            self.assertEqual(len(errors), 1)
            self.assertIn("raw operation error literal", errors[0])

    def test_rejects_raw_readonly_viewer_status(self) -> None:
        with repo_tempdir() as root:
            source = root / "src"
            viewer = source / "readonly_viewer" / "main.cpp"
            viewer.parent.mkdir(parents=True)
            viewer.write_text(
                'SetViewerStatus(owner, L"File could not be opened");\n'
                'SetViewerStatus(owner, localization::Text(L"viewer.error"));\n',
                encoding="utf-8",
            )
            ja = root / "ja.json"
            en = root / "en.json"
            ja.write_text(json.dumps({"viewer.error": "ファイルを開けません"}), encoding="utf-8")
            en.write_text(json.dumps({"viewer.error": "File could not be opened"}), encoding="utf-8")

            errors = validate_locale_usage.validate(source, ja, en)
            self.assertEqual(len(errors), 1)
            self.assertIn("raw viewer message", errors[0])

    def test_ascii_exception_requires_its_reviewed_value(self) -> None:
        with repo_tempdir() as root:
            source = root / "src"
            source.mkdir()
            ja = root / "ja.json"
            en = root / "en.json"
            ja.write_text(json.dumps({"ui.app_title": "Operation failed"}), encoding="utf-8")
            en.write_text(json.dumps({"ui.app_title": "Operation failed"}), encoding="utf-8")

            errors = validate_locale_usage.validate(source, ja, en)
            self.assertIn("Japanese catalog has unapproved ASCII-only text: ui.app_title", errors)

    def test_text_encoding_error_ids_exist_in_both_catalogs(self) -> None:
        source = (REPO_ROOT / "src/core/text_encoding.h").read_text(encoding="utf-8")
        ids = set(re.findall(r'return L"(text_encoding\.[a-z0-9._]+)"', source))
        self.assertGreaterEqual(len(ids), 9)
        for locale in ("ja", "en"):
            catalog = json.loads((REPO_ROOT / f"locales/{locale}.json").read_text(encoding="utf-8"))
            self.assertEqual(ids - set(catalog), set())


class OfficeLocalizationTests(unittest.TestCase):
    def test_docx_conversion_errors_are_catalog_backed(self) -> None:
        source = (REPO_ROOT / "src/office/docx_space_protection.cpp").read_text(encoding="utf-8")
        ja = json.loads((REPO_ROOT / "locales/ja.json").read_text(encoding="utf-8"))
        en = json.loads((REPO_ROOT / "locales/en.json").read_text(encoding="utf-8"))
        ids = set(re.findall(r'OfficeErr\(L"(office\.docx\.[a-z0-9_]+)"\)', source))

        self.assertGreaterEqual(len(ids), 20)
        self.assertFalse(re.search(r'L"[^"\\\r\n]*[\u3040-\u30ff\u3400-\u9fff]', source))
        self.assertTrue(ids <= set(ja))
        self.assertTrue(ids <= set(en))


class OfficeStagingHarnessTests(unittest.TestCase):
    def test_office_conversion_uses_a_complete_docx_after_cpp_staging(self) -> None:
        script = (REPO_ROOT / "tests/scripts/run_docx_space_protection_tests.ps1").read_text(encoding="utf-8-sig")
        self.assertIn("Invoke-DocxProtectionExpectSuccess -Source $fixture -Dest $conversionStaged", script)
        self.assertIn('Copy-Item -LiteralPath $conversionStaged -Destination (Join-Path $conversionInputDir "app_staged.docx")', script)
        self.assertNotIn('Copy-Item -LiteralPath $staged -Destination (Join-Path $conversionInputDir "app_staged.docx")', script)
        self.assertEqual(script.count('Hash -ne $originalFixtureHash'), 2)
        self.assertIn('"--docx-space-protection", "off"', script)


class ReleaseLocaleContentGateTests(unittest.TestCase):
    @staticmethod
    def write_release(directory: Path, locale: str, readme: str = "Release documentation\n", edition: str = "full") -> None:
        (directory / "docs" / "legal").mkdir(parents=True)
        (directory / "sample_workspace").mkdir()
        documents = {
            "docs/README.md": readme,
            "docs/Getting_Started.md": "Getting started\n",
            "docs/Help_Reference.md": "Help reference\n",
            "docs/legal/LICENSE.md": "License\n",
            "docs/legal/THIRD_PARTY_NOTICES.md": "Third-party notices\n",
        }
        for relative, content in documents.items():
            (directory / relative).write_text(content, encoding="utf-8")
        top_documents = {
            "README.md": "[Guide](docs/README.md)\n",
            "LICENSE.md": "[Notices](docs/legal/THIRD_PARTY_NOTICES.md)\n",
            "SECURITY.md": "Security policy\n",
        }
        for name, content in top_documents.items():
            (directory / name).write_text(content, encoding="utf-8")
        (directory / "sample_workspace/workspace.json").write_text(
            json.dumps({"language": locale}), encoding="utf-8"
        )
        for relative in release_locale_content_gate.REQUIRED_LECTURE_SAMPLE_FILES[locale]:
            path = directory / "sample_workspace" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"sample")
        for relative in release_locale_content_gate.REQUIRED_COMPANION_SESSION_NOTES[locale]:
            path = directory / "sample_workspace" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"sample")
        for relative in release_locale_content_gate.REQUIRED_NOTE_FORMAT_SAMPLE_FILES[locale]:
            path = directory / "sample_workspace" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"sample")
        link_sample = release_locale_content_gate.NOTE_FORMAT_LINK_SAMPLE[locale]
        source_session = (
            REPO_ROOT / "release_assets" / "sample_workspace" / locale
            / release_locale_content_gate.NOTE_FORMAT_SESSION_PATH[locale]
        )
        target_session = directory / "sample_workspace" / release_locale_content_gate.NOTE_FORMAT_SESSION_PATH[locale]
        for key in ("note", "pdf", "clrop"):
            shutil.copyfile(source_session / link_sample[key], target_session / link_sample[key])
        starter_relative, expected_pdf, expected_note = release_locale_content_gate.STARTER_SESSION[locale]
        starter = directory / "sample_workspace" / starter_relative
        starter.mkdir(parents=True, exist_ok=True)
        (starter / expected_pdf).write_bytes(b"sample")
        (starter / expected_note).write_bytes(b"sample")
        if edition == "full":
            for relative in release_locale_content_gate.REQUIRED_FULL_CONVERSION_SAMPLE_FILES[locale]:
                path = directory / "sample_workspace" / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"sample")
            for relative in release_locale_content_gate.REQUIRED_FULL_COMPANION_SESSION_NOTES[locale]:
                path = directory / "sample_workspace" / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"sample")

    def test_accepts_complete_english_content(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "en")

            self.assertEqual(release_locale_content_gate.validate_release_directory(root, "en"), [])

    def test_accepts_complete_japanese_lecture_content(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "ja")

            self.assertEqual(release_locale_content_gate.validate_release_directory(root, "ja"), [])

    def test_accepts_lite_without_conversion_session_and_rejects_it_when_present(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "en", edition="lite")

            self.assertEqual(release_locale_content_gate.validate_release_directory(root, "en", "lite"), [])
            conversion_pdf = root / "sample_workspace/01_Lecture_Samples/Session_04_Office_Conversion/feature_overview_and_charts_conversion_result.pdf"
            conversion_pdf.parent.mkdir(parents=True)
            conversion_pdf.write_bytes(b"sample")
            errors = release_locale_content_gate.validate_release_directory(root, "en", "lite")

            self.assertTrue(any("Lite release contains conversion sample session" in error for error in errors))
            self.assertTrue(any("Lite release contains conversion-result PDF sample" in error for error in errors))

    def test_rejects_starter_session_with_more_than_one_note(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "ja")
            starter = root / "sample_workspace/01_講義サンプル/第01回_基本操作"
            (starter / "extra.md").write_text("extra\n", encoding="utf-8")

            errors = release_locale_content_gate.validate_release_directory(root, "ja")

            self.assertTrue(any("starter session must contain exactly one PDF and one note" in error for error in errors))

    def test_rejects_missing_note_format_sample(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "en")
            (root / "sample_workspace/01_Lecture_Samples/Session_02_Note_Formats/table_data.csv").unlink()

            errors = release_locale_content_gate.validate_release_directory(root, "en")

            self.assertTrue(any("required note-format sample is missing" in error for error in errors))

    def test_rejects_missing_companion_session_note(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "ja")
            (root / "sample_workspace/01_講義サンプル/第03回_最初期構想/ノート_最初期構想.clro").unlink()

            errors = release_locale_content_gate.validate_release_directory(root, "ja")

            self.assertTrue(any("required companion session note is missing" in error for error in errors))

    def test_rejects_duplicate_note_format_extension(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "ja")
            session = root / "sample_workspace/01_講義サンプル/第02回_ノート形式"
            (session / "another_note.md").write_text("duplicate\n", encoding="utf-8")

            errors = release_locale_content_gate.validate_release_directory(root, "ja")

            self.assertTrue(any("exactly one file for each required extension" in error for error in errors))

    def test_rejects_mismatched_note_format_pdf_link_pair(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "en")
            pdf = root / "sample_workspace/01_Lecture_Samples/Session_02_Note_Formats/pdf_link_practice.pdf"
            pdf.write_bytes(b"different PDF")

            errors = release_locale_content_gate.validate_release_directory(root, "en")

            self.assertTrue(any("mismatched PDF size" in error for error in errors))
            self.assertTrue(any("mismatched PDF hash" in error for error in errors))

    def test_note_format_samples_demonstrate_their_extensions(self) -> None:
        samples = {
            "ja": {
                "directory": REPO_ROOT / "release_assets/sample_workspace/ja/01_講義サンプル/第02回_ノート形式",
                "clro": "基本操作.clro",
                "markdown": "ノート_数式.md",
                "tex": "ノート_TeX数式.tex",
                "text": "ノート_プレーンテキスト.txt",
                "csv": "ノート_表データ.csv",
                "pdf": "PDFリンク練習.pdf",
                "clrop": "PDFリンク練習.clrop",
            },
            "en": {
                "directory": REPO_ROOT / "release_assets/sample_workspace/en/01_Lecture_Samples/Session_02_Note_Formats",
                "clro": "basic_operation_note.clro",
                "markdown": "math.md",
                "tex": "tex_math_note.tex",
                "text": "plain_text.txt",
                "csv": "table_data.csv",
                "pdf": "pdf_link_practice.pdf",
                "clrop": "pdf_link_practice.clrop",
            },
        }
        for sample in samples.values():
            directory = sample["directory"]
            clro = (directory / sample["clro"]).read_text(encoding="utf-8")
            markdown = (directory / sample["markdown"]).read_text(encoding="utf-8")
            tex = (directory / sample["tex"]).read_text(encoding="utf-8")
            text = (directory / sample["text"]).read_text(encoding="utf-8")
            pdf = directory / sample["pdf"]
            clrop = json.loads((directory / sample["clrop"]).read_text(encoding="utf-8"))
            with (directory / sample["csv"]).open(encoding="utf-8", newline="") as handle:
                rows = list(csv.reader(handle))

            self.assertTrue(all(token in clro for token in ("<u>", "<char=", "<back=", "<link=")))
            self.assertTrue(all(token in markdown for token in ("## ", "> ", ":::", "```", "|")))
            self.assertTrue(all(token in tex for token in ("\\documentclass", "\\begin{align}", "\\begin{tabular}")))
            self.assertIn("#", text)
            self.assertEqual(len(rows), 6)
            self.assertTrue(all(len(row) == 5 for row in rows))
            self.assertIn("<link=session-02-pdf-note-link>", clro)
            self.assertEqual(clrop["pdf_id"]["path"], pdf.name)
            self.assertEqual(clrop["pdf_id"]["size"], pdf.stat().st_size)
            self.assertEqual(clrop["pdf_id"]["sha256"], hashlib.sha256(pdf.read_bytes()).hexdigest())
            markers = [
                item
                for page in clrop["pages"]
                for item in page["items"]
                if item.get("type") == "link-marker" and item.get("link_id") == "session-02-pdf-note-link"
            ]
            self.assertEqual(len(markers), 1)
            self.assertEqual(len(markers[0]["p1"]), 2)
            self.assertNotIn("note_path", markers[0])

    def test_session_two_and_four_companion_notes_are_concise_clro_material(self) -> None:
        samples = {
            "ja": {
                "01_講義サンプル/第03回_最初期構想/ノート_最初期構想.clro": ("# 第03回", "> ", "PDF学習ワークスペース統合画面構成および基本仕様書.pdf"),
                "01_講義サンプル/第04回_Office変換/ノート_Office変換.clro": ("# 第04回", "> ", "PPTX_機能紹介とネイティブ図表_変換結果.pdf"),
            },
            "en": {
                "01_Lecture_Samples/Session_03_Early_Design/early_design_notes.clro": ("# Session 03", "> ", "early_design_reference.pdf"),
                "01_Lecture_Samples/Session_04_Office_Conversion/office_conversion_notes.clro": ("# Session 04", "> ", "feature_overview_and_charts_conversion_result.pdf"),
            },
        }
        for locale, notes in samples.items():
            root = REPO_ROOT / "release_assets/sample_workspace" / locale
            for relative, tokens in notes.items():
                note = root / relative
                self.assertTrue(note.is_file())
                self.assertTrue(all(token in note.read_text(encoding="utf-8") for token in tokens))

    def test_full_conversion_session_has_one_native_chart_pdf_per_locale(self) -> None:
        expected_pdf_names = {
            "ja": "PPTX_機能紹介とネイティブ図表_変換結果.pdf",
            "en": "feature_overview_and_charts_conversion_result.pdf",
        }
        for locale, required_files in release_locale_content_gate.REQUIRED_FULL_CONVERSION_SAMPLE_FILES.items():
            conversion_pdfs = [path for path in required_files if path.endswith(".pdf")]
            self.assertEqual(len(conversion_pdfs), 1, locale)
            self.assertEqual(Path(conversion_pdfs[0]).name, expected_pdf_names[locale])

    def test_rejects_japanese_text_and_mismatched_sample_locale(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "ja")
            (root / "sample_workspace/README.txt").write_text("日本語のサンプル\n", encoding="utf-8")

            errors = release_locale_content_gate.validate_release_directory(root, "en")

            self.assertTrue(any("language does not match" in error for error in errors))
            self.assertTrue(any("contains Japanese locale text: sample_workspace/README.txt" in error for error in errors))

    def test_rejects_broken_link_and_unselected_locale_directory(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "en")
            (root / "docs/README.md").write_text("[Missing](missing.md)\n", encoding="utf-8")
            (root / "docs/ja").mkdir()

            errors = release_locale_content_gate.validate_release_directory(root, "en")

            self.assertTrue(any("Markdown link target is missing: docs/README.md -> missing.md" in error for error in errors))
            self.assertTrue(any("unselected locale or internal directory: docs/ja" in error for error in errors))

    def test_rejects_japanese_filename_in_english_sample(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "en")
            (root / "sample_workspace/日本語.txt").write_text("English text\n", encoding="utf-8")

            errors = release_locale_content_gate.validate_release_directory(root, "en")

            self.assertTrue(any("contains Japanese locale filename: sample_workspace/日本語.txt" in error for error in errors))

    def test_rejects_top_level_locale_document(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "ja")
            (root / "README.en.md").write_text("English release overview\n", encoding="utf-8")

            errors = release_locale_content_gate.validate_release_directory(root, "ja")

            self.assertTrue(any("unexpected top-level locale document: README.en.md" in error for error in errors))

    def test_rejects_missing_or_unexpected_top_level_documents(self) -> None:
        with repo_tempdir() as root:
            self.write_release(root, "ja")
            (root / "README.md").unlink()
            (root / "README.fr.md").write_text("French\n", encoding="utf-8")

            errors = release_locale_content_gate.validate_release_directory(root, "ja")

            self.assertTrue(any("is missing: README.md" in error for error in errors))
            self.assertTrue(any("unexpected top-level locale document: README.fr.md" in error for error in errors))


class ReleaseSetIntegrityGateTests(unittest.TestCase):
    @staticmethod
    def write_zip(release_dir: Path, zip_path: Path, archive_root: str | None = None) -> None:
        with zipfile.ZipFile(zip_path, "w") as archive:
            for path in release_dir.rglob("*"):
                if path.is_file():
                    archive.write(path, Path(archive_root or release_dir.name) / path.relative_to(release_dir))

    def make_release_set(self, root: Path) -> tuple[Path, Path, Path]:
        release_set = root / "release_set"
        snapshot = release_set / "public_snapshot"
        snapshot.mkdir(parents=True)
        (snapshot / "README.md").write_text("public snapshot\n", encoding="utf-8")
        full = release_set / "release_full"
        lite = release_set / "release_lite"
        for directory, text in ((full, "full\n"), (lite, "lite\n")):
            (directory / "docs").mkdir(parents=True)
            (directory / "docs" / "README.md").write_text(text, encoding="utf-8")
            executable = directory / "pdf_note_workspace.exe"
            executable.write_bytes(("app-" + text).encode("utf-8"))
            runtime = directory / "pdfium.dll"
            runtime.write_bytes(b"runtime")
            edition = "full" if directory == full else "lite"
            (directory / "pdf_note_workspace.exe.buildinfo.txt").write_text(
                "format\tpdf-note-build-info-v1\n"
                "version\t1.0.0\n"
                f"edition\t{edition}\n"
                "locale\tja\n"
                f"artifact\tpdf_note_workspace.exe\t{release_set_integrity_gate.sha256_file(executable)}\n"
                f"artifact\tpdfium.dll\t{release_set_integrity_gate.sha256_file(runtime)}\n",
                encoding="utf-8",
            )
            viewer = directory / "readonly_viewer.exe"
            viewer.write_bytes(b"viewer")
            (directory / "readonly_viewer.exe.buildinfo.txt").write_text(
                "format\tpdf-note-build-info-v1\n"
                "version\t1.0.0\n"
                "locale\tja\n"
                f"artifact\treadonly_viewer.exe\t{release_set_integrity_gate.sha256_file(viewer)}\n"
                f"artifact\tpdfium.dll\t{release_set_integrity_gate.sha256_file(runtime)}\n",
                encoding="utf-8",
            )
        (full / "lo").mkdir(parents=True)
        full_zip = release_set / "release_full.zip"
        lite_zip = release_set / "release_lite.zip"
        archive_root = "PDF-Note-Workspace-1.0.0"
        self.write_zip(full, full_zip, archive_root)
        self.write_zip(lite, lite_zip, archive_root)
        (release_set / "release_set_manifest.json").write_text(json.dumps({
            "app_version": "1.0.0",
            "locale": "ja",
            "components": {
                "release": full.name,
                "release_lite": lite.name,
                "public_snapshot": snapshot.name,
                "release_zip": full_zip.name,
                "release_lite_zip": lite_zip.name,
            },
            "zip_roots": {
                "release_zip": archive_root,
                "release_lite_zip": archive_root,
            },
        }), encoding="utf-8")
        allowlist = root / "allowlist.txt"
        allowlist.write_text("README.md\n", encoding="utf-8")
        artifact_manifest = root / "artifacts.tsv"
        artifact_manifest.write_text("# no untracked artifacts in this fixture\n", encoding="utf-8")
        release_set_integrity_gate.write_snapshot_manifest(release_set, allowlist, artifact_manifest)
        return release_set, snapshot, full_zip

    def test_accepts_recorded_snapshot_and_exact_zip_contents(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)

            self.assertEqual(release_set_integrity_gate.validate_release_set(release_set), [])

    def test_rejects_viewer_that_differs_from_build_info(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)
            (release_set / "release_full" / "readonly_viewer.exe").write_bytes(b"changed viewer")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("read-only viewer executable does not match its build-info hash" in error for error in errors))

    def test_rejects_runtime_that_differs_from_build_info(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)
            (release_set / "release_full" / "pdfium.dll").write_bytes(b"changed runtime")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("pdfium.dll does not match its build-info hash" in error for error in errors))

    def test_accepts_legacy_full_runtime_layout(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)
            full = release_set / "release_full"
            (full / "lo").rmdir()
            (full / "libreoffice" / "custom_runtime" / "instdir").mkdir(parents=True)

            self.assertEqual(release_set_integrity_gate.validate_release_set(release_set), [])

    def test_rejects_archive_root_that_does_not_match_the_version(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)
            manifest_path = release_set / "release_set_manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["zip_roots"]["release_zip"] = "PDF-Note-Workspace-9.9.9"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("ZIP root for release_zip must be" in error for error in errors))

    def test_rejects_snapshot_change_after_manifest_creation(self) -> None:
        with repo_tempdir() as root:
            release_set, snapshot, _ = self.make_release_set(root)
            (snapshot / "README.md").write_text("changed after confirmation\n", encoding="utf-8")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("snapshot files differ" in error for error in errors))
            self.assertTrue(any("snapshot tree hash differs" in error for error in errors))

    def test_copied_snapshot_ignores_only_git_metadata_and_detects_changes(self) -> None:
        with repo_tempdir() as root:
            release_set, snapshot, _ = self.make_release_set(root)
            copied = root / "copied_snapshot"
            shutil.copytree(snapshot, copied)
            (copied / ".git").write_text("gitdir: worktree metadata\n", encoding="utf-8")

            self.assertEqual(
                release_set_integrity_gate.validate_external_snapshot(release_set, copied),
                [],
            )

            (copied / "README.md").write_text("changed during submission\n", encoding="utf-8")
            errors = release_set_integrity_gate.validate_external_snapshot(release_set, copied)

            self.assertTrue(any("copied public snapshot files differ" in error for error in errors))
            self.assertTrue(any("copied public snapshot tree hash differs" in error for error in errors))

    def test_rejects_unexpected_zip_file(self) -> None:
        with repo_tempdir() as root:
            release_set, _, full_zip = self.make_release_set(root)
            manifest = json.loads((release_set / "release_set_manifest.json").read_text(encoding="utf-8"))
            archive_root = manifest["zip_roots"]["release_zip"]
            with zipfile.ZipFile(full_zip, "a") as archive:
                archive.writestr(f"{archive_root}/unexpected.txt", "not in the extracted release")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("unexpected files" in error for error in errors))

    def test_explains_how_to_recover_when_release_directory_changes_after_zipping(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)
            (release_set / "release_full" / "docs" / "README.md").write_text(
                "changed after ZIP creation\n", encoding="utf-8"
            )

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("do not run or edit the unpacked release" in error for error in errors))

    def test_rejects_lite_runtime_or_wrong_build_version(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)
            lite = release_set / "release_lite"
            (lite / "lo").mkdir(parents=True)
            build_info = lite / "pdf_note_workspace.exe.buildinfo.txt"
            build_info.write_text(build_info.read_text(encoding="utf-8").replace("version\t1.0.0", "version\t9.9.9"), encoding="utf-8")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("Lite: LibreOffice runtime directory (lo)" in error for error in errors))
            self.assertTrue(any("Lite: application build-info version" in error for error in errors))


class ReleaseStartupSmokeGateTests(unittest.TestCase):
    def test_safe_extract_accepts_the_manifest_archive_root(self) -> None:
        with repo_tempdir() as root:
            release_dir = root / "release"
            release_dir.mkdir()
            (release_dir / "app.exe").write_bytes(b"app")
            archive = root / "release.zip"
            archive_root = "PDF-Note-Workspace-1.0.0"
            with zipfile.ZipFile(archive, "w") as output:
                output.write(release_dir / "app.exe", f"{archive_root}/app.exe")

            destination = root / "extract"
            release_startup_smoke_gate.safe_extract(archive, destination, release_dir, archive_root)

            self.assertEqual((destination / archive_root / "app.exe").read_bytes(), b"app")

    def test_rejects_zip_path_traversal(self) -> None:
        with repo_tempdir() as root:
            archive = root / "bad.zip"
            release_dir = root / "release"
            release_dir.mkdir()
            (release_dir / "app.exe").write_bytes(b"app")
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("../outside.txt", "unsafe")

            with self.assertRaises(ValueError):
                release_startup_smoke_gate.safe_extract(archive, root / "extract", release_dir)

    def test_rejects_previously_started_release_copy(self) -> None:
        for runtime_root in ("workspace", "__pdf_note_workspace__"):
            with self.subTest(runtime_root=runtime_root), repo_tempdir() as root:
                release_startup_smoke_gate.assert_not_previously_started(root)
                (root / runtime_root).mkdir()

                with self.assertRaisesRegex(RuntimeError, "配布ZIPに、初回起動で生成される不要なデータ.*" + runtime_root):
                    release_startup_smoke_gate.assert_not_previously_started(root)


class RepositoryScriptAndTextGateTests(unittest.TestCase):
    def test_rejects_utf16_nul_and_mojibake(self) -> None:
        _, utf16_errors = repo_hygiene_gate.validate_text_bytes(b"\xff\xfeA\x00", label="script.ps1")
        _, nul_errors = repo_hygiene_gate.validate_text_bytes(b"a\x00b", label="script.ps1")
        _, mojibake_errors = repo_hygiene_gate.validate_text_bytes("Ã©".encode("utf-8"), label="script.ps1")

        self.assertTrue(any("UTF-16" in error for error in utf16_errors))
        self.assertTrue(any("NUL byte" in error for error in nul_errors))
        self.assertTrue(any("mojibake" in error for error in mojibake_errors))

    def test_rejects_python_and_json_syntax_errors(self) -> None:
        self.assertTrue(repo_hygiene_gate.validate_python_syntax(
            Path("broken.py"), "def broken(:\n", "broken.py"
        ))
        self.assertTrue(repo_hygiene_gate.validate_json_syntax("{", "broken.json"))


class BuildPublicSiteTests(unittest.TestCase):
    def test_preserves_prior_output_when_staged_install_fails(self) -> None:
        with repo_tempdir() as root:
            output = root / "site" / "github" / "output" / "public"
            output.mkdir(parents=True)
            (output / "existing.md").write_text("prior generation", encoding="utf-8")
            staging = root / "staging"
            staging.mkdir()
            (staging / "new.md").write_text("staged generation", encoding="utf-8")

            original_rename = Path.rename

            def fail_staged_install(path: Path, target: Path) -> Path:
                if path == staging:
                    raise OSError("simulated staged install failure")
                return original_rename(path, target)

            with mock.patch.object(build_public_site, "OUTPUT_DIR", output), \
                 mock.patch.object(Path, "rename", new=fail_staged_install):
                with self.assertRaisesRegex(OSError, "simulated staged install failure"):
                    build_public_site.replace_staged_site(staging)

            self.assertEqual((output / "existing.md").read_text(encoding="utf-8"), "prior generation")
            self.assertTrue((staging / "new.md").is_file())

    def test_builds_only_selected_files_and_keeps_machine_readable_index(self) -> None:
        with repo_tempdir() as root:
            (root / "introduction" / "core").mkdir(parents=True)
            (root / "docs" / "public").mkdir(parents=True)
            (root / "docs" / "images").mkdir(parents=True)
            (root / "site" / "github").mkdir(parents=True)
            (root / ".github").mkdir()

            for name in (
                "index.html", "PORTAL_NOTE.md", "README.md", "LICENSE.md", "LICENSES_INDEX.md",
                "THIRD_PARTY_NOTICES.md", "AGENTS.md",
            ):
                (root / name).write_text("__APP_VERSION__", encoding="utf-8")
            (root / "REPO_VERSION.txt").write_text("1.2.3\n", encoding="utf-8")
            (root / "README.md").write_text(
                "- ソースからビルドする: [docs/public/How_to_Build.md](docs/public/How_to_Build.md)",
                encoding="utf-8",
            )
            (root / "site" / "github" / "index.html").write_text("__APP_VERSION__", encoding="utf-8")
            (root / ".github" / "SECURITY.md").write_text("# Security", encoding="utf-8")
            (root / "introduction" / "core" / "semantic_search_index.json").write_text("{}", encoding="utf-8")
            (root / "introduction" / "core" / "internal.md").write_text("development-only", encoding="utf-8")
            (root / "docs" / "public" / "How_to_Use.md").write_text("# Use __APP_VERSION__", encoding="utf-8")
            (root / "docs" / "public" / "How_to_Build.md").write_text("private", encoding="utf-8")
            (root / "docs" / "public" / "Index.md").write_text(
                "## 開発者向け\n\n| 文書 | 読む場面 |\n| --- | --- |\n"
                "| [How_to_Build.md](How_to_Build.md) | ソースからビルド、テスト、release 作成を行いたい |\n\n",
                encoding="utf-8",
            )
            (root / "docs" / "images" / "overview.png").write_bytes(b"image")
            (root / "site" / "github" / "documentation_portal_allowlist.json").write_text(
                json.dumps({
                    "schema_version": 1,
                    "version_source": "REPO_VERSION.txt",
                    "documentation_portal": {
                        "files": [
                            {"source": "site/github/index.html", "destination": "index.html"},
                        ] + [
                            {"source": name, "destination": name}
                            for name in (
                                "PORTAL_NOTE.md", "README.md", "LICENSE.md",
                                "LICENSES_INDEX.md", "THIRD_PARTY_NOTICES.md",
                            )
                        ] + [
                            {"source": ".github/SECURITY.md", "destination": ".github/SECURITY.md"},
                        ],
                        "trees": [
                            {
                                "source": "introduction",
                                "destination": "introduction",
                                "exclude_prefixes": ["core/internal.md"],
                            },
                            {"source": "docs/images", "destination": "docs/images"},
                        ],
                        "document_markdown": {
                            "source": "docs/public", "destination": "docs/public",
                            "exclude": ["How_to_Build.md"],
                        },
                    },
                }),
                encoding="utf-8",
            )

            with mock.patch.object(build_public_site, "REPO_ROOT", root), \
                 mock.patch.object(build_public_site, "GITHUB_SITE_ROOT", root / "site" / "github"), \
                 mock.patch.object(build_public_site, "ALLOWLIST_PATH", root / "site" / "github" / "documentation_portal_allowlist.json"), \
                 mock.patch.object(build_public_site, "OUTPUT_DIR", root / "_site"):
                self.assertEqual(build_public_site.build_site(documentation_portal=True), 0)

            site = root / "_site"
            self.assertTrue((site / "introduction" / "core" / "semantic_search_index.json").exists())
            self.assertFalse((site / "introduction" / "core" / "internal.md").exists())
            self.assertTrue((site / "docs" / "public" / "How_to_Use.md").exists())
            self.assertFalse((site / "docs" / "public" / "How_to_Build.md").exists())
            self.assertEqual((site / "docs" / "public" / "How_to_Use.md").read_text(encoding="utf-8"), "# Use 1.2.3")
            self.assertIn("How_to_Build.md](", (site / "README.md").read_text(encoding="utf-8"))
            self.assertIn("How_to_Build.md](", (site / "docs" / "public" / "Index.md").read_text(encoding="utf-8"))


class SyncPublicationInputsTests(unittest.TestCase):
    def test_syncs_explicit_documentation_files_without_document_tree_rule(self) -> None:
        with repo_tempdir() as root:
            source = root / "source"
            destination = root / "destination"
            for path, text in {
                "PORTAL_NOTE.md": "Portal entry",
                "docs/public/How_to_Use.md": "use",
            }.items():
                target = source / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            config = source / "site/github/documentation_portal_allowlist.json"
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_text(json.dumps({
                "schema_version": 1,
                "documentation_portal": {
                    "files": [
                        {"source": "PORTAL_NOTE.md", "destination": "PORTAL_NOTE.md"},
                        {"source": "docs/public/How_to_Use.md", "destination": "docs/public/How_to_Use.md"},
                    ],
                },
                "pages_submission": {"files": [], "trees": [], "retired_paths": []},
            }), encoding="utf-8")
            destination.mkdir()
            (destination / "introduction").mkdir()
            (destination / "introduction" / "project_context.xml").write_text("development-only", encoding="utf-8")

            sync_publication_inputs.sync_publication_inputs(source, destination)

            self.assertEqual((destination / "PORTAL_NOTE.md").read_text(encoding="utf-8"), "Portal entry")
            self.assertEqual((destination / "docs/public/How_to_Use.md").read_text(encoding="utf-8"), "use")
            self.assertTrue((destination / "introduction" / "project_context.xml").is_file())

    def test_syncs_allowlisted_github_inputs_and_removes_retired_content(self) -> None:
        with repo_tempdir() as root:
            source = root / "source"
            destination = root / "destination"
            for path, text in {
                "PORTAL_NOTE.md": "Portal entry",
                "introduction/manifest.json": "{}",
                "docs/public/How_to_Use.md": "use",
                "docs/public/How_to_Build.md": "private",
                "docs/images/app.png": "image",
                ".github/FUNDING.yml": "github: [maintainer]",
                ".github/workflows/static.yml": "workflow",
                "site/github/index.html": "portal",
                "site/github/output/public/index.html": "generated",
                "site/github/scripts/sync_publication_inputs.py": "development-only",
            }.items():
                target = source / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            allowlist = {
                "schema_version": 1,
                "documentation_portal": {
                    "files": [{"source": "PORTAL_NOTE.md", "destination": "PORTAL_NOTE.md"}],
                    "trees": [
                        {"source": "introduction", "destination": "introduction"},
                        {"source": "docs/images", "destination": "docs/images"},
                    ],
                    "document_markdown": {
                        "source": "docs/public", "destination": "docs/public", "exclude": ["How_to_Build.md"],
                    },
                },
                "pages_submission": {
                    "files": [
                        {"source": ".github/FUNDING.yml", "destination": ".github/FUNDING.yml"},
                        {"source": ".github/workflows/static.yml", "destination": ".github/workflows/static.yml"},
                    ],
                    "trees": [{"source": "site/github", "destination": "site/github", "exclude_prefixes": ["output/", "scripts/sync_publication_inputs.py"]}],
                    "retired_paths": ["index.html", "tools"],
                },
            }
            config = source / "site/github/documentation_portal_allowlist.json"
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_text(json.dumps(allowlist), encoding="utf-8")
            destination.mkdir()
            (destination / "site/github/output/public").mkdir(parents=True)
            (destination / "site/github/output/public/index.html").write_text("old", encoding="utf-8")
            (destination / "index.html").write_text("retired", encoding="utf-8")
            (destination / "tools/dev").mkdir(parents=True)
            (destination / "tools/dev/sync_publication_inputs.py").write_text("old", encoding="utf-8")
            (destination / "src/app").mkdir(parents=True)
            (destination / "src/app/main.cpp").write_text("public source", encoding="utf-8")
            private_test = destination / "tests/python/test_python_tools.py"
            private_test.parent.mkdir(parents=True)
            private_test.write_text("DEV_PDF-Note-Workspace", encoding="utf-8")

            sync_publication_inputs.sync_publication_inputs(source, destination)

            self.assertEqual((destination / "PORTAL_NOTE.md").read_text(encoding="utf-8"), "Portal entry")
            self.assertTrue((destination / "introduction/manifest.json").is_file())
            self.assertTrue((destination / "docs/images/app.png").is_file())
            self.assertTrue((destination / "docs/public/How_to_Use.md").is_file())
            self.assertFalse((destination / "docs/public/How_to_Build.md").exists())
            self.assertTrue((destination / ".github/workflows/static.yml").is_file())
            self.assertEqual(
                (destination / ".github/FUNDING.yml").read_text(encoding="utf-8"),
                "github: [maintainer]",
            )
            self.assertTrue((destination / "site/github/index.html").is_file())
            self.assertFalse((destination / "site/github/output").exists())
            self.assertFalse((destination / "site/github/scripts/sync_publication_inputs.py").exists())
            self.assertFalse((destination / "tools").exists())
            self.assertFalse((destination / "index.html").exists())
            self.assertEqual((destination / "src/app/main.cpp").read_text(encoding="utf-8"), "public source")
            self.assertEqual(private_test.read_text(encoding="utf-8"), "DEV_PDF-Note-Workspace")

    def test_rejects_private_development_reference(self) -> None:
        with repo_tempdir() as root:
            destination = root / "destination"
            destination.mkdir()
            (destination / "PORTAL_NOTE.md").write_text(
                "DEV_PDF-Note-Workspace must not be published",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "private development reference"):
                sync_publication_inputs.validate_publication_inputs(destination)

    def test_rejects_duplicate_utf8_bom(self) -> None:
        with repo_tempdir() as root:
            destination = root / "destination"
            destination.mkdir()
            (destination / "PORTAL_NOTE.md").write_bytes(b"\xef\xbb\xbf\xef\xbb\xbf# Public document\n")

            with self.assertRaisesRegex(ValueError, "duplicate UTF-8 BOM"):
                sync_publication_inputs.validate_publication_inputs(destination)

if __name__ == "__main__":
    unittest.main()
