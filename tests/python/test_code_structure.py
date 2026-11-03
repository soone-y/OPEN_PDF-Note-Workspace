"""Offline AST fixtures. Dependencies missing => FAIL, never silently skip."""
from __future__ import annotations

import copy
from contextlib import redirect_stdout, redirect_stderr
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import uuid
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.metrics.code_structure.analyze import analyze, main
from tools.metrics.code_structure.compare import compare
from tools.metrics.code_structure.inputs import Snapshot, safe_arguments, validate_signature, REVIEWED_DRIVERS
from tools.metrics.code_structure.model import AnalysisError, empty_report, stable_id, digest_bytes
from tools.metrics.code_structure.report import markdown, save_outputs, source_composition_markdown
from tools.metrics.code_structure.literals import body_composition, literal_kind
from tools.metrics.code_structure.worker import run_worker
from tools.metrics.code_structure.worker import DEADLINE_METHOD
from tools.metrics.code_structure.worker import ActiveClock
from tools.metrics.code_structure.observations import distribution, graph_cycles, merged_bytes, profile
from tools.metrics.code_structure.inventory import build_inventory, cpp_bytes, select_group
from tools.metrics.code_structure.survey import combine, coverage, validate_plan, survey, survey_markdown, main as survey_main
from tools.metrics.code_metrics import analyze_repo
from tools.metrics.code_structure.batch import run_batch, create_json, verify_report, run_lock, verify_execution_deadline
from tools.metrics.code_structure.responsibilities import derive
from tools.metrics.code_structure.rollup import Accumulator
from tools.metrics.code_structure.unused import unused_profile


class RepositoryMetricInputTests(unittest.TestCase):
    def test_decode_failure_is_not_reclassified_as_binary_or_success(self):
        root = ROOT / ".local/repo_resource/tmp" / ("metrics_decode_" + uuid.uuid4().hex)
        root.mkdir(parents=True)
        (root / "bad.md").write_bytes(b"\x81\x00\xff")
        report = root / "result.txt"
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as error:
            code = analyze_repo.main(["--root", str(root), "--scope", "all", "--report", str(report)])
        self.assertEqual(code, 2)
        self.assertIn("ANALYSIS_INCOMPLETE", error.getvalue())
        self.assertIn("bad.md", error.getvalue())
        self.assertFalse(report.exists())

    def test_utf16_bom_and_decoder_failure(self):
        root = ROOT / ".local/repo_resource/tmp" / ("metrics_utf16_" + uuid.uuid4().hex)
        root.mkdir(parents=True)
        file = root / "good.txt"
        file.write_bytes("日本語".encode("utf-16"))
        self.assertEqual(analyze_repo.read_text(file), "日本語")
        file.write_bytes(b"\xff\xfe\x00")
        with self.assertRaises(ValueError):
            analyze_repo.read_text(file)


class BodyPresenceTests(unittest.TestCase):
    def fixture(self):
        root = ROOT / ".local/repo_resource/tmp" / ("body_presence_" + uuid.uuid4().hex)
        root.mkdir(parents=True)
        source = root / "declarations.cpp"
        header = root / "declarations.h"
        source.write_text('#include "declarations.h"\n', encoding="utf-8")
        header.write_text('namespace api { bool Open(int value); }\n', encoding="utf-8")
        database = root / "commands.json"
        database.write_text(json.dumps([{"directory": str(root), "file": str(source),
                                        "arguments": [shutil.which("clang++"), "-std=c++17", "-c", str(source)]}]), encoding="utf-8")
        policy = root / "tools/metrics/code_structure/body_presence.json"
        policy.parent.mkdir(parents=True)
        contract = {"version": 1, "declaration_only_units": [{"source": "declarations.cpp",
                    "source_sha256": digest_bytes(source.read_bytes()), "headers": [{"path": "declarations.h",
                    "sha256": digest_bytes(header.read_bytes())}], "declarations": ["api::Open"]}]}
        policy.write_text(json.dumps(contract), encoding="utf-8")
        return root, database, policy, contract

    def test_explicit_declaration_only_tu_is_parsed_with_presence_evidence(self):
        root, database, _, _ = self.fixture()
        report, _ = analyze(root, ["declarations.cpp"], [root], database=database)
        self.assertEqual(report["status"], "COMPLETE", report["diagnostics"])
        self.assertEqual(report["metrics"]["completed_tu_configurations"], 1)
        self.assertEqual(report["functions"], [])
        self.assertEqual(report["translation_units"][0]["body_presence"]["kind"], "fingerprinted-declaration-only")
        self.assertEqual(report["translation_units"][0]["body_presence"]["observed_declarations"], ["api::Open"])
        self.assertTrue(next(x for x in report["files"] if x["path"] == "declarations.h")["participates"])
        self.assertIn("function-body metrics are unmeasured", markdown(report))

    def test_no_body_without_explicit_contract_is_failure(self):
        root, database, policy, _ = self.fixture()
        policy.write_text(json.dumps({"version": 1, "declaration_only_units": []}), encoding="utf-8")
        report, _ = analyze(root, ["declarations.cpp"], [root], database=database)
        self.assertEqual(report["status"], "FAILED")
        self.assertEqual(report["metrics"]["completed_tu_configurations"], 0)

    def test_declaration_only_source_and_header_changes_are_failure(self):
        for path in ("declarations.cpp", "declarations.h"):
            root, database, _, _ = self.fixture()
            with (root / path).open("a", encoding="utf-8") as stream:
                stream.write("// changed input\n")
            report, _ = analyze(root, ["declarations.cpp"], [root], database=database)
            self.assertEqual(report["status"], "FAILED", path)

    def test_missing_expected_declaration_is_failure(self):
        root, database, policy, contract = self.fixture()
        contract["declaration_only_units"][0]["declarations"] = ["api::Missing"]
        policy.write_text(json.dumps(contract), encoding="utf-8")
        report, _ = analyze(root, ["declarations.cpp"], [root], database=database)
        self.assertEqual(report["status"], "FAILED")

    def test_declaration_only_wrong_scope_is_failure(self):
        root, database, _, _ = self.fixture()
        scope = root / "unrelated"
        scope.mkdir()
        report, _ = analyze(root, ["declarations.cpp"], [scope], database=database)
        self.assertEqual(report["status"], "FAILED")


class CodeStructureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = ROOT / ".local/repo_resource/tmp" / ("code_structure_" + uuid.uuid4().hex)
        cls.root.mkdir(parents=True)
        fixture = ROOT / "tests/fixtures/code_structure"
        for source in fixture.iterdir():
            shutil.copyfile(source, cls.root / source.name)
        compiler = shutil.which("clang++")
        if not compiler:
            raise RuntimeError("required local clang++ missing")
        cls.database = cls.root / "commands.json"
        cls.database.write_text(json.dumps([
            {"directory": str(cls.root), "file": name,
             "arguments": [compiler, "-std=c++17", "-c", name]}
            for name in ("mixed.cpp", "other.cpp")]), encoding="utf-8")
        cls.result, cls.snapshot = analyze(cls.root, ["mixed.cpp", "other.cpp"], [cls.root],
                                          database=cls.database, rules_path=cls.root / "roles.json")
        if cls.result["status"] != "COMPLETE":
            raise AssertionError(json.dumps(cls.result["diagnostics"], indent=2))

    def function(self, name):
        return next(row for row in self.result["functions"] if row["qualified_name"] == name)

    def accesses(self, name):
        identity = self.function(name)["id"]
        return [row for row in self.result["accesses"] if row["function"] == identity]

    def test_macro_reference_writes_and_local_alias(self):
        for name, mode in (("SaveAndMeasure", "write"), ("EditText", "read_write"), ("ChangeReference", "read_write")):
            rows = [row for row in self.accesses(name) if row["mode"] == mode]
            self.assertTrue(rows, name)
            self.assertTrue(any(self.result["symbols"].get(row["target"]["declaration"], {}).get("name") == "typing" for row in rows), name)
        row = next(row for row in self.accesses("SaveAndMeasure") if row["mode"] == "write")
        self.assertIn(row["target"]["precision"], {"symbolic", "declaration-origin"})
        self.assertEqual(row["evidence"]["expansion"]["path"], "editing.cppinc")
        self.assertIsNotNone(row["evidence"]["spelling"]["path"])

    def test_different_object_origins_do_not_merge(self):
        rows = [row for row in self.accesses("Objects") if row["mode"] == "write"]
        self.assertEqual(len(rows), 2)
        self.assertEqual(len({row["state"] for row in rows}), 2)
        self.assertEqual(len({row["target"]["declaration"] for row in rows}), 1)
        self.assertFalse(self.accesses("LocalOnly"))

    def test_parameter_receiver_remains_symbolic(self):
        rows = self.accesses("Parameter")
        self.assertTrue(any(row["target"] and row["target"]["precision"] == "symbolic-parameter" for row in rows))

    def test_possible_writes_const_pointer_and_indirect_call(self):
        rows = self.accesses("Indirect")
        self.assertTrue(any(row["mode"] == "may_write" for row in rows))
        self.assertTrue(any(row["mode"] == "write" and row["target"]["origin"].startswith("pointee:") for row in rows))
        pointer = next(row for row in self.result["symbols"].values() if row["name"] == "fixed_pointer")
        self.assertTrue(pointer["const"])
        self.assertFalse(pointer["pointee_const"])
        identity = self.function("CallIndirect")["id"]
        self.assertTrue(any(row["caller"] == identity and row["resolution"] == "unresolved" for row in self.result["calls"]))

    def test_complexity_excludes_string_and_nested_lambda(self):
        function = self.function("Complex")
        # if + && + for + if + else-if + 2 case labels; lambda is independent.
        self.assertEqual(function["decision_count"], 7)
        self.assertEqual(function["max_nesting"], 3)
        lambdas = [row for row in self.result["functions"] if row["kind"] == "LAMBDA_EXPR" and
                   function["body"]["start"]["offset"] <= row["body"]["start"]["offset"] < function["body"]["end"]["offset"] and
                   row["file"] == function["file"]]
        self.assertEqual(len(lambdas), 1)
        self.assertEqual(lambdas[0]["decision_count"], 1)

    def test_native_body_token_partition_and_nested_lambda_closure(self):
        for function in self.result["functions"]:
            part = function["body_token_partition"]
            self.assertEqual(part["non_literal_tokens"] + sum(part["literal_tokens"].values()), function["body_tokens"])
        whole = profile(self.result)
        self.assertTrue(all(row["body_token_composition"]["status"] == "COMPLETE" for row in whole["configurations"]))
        rendered = markdown(self.result)
        self.assertIn("Non-literal tokens", rendered)
        self.assertIn("String tokens", rendered)

    def test_required_body_partition_missing_fails_and_historic_is_unmeasured(self):
        report = copy.deepcopy(self.result)
        report["functions"][0].pop("body_token_partition")
        with self.assertRaises(AnalysisError):
            profile(report)
        report["functions"][0]["body_token_partition"] = None
        with self.assertRaises(AnalysisError):
            profile(report)
        report["methods"].pop("body_token_partition")
        for row in report["functions"]:
            row.pop("body_token_partition", None)
        self.assertTrue(all(row["body_token_composition"]["status"] == "UNMEASURED" for row in profile(report)["configurations"]))

    def test_long_prose_changes_payload_not_native_structure(self):
        root = ROOT / ".local/repo_resource/tmp" / ("content_partition_" + uuid.uuid4().hex)
        root.mkdir(parents=True)
        prose = "日本語の説明 if for switch while // /* */ \n" * 200
        bodies = {"ShortText": "短い説明", "LongText": prose}
        source = "\n".join('int ' + name + '() { const char* text = R"txt(' + text + ')txt"; return text[0] ? 1 : 0; }' for name, text in bodies.items())
        (root / "content.cpp").write_text(source, encoding="utf-8", newline="\n")
        database = root / "commands.json"
        database.write_text(json.dumps([{"directory": str(root), "file": "content.cpp",
                                         "arguments": [shutil.which("clang++"), "-std=c++17", "-c", "content.cpp"]}]), encoding="utf-8")
        report, snapshot = analyze(root, ["content.cpp"], [root], database=database)
        self.assertEqual(report["status"], "COMPLETE", report["diagnostics"])
        short, long = [next(row for row in report["functions"] if row["qualified_name"] == name) for name in bodies]
        for key in ("decision_count", "max_nesting", "body_tokens", "body_token_partition"):
            self.assertEqual(short[key], long[key], key)
        self.assertEqual(long["decision_count"], 1)
        self.assertEqual(long["body_token_partition"]["literal_tokens"]["string"], 1)
        inventory = build_inventory(root, ["content.cpp"], [], snapshot)
        content = inventory["cpp_content"]
        self.assertEqual(content["string_payload_source_bytes"], sum(len(value.encode()) for value in bodies.values()))
        self.assertEqual(content["string_purpose_status"], "UNCLASSIFIED")
        self.assertIn("文字列内部のSource bytes", source_composition_markdown(inventory))
        snapshot.verify()

    def test_mixed_candidate_and_presentation_control(self):
        mixed = [row for row in self.result["candidates"] if row["rule"] == "R1"]
        self.assertTrue(any(row["file"] == "editing.cppinc" and set(row["contracts"]) == {"storage", "presentation"} for row in mixed))
        self.assertFalse(any(row["file"] == "display.cppinc" for row in mixed))
        self.assertEqual(self.function("Dispatch")["body_role"], "delegation")

    def test_scattering_and_cross_fragment_state(self):
        self.assertTrue(any(row["rule"] == "R2" for row in self.result["candidates"]))
        self.assertTrue(self.result["cross_fragment_states"])
        self.assertTrue(self.result["cross_fragment_calls"])

    def test_scoped_role_anchors_do_not_classify_other_files(self):
        rules = json.loads((self.root / "roles.json").read_text())
        for anchor in rules["anchors"]:
            anchor["file_prefixes"] = ["not-this-module/"]
        report = copy.deepcopy(self.result)
        for key in ("roles", "unclassified", "candidates", "structural_groups", "cross_fragment_calls", "cross_fragment_states", "responsibility_matrix"):
            report[key] = []
        derive(report, rules)
        self.assertEqual(report["metrics"]["classified_functions"], 0)
        scopes = report["metrics"]["role_anchor_scope"]
        self.assertEqual(sum(row["functions"] for row in scopes.values()), len(report["functions"]))
        self.assertEqual(sum(row["with_applicable_anchors"] for row in scopes.values()), 0)
        self.assertEqual(sum(row["classified"] for row in scopes.values()), 0)
        self.assertEqual(sum(row["unclassified_with_applicable_anchors"] for row in scopes.values()), 0)
        original_scopes = self.result["metrics"]["role_anchor_scope"]
        self.assertGreater(sum(row["with_applicable_anchors"] for row in original_scopes.values()), 0)
        self.assertGreater(sum(row["classified"] for row in original_scopes.values()), 0)
        # Indexing incoming calls and state accesses preserves these observations.
        old = {f["id"]: f for f in self.result["functions"]}
        self.assertTrue(all(f["fan_in"] == old[f["id"]]["fan_in"] for f in report["functions"]))
        self.assertTrue(all(s["writers"] == self.result["states"][key]["writers"] for key, s in report["states"].items()))

    def test_parse_error_is_incomplete_even_if_another_tu_completes(self):
        bad = self.root / ("bad_" + uuid.uuid4().hex + ".cpp")
        bad.write_text("void broken( {", encoding="utf-8")
        database = self.root / (uuid.uuid4().hex + ".json")
        rows = json.loads(self.database.read_text(encoding="utf-8"))
        rows.append({"directory": str(self.root), "file": bad.name, "arguments": [shutil.which("clang++"), "-std=c++17", "-c", bad.name]})
        database.write_text(json.dumps(rows), encoding="utf-8")
        result, _ = analyze(self.root, ["mixed.cpp", bad.name], [self.root], database=database)
        self.assertEqual(result["status"], "PARTIAL")
        self.assertEqual(result["metrics"]["completed_tu_configurations"], 1)
        self.assertTrue(markdown(result).rstrip().endswith("ANALYSIS_INCOMPLETE"))

    def test_missing_required_tu_and_decoder_fail_closed(self):
        result, _ = analyze(self.root, ["missing.cpp"], [self.root], database=self.database)
        self.assertEqual(result["status"], "FAILED")
        isolated = self.root.parent / ("code_structure_decode_" + uuid.uuid4().hex)
        isolated.mkdir()
        (isolated / "invalid.cpp").write_bytes(b"\xff\xfe\x00")
        result, _ = analyze(isolated, ["invalid.cpp"], [isolated], database=self.database)
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("decode", result["diagnostics"][0]["message"])

    def test_snapshot_detects_change(self):
        file = self.root / (uuid.uuid4().hex + ".h")
        file.write_text("before", encoding="utf-8")
        snapshot = Snapshot(self.root); snapshot.capture(file)
        file.write_text("after", encoding="utf-8")
        with self.assertRaises(AnalysisError):
            snapshot.verify()

    def test_output_is_exclusive_and_preflight_checks_all_paths(self):
        report = self.root / (uuid.uuid4().hex + ".md")
        save_outputs([(report, "original")], protected=set())
        second = self.root / (uuid.uuid4().hex + ".json")
        with self.assertRaises(AnalysisError):
            save_outputs([(second, "new"), (report, "overwrite")], protected=set())
        self.assertFalse(second.exists())
        self.assertEqual(report.read_text(), "original")
        with self.assertRaises(AnalysisError):
            save_outputs([(second, "new")], protected={second.resolve()})

    def test_comparison_is_same_conditions_only(self):
        same = compare(self.result, copy.deepcopy(self.result))
        self.assertEqual(same["status"], "COMPARABLE")
        different = copy.deepcopy(self.result)
        different["methods"]["decisions"] = "new-method"
        self.assertEqual(compare(self.result, different)["status"], "NOT_COMPARABLE")

    def test_file_move_preserves_dependencies_and_is_reported_as_r4(self):
        moved = copy.deepcopy(self.result)
        function = next(row for row in moved["functions"] if row["qualified_name"] == "EditText")
        function["file"] = "new_editing.cppinc"
        result = compare(self.result, moved)
        self.assertTrue(any(row["function"] == function["id"] for row in result["physical_split_candidates"]))

    def test_multiple_configurations_remain_separate(self):
        rows = json.loads(self.database.read_text(encoding="utf-8"))[:1]
        second = copy.deepcopy(rows[0]); second["arguments"] += ["-DALTERNATE=1"]
        database = self.root / (uuid.uuid4().hex + ".json")
        database.write_text(json.dumps(rows + [second]), encoding="utf-8")
        result, _ = analyze(self.root, ["mixed.cpp"], [self.root], database=database)
        self.assertEqual(result["status"], "COMPLETE")
        self.assertEqual(len(result["provenance"]["configuration_ids"]), 2)
        self.assertEqual(sum(row["name"] == "EditText" for row in result["functions"]), 2)

    def test_cli_existing_report_nonzero_and_no_source_change(self):
        report = self.root / (uuid.uuid4().hex + ".md")
        report.write_text("keep", encoding="utf-8")
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            code = main(["--root", str(self.root), "--compilation-database", str(self.database),
                         "--tu", "mixed.cpp", "--scope", str(self.root), "--report", str(report)])
        self.assertEqual(code, 2)
        self.assertEqual(report.read_text(), "keep")

    def test_cli_worker_stdout_and_dependency_failure(self):
        with redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()):
            code = main(["--root", str(self.root), "--compilation-database", str(self.database),
                         "--tu", "mixed.cpp", "--scope", str(self.root), "--format", "json"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())["status"], "COMPLETE")
        result, _ = analyze(self.root, ["mixed.cpp"], [self.root], database=self.database,
                            library=str(self.root / "missing-libclang.dll"))
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("libclang", result["diagnostics"][0]["message"])

    def test_worker_timeout_reaps_descendant(self):
        script = self.root / (uuid.uuid4().hex + ".py")
        receipt = self.root / (uuid.uuid4().hex + ".pid")
        script.write_text(
            "import sys,time,subprocess\nfrom pathlib import Path\n"
            + "sys.path.insert(0," + repr(str(ROOT)) + ")\n"
            + "from tools.metrics.code_structure.worker import wait_start\n"
            + "wait_start(sys.argv[sys.argv.index('--_start-event')+1] if '--_start-event' in sys.argv else None)\n"
            + "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])\n"
            + "Path(" + repr(str(receipt)) + ").write_text(str(p.pid))\n"
            + "time.sleep(30)\n", encoding="utf-8")
        with self.assertRaisesRegex(AnalysisError, "timeout"):
            run_worker([sys.executable, "-B", str(script)], cwd=self.root, timeout=2)
        self.assertTrue(receipt.exists(), "descendant must have started before timeout")
        import os
        if os.name == "nt":
            import ctypes
            from tools.metrics.code_structure.worker import kernel
            api = kernel()
            api.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
            api.OpenProcess.restype = ctypes.c_void_p
            handle = api.OpenProcess(0x00100000, False, int(receipt.read_text()))
            if handle:
                try:
                    self.assertEqual(api.WaitForSingleObject(handle, 3000), 0)
                finally:
                    api.CloseHandle(handle)
        else:
            with self.assertRaises(ProcessLookupError):
                os.kill(int(receipt.read_text()), 0)

    def completed_worker_with_clock_step(self, active_after, wall_after):
        script = self.root / ("late_worker_" + uuid.uuid4().hex + ".py")
        script.write_text("import sys\n"
                          + "sys.path.insert(0," + repr(str(ROOT)) + ")\n"
                          + "from tools.metrics.code_structure.worker import wait_start\n"
                          + "wait_start(sys.argv[sys.argv.index('--_start-event')+1] if '--_start-event' in sys.argv else None)\n"
                          + "print('COMPLETE')\n", encoding="utf-8")
        original_popen = subprocess.Popen
        collected = []

        def launch(*args, **kwargs):
            process = original_popen(*args, **kwargs)
            original_communicate = process.communicate
            def communicate(*args, **kwargs):
                output = original_communicate(*args, **kwargs)
                collected.append(output)
                return output
            process.communicate = communicate
            return process

        from types import SimpleNamespace
        active_clock = SimpleNamespace(method="windows-unbiased-interrupt-time-precise/v1",
                                       now=lambda: active_after if collected else 0.0)

        with patch("tools.metrics.code_structure.worker.subprocess.Popen", side_effect=launch), \
             patch("tools.metrics.code_structure.worker.ActiveClock", return_value=active_clock), \
             patch("tools.metrics.code_structure.worker.monotonic", side_effect=lambda: wall_after if collected else 0.0):
            return run_worker([sys.executable, "-B", str(script)], cwd=self.root, timeout=2)

    def test_complete_worker_output_returned_after_active_deadline_is_rejected(self):
        with self.assertRaisesRegex(AnalysisError, "timeout") as raised:
            self.completed_worker_with_clock_step(3.0, 3.0)
        self.assertEqual(raised.exception.worker_output.stdout.strip(), "COMPLETE")

    def test_host_sleep_can_extend_wall_time_without_expiring_active_budget(self):
        result = self.completed_worker_with_clock_step(1.0, 9601.2)
        self.assertEqual(result.stdout.strip(), "COMPLETE")
        self.assertEqual(result.resources["active_elapsed_seconds"], 1.0)
        self.assertEqual(result.resources["elapsed_seconds"], 9601.2)
        self.assertEqual(result.resources["excluded_inactive_seconds"], 9600.2)

    def test_windows_active_clock_fallback_failure_and_backwards_are_explicit(self):
        import ctypes
        from types import SimpleNamespace
        values = iter((10_000_000, 20_000_000, 15_000_000))
        def query(pointer):
            ctypes.cast(pointer, ctypes.POINTER(ctypes.c_uint64)).contents.value = next(values)
        with patch("tools.metrics.code_structure.worker.os.name", "nt"), \
             patch("tools.metrics.code_structure.worker.kernel", return_value=SimpleNamespace(QueryUnbiasedInterruptTimePrecise=query)):
            clock = ActiveClock()
            self.assertEqual(clock.now(), 1.0)
            self.assertEqual(clock.now(), 2.0)
            with self.assertRaisesRegex(AnalysisError, "backwards"):
                clock.now()
        def fallback(pointer):
            ctypes.cast(pointer, ctypes.POINTER(ctypes.c_uint64)).contents.value = 30_000_000
            return True
        with patch("tools.metrics.code_structure.worker.os.name", "nt"), \
             patch("tools.metrics.code_structure.worker.kernel", return_value=SimpleNamespace(QueryUnbiasedInterruptTime=fallback)):
            clock = ActiveClock()
            self.assertEqual(clock.method, "windows-unbiased-interrupt-time/v1")
            self.assertEqual(clock.now(), 3.0)
            clock.query = lambda _:False
            with self.assertRaisesRegex(AnalysisError, "read failed"):
                clock.now()
        with patch("tools.metrics.code_structure.worker.os.name", "nt"), \
             patch("tools.metrics.code_structure.worker.kernel", return_value=SimpleNamespace()):
            with self.assertRaisesRegex(AnalysisError, "unavailable"):
                ActiveClock()

    def test_unsafe_arguments_are_rejected(self):
        for arg in ("-Xclang", "-fplugin=anything", "-fmodules", "-save-temps", "-include-pch"):
            with self.subTest(arg=arg), self.assertRaises(AnalysisError):
                safe_arguments([arg], self.root, self.root / "mixed.cpp", self.snapshot)

    def test_whole_body_clones_distinguish_exact_and_identifier_normalized(self):
        rows = self.result["structure_profile"]["clone_candidates"]
        exact = {self.function(x)["id"] for x in ("CloneFirst", "CloneExact")}
        renamed = exact | {self.function("CloneRenamed")["id"]}
        self.assertTrue(any(set(row["functions"]) == exact and row["kind"] == "lexical_body_hash" for row in rows))
        self.assertTrue(any(set(row["functions"]) == renamed and row["kind"] == "normalized_body_hash" for row in rows))
        self.assertEqual(profile(self.result, minimum_clone_tokens=10000)["clone_candidates"], [])
        different = self.function("DifferentLiteral")["id"]
        self.assertFalse(any(different in row["functions"] and row["fingerprint"] == self.function("CloneFirst")["normalized_body_hash"] for row in rows))
        self.assertNotEqual(self.function("LambdaFirst")["lexical_body_hash"], self.function("LambdaSecond")["lexical_body_hash"])

    def test_technology_requires_resolved_call_or_type_not_string(self):
        rows = self.result["structure_profile"]["configurations"][0]["technologies"]
        gdi = next(row for row in rows if row["technology"] == "win32.gdi")
        ids = {row["function"] for row in gdi["functions"]}
        self.assertIn(self.function("TechnologyEvidence")["id"], ids)
        self.assertNotIn(self.function("NoTechnologyEvidence")["id"], ids)
        self.assertEqual(gdi["function_denominator"], self.result["structure_profile"]["configurations"][0]["physical_definitions"])

    def test_same_contract_write_overlap_has_precision_not_equivalence_claim(self):
        ids = {self.function(name)["id"] for name in ("SaveAndMeasure", "LegacySave")}
        row = next(row for row in self.result["structure_profile"]["contract_overlap_candidates"] if set(row["functions"]) == ids)
        self.assertEqual(row["common_contracts"], ["storage"])
        self.assertEqual(row["review"], "unreviewed")
        self.assertTrue(all("precision" in item for item in row["shared_write_targets"]))

    def test_overlapping_local_reports_do_not_inflate_functions_or_edges(self):
        rules = json.loads((self.root / "roles.json").read_text())
        merged = combine([copy.deepcopy(self.result), copy.deepcopy(self.result)], rules)
        for key in ("functions", "calls", "accesses"):
            self.assertEqual(len(merged[key]), len(self.result[key]), key)
        self.assertEqual(merged["metrics"]["functions"], self.result["metrics"]["functions"])

    def test_incremental_rollup_matches_local_combine_and_rejects_changed_definitions(self):
        rules = json.loads((self.root / "roles.json").read_text())
        accumulator = Accumulator()
        accumulator.add(copy.deepcopy(self.result))
        accumulator.add(copy.deepcopy(self.result))
        result = accumulator.finish(rules)
        merged = combine([copy.deepcopy(self.result), copy.deepcopy(self.result)], rules)
        for key in ("metrics", "structure_profile", "unused_profile", "function_references"):
            self.assertEqual(result[key], merged[key])
        accumulator = Accumulator()
        accumulator.add(copy.deepcopy(self.result))
        changed = copy.deepcopy(self.result)
        changed["functions"][0]["body_sha256"] = "different"
        with self.assertRaisesRegex(AnalysisError, "definitions disagree"):
            accumulator.add(changed)

    def test_file_participation_unions_tus_without_duplicate_paths_or_hash_disagreement(self):
        rules = json.loads((self.root / "roles.json").read_text())
        first, second = copy.deepcopy(self.result), copy.deepcopy(self.result)
        first["files"][0]["participates"] = False
        second["files"][0]["participates"] = True
        merged = combine([first, second], rules)
        self.assertEqual(len(merged["files"]), len(self.result["files"]))
        self.assertTrue(next(f for f in merged["files"] if f["path"] == first["files"][0]["path"])["participates"])
        self.assertFalse(first["files"][0]["participates"])
        accumulator = Accumulator()
        accumulator.add(copy.deepcopy(first))
        accumulator.add(copy.deepcopy(second))
        self.assertEqual(accumulator.finish(rules)["files"], merged["files"])
        second["files"][0]["sha256"] = "changed"
        with self.assertRaisesRegex(AnalysisError, "file observations disagree"):
            combine([first, second], rules)

    def test_unused_candidates_keep_callbacks_and_implicit_kinds_and_find_dead_cycle(self):
        database = self.root / ("unused_commands_" + uuid.uuid4().hex + ".json")
        database.write_text(json.dumps([{"directory": str(self.root), "file": "unused.cpp",
                             "arguments": [shutil.which("clang++"), "-std=c++17", "unused.cpp"]}]), encoding="utf-8")
        result, _ = analyze(self.root, ["unused.cpp"], [self.root], database=database)
        self.assertEqual(result["status"], "COMPLETE", result["diagnostics"])
        rows = {row["name"]: row for row in result["unused_profile"]["rows"]}
        for name in ("UnusedLeaf", "DeadFirst", "DeadSecond"):
            self.assertEqual(rows[name]["status"], "unreachable-in-observed-graph")
        for name in ("TableCallback", "PassedCallback", "DispatchBase::Method", "DispatchBase::~DispatchBase", "UninstantiatedTemplate"):
            self.assertEqual(rows[name]["status"], "no-observed-call-but-retained")
        self.assertTrue(rows["TableCallback"]["root_reasons"])
        self.assertTrue(rows["PassedCallback"]["references"])
        self.assertNotIn("UsedLeaf", rows)
        self.assertNotIn("FragmentCallback", rows)
        definition = next(f for f in result["functions"] if f["name"] == "FragmentCallback")
        fragment_calls = [c for c in result["calls"] if c["callee_name"] == "FragmentCallback"]
        self.assertTrue(fragment_calls)
        self.assertTrue(all(c["callee"] == definition["id"] for c in fragment_calls))
        profile_data = unused_profile(result)
        self.assertEqual(profile_data, result["unused_profile"])
        self.assertTrue(all(row["review"] == "unreviewed" for row in profile_data["rows"]))

    def test_function_pointer_converted_lambda_body_is_observed(self):
        database = self.root / ("converted_lambda_commands_" + uuid.uuid4().hex + ".json")
        database.write_text(json.dumps([{"directory": str(self.root), "file": "converted_lambda.cpp",
                             "arguments": [shutil.which("clang++"), "-std=c++17", "converted_lambda.cpp"]}]), encoding="utf-8")
        result, _ = analyze(self.root, ["converted_lambda.cpp"], [self.root], database=database)
        self.assertEqual(result["status"], "COMPLETE", result["diagnostics"])
        lambdas = [f for f in result["functions"] if f["kind"] == "LAMBDA_EXPR"]
        self.assertEqual(len(lambdas), 1)
        self.assertGreater(lambdas[0]["decision_count"], 0)
        parent = next(f for f in result["functions"] if f["name"] == "RegisterConvertedLambda")
        self.assertEqual(parent["decision_count"], 0)
        leaf = next(f for f in result["functions"] if f["name"] == "CallbackLeaf")
        self.assertTrue(any(c["caller"] == lambdas[0]["id"] and c["callee"] == leaf["id"]
                            for c in result["calls"]))
        self.assertFalse(any(c["caller"] == parent["id"] and c["callee"] == leaf["id"]
                             for c in result["calls"]))
        self.assertTrue(any(a["function"] == lambdas[0]["id"] and a["mode"] == "write"
                            for a in result["accesses"]))
        self.assertFalse(any(row["name"] == "CallbackLeaf" and row["status"] == "unreachable-in-observed-graph"
                             for row in result["unused_profile"]["rows"]))

    def test_local_failure_or_method_mismatch_cannot_become_global_complete(self):
        rules = json.loads((self.root / "roles.json").read_text())
        partial = copy.deepcopy(self.result)
        partial["status"] = "PARTIAL"
        self.assertEqual(combine([copy.deepcopy(self.result), partial], rules)["status"], "PARTIAL")
        partial["methods"]["decisions"] = "different"
        with self.assertRaises(AnalysisError):
            combine([copy.deepcopy(self.result), partial], rules)

    def test_physical_inventory_and_body_coverage_are_separate(self):
        inventory = build_inventory(self.root, ["mixed.cpp", "mixed.h", "editing.cppinc", "observations.cppinc"],
                                    [{"id": "fixture", "path": ".", "purpose": "verification"}], self.snapshot)
        self.assertEqual(inventory["file_denominator"], 4)
        self.assertAlmostEqual(sum(row["byte_share"] for row in inventory["breakdowns"]["form"]), 1)
        observed = coverage(inventory, [self.result, self.result])
        self.assertEqual(observed["whole_ast_status"], "PARTIAL")
        self.assertEqual(observed["observed_body_bytes"], coverage(inventory, [self.result])["observed_body_bytes"])
        self.assertLess(observed["observed_body_byte_share"], 1)

    def test_multi_local_workers_then_inventory_and_failure_propagation(self):
        plan_path = self.root / (uuid.uuid4().hex + ".json")
        plan = {"version": 1, "roles": "roles.json", "locals": [
            {"id": "first", "translation_units": ["mixed.cpp"], "scopes": ["."]},
            {"id": "second", "translation_units": ["other.cpp"], "scopes": ["."]}],
            "source_groups": [{"id": "fixture", "path": ".", "purpose": "verification"}]}
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        paths = sorted(p.name for p in self.root.iterdir() if p.suffix in {".h", ".cpp", ".cppinc"})
        with patch("tools.metrics.code_structure.survey.git_sources", return_value=paths), redirect_stderr(io.StringIO()):
            result, _ = survey(self.root, plan_path, database=self.database)
        self.assertEqual(result["status"], "COMPLETE")
        self.assertEqual(len(result["local_stages"]), 2)
        self.assertEqual(result["inventory"]["status"], "COMPLETE")
        self.assertEqual(result["coverage"]["whole_ast_status"], "PARTIAL")
        self.assertIn("ANALYSIS_COMPLETE", survey_markdown(result))
        plan["locals"][0]["translation_units"] = ["missing.cpp"]
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        with patch("tools.metrics.code_structure.survey.git_sources", return_value=paths), redirect_stderr(io.StringIO()):
            result, _ = survey(self.root, plan_path, database=self.database)
        self.assertEqual(result["status"], "PARTIAL")
        self.assertEqual(result["local_stages"][1]["status"], "COMPLETE")
        self.assertEqual(result["cross_area"]["status"], "PARTIAL")


class SurveyContractTests(unittest.TestCase):
    def test_distribution_includes_tail_and_empty_is_unmeasured(self):
        self.assertEqual(distribution([])["max"], None)
        self.assertEqual(distribution(list(range(1, 101))), {"count": 100, "median": 50.5, "p95": 95, "max": 100})

    def test_cycles_are_strong_components_not_just_reachable(self):
        self.assertEqual(graph_cycles([("a", "b"), ("b", "a"), ("b", "c"), ("c", "d")]), [["a", "b"]])

    def test_byte_partition_handles_raw_literals_comments_and_unicode(self):
        text = ' // comment\nconst char* a = R"xx(/* literal */ 日本語)xx"; /* tail */\n'
        result = cpp_bytes(text)
        self.assertEqual(sum(result["bytes"].values()), len(text.encode("utf-8")))
        self.assertEqual(result["bytes"]["comment"], len("// comment/* tail */"))
        self.assertGreater(result["bytes"]["literal"], len("日本語".encode("utf-8")))
        with self.assertRaises(AnalysisError):
            cpp_bytes('R"xx(unterminated')
        continuation = '// continued\\\n"unclosed comment\nint n = 0;'
        self.assertEqual(sum(cpp_bytes(continuation)["bytes"].values()), len(continuation.encode()))
        self.assertEqual(cpp_bytes("1e-3 0x1p+2")["bytes"]["literal"], 10)

    def test_literal_kinds_payload_offsets_unicode_and_escapes(self):
        values = ['u8"日本語"', 'R"tag(/* text */\n文)tag"', '"\\n"', 'L\'文\'', "'x'", "1'000", "0x1p+2", ".5f"]
        source = " + ".join(values)
        result = cpp_bytes(source)
        self.assertEqual(result["literal_counts"], {"string": 3, "character": 2, "number": 3, "other": 0})
        self.assertEqual(sum(result["literal_bytes"].values()), result["bytes"]["literal"])
        self.assertEqual(result["string_payload_source_bytes"], len("日本語/* text */\n文\\n".encode()))
        raw = source.encode()
        for row, spelling in zip(result["strings"], values[:3]):
            observed = raw[row["start_byte"]:row["end_byte"]]
            self.assertEqual(observed, spelling.encode())
            self.assertEqual(digest_bytes(observed), row["sha256"])
            self.assertEqual(row["purpose"], "unclassified")
        self.assertEqual(result["string_purpose"]["unclassified_source_bytes"], result["literal_bytes"]["string"])
        for value in ('"unterminated', '/* unterminated', 'u8R"x(unterminated'):
            with self.assertRaises(AnalysisError):
                cpp_bytes(value)

    def test_ast_literal_kinds_keep_unknown_and_rule_strings(self):
        for spelling, expected in [('u8R"x(pattern)x"', "string"), ('L\'x\'', "character"), ("1'000UL", "number"), (".1e+2", "number"), ("unexpected", "other")]:
            self.assertEqual(literal_kind(spelling), expected)

    def test_missing_or_inconsistent_body_partition_not_zero_success(self):
        old = body_composition([{"body_tokens": 10}])
        self.assertEqual(old["status"], "UNMEASURED")
        self.assertIsNone(old["non_literal_tokens"])
        good = {"body_tokens": 10, "body_token_partition": {
            "method": "clang-body-literal-partition/v1", "body_token_denominator": 10,
            "non_literal_tokens": 7, "literal_tokens": {"string": 1, "character": 1, "number": 1, "other": 0}}}
        self.assertEqual(body_composition([good, {"body_tokens": 10}])["status"], "PARTIAL")
        broken = copy.deepcopy(good)
        broken["body_token_partition"]["literal_tokens"]["string"] = 2
        with self.assertRaises(AnalysisError):
            body_composition([broken])
        self.assertIn("未測定", source_composition_markdown({}))

    def test_physical_intervals_union_not_sum_and_group_longest_path(self):
        self.assertEqual(merged_bytes([(1, 8), (3, 5), (7, 12), (20, 22)]), 13)
        groups = [{"id": "app", "path": "src"}, {"id": "entry", "path": "src/entry"}]
        self.assertEqual(select_group("src/entry/a.cpp", groups)["id"], "entry")
        self.assertEqual(select_group("src/entry_more/a.cpp", groups)["id"], "app")

    def test_invalid_plan_and_no_locals_rejected(self):
        for plan in ({"version": 1, "locals": []}, {"version": 1, "locals": [
                {"id": "a", "translation_units": ["../a.cpp"], "scopes": ["src"]},
                {"id": "b", "translation_units": ["b.cpp"], "scopes": ["src"]}]}):
            with self.assertRaises(AnalysisError):
                validate_plan(plan)

    def test_missing_inventory_file_is_fail_not_zero_bytes(self):
        with self.assertRaises(AnalysisError):
            build_inventory(ROOT, ["tests/fixtures/code_structure/missing.cpp"], [], Snapshot(ROOT))

    def test_survey_cli_failure_is_nonzero_without_output(self):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(survey_main(["--root", str(ROOT), "--plan", "missing-plan.json", "--build-signature", "missing-signature.json"]), 2)


class BatchContractTests(unittest.TestCase):
    def setUp(self):
        self.root = ROOT / ".local/repo_resource/tmp" / ("code_batch_" + uuid.uuid4().hex)
        self.root.mkdir(parents=True)
        self.source = self.root / "source.cpp"
        self.source.write_text("int value() { return 1; }\n", encoding="utf-8")
        self.plan_path = self.root / "plan.json"
        self.plan_path.write_text("{}", encoding="utf-8")
        self.directory = self.root / "out/tests/run"
        build_profile = {"kind": "workspace", "locale": "ja", "edition": "Full", "configuration": "Release"}
        self.targets = [{"id": stable_id([name, "source.cpp"]), "profile": name, "source": "source.cpp",
                         "signature": "signature.json", "build_profile": build_profile} for name in ("first", "second")]
        self.frozen = {"schema": "code-batch/v1", "root": str(self.root), "plan_path": str(self.plan_path),
                       "execution_contract": DEADLINE_METHOD,
                       "plan": {"scopes": ["."], "profiles": []}, "targets": self.targets,
                       "selection": ["source.cpp"], "inventory": {}, "tool_hash": "fixture-tool",
                       "rules_hash": "fixture-rules", "runtime": {"driver": "clang++", "library": "libclang.dll"},
                       "input_manifest": [{"path": "source.cpp", "sha256": digest_bytes(self.source.read_bytes())}]}
        self.frozen["run_id"] = stable_id(self.frozen)
        self.payload = empty_report()
        self.payload.update(status="COMPLETE", translation_units=[{"file": "source.cpp", "status": "COMPLETE",
                            "command": {"build_profile": build_profile}}],
                            metrics={"required_tu_configurations": 1, "completed_tu_configurations": 1},
                            provenance={"tool_hash": "fixture-tool", "rules_hash": "fixture-rules",
                                        "runtime": self.frozen["runtime"], "scopes": ["."], "selected_tus": ["source.cpp"],
                                        "input_manifest": self.frozen["input_manifest"]})

    def worker(self, *args, **kwargs):
        result = subprocess.CompletedProcess(args, 0, json.dumps(self.payload), "")
        result.resources = {"status": "MEASURED", "peak_process_bytes": 123,
                            "deadline_method": DEADLINE_METHOD, "timeout_seconds": kwargs["timeout"], "elapsed_seconds": 0.0,
                            "active_clock_method": ActiveClock().method, "active_elapsed_seconds": 0.0}
        return result

    def run_fixture(self, **kwargs):
        with patch("tools.metrics.code_structure.batch.freeze", return_value=self.frozen), \
             patch("tools.metrics.code_structure.batch.git_sources", return_value=["source.cpp"]), \
             patch("tools.metrics.code_structure.batch.run_worker", side_effect=self.worker) as worker, \
             redirect_stdout(io.StringIO()):
            result = run_batch(self.root, self.plan_path, self.directory, **kwargs)
        return result, worker.call_count

    def test_partial_selection_then_resume_reuses_verified_receipts(self):
        first, calls = self.run_fixture(profiles=["first"])
        self.assertEqual((first["status"], first["completed"], calls), ("PARTIAL", 1, 1))
        second, calls = self.run_fixture(resume=True)
        self.assertEqual((second["status"], second["completed"], calls), ("COMPLETE", 2, 1))
        third, calls = self.run_fixture(resume=True)
        self.assertEqual((third["status"], calls), ("COMPLETE", 0))
        self.assertEqual(len(list(self.directory.glob("execution_*.json"))), 3)

    def test_report_tampering_is_not_rerun_or_accepted(self):
        self.run_fixture(profiles=["first"])
        report = next(self.directory.glob("targets/*/attempt_*/report.json.gz"))
        report.write_bytes(b"changed")
        with self.assertRaisesRegex(AnalysisError, "report hash mismatch"):
            self.run_fixture(resume=True)
        self.assertEqual(report.read_bytes(), b"changed")

    def test_deadline_overrun_receipt_cannot_be_reused(self):
        self.run_fixture(profiles=["first"])
        receipt_path = next(self.directory.glob("targets/*/attempt_*/receipt.json"))
        receipt = json.loads(receipt_path.read_text())
        receipt["active_elapsed_seconds"] = 9601.2
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        timing_path = receipt_path.parent / "timing.json"
        timing = json.loads(timing_path.read_text())
        timing["active_elapsed_seconds"] = 9601.2
        timing_path.write_text(json.dumps(timing), encoding="utf-8")
        before = {p:p.read_bytes() for p in self.directory.rglob("*") if p.is_file()}
        with self.assertRaisesRegex(AnalysisError, "timeout exceeded"):
            self.run_fixture(resume=True)
        self.assertTrue(all(p.read_bytes() == value for p,value in before.items()))

    def test_deadline_evidence_requires_finite_consistent_measurements(self):
        method = "windows-unbiased-interrupt-time-precise/v1"
        resources = {"deadline_method": DEADLINE_METHOD, "timeout_seconds": 2, "elapsed_seconds": 0.5,
                     "active_elapsed_seconds": 0.5, "active_clock_method": method}
        timing = {"elapsed_seconds": 1.0, "active_elapsed_seconds": 1.0,
                  "deadline_method": DEADLINE_METHOD, "active_clock_method": method}
        started = {"timeout_seconds": 2, "active_clock_method": method, "deadline_method": DEADLINE_METHOD}
        verify_execution_deadline(started, timing, resources)
        verify_execution_deadline(started, {**timing,"elapsed_seconds":9601.2}, resources)
        for invalid in (None, True, -1, float("nan"), float("inf"), 2, 9601.2):
            with self.subTest(elapsed=invalid), self.assertRaises(AnalysisError):
                verify_execution_deadline(started, {**timing,"active_elapsed_seconds":invalid}, resources)
        for key,value in (("deadline_method", "missing"), ("timeout_seconds", 3),
                          ("elapsed_seconds", 1.5), ("elapsed_seconds", float("nan")),
                          ("active_clock_method", "unknown"), ("active_elapsed_seconds", 1.5)):
            wrong = {**resources, key:value}
            with self.subTest(key=key,value=value), self.assertRaises(AnalysisError):
                verify_execution_deadline(started, timing, wrong)

    def test_changed_input_blocks_resume_and_preserves_evidence(self):
        self.run_fixture(profiles=["first"])
        before = {p: p.read_bytes() for p in self.directory.rglob("*") if p.is_file()}
        self.source.write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(AnalysisError, "frozen input changed"):
            self.run_fixture(resume=True)
        self.assertTrue(all(p.read_bytes() == value for p, value in before.items()))

    def test_failed_target_does_not_stop_other_target_or_get_receipt(self):
        calls = 0
        def worker(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise AnalysisError("injected timeout")
            return BatchContractTests.worker(self, *args, **kwargs)
        self.worker = worker
        result, count = self.run_fixture(workers=1)
        self.assertEqual((result["status"], result["completed"], count), ("PARTIAL", 1, 2))
        self.assertEqual(len(list(self.directory.glob("targets/*/attempt_*/failure.json"))), 1)
        self.assertEqual(len(list(self.directory.glob("targets/*/attempt_*/receipt.json"))), 1)

    def test_complete_payload_with_failed_exit_is_failure(self):
        def worker(*args, **kwargs):
            result = BatchContractTests.worker(self, *args, **kwargs)
            result.returncode = 7
            return result
        self.worker = worker
        result, _ = self.run_fixture()
        self.assertEqual((result["status"], result["completed"]), ("PARTIAL", 0))
        self.assertEqual(len(list(self.directory.glob("targets/*/attempt_*/report.json.gz"))), 2)

    def test_late_worker_payload_is_preserved_without_complete_receipt(self):
        def worker(*args, **kwargs):
            failure = AnalysisError("injected timeout after complete output")
            failure.worker_output = BatchContractTests.worker(self, *args, **kwargs)
            raise failure
        self.worker = worker
        result, _ = self.run_fixture()
        self.assertEqual((result["status"],result["completed"]), ("PARTIAL",0))
        self.assertEqual(len(list(self.directory.glob("targets/*/attempt_*/report.json.gz"))), 2)
        self.assertEqual(len(list(self.directory.glob("targets/*/attempt_*/failure.json"))), 2)
        self.assertFalse(list(self.directory.glob("targets/*/attempt_*/receipt.json")))

    def test_dependency_hash_disagreement_is_failure(self):
        known = {"source.cpp": "different"}
        with self.assertRaisesRegex(AnalysisError, "input differs between TUs"):
            verify_report(self.payload, self.targets[0], self.frozen, self.root, known)

    def test_counts_and_build_profile_cannot_be_forged_complete(self):
        for field in ("metrics", "translation_units"):
            report = copy.deepcopy(self.payload)
            if field == "metrics":
                report[field]["completed_tu_configurations"] = 0
            else:
                report[field][0]["command"]["build_profile"]["locale"] = "en"
            with self.assertRaises(AnalysisError):
                verify_report(report, self.targets[0], self.frozen, self.root, {})

    def test_existing_directory_and_outside_directory_are_rejected(self):
        self.directory.mkdir(parents=True)
        with self.assertRaises(FileExistsError):
            self.run_fixture()
        with self.assertRaisesRegex(AnalysisError, "under out/tests"):
            run_batch(self.root, self.plan_path, self.root / "src")

    def test_live_owner_lock_blocks_second_owner(self):
        self.directory.mkdir(parents=True)
        with run_lock(self.directory):
            with self.assertRaises(OSError):
                with run_lock(self.directory):
                    self.fail("second owner acquired live lock")
        with run_lock(self.directory):
            pass

    def test_reviewed_viewer_signature_and_mismatches(self):
        driver_hash = next(key for key, value in REVIEWED_DRIVERS.items() if value[0] == "readonly-viewer")
        signature = {"BuildDriverHash": driver_hash, "Locale": "en", "Configuration": "Release", "SourceFiles": ["src/reader.cpp"],
                     "Flags": ["-DPDF_NOTE_LOCALE_EN=1", "-DCLROP_READ_ONLY_BUILD", "-DMD4C_USE_UTF16"]}
        snapshot = Snapshot(self.root)
        manifest = {"ReadOnlyViewerSourceFiles": ["src/reader.cpp"]}
        with patch.object(snapshot, "capture", return_value=b"reviewed driver"), patch.object(snapshot, "json", return_value=manifest), \
             patch("tools.metrics.code_structure.inputs.digest_bytes", return_value=driver_hash):
            self.assertEqual(validate_signature(signature, self.root, snapshot)[0], "readonly-viewer")
            for key, value in (("Locale", "ja"), ("Edition", "Full"), ("SourceFiles", ["src/other.cpp"]), ("Flags", ["-DPDF_NOTE_LOCALE_EN=1"]), ("BuildDriverHash", "unknown")):
                bad = {**signature, key: value}
                with self.assertRaises(AnalysisError):
                    validate_signature(bad, self.root, snapshot)


if __name__ == "__main__":
    unittest.main()
