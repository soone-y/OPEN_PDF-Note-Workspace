[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$testDir = Join-Path $repoRoot 'out/tests'
$compiler = Get-Command g++ -ErrorAction Stop
$python = Get-Command python -ErrorAction Stop
$testExe = Join-Path $testDir 'pdf_annotation_export_tests.exe'
New-Item -ItemType Directory -Force -Path $testDir | Out-Null
Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source -std=gnu++17 -O2 -Wall -Isrc -Ithird_party/pdfium/include `
        tests/unit/pdf_annotation_export_tests.cpp src/file_output/pdf_annotation_export.cpp `
        third_party/pdfium/lib/pdfium.dll.lib -lgdi32 -o $testExe
    if ($LASTEXITCODE -ne 0) { throw 'PDF annotation export tests failed to compile.' }
    $previousPath = $env:PATH
    try {
        $env:PATH = "$(Join-Path $repoRoot 'third_party/pdfium/bin');$(Split-Path -Parent $compiler.Source);$previousPath"
        $fixtureDir = Join-Path $testDir 'pdf_annotation_export'
        & $testExe $fixtureDir
        if ($LASTEXITCODE -ne 0) { throw 'PDF annotation export tests failed.' }
        & $python.Source (Join-Path $repoRoot 'tests/python/pdf_annotation_interop_check.py') --fixtures $fixtureDir
        if ($LASTEXITCODE -ne 0) { throw 'PDF annotation interoperability tests failed.' }
    } finally { $env:PATH = $previousPath }
} finally { Pop-Location }
