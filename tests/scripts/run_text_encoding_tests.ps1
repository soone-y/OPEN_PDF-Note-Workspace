param(
    [string]$ArtifactName = "text_encoding_tests"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $root "out\tests"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir ($ArtifactName + '.exe')

& g++ -std=c++20 -Wall -Wextra -Werror -I (Join-Path $root 'src') `
    (Join-Path $root 'tests\unit\text_encoding_tests.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& $exe
if ($LASTEXITCODE -ne 0) { throw "test failed" }
Write-Output "text encoding tests passed"
