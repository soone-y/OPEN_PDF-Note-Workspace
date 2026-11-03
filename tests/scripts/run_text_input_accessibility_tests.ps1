[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/text_input_accessibility_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$executable = Join-Path $output 'text_input_accessibility_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $repoRoot 'src') `
    (Join-Path $repoRoot 'tests/unit/text_input_accessibility_tests.cpp') `
    (Join-Path $repoRoot 'src/pdf_view/text_input_accessibility.cpp') `
    -loleacc -loleaut32 -lole32 -luuid -static-libgcc -static-libstdc++ -o $executable
if ($LASTEXITCODE -ne 0) { throw 'Text input accessibility contract compilation failed.' }
& $executable *> (Join-Path $output 'result.log')
$testResult = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'result.log')
Write-Host "Evidence: $output"
if ($testResult -ne 0) { throw "Text input accessibility contract failed: $testResult" }
