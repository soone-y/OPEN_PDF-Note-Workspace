[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$compiler = Get-Command gcc -ErrorAction Stop
$outDir = Join-Path $repoRoot "out\tests"
$exe = Join-Path $outDir "md4c_allocation_failure_tests.exe"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source -std=c99 -O2 -Wall -Wextra -DMD4C_USE_UTF16 `
        tests/unit/md4c_allocation_failure_tests.c -o $exe
    if ($LASTEXITCODE -ne 0) { throw "MD4C allocation test compile failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "MD4C allocation cleanup regression failed" }
}
finally { Pop-Location }
