[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/annot_tool_definition_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$executable = Join-Path $output 'annot_tool_definition_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $repoRoot 'src') `
    (Join-Path $repoRoot 'tests/unit/annot_tool_definition_tests.cpp') `
    (Join-Path $repoRoot 'src/core/annot_tool_definition.cpp') `
    -static-libgcc -static-libstdc++ -o $executable *> (Join-Path $output 'compile.log')
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath (Join-Path $output 'compile.log') -Tail 40
    throw "Annotation tool definition compilation failed. Evidence: $output"
}
& $executable *> (Join-Path $output 'result.log')
$testResult = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'result.log')
Write-Host "Evidence: $output"
if ($testResult -ne 0) { throw "Annotation tool definition tests failed: $testResult" }
