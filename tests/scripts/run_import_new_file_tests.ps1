[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot 'out/tests/import_new_file'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$testExe = Join-Path $outputDir 'import_new_file_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -static-libgcc -static-libstdc++ `
    -I (Join-Path $repoRoot 'src') (Join-Path $repoRoot 'tests/unit/import_new_file_tests.cpp') -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'Import new-file tests compilation failed.' }
Push-Location $outputDir
try {
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'Import new-file tests failed.' }
} finally { Pop-Location }
