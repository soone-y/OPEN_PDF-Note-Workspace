"""Keep production build/caller identities and a single implementation after adoption."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReaderMigrationTests(unittest.TestCase):
    @unittest.skipUnless(os.name == 'nt' and shutil.which('powershell.exe'), 'requires Windows PowerShell')
    def test_local_build_layout_preserves_state_and_resets_only_selected_sample(self):
        (ROOT / 'out/tests').mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='local_run_', dir=ROOT / 'out/tests') as temporary:
            fixture = Path(temporary)
            shutil.copytree(ROOT / 'release_assets/sample_workspace', fixture / 'release_assets/sample_workspace')
            script = fixture / 'check.ps1'
            script.write_text(r'''
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $env:LOCAL_RUN_HELPER
$root = $PSScriptRoot
$out = Join-Path $root 'out'
foreach ($bin in @('bin_en','bin_lite_en')) {
    $dir = New-Item -ItemType Directory -Path (Join-Path $out $bin) -Force
    foreach ($name in @('pdf_note_workspace.exe','pdfium.dll','zlib1.dll','libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
        Set-Content -LiteralPath (Join-Path $dir.FullName $name) -Value $bin
    }
}
Set-Content -LiteralPath (Join-Path $out 'bin_en/readonly_viewer.exe') -Value 'viewer'
Invoke-LocalRunLayout -RepositoryRoot $root -Locale en
$run = Join-Path $out 'run_lite_en'
$full = Join-Path $out 'run_en/sample_workspace'
$sample = Join-Path $run 'sample_workspace'
$setup = Join-Path $run 'pdf_note_workspace_setup.json'
$exe = Join-Path $run 'pdf_note_workspace.exe'
$conversion = '01_Lecture_Samples/Session_04_Office_Conversion'
if (!(Test-Path (Join-Path $full $conversion)) -or (Test-Path (Join-Path $sample $conversion))) { throw 'wrong sample edition' }
if (!(Test-Path (Join-Path $run 'readonly_viewer.exe'))) { throw 'missing local viewer' }
$initial = Get-Content -LiteralPath $setup -Raw | ConvertFrom-Json
if ($initial.workspaceRoot -cne 'sample_workspace' -or $initial.workspaceRootMode -cne 'relative') { throw 'wrong initial setup' }
$edited = Join-Path $sample 'edited.txt'
Set-Content -LiteralPath $edited -Value 'sample edit'
Set-Content -LiteralPath $setup -Value '{"workspaceRoot":"other"}'
$other = New-Item -ItemType Directory -Path (Join-Path $run 'other')
Set-Content -LiteralPath (Join-Path $other 'keep.txt') -Value 'untouched'
Set-Content -LiteralPath (Join-Path $out 'bin_lite_en/pdf_note_workspace.exe') -Value 'new binary'
Invoke-LocalRunLayout -RepositoryRoot $root -Locale en -Editions Lite
if (!(Test-Path $edited) -or (Get-Content -LiteralPath $exe) -cne 'new binary') { throw 'repeat build lost edits or failed to refresh exe' }
Invoke-LocalRunLayout -RepositoryRoot $root -Locale en -Editions Lite -Clean
if (!(Test-Path $edited) -or (Test-Path $exe)) { throw 'clean lost edits or left the exe' }
Invoke-LocalRunLayout -RepositoryRoot $root -Locale en -Editions Lite
$link = Join-Path $sample 'outside_link'
$null = New-Item -ItemType Junction -Path $link -Target $other.FullName
$rejected = $false
try { Invoke-LocalRunLayout -RepositoryRoot $root -Locale en -Editions Lite -HardClean }
catch { $rejected = $_.Exception.Message -like '*sample containing a link*' }
if (!$rejected -or !(Test-Path $edited) -or !(Test-Path $exe)) { throw 'unsafe reset was not rejected before deleting files' }
[IO.Directory]::Delete($link)
Invoke-LocalRunLayout -RepositoryRoot $root -Locale en -Editions Lite -HardClean
if ((Test-Path $sample) -or !(Test-Path $full) -or (Test-Path $exe)) { throw 'reset did not remove only the chosen sample' }
Invoke-LocalRunLayout -RepositoryRoot $root -Locale en -Editions Lite
if (!(Test-Path $sample) -or (Test-Path $edited) -or (Get-Content -LiteralPath $setup) -cne '{"workspaceRoot":"other"}' -or
    (Get-Content -LiteralPath (Join-Path $other 'keep.txt')) -cne 'untouched') { throw 'reset/reseed damaged non-sample state' }
''', encoding='utf-8-sig')
            env = dict(os.environ, LOCAL_RUN_HELPER=str(ROOT / 'scripts/build/local_run.ps1'))
            for shell in filter(None, (shutil.which('powershell.exe'), shutil.which('pwsh'))):
                with self.subTest(shell=shell):
                    if (fixture / 'out').exists():
                        shutil.rmtree(fixture / 'out')
                    result = subprocess.run([shell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                                            env=env, capture_output=True, text=True, errors='replace', timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which('pwsh'), 'requires PowerShell 7')
    def test_workspace_clean_preserves_data_in_older_binary_layout(self):
        (ROOT / 'out/tests').mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='bin_clean_', dir=ROOT / 'out/tests') as temporary:
            script = Path(temporary) / 'check.ps1'
            script.write_text(r'''
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($env:WORKSPACE_BUILD, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'build script parse failure' }
foreach ($type in @('FunctionDefinitionAst', 'IfStatementAst')) {
    $statement = $ast.Find({param($node)
        $node.GetType().Name -eq $type -and $node.Extent.Text -match '^(function Remove-BuildArtifacts\s*\{|if \(\$Clean\) \{)'
    }, $true)
    if (!$statement) { throw 'missing build clean implementation' }
    if ($type -eq 'FunctionDefinitionAst') { . ([scriptblock]::Create($statement.Extent.Text)) }
    else { $cleanBlock = [scriptblock]::Create($statement.Extent.Text) }
}
$Clean = $true; $artifactAliasSpecified = $false
$binDir = (New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'bin')).FullName
$objDir = (New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'obj')).FullName
$sample = New-Item -ItemType Directory -Path (Join-Path $binDir 'sample_workspace')
$outputExe = Join-Path $binDir 'pdf_note_workspace.exe'
$buildInfoManifestPath = $outputExe + '.buildinfo.txt'
$dll = Join-Path $binDir 'pdfium.dll'
$keep = @((Join-Path $sample.FullName 'edit.clro'), (Join-Path $binDir 'pdf_note_workspace_setup.json'))
foreach ($file in @($outputExe, $buildInfoManifestPath, $dll) + $keep) { Set-Content -LiteralPath $file -Value 'fixture' }
& $cleanBlock
foreach ($path in @($objDir, $outputExe, $buildInfoManifestPath, $dll)) { if (Test-Path $path) { throw "Artifact survived clean: $path" } }
foreach ($path in $keep) { if (!(Test-Path $path)) { throw "Clean deleted user data: $path" } }
''', encoding='utf-8-sig')
            env = dict(os.environ, WORKSPACE_BUILD=str(ROOT / 'scripts/build/build_workspace.ps1'))
            result = subprocess.run(['pwsh', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                                    env=env, capture_output=True, text=True, errors='replace', timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_build_owns_implementation(self):
        sources = json.loads((ROOT / 'scripts/build/build_sources.json').read_text(encoding='utf-8-sig'))['ReadOnlyViewerSourceFiles']
        self.assertIn('src/readonly_viewer/main.cpp', sources)
        self.assertIn('src/readonly_viewer/reader_document.cpp', sources)
        self.assertIn('src/readonly_viewer/reader_chrome.cpp', sources)
        self.assertIn('src/readonly_viewer/pdf_search.cpp', sources)
        self.assertNotIn('src/readonly_viewer/pdf_preview_panel.cpp', sources)
        self.assertFalse(any('readonly_viewer_next/' in source for source in sources))
        adapter = (ROOT / 'src/readonly_viewer/pdf_search.cpp').read_text(encoding='utf-8')
        self.assertEqual(adapter.count('#include "pdf_preview_panel.cpp"'), 1)

    def test_existing_main_caller_and_settings_identity_preserved(self):
        caller = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        reader = (ROOT / 'src/readonly_viewer/main.cpp').read_text(encoding='utf-8')
        for needle in ('L"PdfReadonlyViewerWindow"',):
            self.assertIn(needle, caller)
            self.assertIn(needle, reader)
        self.assertIn('L"readonly_viewer.exe"', caller)
        self.assertIn('L"readonly_setup.json"', reader)
        self.assertIn('L"readonly.window.title"', reader)
        self.assertIn('dir / kViewerSessionFile', reader)
        self.assertIn('if (!g_sessionStateLoaded) return true;', reader)
        preview = (ROOT / 'src/readonly_viewer_next/main.cpp').read_text(encoding='utf-8')
        self.assertIn('#define PDF_NOTE_READER_PROTOTYPE 1', preview)
        self.assertIn('#include "../readonly_viewer/main.cpp"', preview)
        self.assertNotIn('LRESULT CALLBACK WndProc', preview)

    def test_standard_build_keeps_parser_and_notice_contracts(self):
        build = (ROOT / 'scripts/build/build_readonly_viewer.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('"-DMD4C_USE_UTF16"', build)
        self.assertIn('--variant standard', build)
        self.assertIn('Required read-only viewer license files could not be prepared.', build)
        pack = (ROOT / 'scripts/release/pack_release.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('$readOnlyViewerExeName = "readonly_viewer.exe"', pack)
        self.assertIn('"ReadOnlyViewerSourceFiles"', pack)
        self.assertIn('"README.txt"', pack)

    def test_viewer_freshness_tracks_its_clrop_inputs(self):
        sources = json.loads((ROOT / 'scripts/build/build_sources.json').read_text(encoding='utf-8-sig'))['ReadOnlyViewerSourceFiles']
        self.assertIn('src/clrop/json.cpp', sources)
        self.assertNotIn('src/clrop/bridge.cpp', sources)
        pack = (ROOT / 'scripts/release/pack_release.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('Get-CompilerInputPaths -BuildSourcesPath $buildSourcesPath -SourceProperty "ReadOnlyViewerSourceFiles"', pack)
        self.assertIn('Get-CompilerInputPaths -BuildSourcesPath $buildSourcesPath -SourceProperty "BuildSourceFiles"', pack)
        self.assertIn('Assert-ArtifactFresh -ArtifactPath $readOnlyViewerExePath -InputPaths $viewerInputs', pack)

    @unittest.skipUnless(shutil.which('pwsh'), 'requires PowerShell 7')
    def test_pack_freshness_requires_compiler_dependency_evidence(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'out/tests') as temporary:
            fixture = Path(temporary)
            source = fixture / 'source.cpp'
            header = fixture / 'used.h'
            source.write_text('#include "used.h"\n', encoding='utf-8')
            header.write_text('int value;\n', encoding='utf-8')
            source_relative = source.relative_to(ROOT).as_posix()
            header_relative = header.relative_to(ROOT).as_posix()
            manifest = fixture / 'sources.json'
            manifest.write_text(json.dumps({'ReadOnlyViewerSourceFiles': [source_relative]}), encoding='utf-8')
            objects = fixture / 'objects'
            objects.mkdir()
            dependency = objects / (source_relative.replace('/', '_').replace('.cpp', '.o') + '.d')
            dependency.write_text(f'object.o: {source_relative} {header_relative}\n', encoding='utf-8')
            script = r'''
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($env:PACK_SCRIPT, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'pack script parse failure' }
$function = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Get-CompilerInputPaths'
}, $true))
if ($function.Count -ne 1) { throw 'missing dependency gate' }
. ([scriptblock]::Create($function[0].Extent.Text))
$inputs = @(Get-CompilerInputPaths -BuildSourcesPath $env:PACK_MANIFEST -SourceProperty 'ReadOnlyViewerSourceFiles' -ObjectDirectory $env:PACK_OBJECTS)
if ($env:PACK_HEADER -notin $inputs) { throw 'included header is not tracked' }
Remove-Item -LiteralPath $env:PACK_DEPENDENCY
$rejected = $false
try { Get-CompilerInputPaths -BuildSourcesPath $env:PACK_MANIFEST -SourceProperty 'ReadOnlyViewerSourceFiles' -ObjectDirectory $env:PACK_OBJECTS | Out-Null }
catch { $rejected = $true }
if (-not $rejected) { throw 'missing dependency evidence was accepted' }
'''
            env = dict(os.environ, PACK_SCRIPT=str(ROOT / 'scripts/release/pack_release.ps1'),
                       PACK_MANIFEST=str(manifest), PACK_OBJECTS=str(objects),
                       PACK_HEADER=header_relative, PACK_DEPENDENCY=str(dependency))
            result = subprocess.run(['pwsh', '-NoProfile', '-Command', script], cwd=ROOT, env=env,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which('powershell.exe') or shutil.which('pwsh'), 'requires PowerShell')
    def test_incremental_build_rechecks_dependency_freshness(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'out/tests') as temporary:
            fixture = Path(temporary)
            source = fixture / 'source.cpp'
            header = fixture / 'used.h'
            obj = fixture / 'source.o'
            dep = fixture / 'source.o.d'
            source.write_text('#include "used.h"\n', encoding='utf-8')
            header.write_text('int value;\n', encoding='utf-8')
            obj.write_bytes(b'object')
            old = time.time() - 120
            os.utime(source, (old, old))
            os.utime(header, (old, old))
            source_relative = source.relative_to(ROOT).as_posix()
            header_relative = header.relative_to(ROOT).as_posix()
            script = r'''
$ErrorActionPreference = 'Stop'
$repoRoot = $env:PACK_REPO_ROOT
$tokens = $null; $errors = $null
foreach ($buildScript in @($env:APP_BUILD_SCRIPT, $env:VIEWER_BUILD_SCRIPT)) {
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($buildScript, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw 'build script parse failure' }
    foreach ($name in @('Get-DependencySeparatorIndex', 'Get-DependencyPaths', 'Should-Compile')) {
        $function = @($ast.FindAll({ param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }, $true))
        if ($function.Count -ne 1) { throw "missing $name" }
        . ([scriptblock]::Create($function[0].Extent.Text))
    }
    function Assert-CompileDecision([bool]$Expected, [string]$Scenario) {
        $actual = Should-Compile -Source $env:PACK_SOURCE -Object $env:PACK_OBJECT -DependencyFile $env:PACK_DEPENDENCY
        if ($actual -ne $Expected) { throw "${buildScript}: $Scenario expected $Expected, got $actual" }
    }
    Set-Content -LiteralPath $env:PACK_DEPENDENCY -Value 'object.o: ' -NoNewline
    Assert-CompileDecision $true 'invalid dependency file'
    Set-Content -LiteralPath $env:PACK_DEPENDENCY -Value "object.o: $env:PACK_SOURCE $env:PACK_HEADER" -NoNewline
    $header = Join-Path $repoRoot $env:PACK_HEADER
    $objectTime = [IO.File]::GetLastWriteTimeUtc($env:PACK_OBJECT)
    Set-Content -LiteralPath $header -Value 'int value;'
    [IO.File]::SetLastWriteTimeUtc($header, $objectTime)
    Assert-CompileDecision $false 'dependency timestamp equal to object timestamp'
    [IO.File]::SetLastWriteTimeUtc($header, $objectTime.AddSeconds(2))
    Assert-CompileDecision $true 'same relative dependency after update'
    [IO.File]::SetLastWriteTimeUtc($header, $objectTime.AddSeconds(-2))
    Assert-CompileDecision $false 'same relative dependency after restoring an older timestamp'
    # The current dependency parser splits on whitespace.
    if ($header -notmatch '\s') {
        Set-Content -LiteralPath $env:PACK_DEPENDENCY -Value "object.o: $env:PACK_SOURCE $($header.Replace('\', '/'))" -NoNewline
        Assert-CompileDecision $false 'older dependency at an absolute compiler path'
    }
    Remove-Item -LiteralPath $header
    Assert-CompileDecision $true 'same dependency after deletion'
    Set-Content -LiteralPath $env:PACK_DEPENDENCY -Value "object.o: $env:PACK_SOURCE invalid|path.h" -NoNewline
    Assert-CompileDecision $true 'invalid dependency path'
    Remove-Item -LiteralPath $env:PACK_DEPENDENCY
    Assert-CompileDecision $true 'missing dependency file'
}
'''
            env = dict(os.environ, APP_BUILD_SCRIPT=str(ROOT / 'scripts/build/build_workspace.ps1'),
                       VIEWER_BUILD_SCRIPT=str(ROOT / 'scripts/build/build_readonly_viewer.ps1'),
                       PACK_SOURCE=source_relative, PACK_HEADER=header_relative,
                       PACK_OBJECT=str(obj), PACK_DEPENDENCY=str(dep), PACK_REPO_ROOT=str(ROOT))
            for shell in filter(None, (shutil.which('powershell.exe'), shutil.which('pwsh'))):
                with self.subTest(shell=shell):
                    result = subprocess.run([shell, '-NoProfile', '-Command', script], cwd=ROOT, env=env,
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
