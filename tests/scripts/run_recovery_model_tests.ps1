[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/recovery_model/' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $output
$exe = Join-Path $output 'recovery_model_tests.exe'
$compiler = (Get-Command g++ -ErrorAction Stop).Source
Push-Location -LiteralPath $repoRoot
try {
    & $compiler -std=gnu++17 -O2 -Wall -Wextra -Isrc -Ithird_party/pdfium/include `
        tests/unit/recovery_model_tests.cpp src/recovery/recovery_model.cpp src/clrop/json.cpp tools/recovery_demo/demo_reconnect.cpp tools/recovery_demo/demo_composition.cpp -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Recovery model compilation failed.' }
    $priorPath = $env:PATH
    try {
        $env:PATH = (Split-Path -Parent $compiler) + ';' + $priorPath
        & $exe
        if ($LASTEXITCODE -ne 0) { throw 'Recovery model tests failed.' }
    } finally { $env:PATH = $priorPath }
} finally { Pop-Location }
