[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$exe = Join-Path $outDir "annot_stage_replay_tests.exe"
$compiler = Get-Command g++ -ErrorAction Stop
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source -std=gnu++17 -O2 -Wall -Wextra -Isrc -Ithird_party/pdfium/include `
        tests/unit/annot_stage_replay_tests.cpp src/clrop/json.cpp -o $exe
    if ($LASTEXITCODE -ne 0) { throw "annotation stage replay compile failed" }
    $priorPath = $env:PATH
    try {
        $env:PATH = (Split-Path -Parent $compiler.Source) + ";" + $priorPath
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "annotation stage replay tests failed" }
    }
    finally { $env:PATH = $priorPath }
}
finally { Pop-Location }
