from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "libreoffice_upstream_conversion_test", ROOT / "tools/libreoffice/libreoffice_upstream_conversion_test.py")
upstream = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = upstream
spec.loader.exec_module(upstream)


class LibreOfficeUpstreamConversionTests(unittest.TestCase):
    def setUp(self):
        temp_root = ROOT / ".local/repo_resource/tmp"
        temp_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="upstream_unit_", dir=temp_root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.manifest = self.root / "manifest.json"
        self.payload = json.loads(upstream.MANIFEST.read_text(encoding="utf-8"))
        for item in self.payload["samples"]:
            path = self.source / item["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"mock upstream input")
            item["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.save_manifest()
        self.soffice = self.root / "runtime/program/soffice.com"
        self.soffice.parent.mkdir(parents=True)
        self.soffice.write_bytes(b"mock runtime")
        self.output = self.root / "result"

    def save_manifest(self):
        self.manifest.write_text(json.dumps(self.payload), encoding="utf-8")

    def convert(self, runtime, source, output, profile, timeout, **kwargs):
        self.assertFalse(kwargs["cleanup_runtime_pycache"])
        pdf = output / "source.pdf"
        with upstream.quality.fitz.open() as document:
            document.new_page()
            document.save(pdf)
        return pdf

    def run_mock(self, *, baseline=False):
        with redirect_stdout(io.StringIO()):
            return upstream.run_test(self.source, self.soffice, self.output,
                                     baseline_soffice=self.soffice if baseline else None,
                                     manifest_path=self.manifest)

    def test_manifest_tracks_only_metadata_and_expected_formats(self):
        entries = json.loads(upstream.MANIFEST.read_text(encoding="utf-8"))["samples"]
        self.assertEqual(len(entries), 11)
        self.assertEqual([sum(Path(e["path"]).suffix == suffix for e in entries)
                          for suffix in (".docx", ".pptx", ".xlsx")], [5, 1, 5])

    def test_missing_or_changed_sample_fails_before_conversion(self):
        first = self.source / self.payload["samples"][0]["path"]
        for mutation in (lambda: first.unlink(), lambda: first.write_bytes(b"changed")):
            first.write_bytes(b"mock upstream input")
            mutation()
            with mock.patch.object(upstream.smoke, "convert_one") as convert:
                with self.assertRaises((FileNotFoundError, ValueError)):
                    self.run_mock()
                convert.assert_not_called()
            self.assertFalse(self.output.exists())

    def test_empty_or_duplicate_corpus_is_rejected(self):
        for entries in ([], [self.payload["samples"][0]] * 11):
            self.payload["samples"] = entries
            self.save_manifest()
            with self.assertRaises(ValueError):
                self.run_mock()

    def test_output_cannot_overlap_source_runtime_or_existing_evidence(self):
        for output in (self.source / "result", self.soffice.parent / "result", self.source):
            self.output = output
            with self.assertRaises(ValueError):
                self.run_mock()
        self.output = self.root / "existing"
        self.output.mkdir()
        with self.assertRaises(FileExistsError):
            self.run_mock()

    def test_conversion_only_reports_quality_not_completed(self):
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=self.convert):
            report = self.run_mock()
        self.assertEqual(report["summary"]["completed_documents"], 11)
        self.assertEqual(report["summary"]["quality_comparison"], "not-completed")
        self.assertEqual(report["summary"]["status"], "PASS")

    def test_conversion_failure_is_recorded_and_report_fails(self):
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=RuntimeError("missing Calc")):
            report = self.run_mock()
        self.assertEqual(report["summary"]["errors"], 11)
        self.assertEqual(report["summary"]["status"], "FAIL")
        self.assertTrue((self.output / "report.json").is_file())

    def test_baseline_failure_still_runs_candidate_and_uses_separate_copies(self):
        calls = []
        def convert(*args, **kwargs):
            calls.append(args[1])
            if args[3].parent.name == "baseline":
                args[1].write_bytes(b"damaged baseline copy")
                raise RuntimeError("baseline failure")
            self.assertEqual(args[1].read_bytes(), b"mock upstream input")
            return self.convert(*args, **kwargs)
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=convert):
            report = self.run_mock(baseline=True)
        self.assertEqual(len(calls), 22)
        self.assertEqual(calls[0], calls[1])
        self.assertEqual((self.output / "case_00/baseline/source.docx").read_bytes(), b"damaged baseline copy")
        self.assertEqual(report["scores"]["runtimes"]["baseline"]["failure"], 11)
        self.assertEqual(report["scores"]["runtimes"]["candidate"]["success"], 11)
        self.assertEqual(report["scores"]["outcomes"], {"candidate_only_success": 11})
        self.assertIsNone(report["scores"]["equality_rate"])
        self.assertFalse(report["summary"]["acceptance_passed"])
        self.assertTrue((self.output / "results.csv").is_file())

    def test_candidate_failure_is_distinct_from_both_failed(self):
        def convert(*args, **kwargs):
            if args[3].parent.name == "candidate":
                raise RuntimeError("candidate failure")
            return self.convert(*args, **kwargs)
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=convert):
            report = self.run_mock(baseline=True)
        self.assertEqual(report["scores"]["outcomes"], {"baseline_only_success": 11})
        self.assertEqual(report["scores"]["runtimes"]["candidate"]["failure"], 11)
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=RuntimeError("both fail")):
            self.output = self.root / "both_failed"
            report = self.run_mock(baseline=True)
        self.assertEqual(report["scores"]["outcomes"], {"both_failed": 11})

    def test_printed_input_path_does_not_create_a_comparison_difference(self):
        def convert(runtime, source, output, profile, timeout, **kwargs):
            self.assertTrue(kwargs["repeatable_random"])
            pdf = output / "source.pdf"
            with upstream.quality.fitz.open() as document:
                document.new_page().insert_text((36, 36), str(source))
                document.save(pdf)
            return pdf
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=convert):
            report = self.run_mock(baseline=True)
        self.assertEqual(report["scores"]["outcomes"], {"both_success_equal": 11})
        for item in report["results"]:
            paths = [Path(run["input_snapshot"]) for run in item["runs"].values()]
            self.assertNotEqual(paths[0], paths[1])
            self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())

    def test_comparison_failure_does_not_reduce_conversion_success_rate(self):
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=self.convert), \
             mock.patch.object(upstream.quality, "compare_pdfs", side_effect=RuntimeError("comparison failed")):
            report = self.run_mock(baseline=True)
        self.assertEqual(report["scores"]["outcomes"], {"comparison_failed": 11})
        self.assertEqual(report["scores"]["runtimes"]["baseline"]["success_rate"], 1.0)
        self.assertEqual(report["scores"]["compared_documents"], 0)
        self.assertIsNone(report["scores"]["equality_rate"])

    def test_score_denominators_include_conversion_failures(self):
        records = [
            {"outcome": "both_success_equal", "changed": False, "error": None,
             "runs": {"baseline": {"status": "SUCCESS"}, "candidate": {"status": "SUCCESS"}}},
            {"outcome": "both_failed", "changed": None, "error": "failure",
             "runs": {"baseline": {"status": "FAIL"}, "candidate": {"status": "FAIL"}}},
        ]
        score = upstream.score_results(records, ["baseline", "candidate"])
        self.assertEqual(score["runtimes"]["candidate"]["success_rate"], 0.5)
        self.assertEqual(score["compared_documents"], 1)
        self.assertEqual(score["equality_rate"], 1.0)

    def test_checkpoint_retries_transient_denial_and_propagates_permanent_denial(self):
        with mock.patch.object(upstream.quality, "write_json_atomic", side_effect=[PermissionError("shared"), None]) as write, \
             mock.patch.object(upstream.time, "sleep"):
            upstream.write_checkpoint(self.output / "partial.json", {"status": "INCOMPLETE"})
        self.assertEqual(write.call_count, 2)
        with mock.patch.object(upstream.quality, "write_json_atomic", side_effect=PermissionError("denied")) as write, \
             mock.patch.object(upstream.time, "sleep"):
            with self.assertRaises(PermissionError):
                upstream.write_checkpoint(self.output / "partial.json", {})
        self.assertEqual(write.call_count, 11)

    def test_actual_pdf_comparison_detects_text_and_pixel_change(self):
        before, after = self.root / "before.pdf", self.root / "after.pdf"
        for path, text in ((before, "present"), (after, "missing")):
            with upstream.quality.fitz.open() as document:
                document.new_page().insert_text((72, 72), text)
                document.save(path)
        comparison = upstream.quality.compare_pdfs(before, after, self.root / "review",
                                                   dpi=144, pixel_threshold=8)
        self.assertTrue(upstream.comparison_changed(comparison))

    def test_comparison_detects_each_quality_dimension_and_rejects_zero_pages(self):
        same = {"structural_issues": [], "semantic_issues": [], "reference_only_pdf_fonts": [],
                "candidate_only_pdf_fonts": [], "pages": [{"text_similarity": 1.0, "different_pixels": 0}]}
        self.assertFalse(upstream.comparison_changed(same))
        for key in ("structural_issues", "semantic_issues", "reference_only_pdf_fonts", "candidate_only_pdf_fonts"):
            self.assertTrue(upstream.comparison_changed({**same, key: ["changed"]}))
        self.assertTrue(upstream.comparison_changed({**same, "pages": [{"text_similarity": 0.9, "different_pixels": 0}]}))
        self.assertTrue(upstream.comparison_changed({**same, "pages": [{"text_similarity": 1.0, "different_pixels": 1}]}))
        with self.assertRaises(ValueError):
            upstream.comparison_changed({**same, "pages": []})

    def test_incomplete_comparison_is_not_passed(self):
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=self.convert), \
             mock.patch.object(upstream.quality, "compare_pdfs", side_effect=RuntimeError("comparison failed")):
            report = self.run_mock(baseline=True)
        self.assertEqual(report["summary"]["status"], "FAIL")
        self.assertEqual(report["summary"]["quality_comparison"], "not-completed")

    def test_changed_conversion_copy_fails(self):
        def mutate(*args, **kwargs):
            pdf = self.convert(*args, **kwargs)
            args[1].write_bytes(b"changed by converter")
            return pdf
        with mock.patch.object(upstream.smoke, "convert_one", side_effect=mutate):
            report = self.run_mock()
        self.assertEqual(report["summary"]["errors"], 11)

    def test_cli_propagates_incomplete_and_failed_check(self):
        args = ["--source-dir", str(self.source)]
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            with mock.patch.object(upstream, "run_test", side_effect=FileNotFoundError("missing sample")):
                self.assertEqual(upstream.main(args), 2)
            with mock.patch.object(upstream, "run_test", return_value={"summary": {"status": "FAIL"}}):
                self.assertEqual(upstream.main(args), 1)


if __name__ == "__main__":
    unittest.main()
