[CmdletBinding()]
param(
    [string]$ArtifactName = "note_parser_tests_alias"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$src = Join-Path $repoRoot "tests\unit\note_parser_tests.cpp"
if ($ArtifactName -notmatch '^[A-Za-z0-9._-]+$') {
    throw "ArtifactName must be a file-name-safe alias."
}
$exe = Join-Path $outDir "$ArtifactName.exe"
$md4cObj = Join-Path $outDir "${ArtifactName}_md4c.obj"

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "g++ not found in PATH."
}
$compilerDir = Split-Path -Parent $compiler.Source

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$cArgs = @(
    "-x",
    "c",
    "-std=c99",
    "-O2",
    "-Wall",
    "-DMD4C_USE_UTF16",
    "-Ithird_party/md4c/src",
    "-c",
    "third_party/md4c/src/md4c.c",
    "-o",
    $md4cObj
)

$cppArgs = @(
    "-O2",
    "-Wall",
    "-std=gnu++17",
    "-Isrc",
    "-Ithird_party/md4c/src",
    "-Ithird_party/pdfium/include",
    $src,
    "src/note/note_model.cpp",
    "src/note/note_dirty_graph.cpp",
    "src/note/note_history.cpp",
    "src/note/note_layout.cpp",
    "src/note/note_kernel.cpp",
    "src/note/note_identity.cpp",
    "src/note/note_identity_store.cpp",
    "src/note/note_persistence.cpp",
    "src/note/note_presentation.cpp",
    "src/note/note_revision_gate.cpp",
    "src/note/note_transaction.cpp",
    "src/note/note_text_core.cpp",
    "src/note/note_text_boundaries.cpp",
    "src/note/note_workspace_index.cpp",
    "src/note/note_workspace_service.cpp",
    "src/note/note_semantic_index.cpp",
    "src/note/note_math.cpp",
    "src/note/note_export.cpp",
    "src/note/note_md4c_adapter.cpp",
    "src/note/note_parser.cpp",
    "src/math/math_render.cpp",
    $md4cObj,
    "-lgdi32",
    "-o",
    $exe
)

Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source @cArgs
    if ($LASTEXITCODE -ne 0) {
        throw "compile failed"
    }

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
