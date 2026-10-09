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

    @unittest.skipUnless((shutil.which("powershell.exe") or shutil.which("powershell")) and shutil.which("rg"),
                         "requires PowerShell and ripgrep")
    def test_safety_scan_allows_only_exact_md4c_parser_url_fixture(self):
        with tempfile.TemporaryDirectory(prefix="pdf_note_scan_") as temporary:
            root = Path(temporary)
            script = root / "scan_contract.ps1"
            script.write_text(r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$sourcePath = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/scripts/run_repo_checks.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Unable to parse repository checks.' }
foreach ($fn in $ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]}, $true)) {
    if ($fn.Name -in @('Invoke-RipgrepScan', 'Invoke-SafetyScans')) { Invoke-Expression $fn.Extent.Text }
}
function Append-StepLog { param([string]$Line) }
function Assert-FileOutputSystemDialogPolicy { }
$safetyScanIgnoreFile = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/config/safety_scan_ignore_globs.txt'
$scanRoot = Join-Path $PSScriptRoot 'scan_root'
$fixturePath = Join-Path $scanRoot 'tests/unit/md4c_allocation_failure_tests.c'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $fixturePath) | Out-Null
$original = [IO.File]::ReadAllText((Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/unit/md4c_allocation_failure_tests.c'))
$cases = @{
    exact_fixture = $original
    changed_url = $original.Replace('https://example.invalid/', 'https://other.invalid/')
    extra_url = $original.Replace('test@example.invalid\n"),', 'test@example.invalid\n"), /* https://other.invalid/ */')
    network_api = $original + "`nvoid forbidden(void) { socket(); }`n"
    sound_api = $original + "`nvoid forbidden(void) { Beep(1, 1); }`n"
    other_file = $original
}
$results = @{}
Push-Location -LiteralPath $scanRoot
try {
    foreach ($name in $cases.Keys) {
        [IO.File]::WriteAllText($fixturePath, $cases[$name])
        $otherPath = Join-Path $scanRoot 'other.c'
        if ($name -eq 'other_file') { [IO.File]::WriteAllText($otherPath, $original) }
        $failed = $false
        try { Invoke-SafetyScans } catch { $failed = $true }
        $results[$name] = $failed
        if (Test-Path -LiteralPath $otherPath) { Remove-Item -LiteralPath $otherPath }
    }
} finally { Pop-Location }
Write-Output ('RESULT:' + ($results | ConvertTo-Json -Compress))
''', encoding="utf-8")
            environment = os.environ.copy()
            environment["PDF_NOTE_CONTRACT_REPO_ROOT"] = str(REPO_ROOT)
            result = subprocess.run([
                shutil.which("powershell.exe") or shutil.which("powershell"), "-NoProfile",
                "-ExecutionPolicy", "Bypass", "-File", str(script),
            ], capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(next(line[len("RESULT:"):] for line in result.stdout.splitlines()
                                   if line.startswith("RESULT:")))
            self.assertEqual(data, {"exact_fixture": False, "changed_url": True, "extra_url": True,
                                    "network_api": True, "sound_api": True, "other_file": True})

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

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_upstream_office_entry_distinguishes_skip_conversion_comparison_and_failure(self):
        temp_root = REPO_ROOT / ".local/repo_resource/tmp"
        temp_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="upstream_contract_", dir=temp_root) as temporary:
            script = Path(temporary) / "upstream_contract.ps1"
            script.write_text(r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$sourcePath = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/scripts/run_repo_checks.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Unable to parse repository checks.' }
$block = @($ast.FindAll({param($node)
    $node -is [System.Management.Automation.Language.IfStatementAst] -and
    $node.Clauses[0].Item1.Extent.Text -eq '$IncludeOfficeConversionTests' -and
    $node.Extent.Text.Contains('$officeUpstreamConversionScript')
}, $true))
if ($block.Count -ne 1) { throw 'Missing unique upstream check integration.' }
$officeConversionFixtureScript = 'fixture.ps1'; $officeUpstreamConversionScript = 'upstream.ps1'
$OfficeSoffice = ''; $KeepOfficeConversionOutputs = $false; $IncludeOfficeConversionTests = $true
$OfficeReductionAcceptance = $false; $OfficeUpstreamGroup = ''; $OfficeUpstreamSample = ''
$OfficeFixedTime = $null
function Invoke-Step {
    param([string]$Name, [scriptblock]$Action)
    $script:steps.Add($Name)
    $output = @(& $Action)
    if ($output.Count -ne 0) { throw 'Unexpected step output.' }
}
function Invoke-ChildPowerShellScript {
    param([string]$ScriptPath, [string[]]$Arguments)
    $script:calls.Add(@{path=$ScriptPath; arguments=$Arguments})
    if ($script:injectFailure -and $ScriptPath -eq 'upstream.ps1') { throw 'upstream failed' }
}
$results = @{}
foreach ($case in @('skip', 'conversion', 'comparison', 'failure', 'acceptance', 'fixed', 'fixed_failure')) {
    $script:steps = [Collections.Generic.List[string]]::new()
    $script:calls = [Collections.Generic.List[object]]::new()
    $OfficeUpstreamSourceDir = if ($case -eq 'skip') { '' } else { 'local source' }
    $OfficeBaselineSoffice = if ($case -in @('comparison', 'failure', 'acceptance', 'fixed', 'fixed_failure')) { 'old runtime' } else { '' }
    $OfficeReductionAcceptance = $case -eq 'acceptance'
    $OfficeFixedTime = if ($case -in @('fixed', 'fixed_failure')) { 1767323045 } else { $null }
    $script:injectFailure = $case -in @('failure', 'fixed_failure')
    $failed = $false; $messages = ''
    try { $messages = (Invoke-Expression $block[0].Extent.Text 6>&1 | Out-String) } catch { $failed = $true }
    $results[$case] = @{failed=$failed; steps=@($script:steps); calls=@($script:calls); skip=$messages.Contains('[SKIP]')}
}
Write-Output ('RESULT:' + ($results | ConvertTo-Json -Depth 5 -Compress))
''', encoding="utf-8")
            environment = os.environ.copy()
            environment["PDF_NOTE_CONTRACT_REPO_ROOT"] = str(REPO_ROOT)
            result = subprocess.run([
                shutil.which("powershell.exe") or shutil.which("powershell"), "-NoProfile",
                "-ExecutionPolicy", "Bypass", "-File", str(script),
            ], capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(next(line[len("RESULT:"):] for line in result.stdout.splitlines()
                                   if line.startswith("RESULT:")))
            self.assertTrue(data["skip"]["skip"])
            self.assertEqual(len(data["skip"]["calls"]), 1)
            self.assertEqual(data["conversion"]["steps"][-1], "Upstream Office Conversion Only")
            self.assertEqual(data["comparison"]["steps"][-1], "Upstream Office PDF Regression Comparison")
            self.assertEqual(data["comparison"]["calls"][-1]["arguments"],
                             ["-SourceDir", "local source", "-BaselineSoffice", "old runtime"])
            self.assertTrue(data["failure"]["failed"])
            self.assertEqual(data["acceptance"]["steps"][-1], "Office Reduction Full Corpus Acceptance")
            self.assertEqual(data["acceptance"]["calls"][-1]["arguments"][-1], "-Acceptance")
            self.assertEqual(data['fixed']['calls'][-1]['arguments'][-2:], ['-FixedTime', '1767323045'])
            self.assertFalse(data['fixed']['failed'])
            self.assertTrue(data['fixed_failure']['failed'])
            for case in ("skip", "conversion", "comparison"):
                self.assertFalse(data[case]["failed"])

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_pdf_annotation_gate_runs_by_default_and_propagates_failure(self):
        temp_root = REPO_ROOT / ".local/repo_resource/tmp"
        temp_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pdf_annotation_gate_", dir=temp_root) as temporary:
            script = Path(temporary) / "gate_contract.ps1"
            script.write_text(r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$sourcePath = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/scripts/run_repo_checks.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Unable to parse repository checks.' }
$block = @($ast.FindAll({param($node)
    $node -is [System.Management.Automation.Language.IfStatementAst] -and
    $node.Clauses[0].Item1.Extent.Text -eq '-not $SkipPdfAnnotationExportTests'
}, $true))
if ($block.Count -ne 1) { throw 'Missing unique PDF annotation gate.' }
function Invoke-Step { param($Name, $Action) $script:steps += $Name; & $Action }
function Invoke-ChildPowerShellScript {
    param($ScriptPath)
    $script:calls += $ScriptPath
    if ($script:injectFailure) { throw 'independent-engine checker failed' }
}
$pdfAnnotationExportScript = 'run_pdf_annotation_export_tests.ps1'
$results = @{}
foreach ($case in @('default', 'skip', 'failure')) {
    $SkipPdfAnnotationExportTests = $case -eq 'skip'
    $script:injectFailure = $case -eq 'failure'
    $script:steps = @(); $script:calls = @(); $failed = $false
    try { Invoke-Expression $block[0].Extent.Text } catch { $failed = $true }
    $results[$case] = @{failed=$failed; steps=@($script:steps); calls=@($script:calls)}
}
Write-Output ('RESULT:' + ($results | ConvertTo-Json -Depth 4 -Compress))
''', encoding="utf-8")
            environment = os.environ.copy()
            environment["PDF_NOTE_CONTRACT_REPO_ROOT"] = str(REPO_ROOT)
            result = subprocess.run([
                shutil.which("powershell.exe") or shutil.which("powershell"), "-NoProfile",
                "-ExecutionPolicy", "Bypass", "-File", str(script),
            ], capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(next(line[len("RESULT:"):] for line in result.stdout.splitlines()
                                   if line.startswith("RESULT:")))
            self.assertEqual(data["skip"], {"failed": False, "steps": [], "calls": []})
            for case in ("default", "failure"):
                self.assertEqual(data[case], {
                    "failed": case == "failure",
                    "steps": ["PDF Annotation Export And Interoperability Tests"],
                    "calls": ["run_pdf_annotation_export_tests.ps1"],
                })

    def test_pdf_interop_missing_dependency_is_not_silently_skipped(self):
        interop = load_module("pdf_annotation_interop_check", "tests/python/pdf_annotation_interop_check.py")
        with mock.patch.dict(sys.modules, {"pypdf": None}), self.assertRaises(ModuleNotFoundError):
            interop.check(REPO_ROOT / "out/tests/absent_pdf_annotation_fixtures")
        with self.assertRaises(RuntimeError):
            interop.require(False, "unverifiable annotation")

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_reduction_acceptance_rejects_missing_inputs_and_subset_before_build(self):
        shell = shutil.which("powershell.exe") or shutil.which("powershell")
        script = REPO_ROOT / "tests/scripts/run_repo_checks.ps1"
        for options in ([], ["-OfficeUpstreamSourceDir", "source"],
                        ["-OfficeBaselineSoffice", "baseline"],
                        ["-OfficeUpstreamSourceDir", "source", "-OfficeBaselineSoffice", "baseline",
                         "-OfficeUpstreamGroup", "calc-related"]):
            with self.subTest(options=options):
                result = subprocess.run([shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                         str(script), "-OfficeReductionAcceptance", *options],
                                        capture_output=True, text=True, timeout=30, cwd=REPO_ROOT)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Office reduction acceptance", result.stderr)


if __name__ == "__main__":
    unittest.main()
