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
    (Join-Path $repoRoot "src\workspace\operation_record_store.cpp"),
    "-o",
    $exe
)

Push-Location -LiteralPath $repoRoot
try {
    # Management-folder names are built only in src/core/workspace_layout.h.
    # Display text may mention them; quoted wide literals in code may not.
    Write-Host "Checking management-folder literals..." -ForegroundColor Cyan
    $layoutHeader = Join-Path $repoRoot "src\core\workspace_layout.h"
    $layoutText = [IO.File]::ReadAllText($layoutHeader)
    $folderNames = @([regex]::Matches($layoutText, 'inline constexpr wchar_t k\w+Name\[\] = L"(__[a-z_]+__)"') |
        ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -ne '__pdf_note_workspace__' })
    if ($folderNames.Count -lt 10) { throw "workspace_layout.h folder names were not found." }
    $banned = @($folderNames | Where-Object { $_ -notin @('__settings__', '__theme__', '__memo__', '__log__') }) + @('__escape__', '__stage__')
    $violations = @()
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repoRoot "src") -Recurse -File -Include *.cpp,*.h,*.cppinc,*.inc) {
        if ($file.FullName -eq $layoutHeader) { continue }
        $lineNo = 0
        foreach ($line in [IO.File]::ReadAllLines($file.FullName)) {
            $lineNo++
            foreach ($name in $banned) {
                if ($line.Contains('L"' + $name + '"')) { $violations += "$($file.FullName):${lineNo}: $name" }
            }
        }
    }
    if ($violations.Count -gt 0) {
        $violations | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        throw "management-folder literal outside workspace_layout.h"
    }

    # DELETE_OR_KEEP.txt (both languages) and the help reference list every folder.
    foreach ($locale in @('ja', 'en')) {
        $catalog = Get-Content -LiteralPath (Join-Path $repoRoot "locales\$locale.json") -Raw -Encoding UTF8 | ConvertFrom-Json
        $guide = $catalog.'workspace.delete_guide.body'
        if (-not $guide) { throw "workspace.delete_guide.body is missing in $locale." }
        $help = Get-Content -LiteralPath (Join-Path $repoRoot "docs\$locale\Help_Reference.md") -Raw -Encoding UTF8
        foreach ($name in $folderNames) {
            if (-not $guide.Contains($name)) { throw "DELETE_OR_KEEP.txt ($locale) does not mention $name." }
            if (-not $help.Contains($name)) { throw "docs/$locale/Help_Reference.md does not mention $name." }
        }
    }
    Write-Host "Management-folder layout checks passed ($($folderNames.Count) folders)." -ForegroundColor Green

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
