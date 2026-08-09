from __future__ import annotations

import importlib.util
import io
import json
import shutil
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
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


migrate_clrop_v1 = load_module("migrate_clrop_v1", "tools/migration/migrate_clrop_v1.py")
analyze_build_logs = load_module("analyze_build_logs", "tools/metrics/analyze_build_logs.py")
analyze_document_language = load_module("analyze_document_language", "tools/metrics/analyze_document_language.py")
analyze_repo = load_module("analyze_repo", "tools/metrics/code_metrics/analyze_repo.py")
code_metrics_gui = load_module("code_metrics_gui", "tools/metrics/code_metrics/gui.py")
libreoffice_reduce = load_module("libreoffice_reduce", "tools/libreoffice/libreoffice_reduce.py")
libreoffice_smoke_test = load_module("libreoffice_smoke_test", "tools/libreoffice/libreoffice_smoke_test.py")
libreoffice_conversion_quality_test = load_module(
    "libreoffice_conversion_quality_test", "tools/libreoffice/libreoffice_conversion_quality_test.py"
)
render_human_docs = load_module("render_human_docs", "site/github/scripts/render_human_docs.py")
build_public_site = load_module("build_public_site", "site/github/scripts/build_public_site.py")
validate_public_site = load_module("validate_public_site", "site/github/scripts/validate_public_site.py")
validate_introduction_site = load_module(
    "validate_introduction_site", "site/cloudflare/scripts/validate_introduction_site.py"
)
release_license_gate = load_module("release_license_gate", "tools/release_checks/release_license_gate.py")
release_text_gate = load_module("release_text_gate", "tools/release_checks/release_text_gate.py")
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


class AnnotationToolPolicyTests(unittest.TestCase):
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

    def test_annotation_input_warns_when_annotations_are_hidden(self) -> None:
        source = (REPO_ROOT / "src/pdf_view/input.cppinc").read_text(encoding="utf-8")
        self.assertIn("NotifyAnnotationInputWhileHidden", source)
        self.assertIn("注釈表示がOFFです。入力した注釈は保存されますが", source)
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

            ignored = root / "__resource__" / "__tmp__" / "__stage__" / "clrop" / "staged.clrop"
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

        self.assertIn("docs/internal/architecture/persistence_保存系現行実装整理方針_2026-04-29.md", report["read"])
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

    def test_release_packaging_changes_propose_libreoffice_runtime_gate(self) -> None:
        report = change_impact.recommendations_for_paths(["scripts/release/pack_release.ps1"])

        self.assertIn(
            "python tools/release_checks/libreoffice_runtime_gate.py --image third_party/libreoffice/custom_runtime/instdir",
            report["run"],
        )


class ExportPublicSnapshotTests(unittest.TestCase):
    def test_main_uses_gui_destination_selection_by_default(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("README.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n", encoding="utf-8")

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


class LibreOfficeRuntimeGateTests(unittest.TestCase):
    def test_minimal_converter_without_prohibited_indicator_passes(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            (program / "soffice.com").write_bytes(b"MZ")

            violations = libreoffice_runtime_gate.collect_violations(root / "image")

            self.assertEqual(violations, [])

    def test_known_communication_runtime_file_is_rejected(self) -> None:
        with repo_tempdir() as root:
            program = root / "image" / "program"
            program.mkdir(parents=True)
            (program / "soffice.com").write_bytes(b"MZ")
            (program / "libcurl.dll").write_bytes(b"MZ")

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
            soffice.write_bytes(b"MZ")
            merged.write_bytes(b"MZ")

            def fake_scan(path, _root, _queries, min_string, max_strings):
                imports = ["WINHTTP.dll"] if path == merged else []
                return binary_scan.BinaryFinding(
                    path=path.name,
                    size_bytes=path.stat().st_size,
                    import_dlls=imports,
                    import_symbols=[],
                    matched_strings=[],
                    matched_imports=[],
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
            soffice.write_bytes(b"MZ")
            merged.write_bytes(b"MZ")

            def fake_scan(path, _root, _queries, min_string, max_strings):
                imports = ["winmm.dll"] if path == merged else []
                return binary_scan.BinaryFinding(
                    path=path.name,
                    size_bytes=path.stat().st_size,
                    import_dlls=imports,
                    import_symbols=[],
                    matched_strings=[],
                    matched_imports=[],
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
            soffice.write_bytes(b"MZ")
            merged.write_bytes(b"MZ")

            def fake_scan(path, _root, _queries, min_string, max_strings):
                markers = ["PlaySoundW"] if path == merged else []
                return binary_scan.BinaryFinding(
                    path=path.name,
                    size_bytes=path.stat().st_size,
                    import_dlls=[],
                    import_symbols=[],
                    matched_strings=[],
                    matched_imports=markers,
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
            (program / "soffice.com").write_bytes(b"MZ")
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
            path.write_bytes((name + "\n").encode("utf-8"))
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
    def test_generates_html_for_human_and_ai_docs(self) -> None:
        with repo_tempdir() as site_dir:
            readme = site_dir / "README.md"
            readme.write_text("# Project README\n\nSome text.", encoding="utf-8")

            doc_dir = site_dir / "docs" / "public"
            doc_dir.mkdir(parents=True)
            use_doc = doc_dir / "How_to_Use.md"
            use_doc.write_text("# How to Use\n\nUsage steps.", encoding="utf-8")

            for_ai = site_dir / "For_AI.md"
            for_ai.write_text("# For AI\n\nAI rules.", encoding="utf-8")

            code = render_human_docs.main([str(site_dir)])
            self.assertEqual(code, 0)

            # 人間用ドキュメントの .html が作られていること
            self.assertTrue((site_dir / "README.html").exists())
            self.assertTrue((site_dir / "docs" / "public" / "How_to_Use.html").exists())
            human_html = (site_dir / "docs" / "public" / "How_to_Use.html").read_text(encoding="utf-8")
            self.assertIn('class="site-menu"', human_html)
            self.assertIn('GitHub リポジトリ', human_html)
            self.assertNotIn('☰', human_html)
            # 生の .md も残っていること
            self.assertTrue(readme.exists())
            self.assertTrue(use_doc.exists())
            # AI向け資料もブラウザ用HTMLを生成し、生の .md は残すこと
            self.assertTrue((site_dir / "For_AI.html").exists())
            self.assertTrue(for_ai.exists())
            ai_html = (site_dir / "For_AI.html").read_text(encoding="utf-8")
            self.assertNotIn('class="site-menu"', ai_html)
            self.assertIn('Raw Markdown', ai_html)
            self.assertNotIn('📄', ai_html)


class AiDocumentationStructureTests(unittest.TestCase):
    def test_manifest_is_the_only_question_route_definition(self) -> None:
        entry = (REPO_ROOT / "For_AI.md").read_text(encoding="utf-8")
        manifest = json.loads((REPO_ROOT / "for_ai" / "manifest.json").read_text(encoding="utf-8-sig"))

        self.assertIn("<ai_concierge_instruction", entry)
        self.assertNotIn("ai_conierge_instruction", entry)
        self.assertIn("<default_operational_mode>read_only_inquiry_response</default_operational_mode>", entry)
        self.assertNotIn("[ROUTE_KEY:", entry)
        self.assertNotIn("TARGET_NODES:", entry)
        self.assertIn("route_definition_policy", manifest)
        self.assertTrue(manifest["route_definition_policy"])

        route_ids = [route["id"] for route in manifest["routes"]]
        self.assertEqual(len(route_ids), len(set(route_ids)))
        self.assertIn("installation_and_updates", route_ids)
        self.assertIn("customization_and_settings", route_ids)
        self.assertIn("save_and_recovery_procedures", route_ids)
        self.assertIn("support_and_feedback", route_ids)
        self.assertNotIn("repository_navigation_and_evidence", route_ids)
        self.assertNotIn("code_reference", route_ids)
        self.assertNotIn("safe_change", route_ids)
        self.assertNotIn("ai_documentation_quality", route_ids)


class PublicSiteValidationTests(unittest.TestCase):
    @staticmethod
    def write_minimal_site(root: Path) -> None:
        for relative, text in {
            "index.html": (
                '<html><meta name="ai-agent-entrypoint" content="For_AI.md">'
                '<meta name="ai-manifest" content="for_ai/manifest.json">'
                '<a href="llms.txt">LLMs</a>'
                '<a href="for_ai/ai_context.md">Context summary</a>'
                '<a href="For_AI.md">AI</a>'
                '<a href="for_ai/manifest.json">Manifest</a>'
                '<a href="for_ai/project_context.xml">Context</a></html>'
            ),
            "llms.txt": (
                "# Test\n\n"
                "- [Context](for_ai/ai_context.md)\n"
                "- [AI](For_AI.md)\n"
                "- [Manifest](for_ai/manifest.json)\n"
                "- [Project context](for_ai/project_context.xml)"
            ),
            "For_AI.md": "# AI",
            "For_AI.html": "<html></html>",
            "README.md": "# README",
            "README.html": "<html></html>",
            "for_ai/project_context.xml": "<context/>",
            "for_ai/ai_context.md": "# AI context",
            "for_ai/ai_context.html": "<html></html>",
            "for_ai/manifest.json": json.dumps({
                "schema_version": 1,
                "entry_point": "../For_AI.md",
                "routes": [{"id": "overview", "when": "概要", "read": ["../README.md"]}],
            }),
        }.items():
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")

    def test_rejects_development_only_reference(self) -> None:
        with repo_tempdir() as root:
            for relative, text in {
                "index.html": "<html></html>",
                "For_AI.md": "# AI",
                "README.md": "# README\n\ndocs/internal/ is private.",
                "for_ai/manifest.json": '{"routes": []}',
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

    def test_rejects_missing_rendered_html_and_invalid_manifest_route(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "README.html").unlink()
            (root / "for_ai" / "manifest.json").write_text(json.dumps({
                "schema_version": 1,
                "entry_point": "../For_AI.md",
                "routes": [{"id": "overview", "when": "概要", "read": ["../../private.md"]}],
            }), encoding="utf-8")

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("rendered HTML is missing" in error for error in errors))
            self.assertTrue(any("manifest route 'overview' escapes public site" in error for error in errors))

    def test_rejects_missing_portal_machine_readable_metadata(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "index.html").write_text("<html></html>", encoding="utf-8")

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("ai-agent-entrypoint" in error for error in errors))
            self.assertTrue(any("ai-manifest" in error for error in errors))

    def test_rejects_missing_visible_ai_entry_link(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "index.html").write_text(
                '<html><meta name="ai-agent-entrypoint" content="For_AI.md">'
                '<meta name="ai-manifest" content="for_ai/manifest.json">'
                '<a href="For_AI.md">AI</a></html>',
                encoding="utf-8",
            )

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("must visibly link to AI entry document" in error for error in errors))

    def test_rejects_missing_llms_ai_document_link(self) -> None:
        with repo_tempdir() as root:
            self.write_minimal_site(root)
            (root / "llms.txt").write_text("# Test\n\n- [AI](For_AI.md)", encoding="utf-8")

            errors = validate_public_site.validate_site(root)

            self.assertTrue(any("llms.txt must link to AI document" in error for error in errors))


class IntroductionSiteValidationTests(unittest.TestCase):
    @staticmethod
    def write_minimal_site(root: Path) -> None:
        for relative, content in {
            "index.html": (
                '<a href="https://soone-y.github.io/OPEN_PDF-Note-Workspace/For_AI.md">AI</a>'
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
            "llms.txt": "https://soone-y.github.io/OPEN_PDF-Note-Workspace/For_AI.md\n",
            "_headers": "/*\n  Content-Signal: search=yes, ai-input=yes, ai-train=no, use=reference\n*/\n",
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
    def write_release(release_dir: Path) -> None:
        for relative_path in release_license_gate.REQUIRED_LICENSE_FILES:
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
                    if path.is_file() and path.relative_to(release_dir).as_posix() != "licenses/zlib/zlib.txt":
                        archive.write(path, Path(release_dir.name) / path.relative_to(release_dir))

            errors = release_license_gate.validate_release_zip(release_dir, zip_path)

            self.assertTrue(any("licenses/zlib/zlib.txt" in error for error in errors))

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


class ReleaseSetIntegrityGateTests(unittest.TestCase):
    @staticmethod
    def write_zip(release_dir: Path, zip_path: Path) -> None:
        with zipfile.ZipFile(zip_path, "w") as archive:
            for path in release_dir.rglob("*"):
                if path.is_file():
                    archive.write(path, Path(release_dir.name) / path.relative_to(release_dir))

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
            edition = "full" if directory == full else "lite"
            (directory / "pdf_note_workspace.exe.buildinfo.txt").write_text(
                "format\tpdf-note-build-info-v1\n"
                "version\t1.0.0\n"
                f"edition\t{edition}\n"
                f"artifact\tpdf_note_workspace.exe\t{release_set_integrity_gate.sha256_file(executable)}\n",
                encoding="utf-8",
            )
        (full / "libreoffice" / "custom_runtime" / "instdir").mkdir(parents=True)
        full_zip = release_set / "release_full.zip"
        lite_zip = release_set / "release_lite.zip"
        self.write_zip(full, full_zip)
        self.write_zip(lite, lite_zip)
        (release_set / "release_set_manifest.json").write_text(json.dumps({
            "app_version": "1.0.0",
            "components": {
                "release": full.name,
                "release_lite": lite.name,
                "public_snapshot": snapshot.name,
                "release_zip": full_zip.name,
                "release_lite_zip": lite_zip.name,
            }
        }), encoding="utf-8")
        allowlist = root / "allowlist.txt"
        allowlist.write_text("README.md\n", encoding="utf-8")
        release_set_integrity_gate.write_snapshot_manifest(release_set, allowlist)
        return release_set, snapshot, full_zip

    def test_accepts_recorded_snapshot_and_exact_zip_contents(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)

            self.assertEqual(release_set_integrity_gate.validate_release_set(release_set), [])

    def test_rejects_snapshot_change_after_manifest_creation(self) -> None:
        with repo_tempdir() as root:
            release_set, snapshot, _ = self.make_release_set(root)
            (snapshot / "README.md").write_text("changed after confirmation\n", encoding="utf-8")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("snapshot files differ" in error for error in errors))
            self.assertTrue(any("snapshot tree hash differs" in error for error in errors))

    def test_rejects_unexpected_zip_file(self) -> None:
        with repo_tempdir() as root:
            release_set, _, full_zip = self.make_release_set(root)
            with zipfile.ZipFile(full_zip, "a") as archive:
                archive.writestr("release_full/unexpected.txt", "not in the extracted release")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("unexpected files" in error for error in errors))

    def test_rejects_lite_runtime_or_wrong_build_version(self) -> None:
        with repo_tempdir() as root:
            release_set, _, _ = self.make_release_set(root)
            lite = release_set / "release_lite"
            (lite / "libreoffice" / "custom_runtime").mkdir(parents=True)
            build_info = lite / "pdf_note_workspace.exe.buildinfo.txt"
            build_info.write_text(build_info.read_text(encoding="utf-8").replace("version\t1.0.0", "version\t9.9.9"), encoding="utf-8")

            errors = release_set_integrity_gate.validate_release_set(release_set)

            self.assertTrue(any("Lite: LibreOffice custom runtime" in error for error in errors))
            self.assertTrue(any("Lite: application build-info version" in error for error in errors))


class ReleaseStartupSmokeGateTests(unittest.TestCase):
    def test_rejects_zip_path_traversal(self) -> None:
        with repo_tempdir() as root:
            archive = root / "bad.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("../outside.txt", "unsafe")

            with self.assertRaises(ValueError):
                release_startup_smoke_gate.safe_extract(archive, root / "extract")


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
    def test_builds_only_selected_files_and_keeps_machine_readable_index(self) -> None:
        with repo_tempdir() as root:
            (root / "for_ai" / "core").mkdir(parents=True)
            (root / "docs" / "public").mkdir(parents=True)
            (root / "docs" / "images").mkdir(parents=True)
            (root / "site" / "github").mkdir(parents=True)
            (root / ".github").mkdir()

            for name in (
                "index.html", "For_AI.md", "README.md", "LICENSE.md", "LICENSES_INDEX.md",
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
            (root / "for_ai" / "core" / "semantic_search_index.json").write_text("{}", encoding="utf-8")
            (root / "for_ai" / "core" / "internal.md").write_text("development-only", encoding="utf-8")
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
                            {"source": name, "destination": name}
                            for name in (
                                "For_AI.md", "README.md", "LICENSE.md",
                                "LICENSES_INDEX.md", "THIRD_PARTY_NOTICES.md",
                            )
                        ] + [
                            {"source": ".github/SECURITY.md", "destination": ".github/SECURITY.md"},
                        ],
                        "trees": [
                            {
                                "source": "for_ai",
                                "destination": "for_ai",
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
            self.assertTrue((site / "for_ai" / "core" / "semantic_search_index.json").exists())
            self.assertFalse((site / "for_ai" / "core" / "internal.md").exists())
            self.assertTrue((site / "docs" / "public" / "How_to_Use.md").exists())
            self.assertFalse((site / "docs" / "public" / "How_to_Build.md").exists())
            self.assertEqual((site / "docs" / "public" / "How_to_Use.md").read_text(encoding="utf-8"), "# Use 1.2.3")
            self.assertNotIn("How_to_Build.md](", (site / "README.md").read_text(encoding="utf-8"))
            self.assertNotIn("How_to_Build.md](", (site / "docs" / "public" / "Index.md").read_text(encoding="utf-8"))


class SyncPublicationInputsTests(unittest.TestCase):
    def test_syncs_explicit_documentation_files_without_document_tree_rule(self) -> None:
        with repo_tempdir() as root:
            source = root / "source"
            destination = root / "destination"
            for path, text in {
                "For_AI.md": "AI entry",
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
                        {"source": "For_AI.md", "destination": "For_AI.md"},
                        {"source": "docs/public/How_to_Use.md", "destination": "docs/public/How_to_Use.md"},
                    ],
                },
                "pages_submission": {"files": [], "trees": [], "retired_paths": []},
            }), encoding="utf-8")
            destination.mkdir()
            (destination / "for_ai").mkdir()
            (destination / "for_ai" / "project_context.xml").write_text("development-only", encoding="utf-8")

            sync_publication_inputs.sync_publication_inputs(source, destination)

            self.assertEqual((destination / "For_AI.md").read_text(encoding="utf-8"), "AI entry")
            self.assertEqual((destination / "docs/public/How_to_Use.md").read_text(encoding="utf-8"), "use")
            self.assertFalse((destination / "for_ai" / "project_context.xml").exists())

    def test_syncs_allowlisted_github_inputs_and_removes_retired_content(self) -> None:
        with repo_tempdir() as root:
            source = root / "source"
            destination = root / "destination"
            for path, text in {
                "For_AI.md": "AI entry",
                "for_ai/manifest.json": "{}",
                "docs/public/How_to_Use.md": "use",
                "docs/public/How_to_Build.md": "private",
                "docs/images/app.png": "image",
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
                    "files": [{"source": "For_AI.md", "destination": "For_AI.md"}],
                    "trees": [
                        {"source": "for_ai", "destination": "for_ai"},
                        {"source": "docs/images", "destination": "docs/images"},
                    ],
                    "document_markdown": {
                        "source": "docs/public", "destination": "docs/public", "exclude": ["How_to_Build.md"],
                    },
                },
                "pages_submission": {
                    "files": [{"source": ".github/workflows/static.yml", "destination": ".github/workflows/static.yml"}],
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

            sync_publication_inputs.sync_publication_inputs(source, destination)

            self.assertEqual((destination / "For_AI.md").read_text(encoding="utf-8"), "AI entry")
            self.assertTrue((destination / "for_ai/manifest.json").is_file())
            self.assertTrue((destination / "docs/images/app.png").is_file())
            self.assertTrue((destination / "docs/public/How_to_Use.md").is_file())
            self.assertFalse((destination / "docs/public/How_to_Build.md").exists())
            self.assertTrue((destination / ".github/workflows/static.yml").is_file())
            self.assertTrue((destination / "site/github/index.html").is_file())
            self.assertFalse((destination / "site/github/output").exists())
            self.assertFalse((destination / "site/github/scripts/sync_publication_inputs.py").exists())
            self.assertFalse((destination / "tools").exists())
            self.assertFalse((destination / "index.html").exists())

    def test_rejects_private_development_reference(self) -> None:
        with repo_tempdir() as root:
            destination = root / "destination"
            destination.mkdir()
            (destination / "For_AI.md").write_text(
                "DEV_PDF-Note-Workspace must not be published",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "private development reference"):
                sync_publication_inputs.validate_publication_inputs(destination)

if __name__ == "__main__":
    unittest.main()
