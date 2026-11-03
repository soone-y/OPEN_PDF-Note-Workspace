[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/page_rotation_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$exe = Join-Path $output 'page_rotation_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $repoRoot 'src') `
    -I (Join-Path $repoRoot 'third_party/pdfium/include') `
    (Join-Path $repoRoot 'tests/unit/page_rotation_tests.cpp') `
    (Join-Path $repoRoot 'third_party/pdfium/lib/pdfium.dll.lib') -lgdi32 -lmsimg32 `
    -static-libgcc -static-libstdc++ -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Page rotation test compilation failed.' }
$previousPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $repoRoot 'third_party/pdfium/bin');$previousPath"
    & $exe *> (Join-Path $output 'result.log')
    $result = $LASTEXITCODE
} finally { $env:PATH = $previousPath }
Get-Content -LiteralPath (Join-Path $output 'result.log')
Write-Host "Evidence: $output"
if ($result -ne 0) { throw "Page rotation geometry failed: $result" }
& (Join-Path $PSScriptRoot 'run_pdf_annotation_display_tests.ps1')
