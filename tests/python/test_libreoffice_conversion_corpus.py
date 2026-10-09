from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/libreoffice"))
import libreoffice_conversion_corpus as corpus
import libreoffice_upstream_conversion_test as upstream
import libreoffice_runtime_removal_trial as removal


class LibreOfficeConversionCorpusTests(unittest.TestCase):
    def setUp(self):
        temp_root = ROOT / ".local/repo_resource/tmp"
        temp_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="corpus_unit_", dir=temp_root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.paths = ["sw/qa/extras/ooxmlexport/data/embedded.docx", "sd/qa/unit/data/pptx/slide.pptx", "sc/qa/unit/data/xlsx/sheet.xlsx"]
        for relative in self.paths:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("word/embeddings/oleObject1.bin", b"mock")
                archive.writestr("xl/worksheets/sheet1.xml", b'<worksheet><f>1+1</f><conditionalFormatting/></worksheet>')
                archive.writestr("xl/pivotTables/pivotTable1.xml", b'<pivotTableDefinition/>')
        self.catalog = corpus.build_catalog(self.source, "unit-source")
        self.catalog_path = self.root / "catalog.json"
        self.save()
        self.runtime = self.root / "runtime/program/soffice.com"
        self.runtime.parent.mkdir(parents=True)
        self.runtime.write_bytes(b"mock runtime")

    def save(self):
        self.catalog_path.write_text(json.dumps(self.catalog), encoding="utf-8")

    def convert(self, runtime, source, output, profile, timeout, **kwargs):
        self.assertTrue(kwargs["own_process_tree"])
        self.assertFalse(kwargs["cleanup_runtime_pycache"])
        pdf = output / "source.pdf"
        with upstream.quality.fitz.open() as document:
            document.new_page().insert_text((72, 72), "fixed input")
            document.save(pdf)
        return pdf

    def run_acceptance(self):
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=self.convert), redirect_stdout(io.StringIO()):
            return upstream.run_test(self.source, self.runtime, self.root / "result",
                                     baseline_soffice=self.runtime, manifest_path=self.catalog_path, acceptance=True)

    def test_discovery_keeps_every_path_including_duplicate_content_and_bad_containers(self):
        malformed = self.source / "sd/qa/unit/data/pptx/fail/broken.pptx"
        malformed.parent.mkdir(parents=True)
        malformed.write_bytes(b"broken zip")
        outside = self.source / "workdir/generated.docx"
        outside.parent.mkdir()
        outside.write_bytes(b"not QA")
        result = corpus.build_catalog(self.source, "unit")
        self.assertEqual(result["sample_count"], 4)
        item = next(item for item in result["samples"] if item["path"].endswith("broken.pptx"))
        self.assertIn("inspection-incomplete", item["groups"])
        self.assertIn("exception-candidates", item["groups"])
        self.assertEqual(len({item["sha256"] for item in result["samples"]}), 2)

    def test_classification_uses_container_features(self):
        groups = corpus.classify(self.source, self.paths[0])
        for group in ("embedded", "calc-related", "formulas", "conditional-format", "pivot", "writer-export"):
            self.assertIn(group, groups)

    def fixture_metadata(self):
        item = self.catalog["samples"][0]
        original = self.source / item["path"]
        payload = original.read_bytes()
        original.write_bytes(corpus.decode_rc4_cve(payload))
        item["sha256"] = corpus.sha256(original)
        source_test = self.source / "unotest/source/cpp/filters-test.cxx"
        source_test.parent.mkdir(parents=True)
        source_test.write_bytes(b"mock official preparation")
        entry = {"path": item["path"], "codec": "rc4-cve", "raw_sha256": item["sha256"],
                 "decoded_sha256": hashlib.sha256(payload).hexdigest()}
        metadata = {"version": 1, "source_test": source_test.relative_to(self.source).as_posix(),
                    "source_test_sha256": corpus.sha256(source_test), "samples": [entry]}
        path = self.root / "encodings.json"
        path.write_text(json.dumps(metadata), encoding="utf-8")
        self.save()
        return item, original, payload, source_test, entry, metadata, path

    def test_fixture_preparation_pins_original_payload_and_official_source(self):
        item, original, payload, source_test, entry, metadata, path = self.fixture_metadata()
        raw = original.read_bytes()
        with mock.patch.object(corpus, "ENCODINGS", path):
            encodings = corpus.fixture_encodings(self.source, self.catalog["samples"])
        staged = self.root / "input.docx"
        self.assertEqual(corpus.prepare_input(original, staged, item, encodings[item["path"]]), entry["decoded_sha256"])
        self.assertEqual(staged.read_bytes(), payload)
        self.assertEqual(original.read_bytes(), raw)
        source_test.write_bytes(b"changed official source")
        with mock.patch.object(corpus, "ENCODINGS", path), self.assertRaisesRegex(ValueError, "preparation source changed"):
            corpus.fixture_encodings(self.source, self.catalog["samples"])

    def test_fixture_raw_and_decoded_hash_errors_never_prepare_input(self):
        item, original, payload, source_test, entry, metadata, path = self.fixture_metadata()
        staged = self.root / "input.docx"
        with self.assertRaisesRegex(ValueError, "Decoded input hash mismatch"):
            corpus.prepare_input(original, staged, item, {**entry, "decoded_sha256": "0" * 64})
        self.assertFalse(staged.exists())
        original.write_bytes(b"changed raw fixture")
        with self.assertRaisesRegex(ValueError, "Original input hash mismatch"):
            corpus.prepare_input(original, staged, item, entry)
        with mock.patch.object(corpus, "ENCODINGS", path), self.assertRaisesRegex(ValueError, "raw hash mismatch"):
            corpus.fixture_encodings(self.source, self.catalog["samples"])

    def test_encoded_input_runs_both_converters_with_payload_and_records_both_hashes(self):
        item, original, payload, source_test, entry, metadata, path = self.fixture_metadata()
        calls = []
        selected_case = f"case_{self.catalog['samples'].index(item):02d}"
        def convert(runtime, source, *args, **kwargs):
            if source.parent.parent.name == selected_case:
                self.assertEqual(source.read_bytes(), payload)
                calls.append(source)
            return self.convert(runtime, source, *args, **kwargs)
        with mock.patch.object(corpus, "ENCODINGS", path), \
             mock.patch.object(upstream.smoke, "convert_one", side_effect=convert), redirect_stdout(io.StringIO()):
            report = upstream.run_test(self.source, self.runtime, self.root / "result",
                                      baseline_soffice=self.runtime, manifest_path=self.catalog_path, acceptance=True)
        record = next(row for row in report["results"] if row["path"] == item["path"])
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], calls[1])
        self.assertEqual(record["sha256"], entry["raw_sha256"])
        self.assertEqual(record["conversion_input_sha256"], entry["decoded_sha256"])
        self.assertEqual(report["fixture_encodings_sha256"], corpus.sha256(path))
        self.assertEqual(record["outcome"], "both_success_equal")
        self.assertEqual(corpus.sha256(original), entry["raw_sha256"])

    def test_encoding_metadata_change_fails_without_final_report(self):
        item, original, payload, source_test, entry, metadata, path = self.fixture_metadata()
        def convert(*args, **kwargs):
            path.write_text(json.dumps(metadata) + " ", encoding="utf-8")
            return self.convert(*args, **kwargs)
        with mock.patch.object(corpus, "ENCODINGS", path), \
             mock.patch.object(upstream.smoke, "convert_one", side_effect=convert), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "encoding metadata changed"):
                upstream.run_test(self.source, self.runtime, self.root / "result",
                                  baseline_soffice=self.runtime, manifest_path=self.catalog_path, acceptance=True)
        self.assertFalse((self.root / "result/report.json").exists())

    def test_group_individual_union_and_unknown_selector_rejection(self):
        selected = corpus.select(self.catalog, ["format-docx"], [self.paths[1], self.paths[0]], acceptance=False)
        self.assertEqual({item["path"] for item in selected}, set(self.paths[:2]))
        for groups, samples in ((["unknown"], []), ([], ["not-in-catalog.docx"])):
            with self.assertRaises(ValueError):
                corpus.select(self.catalog, groups, samples, acceptance=False)

    def test_vml_formula_and_table_style_do_not_claim_cell_formula_or_table_content(self):
        path = self.source / self.paths[0]
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("word/document.xml", b'<document><v:f eqn="drawing"/></document>')
            archive.writestr("word/styles.xml", b'<styles><w:tblPr/></styles>')
        groups = corpus.classify(self.source, self.paths[0])
        self.assertNotIn("formulas", groups)
        self.assertNotIn("tables", groups)

    def test_acceptance_rejects_subsets_and_requires_baseline(self):
        for groups, samples in ((["all"], []), ([], [self.paths[0]])):
            with self.assertRaises(ValueError):
                corpus.select(self.catalog, groups, samples, acceptance=True)
        with self.assertRaises(ValueError):
            upstream.run_test(self.source, self.runtime, self.root / "result", manifest_path=self.catalog_path, acceptance=True)

    def test_acceptance_detects_added_missing_and_changed_inputs(self):
        extra = self.source / "sc/qa/unit/data/xlsx/extra.xlsx"
        extra.write_bytes(b"new")
        with self.assertRaises(ValueError):
            upstream.collect_samples(self.source, self.catalog_path, acceptance=True)
        extra.unlink()
        first = self.source / self.paths[0]
        saved = first.read_bytes()
        first.unlink()
        with self.assertRaises(ValueError):
            upstream.collect_samples(self.source, self.catalog_path, acceptance=True)
        first.write_bytes(saved + b"changed")
        with self.assertRaises(ValueError):
            upstream.collect_samples(self.source, self.catalog_path, acceptance=True)

    def test_group_counts_and_scope_cannot_hide_missing_entries(self):
        for mutation in (lambda: self.catalog.update(sample_count=2),
                         lambda: self.catalog["groups"]["all"].update(count=2),
                         lambda: self.catalog.update(extensions=[".docx"])):
            self.catalog = corpus.build_catalog(self.source, "unit")
            mutation()
            self.save()
            with self.assertRaises(ValueError):
                corpus.read_catalog(self.catalog_path)

    def test_full_acceptance_pass_and_removal_gate_bind_catalog(self):
        report = self.run_acceptance()
        self.assertTrue(report["summary"]["acceptance_passed"])
        self.assertEqual(report["summary"]["completed_documents"], 3)
        self.assertTrue(removal.upstream_report_passed(report, acceptance=True, catalog_path=self.catalog_path))
        for field, value in (("catalog_sha256", "changed"), ("selected_groups", ["all"]), ("acceptance", False)):
            changed = copy.deepcopy(report)
            changed[field] = value
            self.assertFalse(removal.upstream_report_passed(changed, acceptance=True, catalog_path=self.catalog_path))
        changed = copy.deepcopy(report)
        changed["results"].pop()
        self.assertFalse(removal.upstream_report_passed(changed, acceptance=True, catalog_path=self.catalog_path))
        self.assertEqual(list((self.root / "result").rglob("page_*.png")), [])

    def test_full_acceptance_failure_does_not_exclude_baseline_failure(self):
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=RuntimeError("baseline failed")), redirect_stdout(io.StringIO()):
            report = upstream.run_test(self.source, self.runtime, self.root / "result",
                                       baseline_soffice=self.runtime, manifest_path=self.catalog_path, acceptance=True)
        self.assertFalse(report["summary"]["acceptance_passed"])
        self.assertEqual(report["summary"]["errors"], 3)
        self.assertEqual(report["summary"]["expected_documents"], 3)
        self.assertTrue((self.root / "result/case_00/result.json").is_file())
        self.assertEqual(json.loads((self.root / "result/partial.json").read_text())["status"], "INCOMPLETE")

    def test_acceptance_detects_runtime_change(self):
        def mutate(*args, **kwargs):
            pdf = self.convert(*args, **kwargs)
            self.runtime.write_bytes(b"changed")
            return pdf
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=mutate), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "Runtime identity changed"):
                upstream.run_test(self.source, self.runtime, self.root / "result",
                                  baseline_soffice=self.runtime, manifest_path=self.catalog_path, acceptance=True)
        self.assertFalse((self.root / "result/report.json").exists())

    def test_removal_command_carries_groups_and_acceptance(self):
        args = SimpleNamespace(upstream_source_dir="source", timeout=30, acceptance=False,
                               upstream_group=["calc-related", "math"], upstream_sample=[self.paths[0]])
        command = removal.upstream_command(args, ROOT, self.runtime.parent.parent, self.runtime.parent.parent, self.root / "result")
        self.assertEqual(command.count("--group"), 2)
        self.assertIn("--sample", command)
        args.acceptance = True; args.upstream_group = []; args.upstream_sample = []
        self.assertIn("--acceptance", removal.upstream_command(args, ROOT, self.root, self.root, self.root / "result"))

    def test_removal_timeout_tracks_selected_count_and_fits_platform_wait(self):
        import threading
        args = SimpleNamespace(timeout=180, acceptance=False, upstream_group=["format-docx"], upstream_sample=[])
        self.assertEqual(removal.upstream_timeout(args, self.catalog_path), 780)
        args.acceptance = True; args.upstream_group = []
        self.assertEqual(removal.upstream_timeout(args, self.catalog_path), 2100)
        args.timeout = int(threading.TIMEOUT_MAX)
        self.assertLess(removal.upstream_timeout(args, self.catalog_path), threading.TIMEOUT_MAX)
        result = removal.run_command([sys.executable, "-c", "pass"], ROOT, removal.upstream_timeout(args, self.catalog_path))
        self.assertEqual(result["exit_code"], 0)

    def test_timeout_terminates_owned_process_tree(self):
        process = mock.Mock(pid=123, returncode=-1)
        process.communicate.side_effect = [subprocess.TimeoutExpired("converter", 1), ("", None)]
        process.poll.side_effect = [None, None]
        with mock.patch.object(upstream.smoke.subprocess, "Popen", return_value=process), \
             mock.patch.object(upstream.smoke.subprocess, "run") as taskkill:
            with self.assertRaises(subprocess.TimeoutExpired):
                upstream.smoke.run_owned_conversion(["converter"], self.root, {}, 1)
        taskkill.assert_called_once()
        self.assertEqual(taskkill.call_args.args[0], ["taskkill", "/PID", "123", "/T", "/F"])
        process.kill.assert_called_once()

    def test_repeatable_random_seed_changes_child_environment_only(self):
        import os
        output = self.root / "seed_output"
        output.mkdir()
        source = self.source / self.paths[0]
        (output / (source.stem + ".pdf")).write_bytes(b"mock PDF")
        completed = SimpleNamespace(returncode=0, stdout="")
        with mock.patch.dict(os.environ, {"SAL_RAND_REPEATABLE": "parent-value"}), \
             mock.patch.object(upstream.smoke, "run_owned_conversion", return_value=completed) as run, \
             mock.patch.object(upstream.smoke, "validate_pdf"):
            upstream.smoke.convert_one(self.runtime, source, output, self.root / "seed_case/profile", 30,
                                       own_process_tree=True, cleanup_runtime_pycache=False, repeatable_random=True)
            self.assertEqual(os.environ["SAL_RAND_REPEATABLE"], "parent-value")
        self.assertEqual(run.call_args.args[2]["SAL_RAND_REPEATABLE"], "1")

    def test_profile_configuration_uses_supported_registry_items_root_and_absolute_urls(self):
        import xml.etree.ElementTree as xml
        profile = self.root / "profile_config"
        upstream.smoke.write_profile_path_config(profile)
        tree = xml.parse(profile / "user/registrymodifications.xcu")
        namespace = "{http://openoffice.org/2001/registry}"
        # Official configmgr/source/xcuparser.cxx accepts oor:items for user
        # registry modifications; oor:data is not a supported root.
        self.assertEqual(tree.getroot().tag, namespace + "items")
        paths = {item.get(namespace + "path"): item for item in tree.getroot()}
        for name in ("Work", "Backup", "Temp"):
            item = paths["/org.openoffice.Office.Paths/Paths/" + name]
            prop = next(p for p in item if p.get(namespace + "name") == "WritePath")
            expected = (profile.parent / "local" / name.lower()).resolve().as_uri()
            self.assertEqual(prop.find("value").text, expected)

    def test_removal_quality_exit_failure_cannot_be_hidden_by_identical_reports(self):
        baseline = self.root / "baseline.json"
        baseline.write_text(json.dumps({"summary": {}, "results": []}), encoding="utf-8")
        unused = self.runtime.parent / "unused.dll"
        unused.write_bytes(b"disposable")
        args = SimpleNamespace(runtime_root=str(self.runtime.parent.parent), input_dir=str(self.source),
                               output=str(self.root / "trial.json"), remove=["program/unused.dll"], remove_list=None,
                               work_root=str(self.root / "work"), timeout=30, keep_candidate=True,
                               baseline_report=str(baseline), upstream_source_dir=None, acceptance=False,
                               upstream_group=[], upstream_sample=[])
        def command(command, cwd, timeout):
            quality_command = any(str(part).endswith("libreoffice_conversion_quality_test.py") for part in command)
            if quality_command:
                report = self.root / "work/quality/quality_report.json"
                report.parent.mkdir(parents=True)
                report.write_text(baseline.read_text(encoding="utf-8"), encoding="utf-8")
            return {"exit_code": 1 if quality_command else 0, "output": "", "timed_out": False}
        with mock.patch.object(removal, "parse_args", return_value=args), \
             mock.patch.object(removal, "run_command", side_effect=command), redirect_stdout(io.StringIO()):
            self.assertEqual(removal.main(), 1)
        self.assertFalse(json.loads((self.root / "trial.json").read_text())["acceptance_passed"])
        self.assertTrue(unused.is_file())

    def test_removal_acceptance_requires_complete_upstream_evidence_even_when_command_exits_zero(self):
        unused = self.runtime.parent / "unused.dll"
        unused.write_bytes(b"disposable")
        checker = removal.upstream_report_passed
        for produce_report in (False, True):
            with self.subTest(produce_report=produce_report):
                case = self.root / ("with_report" if produce_report else "without_report")
                args = SimpleNamespace(runtime_root=str(self.runtime.parent.parent), input_dir=str(self.source),
                                       output=str(case / "trial.json"), remove=["program/unused.dll"], remove_list=None,
                                       work_root=str(case / "work"), timeout=30, keep_candidate=False,
                                       baseline_report=None, upstream_source_dir=str(self.source), acceptance=True,
                                       upstream_group=[], upstream_sample=[])
                def command(command, cwd, timeout):
                    if any(str(part).endswith("libreoffice_upstream_conversion_test.py") for part in command):
                        self.assertIn("--acceptance", command)
                        if produce_report:
                            upstream.run_test(self.source, case / "work/candidate/program/soffice.com", case / "work/upstream",
                                              baseline_soffice=self.runtime, manifest_path=self.catalog_path, acceptance=True)
                    return {"exit_code": 0, "output": "", "timed_out": False}
                with mock.patch.object(removal, "parse_args", return_value=args), \
                     mock.patch.object(removal, "run_command", side_effect=command), \
                     mock.patch.object(upstream.smoke, "convert_one", side_effect=self.convert), \
                     mock.patch.object(removal, "upstream_report_passed", side_effect=lambda report, acceptance: checker(report, acceptance=acceptance, catalog_path=self.catalog_path)), \
                     redirect_stdout(io.StringIO()):
                    self.assertEqual(removal.main(), 0 if produce_report else 1)
                report = json.loads((case / "trial.json").read_text())
                self.assertEqual(report["acceptance_passed"], produce_report)
                self.assertTrue(report["kept_candidate"])
                self.assertTrue((case / "work/candidate").is_dir())
        self.assertTrue(unused.is_file())


if __name__ == "__main__":
    unittest.main()
