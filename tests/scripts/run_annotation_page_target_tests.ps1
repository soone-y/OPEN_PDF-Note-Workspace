[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repo ('out/tests/annotation_page_target/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$exe = Join-Path $output 'annotation_page_target_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror "-I$(Join-Path $repo 'src')" "-I$(Join-Path $repo 'third_party/pdfium/include')" `
    (Join-Path $repo 'tests/unit/annotation_page_target_tests.cpp') -static-libgcc -static-libstdc++ -o $exe
if ($LASTEXITCODE -ne 0) { throw "Annotation page target compile failed: $output" }
& $exe *> (Join-Path $output 'result.log')
$result = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'result.log')
Write-Host "Evidence: $output"
if ($result -ne 0) { throw "Annotation page target tests failed: $result" }
