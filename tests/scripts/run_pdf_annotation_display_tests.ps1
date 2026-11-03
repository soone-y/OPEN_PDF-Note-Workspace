[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/pdf_annotation_display_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$exe = Join-Path $output 'pdf_annotation_display_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $repoRoot 'src') `
    -I (Join-Path $repoRoot 'third_party/pdfium/include') `
    (Join-Path $repoRoot 'tests/unit/pdf_annotation_display_tests.cpp') `
    (Join-Path $repoRoot 'third_party/pdfium/lib/pdfium.dll.lib') -lgdi32 -lmsimg32 `
    -static-libgcc -static-libstdc++ -o $exe
if ($LASTEXITCODE -ne 0) { throw 'PDF annotation display test compilation failed.' }
$previousPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $repoRoot 'third_party/pdfium/bin');$previousPath"
    & $exe *> (Join-Path $output 'result.log')
    $result = $LASTEXITCODE
} finally { $env:PATH = $previousPath }
Get-Content -LiteralPath (Join-Path $output 'result.log')
Write-Host "Evidence: $output"
if ($result -ne 0) { throw "PDF annotation display failed: $result" }
