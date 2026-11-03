from __future__ import annotations

import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which("powershell.exe") or shutil.which("pwsh")


@unittest.skipUnless(POWERSHELL, "requires PowerShell")
class ReleaseEntrypointTests(unittest.TestCase):
    def setUp(self):
        base = REPO_ROOT / "out/tests"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="release_entry_", dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for relative in ("release.ps1", "scripts/release/release_version.ps1"):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO_ROOT / relative, target)
        self.version = self.root / "REPO_VERSION.txt"
        self.version.write_text("0.9.503\n", encoding="utf-8")
        records = self.root / "docs/internal/archive/operations/publish_records"
        records.mkdir(parents=True)
        (records / "PUBLISH_CHECKLIST_submitted.txt").write_text(
            "版番号 / タグ: v0.9.503\n提出結果: fixture submitted\n", encoding="utf-8")
        self.write_script("build.ps1", r'''
param([string]$Locale,[switch]$Clean,[switch]$Rebuild,[switch]$VerboseOutput)
$v=(Get-Content (Join-Path $PSScriptRoot 'REPO_VERSION.txt') -Raw).Trim()
Add-Content (Join-Path $PSScriptRoot 'calls.txt') "build $Locale $v clean=$Clean rebuild=$Rebuild"
if (Test-Path (Join-Path $PSScriptRoot 'fail_build')) { throw 'injected build failure' }
''')
        self.write_script("scripts/release/make_release_set.ps1", r'''
param([string]$Locale,[switch]$DeferPostCreationValidation)
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$v=(Get-Content (Join-Path $root 'REPO_VERSION.txt') -Raw).Trim()
Add-Content (Join-Path $root 'calls.txt') "package $Locale $v"
Write-Output "RELEASE_SET_PATH=fixture_$Locale"
''')
        stub = "param([string]$Locale,[switch]$Rebuild,[switch]$VerboseOutput)\n"
        for relative in ("scripts/build/build_workspace.ps1", "scripts/build/build_readonly_viewer.ps1"):
            self.write_script(relative, stub)
        self.write_script("scripts/release/pack_release.ps1", "param($OutBaseDir,$NamePrefix,$Locale,[switch]$NoChecksums,[switch]$TestOnly)\n")

    def write_script(self, relative, content):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8-sig")

    def invoke(self, *arguments):
        result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                 str(self.root / "release.ps1"), *arguments], cwd=self.root,
                                env=dict(os.environ, NO_COLOR="1"), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
        return result.returncode, result.stdout.decode("utf-8", errors="replace")

    def test_top_document_links_follow_the_selected_package_tree(self):
        from tools.release_checks.release_locale_content_gate import validate_markdown_links

        readme = (REPO_ROOT / "README.md").read_text(encoding="utf-8-sig")
        readme += "\n[anchored setup](docs/ja/Getting_Started.md#storage)\n"
        readme += "[nested notice](docs/en/legal/THIRD_PARTY_NOTICES.md)\n"
        (self.root / "README.md").write_text(readme, encoding="utf-8")
        for relative in ("LICENSE.md", ".github/SECURITY.md"):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO_ROOT / relative, target)
        original = (self.root / "README.md").read_bytes()
        self.write_script("copy_top.ps1", r'''
param([string]$Source,[string]$Repository,[string]$Output,[string]$Locale)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$DryRun=$false
$tokens=$null; $errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($Source,[ref]$tokens,[ref]$errors)
if($errors.Count) { throw 'Packaging script parse failure' }
foreach($name in @('Ensure-Directory','Copy-File','Copy-ReleaseTopDocuments')) {
    $functions=@($ast.FindAll({param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    },$true))
    if($functions.Count -ne 1) { throw "Missing function: $name" }
    . ([scriptblock]::Create($functions[0].Extent.Text))
}
Copy-ReleaseTopDocuments -ReleaseRoot $Output -RepositoryRoot $Repository -Locale $Locale
''')
        for locale in ("ja", "en"):
            with self.subTest(locale=locale):
                release = self.root / f"top_documents_{locale}"
                for relative in ("docs/README.md", "docs/Getting_Started.md", "docs/legal/THIRD_PARTY_NOTICES.md"):
                    path = release / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text("# Fixture\n", encoding="utf-8")
                image = release / "docs/images/app_overview.png"
                image.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO_ROOT / "docs/images/app_overview.png", image)
                result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                         str(self.root / "copy_top.ps1"), "-Source", str(REPO_ROOT / "scripts/release/pack_release.ps1"),
                                         "-Repository", str(self.root), "-Output", str(release), "-Locale", locale],
                                        cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout.decode("utf-8", errors="replace"))
                text = (release / "README.md").read_text(encoding="utf-8-sig")
                self.assertTrue("](docs/Getting_Started.md#storage)" in text, "Setup fragment must use the package path")
                self.assertTrue("](docs/legal/THIRD_PARTY_NOTICES.md)" in text, "Nested notices must use the package path")
                self.assertEqual(validate_markdown_links(release, release), [])
                self.assertEqual((self.root / "README.md").read_bytes(), original)
                (release / "docs/Getting_Started.md").unlink()
                self.assertTrue(any("docs/Getting_Started.md" in error
                                    for error in validate_markdown_links(release, release)),
                                "The link gate must still reject a missing packaged document")

    def test_version_is_fixed_before_build_and_package_and_retry_retains_it(self):
        (self.root / "fail_build").touch()
        code, output = self.invoke("-Locale", "en")
        self.assertNotEqual(code, 0, output)
        self.assertEqual(self.version.read_text(encoding="utf-8").strip(), "0.9.504", output)
        (self.root / "fail_build").unlink()
        code, output = self.invoke("-Locale", "en")
        self.assertEqual(code, 0, output)
        calls = (self.root / "calls.txt").read_text(encoding="utf-8-sig")
        self.assertEqual(calls.splitlines(), ["build en 0.9.504 clean=False rebuild=False"] * 2 + ["package en 0.9.504"])
        self.assertEqual(len(list((self.root / "out/logs/release/version_backups").glob("*.txt"))), 1)

    @unittest.skipUnless(os.name == "nt", "requires Windows replacement semantics")
    def test_replace_denial_preserves_version_and_retry_uses_submitted_history(self):
        original = self.version.read_bytes()
        self.version.chmod(stat.S_IREAD)
        try:
            code, output = self.invoke("-Locale", "en")
            self.assertNotEqual(code, 0, output)
            self.assertIn("UnauthorizedAccessException", output)
            self.assertEqual(self.version.read_bytes(), original, output)
            self.assertFalse((self.root / "calls.txt").exists(), output)
        finally:
            self.version.chmod(stat.S_IREAD | stat.S_IWRITE)

        code, output = self.invoke("-Locale", "en")
        self.assertEqual(code, 0, output)
        self.assertEqual(self.version.read_text(encoding="utf-8").strip(), "0.9.504", output)
        backups = list((self.root / "out/logs/release/version_backups").glob("*.txt"))
        self.assertEqual(len(backups), 1, output)
        self.assertEqual(backups[0].read_bytes(), original)

    def test_publish_rejects_failed_or_missing_locale_gate_before_startup(self):
        source = (REPO_ROOT / "publish.ps1").read_text(encoding="utf-8-sig")
        start = source.index("function Get-ValidationResult(")
        end = source.index("\nfunction ", start + 1)
        self.write_script("check_locale_gate.ps1", "param([string]$Expected)\n" + source[start:end] + r'''
$ErrorActionPreference='Stop'
try { Get-ValidationResult $PSScriptRoot; throw 'Unexpected validation success' }
catch { if($_.Exception.Message -cne $Expected) { throw } }
''')
        tools = self.root / "tools/release_checks"
        tools.mkdir(parents=True)
        (tools / "release_set_integrity_gate.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
        gate = tools / "release_locale_content_gate.py"
        gate.write_text("raise SystemExit(17)\n", encoding="utf-8")
        for expected in ("Release locale-content gate failed. Publishing is blocked.",
                         f"Missing release locale-content gate: {gate}"):
            with self.subTest(expected=expected):
                result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                         str(self.root / "check_locale_gate.ps1"), "-Expected", expected],
                                        cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout.decode("utf-8", errors="replace"))
                if gate.exists():
                    gate.unlink()

    def test_test_clean_and_invalid_arguments_preserve_version_bytes(self):
        for arguments, expected in ((["-Test"], 0), (["-Clean"], 0), (["-Lite"], 1),
                                    (["-AllLocales", "-Locale", "ja"], 1)):
            with self.subTest(arguments=arguments):
                original = self.version.read_bytes()
                code, output = self.invoke(*arguments)
                self.assertEqual(code, expected, output)
                self.assertEqual(self.version.read_bytes(), original)
                self.assertFalse((self.root / "out/release_version.lock").exists())

    def test_all_locale_builds_keep_distinct_logs_even_in_the_same_second(self):
        source = (REPO_ROOT / "build.ps1").read_text(encoding="utf-8-sig")
        # Pin only the fixture script's filename clock; parent and child then
        # deterministically request the same timestamp without changing the OS.
        source = source.replace("Set-StrictMode -Version Latest", r'''
function Get-Date { param([string]$Format) return '20261030_000000' }
Set-StrictMode -Version Latest
''', 1)
        self.write_script("build.ps1", source)
        stub = r'''
param([string]$Locale,[string]$Edition,[switch]$Clean,[switch]$Rebuild,[switch]$VerboseOutput)
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Add-Content (Join-Path $root 'build_calls.txt') $Locale
'''
        for child in ("scripts/build/build_workspace.ps1", "scripts/build/build_readonly_viewer.ps1"):
            self.write_script(child, stub)
        result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                 str(self.root / "build.ps1"), "-AllLocales", "-Clean"], cwd=self.root,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout.decode("utf-8", errors="replace"))
        logs = list((self.root / "out/logs/build").glob("build_20261030_000000_*.log"))
        self.assertEqual(len(logs), 3, "Parent, JA and EN must each retain a primary transcript")
        self.assertTrue(all(path.stat().st_size > 0 for path in logs))
        self.assertEqual((self.root / "build_calls.txt").read_text(encoding="utf-8-sig").splitlines(),
                         ["ja"] * 3 + ["en"] * 3)

    def assert_log_is_last(self, output, expected_directory):
        lines = [line.strip() for line in output.splitlines() if line.strip()]
        path = str(expected_directory)
        self.assertIn(path, lines[-1], output)
        final_log = next(expected_directory.glob("*.log"))
        self.assertIn(str(final_log), lines[-1], output)
        self.assertGreater(final_log.stat().st_size, 0)
        return final_log

    def test_release_failure_retains_error_and_repeats_primary_log_last(self):
        (self.root / "fail_build").touch()
        code, output = self.invoke()
        self.assertNotEqual(code, 0, output)
        self.assertIn("injected build failure", output)
        log = self.assert_log_is_last(output, self.root / "out/logs/release")
        self.assertIn("injected build failure", log.read_text(encoding="utf-8-sig", errors="replace"))

    def test_build_and_publish_failure_repeat_their_own_primary_log_last(self):
        for entry, arguments in (("build.ps1", ["-Clean", "-Rebuild"]),
                                 ("publish.ps1", ["-Mode", "Verify"])):
            with self.subTest(entry=entry):
                shutil.copyfile(REPO_ROOT / entry, self.root / entry)
                result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                         str(self.root / entry), *arguments], cwd=self.root,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                output = result.stdout.decode("utf-8", errors="replace")
                self.assertNotEqual(result.returncode, 0, output)
                self.assert_log_is_last(output, self.root / "out/logs" / Path(entry).stem)

    def test_build_and_publish_capture_child_output_without_losing_failure(self):
        failure = "Write-Output 'child stdout marker'\n[Console]::Error.WriteLine('child stderr marker')\nexit 23\n"
        for entry, child, parameters in (
                ("build.ps1", "scripts/build/build_workspace.ps1", "param($Locale,[switch]$Rebuild,$Edition)\n"),
                ("publish.ps1", "release.ps1", "param([switch]$AllLocales,[switch]$DeferPostCreationValidation)\n")):
            with self.subTest(entry=entry):
                shutil.copyfile(REPO_ROOT / entry, self.root / entry)
                self.write_script(child, parameters + failure)
                result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                         str(self.root / entry)], cwd=self.root, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=30)
                output = result.stdout.decode("utf-8", errors="replace")
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn("23", output)
                log = self.assert_log_is_last(output, self.root / "out/logs" / Path(entry).stem)
                logged = log.read_text(encoding="utf-8-sig", errors="replace")
                self.assertIn("child stdout marker", logged)
                self.assertIn("child stderr marker", logged)

    def test_release_set_failure_repeats_detail_log_after_diagnostics(self):
        repository = self.root / "repository"
        entry = repository / "scripts/release/make_release_set.ps1"
        entry.parent.mkdir(parents=True)
        shutil.copyfile(REPO_ROOT / "scripts/release/make_release_set.ps1", entry)
        (repository / "REPO_VERSION.txt").write_text("0.9.504\n", encoding="utf-8")
        # A missing gate fails after detail-log initialization. No compilation,
        # application execution or real release-set modification is involved.
        result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(entry),
                                 "-OutBaseDir", str(self.root / "sets")], cwd=repository,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        output = result.stdout.decode("utf-8", errors="replace")
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("failure log tail", output)
        self.assert_log_is_last(output, repository / "out/logs/release_set")


if __name__ == "__main__":
    unittest.main()
