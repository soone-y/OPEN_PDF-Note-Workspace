from __future__ import annotations

import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]


def load_module(name: str, relative: str):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


binary_scan = load_module("binary_scan", "tools/release_checks/binary_scan.py")
runtime_gate = load_module("libreoffice_runtime_gate", "tools/release_checks/libreoffice_runtime_gate.py")
content_gate = load_module("public_snapshot_content_gate", "tools/release_checks/public_snapshot_content_gate.py")
smask = load_module("check_pdf_smask_zero", "tests/scripts/check_pdf_smask_zero.py")
pe_fixtures = load_module("pe_fixtures", "tests/python/pe_fixtures.py")


class CheckFailureContractTests(unittest.TestCase):
    def run_quietly(self, function, arguments):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return function(arguments)

    def test_valid_pe32_and_pe64_imports_and_ordinals(self):
        for pe64 in (False, True):
            with self.subTest(pe64=pe64):
                self.assertEqual(binary_scan.parse_pe_imports_with_status(pe_fixtures.make_pe(pe64=pe64)),
                                 ([], [], "valid"))
                self.assertEqual(binary_scan.parse_pe_imports_with_status(
                    pe_fixtures.make_pe("winhttp.dll", pe64=pe64)),
                    (["winhttp.dll"], ["ImportedFunction"], "valid"))
                self.assertEqual(binary_scan.parse_pe_imports_with_status(
                    pe_fixtures.make_pe("kernel32.dll", symbol=None, pe64=pe64)),
                    (["kernel32.dll"], [], "valid"))

    def test_partial_pe_imports_are_rejected_without_partial_results(self):
        original = pe_fixtures.make_pe("winhttp.dll")
        mutations = {
            "unmapped directory": (0x98 + 120, 0x9000),
            "directory size excludes terminator": (0x98 + 124, 20),
            "unmapped DLL name": (512 + 12, 0x9000),
            "unmapped thunk": (512, 0x9000),
            "unmapped import name": (672, 0x9000),
            "virtual padding only": (0x98 + 240 + 16, 128),
            "null name": (512 + 12, 0),
        }
        for name, (offset, value) in mutations.items():
            with self.subTest(name=name):
                broken = bytearray(original)
                struct.pack_into("<I", broken, offset, value)
                self.assertEqual(binary_scan.parse_pe_imports_with_status(broken), ([], [], "invalid-pe"))

        for name, broken in (
            ("truncated section", original[:-1]),
            ("unterminated DLL name", original[:640] + b"a" * (1024 - 640)),
            ("unterminated thunks", original[:672] + struct.pack("<Q", 0x8000000000000001) * 44),
        ):
            with self.subTest(name=name):
                self.assertEqual(binary_scan.parse_pe_imports_with_status(broken), ([], [], "invalid-pe"))

    def test_two_value_pe_api_cannot_hide_parse_failure(self):
        self.assertEqual(binary_scan.parse_pe_imports(pe_fixtures.make_pe()), ([], []))
        for data in (b"MZ", b"not PE"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                binary_scan.parse_pe_imports(data)

    def test_strict_binary_scan_rejects_missing_and_empty_include_even_after_valid_include(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "valid.exe").write_bytes(pe_fixtures.make_pe())
            (root / "empty").mkdir()
            for include in ("absent", "empty"):
                with self.subTest(include=include):
                    code = self.run_quietly(binary_scan.main, [
                        "--root", str(root), "--include", "valid.exe", "--include", include,
                        "--fail-on-unparseable-pe",
                    ])
                    self.assertEqual(code, 2)

    def test_strict_binary_scan_rejects_unmapped_import_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            broken = bytearray(pe_fixtures.make_pe("winhttp.dll"))
            struct.pack_into("<I", broken, 0x98 + 120, 0x9000)
            (root / "broken.exe").write_bytes(broken)
            self.assertEqual(self.run_quietly(binary_scan.main, [
                "--root", str(root), "--include", "broken.exe", "--imports-only",
                "--imported-dll", "winhttp.dll", "--fail-on-import", "--fail-on-unparseable-pe",
            ]), 1)

    def test_binary_scan_directory_access_failure_cannot_pass(self):
        def fail_walk(_root, *, onerror, followlinks):
            onerror(PermissionError("unreadable directory"))
            return iter(())
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(binary_scan.os, "walk", side_effect=fail_walk):
            self.assertEqual(self.run_quietly(binary_scan.main, [
                "--root", temporary, "--include", ".", "--fail-on-unparseable-pe",
            ]), 2)

    def test_runtime_rejects_malformed_entrypoint_and_dll(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            program = root / "program"
            program.mkdir()
            for filename in ("soffice.com", "broken.dll"):
                (program / filename).write_bytes(b"MZ")
            violations = runtime_gate.collect_violations(root)
            self.assertEqual({item.path for item in violations if item.kind == "unparseable-pe"},
                             {"program/soffice.com", "program/broken.dll"})
            self.assertEqual(self.run_quietly(runtime_gate.main, ["--image", str(root)]), 1)

    def test_public_content_limit_is_a_failure_including_file_growth(self):
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(content_gate, "MAX_TEXT_BYTES", 8):
            root = Path(temporary)
            path = root / "large.txt"
            path.write_bytes(b"123456789")
            self.assertEqual(self.run_quietly(content_gate.main, ["--snapshot", str(root)]), 1)
            self.assertEqual(content_gate.scan_text(path, content_gate.PurePosixPath(path.name), ()),
                             [content_gate.Violation("public-text-scan-limit-exceeded", path.name)])
            path.write_bytes(b"small")
            with mock.patch.object(Path, "read_bytes", return_value=b"123456789"):
                self.assertEqual(content_gate.scan_text(path, content_gate.PurePosixPath(path.name), ())[0].kind,
                                 "public-text-scan-limit-exceeded")

    def test_public_content_scans_c_family_and_script_extensions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for suffix in (".c", ".cc", ".cxx", ".c++", ".hpp", ".hxx", ".inl", ".ipp", ".rc", ".js", ".cmd"):
                with self.subTest(suffix=suffix):
                    path = root / ("example" + suffix)
                    path.write_text("-----BEGIN " + "PRIVATE KEY-----", encoding="utf-8")
                    self.assertTrue(any(item.kind == "private-key" for item in content_gate.collect_violations(root)))
                    path.unlink()

    def test_public_content_unreadable_directory_cannot_pass(self):
        def fail_walk(_root, *, onerror, followlinks):
            onerror(PermissionError("unreadable directory"))
            return iter(())
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(content_gate.os, "walk", side_effect=fail_walk):
            self.assertEqual(self.run_quietly(content_gate.main, ["--snapshot", temporary]), 1)

    def mask_pdf(self, payload: bytes, header: bytes = b"", generation: int = 0) -> bytes:
        return (b"%PDF-1.4\n1 0 obj\n<< /SMask 2 " + str(generation).encode() + b" R >>\nendobj\n2 " +
                str(generation).encode() + b" obj\n<< /Length " + str(len(payload)).encode() + b" " +
                header + b" >>\nstream\n" + payload + b"\nendstream\nendobj\n")

    def test_smask_supported_zero_and_nonzero_masks_and_none(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "mask.pdf"
            for flate in (False, True):
                for data, expected in ((b"\0" * 16, 2), (b"\0\1", 0)):
                    with self.subTest(flate=flate, expected=expected):
                        payload = zlib.compress(data) if flate else data
                        path.write_bytes(self.mask_pdf(payload, b"/Filter /FlateDecode" if flate else b"", generation=3))
                        self.assertEqual(self.run_quietly(smask.main, [str(path)]), expected)
            path.write_bytes(b"%PDF-1.4\n1 0 obj\n<< /SMask /None >>\nendobj\n")
            self.assertEqual(self.run_quietly(smask.main, [str(path)]), 0)

    def test_smask_inspection_failure_cannot_pass(self):
        cases = {
            "missing object": b"%PDF-1.4\n1 0 obj\n<< /SMask 99 0 R >>\nendobj\n",
            "bad compression": self.mask_pdf(b"not-zlib", b"/Filter /FlateDecode"),
            "unsupported filter": self.mask_pdf(b"data", b"/Filter /DCTDecode"),
            "multiple filters": self.mask_pdf(b"data", b"/Filter [/FlateDecode /ASCII85Decode]"),
            "decode parameters": self.mask_pdf(b"data", b"/DecodeParms << /Predictor 12 >>"),
            "indirect length": self.mask_pdf(b"data").replace(b"/Length 4", b"/Length 4 0 R"),
            "incorrect length": self.mask_pdf(b"data").replace(b"/Length 4", b"/Length 99999"),
            "missing stream": self.mask_pdf(b"data").replace(b"stream\n", b"invalid\n"),
            "empty stream": self.mask_pdf(b""),
            "not PDF": b"not a PDF",
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "mask.pdf"
            for name, pdf in cases.items():
                with self.subTest(name=name):
                    path.write_bytes(pdf)
                    self.assertEqual(self.run_quietly(smask.main, [str(path)]), 1)

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_repo_step_contract_preserves_failures_and_rejects_implicit_skip(self):
        with tempfile.TemporaryDirectory(prefix="pdf_note_step_") as temporary:
            script = Path(temporary) / "step_contract.ps1"
            script.write_text(r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$sourcePath = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/scripts/run_repo_checks.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Unable to parse repository checks.' }
$names = @('Invoke-Step', 'Append-StepLog', 'Get-SafeLogFileName', 'Write-StepLogHeader',
    'Write-StepLogFooter', 'Write-FailureTail', 'Invoke-BinaryArtifactScan', 'Invoke-LoggedCommand')
foreach ($fn in $ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]}, $true)) {
    if ($fn.Name -in $names) { Invoke-Expression $fn.Extent.Text }
}
$repoCheckLogRoot = $PSScriptRoot
$script:CurrentStepLogPath = ''; $script:CurrentStepName = ''
$VerboseOutput = $false; $FailureTailLines = 1; $SkipBuild = $true
$binaryOutputDir = Join-Path $PSScriptRoot 'absent_full'
$liteBinaryOutputDir = Join-Path $PSScriptRoot 'absent_lite'
$shellPath = (Get-Command powershell.exe -ErrorAction Stop).Source
$cases = @{
    success = { Write-Host 'diagnostic' }
    false_result = { return $false }
    zero_result = { return 0 }
    true_result = { return $true }
    exception = { throw 'failed check' }
    nonterminating_error = { Write-Error 'failed check' }
    missing_artifacts = { Invoke-BinaryArtifactScan }
    native_failure = { Invoke-LoggedCommand -FilePath $shellPath -Arguments @('-NoProfile', '-Command', 'exit 7') }
}
$results = @{}
foreach ($name in $cases.Keys) {
    $failed = $false
    try { Invoke-Step -Name $name -Action $cases[$name] } catch { $failed = $true }
    $log = Get-Content -LiteralPath (Join-Path $PSScriptRoot (Get-SafeLogFileName $name)) -Raw
    $results[$name] = @{ failed = $failed; fail_log = $log.Contains('status: FAIL'); pass_log = $log.Contains('status: PASS') }
}
Write-Output ('RESULT:' + ($results | ConvertTo-Json -Depth 3 -Compress))
''', encoding="utf-8")
            environment = os.environ.copy()
            environment["PDF_NOTE_CONTRACT_REPO_ROOT"] = str(REPO_ROOT)
            result = subprocess.run([
                shutil.which("powershell.exe") or shutil.which("powershell"), "-NoProfile",
                "-ExecutionPolicy", "Bypass", "-File", str(script),
            ], capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(next(line[len("RESULT:"):] for line in result.stdout.splitlines() if line.startswith("RESULT:")))
            for name, item in data.items():
                with self.subTest(name=name):
                    expected_failure = name != "success"
                    self.assertEqual(item, {"failed": expected_failure, "fail_log": expected_failure,
                                            "pass_log": not expected_failure})


if __name__ == "__main__":
    unittest.main()
