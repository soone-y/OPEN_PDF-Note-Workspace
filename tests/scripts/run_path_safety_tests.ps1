[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$src = Join-Path $repoRoot "tests\unit\path_safety_tests.cpp"
$exe = Join-Path $outDir "path_safety_tests.exe"

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "g++ not found in PATH."
}
$compilerDir = Split-Path -Parent $compiler.Source

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$args = @(
    "-std=gnu++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-Isrc",
    "-Ithird_party/pdfium/include",
    $src,
    "-o",
    $exe
)

Push-Location -LiteralPath $repoRoot
try {
    Write-Host "Compiling path safety tests..." -ForegroundColor Cyan
    & $compiler.Source @args
    if ($LASTEXITCODE -ne 0) {
        throw "compile failed"
    }

    Write-Host "Running path safety tests..." -ForegroundColor Cyan
    $oldPath = $env:PATH
    try {
        $env:PATH = "$compilerDir;$oldPath"
        & $exe
        if ($LASTEXITCODE -ne 0) {
            throw "test failed"
        }
    }
    finally {
        $env:PATH = $oldPath
    }

    Write-Host "All path safety tests passed." -ForegroundColor Green
}
finally {
    Pop-Location
}
