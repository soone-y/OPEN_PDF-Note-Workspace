[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/note_selection_transport_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$executable = Join-Path $output 'note_selection_transport_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $repoRoot 'src') `
    (Join-Path $repoRoot 'tests/unit/note_selection_transport_tests.cpp') `
    (Join-Path $repoRoot 'src/note/note_source_line_map.cpp') `
    -static-libgcc -static-libstdc++ -o $executable *> (Join-Path $output 'compile.log')
if ($LASTEXITCODE -ne 0) { throw "Selection transport compilation failed. Evidence: $output" }
& $executable *> (Join-Path $output 'result.log')
$testResult = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'result.log')
Write-Host "Evidence: $output"
if ($testResult -ne 0) { throw "Selection transport tests failed: $testResult" }
