[CmdletBinding()]
param(
    [string]$ArtifactName = "mermaid_subset_parser_tests"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$source = Join-Path $repoRoot "tests\unit\mermaid_subset_parser_tests.cpp"
if ($ArtifactName -notmatch '^[A-Za-z0-9._-]+$') {
    throw "ArtifactName must be a file-name-safe alias."
}
$exe = Join-Path $outDir "$ArtifactName.exe"

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "g++ not found in PATH."
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$args = @(
    "-O2",
    "-Wall",
    "-std=gnu++17",
    "-Isrc",
    $source,
    "src/readonly_viewer/mermaid_subset_parser.cpp",
    "-o",
    $exe
)

Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source @args
    if ($LASTEXITCODE -ne 0) {
        throw "compile failed"
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        throw "test failed"
    }
}
finally {
    Pop-Location
}
