from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which("powershell.exe") or shutil.which("pwsh")


@unittest.skipUnless(POWERSHELL, "requires PowerShell")
class BuildVersionRefreshTests(unittest.TestCase):
    def setUp(self):
        base = ROOT / "out/tests"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="version_refresh_", dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def run_ps(self, source):
        script = self.root / "check.ps1"
        script.write_text("param([string]$Support)\n$ErrorActionPreference='Stop'\n" + source, encoding="utf-8-sig")
        return subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                               "-Support", str(ROOT / "scripts/build/build_version.ps1")], cwd=self.root,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)

    def test_version_evidence_and_timestamp_changes_require_refresh(self):
        result = self.run_ps(r'''
. $Support
$v=Join-Path $PSScriptRoot 'version.txt'; $e=Join-Path $PSScriptRoot 'app.exe'; $b=Join-Path $PSScriptRoot 'buildinfo.txt'
$old=[DateTime]::UtcNow.AddMinutes(-10)
function Check([bool]$Expected) {
    $actual=Test-BuildVersionNeedsRefresh -VersionPath $v -Version $version -ExecutablePath $e -BuildInfoPath $b
    if($actual -ne $Expected) { throw "Unexpected refresh decision for $case" }
}
$version='1.2.3'; Set-Content $v $version; Set-Content $e 'binary'
[IO.File]::SetLastWriteTimeUtc($v,$old)
Set-Content $b "format`tpdf-note-build-info-v1`nversion`t1.2.3"
$case='unchanged real tab record'; Check $false
$version='1.2.4'; Set-Content $v $version; [IO.File]::SetLastWriteTimeUtc($v,$old)
$case='different version with preserved timestamp'; Check $true
Set-Content $b "format`tpdf-note-build-info-v1`nversion`t1.2.4"
[IO.File]::SetLastWriteTimeUtc($v,[DateTime]::UtcNow.AddMinutes(1))
$case='newer version file with matching record'; Check $true
[IO.File]::SetLastWriteTimeUtc($v,$old)
foreach($record in @('invalid',"format`tpdf-note-build-info-v1`nversion`t1.2.4`nversion`t1.2.4",
                    'format`tpdf-note-build-info-v1`nversion`t1.2.4')) {
    Set-Content $b $record; $case='malformed/duplicate/literal backtick record'; Check $true
}
Remove-Item -LiteralPath $b; $case='missing evidence'; Check $true
Set-Content $b "format`tpdf-note-build-info-v1`nversion`t1.2.4"
Remove-Item -LiteralPath $e; $case='missing executable'; Check $true
Set-Content $v '1.2.5'
$rejected=$false; try { Check $true } catch { $rejected=$true }
if(-not $rejected) { throw 'Changing version input was accepted' }
''')
        self.assertEqual(result.returncode, 0, result.stdout.decode("utf-8", errors="replace"))

    def test_actual_driver_selection_overrides_subset_only_for_version_source(self):
        for relative, version_source in (("build_workspace.ps1", "src/core/app_core.cpp"),
                                         ("build_readonly_viewer.ps1", "src/readonly_viewer/main.cpp")):
            with self.subTest(driver=relative):
                source = (ROOT / "scripts/build" / relative).read_text(encoding="utf-8-sig")
                fragment = re.search(r'(?ms)^        if \(\$versionChanged -and \$src -eq "[^"]+"\) \{.*?^        \}', source)
                self.assertIsNotNone(fragment)
                result = self.run_ps("""
$cases=@(@{Changed=$true;Source='%s';Expected=$true},@{Changed=$false;Source='%s';Expected=$false},
         @{Changed=$true;Source='src/other.cpp';Expected=$false})
foreach($case in $cases) {
    $versionChanged=$case.Changed; $src=$case.Source; $isTargetFile=$false; $forceCompileThis=$false
%s
    if($isTargetFile -ne $case.Expected -or $forceCompileThis -ne $case.Expected) { throw 'Invalid source selection' }
}
""" % (version_source, version_source, fragment.group(0)))
                self.assertEqual(result.returncode, 0, result.stdout.decode("utf-8", errors="replace"))

    def test_link_failure_preserves_existing_metadata_and_returns_failure(self):
        source = (ROOT / "scripts/build/build_workspace.ps1").read_text(encoding="utf-8-sig")
        fragment = re.search(r'(?ms)^        if \(\$firstLinkExitCode -ne 0\) \{.*?^        \}', source)
        self.assertIsNotNone(fragment)
        info = self.root / "info.txt"
        info.write_text("original metadata", encoding="utf-8")
        result = self.run_ps(r'''
$outputExe=Join-Path $PSScriptRoot 'app.exe'; Set-Content $outputExe 'existing binary'
$firstLinkExitCode=23; $linkDeferred=$false
function Write-BuildFailureTail {}
''' + fragment.group(0) + "\nSet-Content (Join-Path $PSScriptRoot 'info.txt') 'INVALID NEW METADATA'\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(info.read_text(encoding="utf-8"), "original metadata")
