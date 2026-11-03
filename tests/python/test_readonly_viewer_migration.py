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

    @unittest.skipUnless(shutil.which('pwsh'), 'requires PowerShell 7')
    def test_incremental_build_repairs_invalid_dependency_files(self):
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
    Set-Content -LiteralPath $env:PACK_DEPENDENCY -Value 'object.o: ' -NoNewline
    if (-not (Should-Compile -Source $env:PACK_SOURCE -Object $env:PACK_OBJECT -DependencyFile $env:PACK_DEPENDENCY)) {
        throw 'invalid dependency file skipped compilation'
    }
    Set-Content -LiteralPath $env:PACK_DEPENDENCY -Value "object.o: $env:PACK_SOURCE $env:PACK_HEADER" -NoNewline
    if (Should-Compile -Source $env:PACK_SOURCE -Object $env:PACK_OBJECT -DependencyFile $env:PACK_DEPENDENCY) {
        throw 'valid unchanged dependencies caused compilation'
    }
}
'''
            env = dict(os.environ, APP_BUILD_SCRIPT=str(ROOT / 'scripts/build/build_workspace.ps1'),
                       VIEWER_BUILD_SCRIPT=str(ROOT / 'scripts/build/build_readonly_viewer.ps1'),
                       PACK_SOURCE=source_relative, PACK_HEADER=header_relative,
                       PACK_OBJECT=str(obj), PACK_DEPENDENCY=str(dep))
            result = subprocess.run(['pwsh', '-NoProfile', '-Command', script], cwd=ROOT, env=env,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
