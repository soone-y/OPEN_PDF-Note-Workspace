[CmdletBinding()]
param(
    [string]$ArtifactName = "note_presentation_tests"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$src = Join-Path $repoRoot "tests\unit\note_presentation_tests.cpp"
if ($ArtifactName -notmatch '^[A-Za-z0-9._-]+$') {
    throw "ArtifactName must be a file-name-safe alias."
}
$exe = Join-Path $outDir "$ArtifactName.exe"

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "g++ not found in PATH."
}
$compilerDir = Split-Path -Parent $compiler.Source

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$cppArgs = @(
    "-O2",
    "-Wall",
    "-Wextra",
    "-std=gnu++17",
    "-Isrc",
    $src,
    "src/note/note_presentation.cpp",
    "-o",
    $exe
)

Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source @cppArgs
    if ($LASTEXITCODE -ne 0) {
        throw "compile failed"
    }

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
}
finally {
    Pop-Location
}
