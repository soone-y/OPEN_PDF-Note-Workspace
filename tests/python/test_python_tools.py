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
analyze_repo = load_module("analyze_repo", "tools/metrics/code_metrics/analyze_repo.py")
code_metrics_gui = load_module("code_metrics_gui", "tools/metrics/code_metrics/gui.py")
libreoffice_reduce = load_module("libreoffice_reduce", "tools/libreoffice/libreoffice_reduce.py")
libreoffice_smoke_test = load_module("libreoffice_smoke_test", "tools/libreoffice/libreoffice_smoke_test.py")
libreoffice_conversion_quality_test = load_module(
    "libreoffice_conversion_quality_test", "tools/libreoffice/libreoffice_conversion_quality_test.py"
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

    def test_main_applies_app_version_to_version_tracked_documents_only_in_snapshot(self) -> None:
        with repo_tempdir() as root:
            ops = root / "docs" / "internal" / "operations"
            ops.mkdir(parents=True)
            allowlist = ops / "allowlist.txt"
            gitignore_template = ops / "public.gitignore"
            allowlist.write_text("APP_VERSION.txt\nREADME.md\nLICENSE.md\n", encoding="utf-8")
            gitignore_template.write_text("out/\n", encoding="utf-8")
            (root / "APP_VERSION.txt").write_text("0.8.48\n", encoding="utf-8")
            (root / "README.md").write_text("# Project\n\n対象アプリ版: __APP_VERSION__\n", encoding="utf-8")
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
            self.assertIn("対象アプリ版: 0.8.48", (dest / "README.md").read_text(encoding="utf-8"))
            self.assertIn("対象アプリ版: __APP_VERSION__", (root / "README.md").read_text(encoding="utf-8"))
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


if __name__ == "__main__":
    unittest.main()
