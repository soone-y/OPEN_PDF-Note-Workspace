[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot "out/tests"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$testExe = Join-Path $outputDir "artifact_usage_tests.exe"
& $compiler -std=c++17 -Wall -Wextra -Werror -static-libgcc -static-libstdc++ -I (Join-Path $repoRoot "src") `
    (Join-Path $repoRoot "tests/unit/artifact_usage_tests.cpp") `
    (Join-Path $repoRoot "src/diagnostics/artifact_usage.cpp") -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Artifact capacity test compilation failed." }
Push-Location $repoRoot
try {
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "Artifact capacity tests failed." }
} finally { Pop-Location }
