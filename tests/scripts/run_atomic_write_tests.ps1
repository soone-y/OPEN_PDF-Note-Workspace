[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = $repoRoot
$src = Join-Path $repoRoot "tests\unit\atomic_write_tests.cpp"
$outDir = Join-Path $repoRoot "out\tests"
$exe = Join-Path $outDir "atomic_write_tests.exe"
$resourceSource = Join-Path $repoRoot "src\resources\app.rc"
$resourceObject = Join-Path $outDir "atomic_write_tests_resource.o"

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "g++ not found in PATH."
}
$resourceCompiler = Get-Command windres -ErrorAction SilentlyContinue
if (-not $resourceCompiler) {
    throw "windres not found in PATH."
}
$compilerDir = Split-Path -Parent $compiler.Source

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host "Compiling long-path manifest resource..." -ForegroundColor Cyan
& $resourceCompiler.Source "--codepage=65001" "-I" (Join-Path $repoRoot "src") "-I" (Join-Path $repoRoot "src\resources") `
    "-i" $resourceSource "-o" $resourceObject "-O" "coff"
if ($LASTEXITCODE -ne 0) {
    throw "resource compile failed"
}

$args = @(
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-I$root\src",
    $src,
    $resourceObject,
    "-o",
    $exe
)

Write-Host "Compiling atomic_write tests..." -ForegroundColor Cyan
& $compiler.Source @args
if ($LASTEXITCODE -ne 0) {
    throw "compile failed"
}

Write-Host "Running atomic_write tests..." -ForegroundColor Cyan
$oldPath = $env:PATH
try {
    $env:PATH = "$compilerDir;$oldPath"
    & $exe
    $code = $LASTEXITCODE
}
finally {
    $env:PATH = $oldPath
}

if ($code -ne 0) {
    throw "tests failed (exit=$code)"
}

Write-Host "All atomic_write tests passed." -ForegroundColor Green
