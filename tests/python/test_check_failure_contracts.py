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
check_evaluator = load_module("check_evaluator", "tools/dev/evaluate_checks.py")


class CheckFailureContractTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_repo_aggregate_reaches_last_step_and_counts_all_failures(self):
        # Keep the real plan, options, logging, loop and final exit. Replace only
        # check actions so this evaluates the runner without building or UI use.
        generator = r'''
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$sourcePath = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/scripts/run_repo_checks.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Runner parse failure' }
$source = [IO.File]::ReadAllText($sourcePath)
$replacements = @()
foreach ($command in $ast.FindAll({param($n)
    $n -is [System.Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Invoke-Step'
}, $true)) {
    $elements = $command.CommandElements
    for ($i = 0; $i -lt $elements.Count - 1; $i++) {
        if ($elements[$i] -is [System.Management.Automation.Language.CommandParameterAst] -and
            $elements[$i].ParameterName -eq 'Action' -and
            $elements[$i + 1] -is [System.Management.Automation.Language.ScriptBlockExpressionAst]) {
            $replacements += $elements[$i + 1].Extent
        }
    }
}
if ($replacements.Count -lt 30) { throw 'Incomplete fixture action replacement' }
$replacement = @'
{
    if ($env:PDF_NOTE_CHECK_FIXTURE_MODE -eq 'interrupt' -and $script:CurrentStepName -eq 'Build') {
        Invoke-LoggedCommand -FilePath $powershellExe -Arguments @('-NoProfile', '-Command', 'exit 130')
    }
    if ($env:PDF_NOTE_CHECK_FIXTURE_MODE -eq 'multiple') {
        switch ($script:CurrentStepName) {
            'Build' { throw 'fixture build failure' }
            'Workspace Memo Tests' {
                Invoke-LoggedCommand -FilePath $powershellExe -Arguments @('-NoProfile', '-Command', 'exit 7')
            }
            'Text Encoding Tests' { return $false }
        }
    }
    Write-Host 'fixture action completed'
}
'@
foreach ($extent in $replacements | Sort-Object StartOffset -Descending) {
    $source = $source.Substring(0, $extent.StartOffset) + $replacement + $source.Substring($extent.EndOffset)
}
$destination = Join-Path $env:PDF_NOTE_CONTRACT_FIXTURE 'tests/scripts/run_repo_checks.ps1'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
[IO.File]::WriteAllText($destination, $source, [Text.UTF8Encoding]::new($true))
'''
        shell = shutil.which("powershell.exe") or shutil.which("powershell")
        with tempfile.TemporaryDirectory(prefix="pdf_note_aggregate_") as temporary:
            root = Path(temporary)
            script = root / "generate.ps1"
            script.write_text(generator, encoding="utf-8")
            environment = os.environ.copy()
            environment.update(PDF_NOTE_CONTRACT_REPO_ROOT=str(REPO_ROOT), PDF_NOTE_CONTRACT_FIXTURE=temporary)
            generated = subprocess.run([shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)], capture_output=True,
                                       text=True, env=environment, timeout=30)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            fixture = root / "tests/scripts/run_repo_checks.ps1"
            for case, aggregate, mode in (("multiple", True, "multiple"), ("success", True, "success"),
                                           ("fail_fast", False, "multiple"), ("interrupt", True, "interrupt")):
                with self.subTest(case=case):
                    environment["PDF_NOTE_CHECK_FIXTURE_MODE"] = mode
                    log_dir = root / case
                    args = [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(fixture), "-LogDir", str(log_dir), "-FailureTailLines", "1"]
                    if aggregate:
                        args += ["-ContinueOnFailure"]
                    result = subprocess.run(args, capture_output=True, text=True, env=environment, timeout=60)
                    data = json.loads((log_dir / "summary.json").read_text(encoding="utf-8-sig"))
                    self.assertEqual(result.returncode, 0 if mode == "success" else (130 if mode == "interrupt" else 1), result.stdout + result.stderr)
                    self.assertGreater(data["counts"]["selected"], 30)
                    self.assertEqual(data["counts"]["selected"], len(data["steps"]))
                    self.assertEqual(sum(data["counts"][key] for key in ("pass", "fail", "not_run")),
                                     data["counts"]["selected"])
                    self.assertEqual(data["steps"][-1]["name"], "Binary Artifact Network Import Scan")
                    self.assertEqual(data["steps"][-1]["status"], "PASS" if aggregate and mode != "interrupt" else "NOT_RUN")
                    self.assertEqual(data["counts"]["fail"], 0 if mode == "success" else (3 if aggregate and mode != "interrupt" else 1))
                    self.assertEqual(data["counts"]["not_run"], 0 if aggregate and mode != "interrupt" else data["counts"]["selected"] - 1)
                    self.assertEqual(data["status"], "PASS" if mode == "success" else "FAIL")
                    for row in data["steps"]:
                        if row["status"] in ("PASS", "FAIL"):
                            detail = Path(row["log"]).read_text(encoding="utf-8-sig")
                            self.assertIn("status: " + row["status"], detail)
                    self.assertIn("Repository check summary:", result.stdout)
            # A requested-but-disabled check must remain in the denominator.
            skipped_logs = root / "disabled"
            skipped = subprocess.run([shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(fixture),
                                      "-ContinueOnFailure", "-OnlyStep", "Atomic Write Tests", "-SkipAtomicWrite",
                                      "-LogDir", str(skipped_logs)],
                                     capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(skipped.returncode, 1)
            disabled = json.loads((skipped_logs / "summary.json").read_text(encoding="utf-8-sig"))
            self.assertEqual(disabled["counts"], {"selected": 1, "pass": 0, "fail": 0, "not_run": 1})
            self.assertTrue(disabled["partial_run"])
            self.assertIn("SkipAtomicWrite", disabled["skip_options"])
            # Even when log creation fails, each planned step is attempted and
            # its failure remains visible in the primary console/transcript.
            blocked_logs = root / "log_path_is_a_file"
            blocked_logs.write_text("keep this evidence", encoding="utf-8")
            broken = subprocess.run([shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(fixture),
                                     "-ContinueOnFailure", "-LogDir", str(blocked_logs)],
                                    capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(broken.returncode, 1)
            self.assertEqual(blocked_logs.read_text(encoding="utf-8"), "keep this evidence")
            self.assertIn(f"Selected={data['counts']['selected']}; PASS=0; FAIL={data['counts']['selected']}; NOT_RUN=0", broken.stdout)
            self.assertIn("Unable to write repository check summary", broken.stdout)

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_publish_requests_aggregate_checks_and_rejects_failure(self):
        command = r'''
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'publish.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Publish parse failure' }
$functions = @($ast.FindAll({param($n)
    $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-FullRepositoryChecks'
}, $true))
if ($functions.Count -ne 1) { throw 'Missing publish check entry point' }
$functionsFile = Join-Path (Split-Path -Parent $env:PDF_NOTE_FAKE_CHECK_HOST) 'functions.ps1'
[IO.File]::WriteAllText($functionsFile, $functions[0].Extent.Text)
. $functionsFile
function Get-Process { param($Id, $ErrorAction) [pscustomobject]@{ Path = $env:PDF_NOTE_FAKE_CHECK_HOST } }
function Write-Info { param($Message) Write-Host $Message }
foreach ($code in @(0, 7)) {
    $env:PDF_NOTE_FAKE_CHECK_EXIT = [string]$code
    $failed = $false
    try { Invoke-FullRepositoryChecks }
    catch {
        if ($_.Exception.Message -notlike '*final confirmation word will not be issued*') { throw }
        $failed = $true
    }
    if ($failed -ne ($code -ne 0)) { throw 'Publish accepted failed checks or rejected success' }
}
Write-Host 'PUBLISH_AGGREGATE_CONTRACT_PASS'
'''
        with tempfile.TemporaryDirectory(prefix="pdf_note_publish_checks_") as temporary:
            root = Path(temporary)
            entry = root / "tests/scripts/run_repo_checks.ps1"
            entry.parent.mkdir(parents=True)
            entry.write_text("throw 'Real checks must not be called by this fixture'", encoding="utf-8")
            host = root / "fake_host.ps1"
            host.write_text("if ($args -notcontains '-ContinueOnFailure') { throw 'Aggregate mode missing' }\n"
                            "if (@($args | Where-Object { $_ -like '-Skip*' -or $_ -eq '-OnlyStep' }).Count) "
                            "{ throw 'Publish requested partial checks' }\n"
                            "Write-Output 'fixture summary emitted'\n"
                            "$global:LASTEXITCODE = [int]$env:PDF_NOTE_FAKE_CHECK_EXIT\n", encoding="utf-8")
            script = root / "publish_contract.ps1"
            script.write_text(command, encoding="utf-8")
            environment = os.environ.copy()
            environment.update(PDF_NOTE_CONTRACT_REPO_ROOT=str(REPO_ROOT), PDF_NOTE_FAKE_CHECK_HOST=str(host))
            result = subprocess.run([shutil.which("powershell.exe") or shutil.which("powershell"),
                                     "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                                    capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("PUBLISH_AGGREGATE_CONTRACT_PASS", result.stdout)

    def test_evaluation_distinguishes_pass_fail_skip_and_empty(self):
        class Probes(unittest.TestCase):
            def good(self):
                pass

            def bad(self):
                self.fail("intentional negative evaluation fixture")

            @unittest.skip("required environment unavailable")
            def skipped(self):
                pass

            @unittest.expectedFailure
            def known_gap(self):
                self.fail("known evaluation gap")

        for name, expected in (("good", "PASS"), ("bad", "FAIL"),
                               ("skipped", "INCOMPLETE"), ("known_gap", "INCOMPLETE"),
                               (None, "INCOMPLETE")):
            with self.subTest(probe=name):
                suite = unittest.TestSuite([] if name is None else [Probes(name)])
                result = check_evaluator.evaluate_suite(suite, io.StringIO())
                self.assertEqual(result["status"], expected)
                self.assertEqual(result["tests_run"], 0 if name is None else 1)

    def test_evaluation_receipt_and_exit_reject_skip_empty_and_changed_inputs(self):
        for case in ("pass", "skip", "empty", "changed", "error"):
            with self.subTest(case=case), tempfile.TemporaryDirectory(prefix="pdf_note_evaluation_") as temporary:
                def probe():
                    if case == "skip":
                        raise unittest.SkipTest("required fixture missing")

                suite = unittest.TestSuite([] if case == "empty" else [unittest.FunctionTestCase(probe)])
                snapshots = [{"input.py": "before"}, {"input.py": "after" if case == "changed" else "before"}]
                if case == "error":
                    snapshots = [OSError("unreadable evaluation input")]
                with mock.patch.object(check_evaluator, "ROOT", Path(temporary)), \
                     mock.patch.object(check_evaluator, "CHECK_ENTRIES", ()), \
                     mock.patch.object(check_evaluator, "git", return_value="fixture_git"), \
                     mock.patch.object(check_evaluator, "input_snapshot", side_effect=snapshots), \
                     mock.patch.object(unittest.TestLoader, "loadTestsFromName", return_value=suite), \
                     mock.patch.object(sys, "path", sys.path.copy()), redirect_stdout(io.StringIO()):
                    exit_code = check_evaluator.main(["--evaluate", "--profile", "failure-contracts"])
                reports = list(Path(temporary).glob("out/tests/check_evaluation/*/evaluation.json"))
                self.assertEqual(len(reports), 1)
                data = json.loads(reports[0].read_text(encoding="utf-8"))
                expected = {"pass": "PASS", "skip": "INCOMPLETE", "empty": "INCOMPLETE",
                            "changed": "INPUT_CHANGED", "error": "ERROR"}[case]
                self.assertEqual(data["profiles"][0]["status"], expected)
                self.assertEqual(exit_code, 0 if case == "pass" else 1)
                self.assertEqual(data["status"], "PASS" if case == "pass" else "FAIL")
                self.assertIn("evaluated_at", data["profiles"][0])
                if case != "error":
                    self.assertTrue(Path(data["profiles"][0]["log"]).is_file())

    def test_evaluation_rejects_missing_source_scope(self):
        with tempfile.TemporaryDirectory(prefix="pdf_note_empty_scope_") as temporary, \
             mock.patch.object(check_evaluator, "ROOT", Path(temporary)), \
             mock.patch.object(check_evaluator, "INPUT_PATTERNS", ("absent/*.py",)):
            with self.assertRaisesRegex(RuntimeError, "has no files"):
                check_evaluator.input_snapshot()

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_ui_evidence_retains_failure_and_refuses_reuse(self):
        command = r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$sourcePath = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/scripts/run_ui_automation_fault_tests.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Runner parse failure' }
$helper = @($ast.FindAll({param($n)
    $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $n.Name -eq 'Write-UiAutomationFailureEvidence'
}, $true))
if ($helper.Count -ne 1) { throw 'Missing evidence helper' }
Invoke-Expression $helper[0].Extent.Text
$handler = @($ast.FindAll({param($n)
    $n -is [System.Management.Automation.Language.CatchClauseAst] -and
    $n.Extent.Text.Contains('Write-UiAutomationFailureEvidence -Paths')
}, $true))
if ($handler.Count -ne 1) { throw 'Missing failure handler' }
$parameter = @($ast.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq 'ArtifactName' })
$first = Invoke-Expression $parameter[0].DefaultValue.Extent.Text
$second = Invoke-Expression $parameter[0].DefaultValue.Extent.Text
if ($first -eq $second -or $first -notmatch '^ui_automation_[a-f0-9]{32}$') { throw 'Evidence identity reused' }
$source = [IO.File]::ReadAllText($sourcePath)
$boundIdentity = $source.IndexOf("`$PSBoundParameters['ArtifactName'] = `$ArtifactName")
$dispatch = $source.IndexOf('if (Invoke-AppTestDesktop')
if ($boundIdentity -lt 0 -or $dispatch -lt 0 -or $boundIdentity -gt $dispatch) {
    throw 'Identity not passed to isolated child'
}
$outDir = Join-Path $env:PDF_NOTE_CONTRACT_FIXTURE 'previous'
New-Item -ItemType Directory -Path $outDir | Out-Null
$marker = Join-Path $outDir 'keep.txt'
[IO.File]::WriteAllText($marker, 'previous evidence')
$guard = @($ast.FindAll({param($n)
    $n -is [System.Management.Automation.Language.IfStatementAst] -and
    $n.Clauses[0].Item1.Extent.Text -eq 'Test-Path -LiteralPath $outDir'
}, $true))
if ($guard.Count -ne 1) { throw 'Missing output reuse guard' }
$refused = $false
try { Invoke-Expression $guard[0].Extent.Text } catch { $refused = $true }
if (-not $refused -or [IO.File]::ReadAllText($marker) -ne 'previous evidence') { throw 'Previous evidence lost' }
$workspaceRoot = $outDir
$resultFile = Join-Path $outDir 'result.txt'; $traceFile = Join-Path $outDir 'absent.trace'
[IO.File]::WriteAllLines($resultFile, [string[]]@(0..34 | ForEach-Object { 'evidence-line-' + $_ }))
$failed = $false
try { Invoke-Expression ("try { throw 'original-child-failure' } " + $handler[0].Extent.Text) }
catch { $failed = $_.Exception.Message -eq 'original-child-failure' }
if (-not $failed) { throw 'Child failure was swallowed or replaced' }
$large = Join-Path $outDir 'large.trace'
$stream = [IO.File]::Create($large); $stream.SetLength(16MB + 1); $stream.Dispose()
Write-UiAutomationFailureEvidence -Paths @($large)
if ((Get-Item -LiteralPath $large).Length -ne (16MB + 1)) { throw 'Evidence truncated' }
$locked = [IO.File]::Open($resultFile, 'Open', 'ReadWrite', 'None')
try {
    $failed = $false
    try { Invoke-Expression ("try { throw 'original-child-failure' } " + $handler[0].Extent.Text) }
    catch { $failed = $_.Exception.Message -eq 'original-child-failure' }
    if (-not $failed) { throw 'Diagnostic read error replaced child failure' }
} finally { $locked.Dispose() }
Write-Host 'EVIDENCE_CONTRACT_PASS'
'''
        with tempfile.TemporaryDirectory(prefix="pdf_note_evidence_") as temporary:
            script = Path(temporary) / "evidence.ps1"
            script.write_text(command, encoding="utf-8")
            environment = os.environ.copy()
            environment.update(PDF_NOTE_CONTRACT_REPO_ROOT=str(REPO_ROOT), PDF_NOTE_CONTRACT_FIXTURE=temporary)
            result = subprocess.run([shutil.which("powershell.exe") or shutil.which("powershell"),
                                     "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                                    capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            for marker in ("EVIDENCE_CONTRACT_PASS", "evidence-line-34", "Evidence file was not created",
                           "16 MiB", "Unable to read diagnostic evidence"):
                self.assertIn(marker, result.stdout)
            self.assertNotIn("evidence-line-0\n", result.stdout)

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_settings_workflow_is_independent_and_propagates_failure(self):
        command = r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$path = Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT 'tests/scripts/run_repo_checks.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Runner parse failure' }
$blocks = @($ast.FindAll({param($n)
    $n -is [System.Management.Automation.Language.IfStatementAst] -and
    $n.Clauses[0].Item1.Extent.Text -eq '-not $SkipUiAutomation -and -not $SkipSettingsWorkflowTests'
}, $true))
if ($blocks.Count -ne 1) { throw 'Missing independent settings workflow step' }
function Invoke-Step { param($Name, $Action) $script:step = $Name; & $Action }
function Invoke-ChildPowerShellScript {
    param($ScriptPath)
    if ($ScriptPath -ne 'settings.ps1') { throw 'Wrong workflow entry point' }
    $script:calls++
    if ($script:injectFailure) { throw 'workflow-child-failure' }
}
$settingsWorkflowTestScript = 'settings.ps1'
foreach ($case in @('default', 'skip_ui', 'skip_workflow', 'skip_memo', 'failure')) {
    $SkipUiAutomation = $case -eq 'skip_ui'; $SkipSettingsWorkflowTests = $case -eq 'skip_workflow'
    $SkipWorkspaceMemoTests = $case -eq 'skip_memo'; $script:injectFailure = $case -eq 'failure'
    $script:calls = 0; $script:step = ''; $failed = $false
    try { Invoke-Expression $blocks[0].Extent.Text }
    catch { if ($_.Exception.Message -ne 'workflow-child-failure') { throw }; $failed = $true }
    $expectedCalls = if ($case -in @('skip_ui', 'skip_workflow')) { 0 } else { 1 }
    if ($script:calls -ne $expectedCalls -or $failed -ne ($case -eq 'failure')) { throw "Wrong workflow result: $case" }
    if ($expectedCalls -and $script:step -ne 'Settings And Workspace Workflow Tests') { throw 'Wrong step identity' }
}
Write-Host 'WORKFLOW_CONTRACT_PASS'
'''
        environment = os.environ.copy()
        environment["PDF_NOTE_CONTRACT_REPO_ROOT"] = str(REPO_ROOT)
        result = subprocess.run([shutil.which("powershell.exe") or shutil.which("powershell"),
                                 "-NoProfile", "-Command", command],
                                capture_output=True, text=True, env=environment, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("WORKFLOW_CONTRACT_PASS", result.stdout)

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_pdf_textbox_reedit_gate_rejects_missing_cases(self):
        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        command = r'''
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $env:PDF_NOTE_REEDIT_GATE_REPO 'tests/scripts/run_ui_automation_fault_tests.ps1'),
    [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Runner parse failure' }
$functions = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Assert-PdfTextboxReeditFontTrace'
}, $true))
if ($functions.Count -ne 1) { throw 'Missing textbox reedit gate' }
Invoke-Expression $functions[0].Extent.Text
$lines = @('pdf_textbox:presets_theme_ok cases=4', 'pdf_textbox:geometry_ok cases=192', 'pdf_textbox:reedit_hover_ok cases=432',
    'pdf_textbox:reedit_font_ok cases=6 cycles=48') +
    @(1..6 | ForEach-Object { 'pdf_textbox:reedit_font_case_ok' }) +
    @(1..48 | ForEach-Object { 'pdf_textbox:reedit_font_observation scale=2 adaptive=1 valid=1' })
if (@(Assert-PdfTextboxReeditFontTrace ($lines -join "`n")).Count -ne 0) {
    throw 'Successful gate returned unexpected output'
}
foreach ($index in @(0, 1, 2, 3, 4, 10)) {
    $partial = @(for ($j = 0; $j -lt $lines.Count; ++$j) { if ($j -ne $index) { $lines[$j] } })
    $rejected = $false
    try { Assert-PdfTextboxReeditFontTrace ($partial -join "`n") } catch { $rejected = $true }
    if (-not $rejected) { throw "Incomplete reedit evidence accepted at $index" }
}
foreach ($count in @(431, 433)) {
    $partial = ($lines -join "`n").Replace('reedit_hover_ok cases=432', "reedit_hover_ok cases=$count")
    $rejected = $false
    try { Assert-PdfTextboxReeditFontTrace $partial } catch { $rejected = $true }
    if (-not $rejected) { throw "Incorrect hover case count accepted: $count" }
}
foreach ($count in @(3, 5)) {
    $partial = ($lines -join "`n").Replace('presets_theme_ok cases=4', "presets_theme_ok cases=$count")
    $rejected = $false
    try { Assert-PdfTextboxReeditFontTrace $partial } catch { $rejected = $true }
    if (-not $rejected) { throw "Incorrect preset/theme case count accepted: $count" }
}
'''
        env = dict(os.environ, PDF_NOTE_REEDIT_GATE_REPO=str(REPO_ROOT))
        result = subprocess.run([powershell, "-NoProfile", "-Command", command],
                                capture_output=True, text=True, timeout=30, env=env, cwd=REPO_ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_ime_geometry_gate_rejects_missing_observations(self):
        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        command = r'''
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $env:PDF_NOTE_IME_GATE_REPO 'tests/scripts/run_ui_automation_fault_tests.ps1'),
    [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Runner parse failure' }
$functions = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Assert-ImeGeometryTrace'
}, $true))
if ($functions.Count -ne 1) { throw 'Missing IME gate' }
Invoke-Expression $functions[0].Extent.Text
$lines = @(
    'math_render:ime_candidate_requests_valid=1',
    'math_render:ime_candidate_requests_valid=1',
    'math_render:ime_candidate_requests_valid=1',
    'math_render:ime_character_query valid=1 answered=1',
    'math_render:ime_character_query valid=1 answered=1',
    'math_render:ime_offscreen_candidate_unchanged=1 caret=-35:0',
    'math_render:ime_visible_prerequisite_recovery=1',
    'math_render:ime_heading_band_valid=1 caret_bottom=17 row_bottom=35',
    'math_render:ime_geometry_only',
    'automation:ime_geometry_ok'
)
foreach ($focused in @($false, $true)) {
    $output = @(Assert-ImeGeometryTrace -TraceText ($lines -join "`n") -Focused:$focused)
    if ($output.Count -ne 0) { throw 'Successful gate returned unexpected output' }
}
foreach ($index in 0..($lines.Count - 1)) {
    $partial = @(for ($j = 0; $j -lt $lines.Count; ++$j) {
        if ($j -ne $index) { $lines[$j] }
    })
    $rejected = $false
    try { Assert-ImeGeometryTrace -TraceText ($partial -join "`n") -Focused }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Incomplete IME evidence accepted at $index" }
}
'''
        env = dict(os.environ, PDF_NOTE_IME_GATE_REPO=str(REPO_ROOT))
        result = subprocess.run([powershell, "-NoProfile", "-Command", command],
                                capture_output=True, text=True, timeout=30, env=env, cwd=REPO_ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_ime_geometry_mode_rejects_conflicting_selection(self):
        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        script = REPO_ROOT / "tests/scripts/run_ui_automation_fault_tests.ps1"
        for options in (["-ImeGeometryOnly"],
                        ["-MathRenderOnly", "-RenderedClickOnly", "-ImeGeometryOnly"]):
            with self.subTest(options=options):
                result = subprocess.run([powershell, "-NoProfile", "-ExecutionPolicy", "Bypass",
                                         "-File", str(script), *options],
                                        capture_output=True, text=True, timeout=30, cwd=REPO_ROOT)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("-ImeGeometryOnly requires -MathRenderOnly", result.stderr)

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
    def test_safety_scan_allows_only_exact_parser_url_fixtures(self):
        with tempfile.TemporaryDirectory(prefix="pdf_note_scan_") as temporary:
            root = Path(temporary)
            script = root / "scan_contract.ps1"
            script.write_text(r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
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
$results = @{}
New-Item -ItemType Directory -Force -Path $scanRoot | Out-Null
Push-Location -LiteralPath $scanRoot
try {
foreach ($relativeFixture in @('tests/unit/md4c_allocation_failure_tests.c')) {
$fixturePath = Join-Path $scanRoot $relativeFixture
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $fixturePath) | Out-Null
$original = [IO.File]::ReadAllText((Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT $relativeFixture))
if (-not $original.Contains('https://example.invalid/')) { throw 'Parser URL fixture no longer contains its expected input.' }
$cases = @{
    exact_fixture = $original
    changed_url = $original.Replace('https://example.invalid/', 'https://other.invalid/')
    extra_url = [regex]::Replace($original, '(?m)^(.*https://example\.invalid/.*)$', '$1 /* https://other.invalid/ */')
    extra_line = $original + "`n// https://other.invalid/`n"
    network_api = $original + "`nvoid forbidden(void) { socket(); }`n"
    sound_api = $original + "`nvoid forbidden(void) { Beep(1, 1); }`n"
    other_file = $original
    retired_fixture = $original
}
    $fixtureResults = @{}
    foreach ($name in $cases.Keys) {
        [IO.File]::WriteAllText($fixturePath, $cases[$name])
        $otherPath = Join-Path $scanRoot 'other.c'
        if ($name -eq 'other_file') { [IO.File]::WriteAllText($otherPath, $original) }
        $retiredPath = Join-Path $scanRoot 'tests/unit/note_clro_fullwidth_regression.h'
        if ($name -eq 'retired_fixture') {
            # This former parser input has been removed; its exact old line must
            # no longer grant a URL exception at the old path.
            $retiredLine = '    {L"' + [char]0xff1c + 'https://example.invalid/a' + [char]0xff0a + 'b' + [char]0xff1e + '", "href=\"https://example.invalid/a"},'
            [IO.File]::WriteAllText($retiredPath, $retiredLine)
        }
        if ($name -in @('changed_url', 'extra_url') -and $cases[$name] -ceq $original) {
            throw "Fixture mutation did not change its input: $relativeFixture/$name"
        }
        $failed = $false
        $failure = ''
        try { Invoke-SafetyScans } catch { $failed = $true; $failure = $_.Exception.Message }
        $fixtureResults[$name] = @{ failed = $failed; error = $failure }
        if (Test-Path -LiteralPath $otherPath) { Remove-Item -LiteralPath $otherPath }
        if (Test-Path -LiteralPath $retiredPath) { Remove-Item -LiteralPath $retiredPath }
    }
    $results[$relativeFixture] = $fixtureResults
    Remove-Item -LiteralPath $fixturePath
}
} finally { Pop-Location }
Write-Output ('RESULT:' + ($results | ConvertTo-Json -Depth 4 -Compress))
''', encoding="utf-8")
            environment = os.environ.copy()
            environment["PDF_NOTE_CONTRACT_REPO_ROOT"] = str(REPO_ROOT)
            result = subprocess.run([
                shutil.which("powershell.exe") or shutil.which("powershell"), "-NoProfile",
                "-ExecutionPolicy", "Bypass", "-File", str(script),
            ], capture_output=True, text=True, encoding="utf-8", env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(next(line[len("RESULT:"):] for line in result.stdout.splitlines()
                                   if line.startswith("RESULT:")))
            expected = {"exact_fixture": False, "changed_url": True, "extra_url": True,
                        "extra_line": True, "network_api": True, "sound_api": True,
                        "other_file": True, "retired_fixture": True}
            self.assertEqual(set(data), {"tests/unit/md4c_allocation_failure_tests.c"})
            for fixture, observations in data.items():
                self.assertEqual(set(observations), set(expected))
                for case, failed in expected.items():
                    with self.subTest(fixture=fixture, case=case):
                        self.assertEqual(observations[case]["failed"], failed, observations[case])

    @unittest.skipUnless((shutil.which("powershell.exe") or shutil.which("powershell")) and shutil.which("rg"),
                         "requires PowerShell and ripgrep")
    def test_safety_scan_limits_reviewed_dialog_calls_and_network_denial_mock(self):
        with tempfile.TemporaryDirectory(prefix="pdf_note_dialog_scan_") as temporary:
            script = Path(temporary) / "dialog_scan_contract.ps1"
            script.write_text(r'''
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
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
New-Item -ItemType Directory -Force -Path $scanRoot | Out-Null
$results = @{}
Push-Location -LiteralPath $scanRoot
try {
    foreach ($relative in @('src/recovery/stage_recovery_window.cpp', 'tools/recovery_demo/demo_ui.cpp', 'tests/python/test_site_validation.py')) {
        $path = Join-Path $scanRoot $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        $original = [IO.File]::ReadAllText((Join-Path $env:PDF_NOTE_CONTRACT_REPO_ROOT $relative))
        $cases = @{
            exact_calls = $original
            changed_api = $original.Replace('GetOpenFileNameW(', 'SHBrowseForFolderW(')
            extra_call = $original + "`nvoid forbidden() { GetOpenFileNameW(nullptr); }`n"
            extra_same_line = [regex]::Replace($original, '(?m)^(.*GetOpenFileNameW.*)$', '$1 GetSaveFileNameW(nullptr);')
            other_file = $original
        }
        if ($relative.EndsWith('.py')) {
            $cases.changed_api = $original.Replace('AssertionError("network forbidden")', 'None')
            $cases.extra_call = $original + "`nsocket.socket()`n"
            $cases.extra_same_line = [regex]::Replace($original, '(?m)^(.*mock\.patch\("socket\.socket".*)$', ('$1 # socket.' + 'socket()'))
        }
        $observations = @{}
        foreach ($name in $cases.Keys) {
            [IO.File]::WriteAllText($path, $cases[$name])
            $other = Join-Path $scanRoot ('other' + [IO.Path]::GetExtension($relative))
            if ($name -eq 'other_file') { [IO.File]::WriteAllText($other, $original) }
            $failed = $false
            try { Invoke-SafetyScans } catch { $failed = $true }
            $observations[$name] = $failed
            if (Test-Path -LiteralPath $other) { Remove-Item -LiteralPath $other }
        }
        $results[$relative] = $observations
        Remove-Item -LiteralPath $path
    }
} finally { Pop-Location }
Write-Output ('RESULT:' + ($results | ConvertTo-Json -Compress))
''', encoding="utf-8")
            environment = os.environ.copy()
            environment["PDF_NOTE_CONTRACT_REPO_ROOT"] = str(REPO_ROOT)
            result = subprocess.run([
                shutil.which("powershell.exe") or shutil.which("powershell"), "-NoProfile",
                "-ExecutionPolicy", "Bypass", "-File", str(script),
            ], capture_output=True, text=True, encoding="utf-8", env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(next(line[len("RESULT:"):] for line in result.stdout.splitlines()
                                   if line.startswith("RESULT:")))
            expected = {"exact_calls": False, "changed_api": True, "extra_call": True,
                        "extra_same_line": True, "other_file": True}
            self.assertEqual(data, {name: expected for name in (
                "src/recovery/stage_recovery_window.cpp", "tools/recovery_demo/demo_ui.cpp",
                "tests/python/test_site_validation.py")})

    @unittest.skipUnless(shutil.which("powershell.exe") or shutil.which("powershell"), "requires PowerShell")
    def test_repo_step_selection_rejects_unknown_and_skipped_steps(self):
        runner = REPO_ROOT / "tests/scripts/run_repo_checks.ps1"
        command = [shutil.which("powershell.exe") or shutil.which("powershell"),
                   "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(runner)]
        listing = subprocess.run(command + ["-ListSteps"], capture_output=True, text=True, timeout=30)
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("Atomic Write Tests", listing.stdout.splitlines())
        unknown = subprocess.run(command + ["-OnlyStep", "not a step"], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(unknown.returncode, 0)
        self.assertIn("Unknown step", unknown.stderr)
        skipped = subprocess.run(command + ["-OnlyStep", "Atomic Write Tests", "-SkipAtomicWrite"],
                                 capture_output=True, text=True, timeout=30)
        self.assertNotEqual(skipped.returncode, 0)
        self.assertIn("not executed", skipped.stdout)

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
$OnlyStep = @()
$script:PlanningSteps = $false; $script:StepResults = [ordered]@{}
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
$OnlyStep = @('chosen')
Invoke-Step -Name 'excluded' -Action { throw 'excluded action ran' }
Invoke-Step -Name 'chosen' -Action { Write-Host 'selected' }
$results['selection'] = @{ failed = (Test-Path (Join-Path $PSScriptRoot 'excluded.log')); fail_log = $false; pass_log = ($script:StepResults['chosen'].status -eq 'PASS') }
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
                    expected_failure = name not in ("success", "selection")
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
