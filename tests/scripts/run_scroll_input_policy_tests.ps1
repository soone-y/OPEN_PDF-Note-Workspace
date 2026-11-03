[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/scroll_input_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$executable = Join-Path $output 'scroll_input_policy_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $repoRoot 'src') `
    (Join-Path $repoRoot 'tests/unit/scroll_input_policy_tests.cpp') `
    -static-libgcc -static-libstdc++ -lcomctl32 -o $executable
if ($LASTEXITCODE -ne 0) { throw 'Scroll input policy compilation failed.' }
& $executable *> (Join-Path $output 'result.log')
$testResult = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'result.log')
Write-Host "Evidence: $output"
if ($testResult -ne 0) { throw "Scroll input policy failed: $testResult" }
