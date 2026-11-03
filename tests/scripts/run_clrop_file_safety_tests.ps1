[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$testTemp = [System.IO.Path]::GetFullPath((Join-Path $outDir 'clrop_file_safety_temp'))
$workspaceRoot = [System.IO.Path]::GetFullPath($repoRoot)
if (-not $testTemp.StartsWith($workspaceRoot + [System.IO.Path]::DirectorySeparatorChar,
                              [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'CLROP safety test temp escaped the workspace.'
}
$src = Join-Path $repoRoot "tests\integration\clrop_file_safety_tests.cpp"
$exe = Join-Path $outDir "clrop_file_safety_tests.exe"

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "g++ not found in PATH."
}
$compilerDir = Split-Path -Parent $compiler.Source

function Assert-OwnedTestTempAncestry {
    if ((Get-Item -LiteralPath $workspaceRoot).Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw 'CLROP safety test workspace is a reparse point.'
    }
    for ($path = $testTemp; $path -ne $workspaceRoot; $path = [System.IO.Path]::GetDirectoryName($path)) {
        if (-not $path -or ($path -ne $testTemp -and
            -not $path.StartsWith($workspaceRoot + [System.IO.Path]::DirectorySeparatorChar,
                                  [System.StringComparison]::OrdinalIgnoreCase))) {
            throw 'CLROP safety test temp ancestry escaped the workspace.'
        }
        if (Test-Path -LiteralPath $path) {
            if ((Get-Item -LiteralPath $path).Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
                throw 'CLROP safety test temp ancestry contains a reparse point.'
            }
        }
    }
}

Assert-OwnedTestTempAncestry
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
New-Item -ItemType Directory -Force -Path $testTemp | Out-Null
Assert-OwnedTestTempAncestry

$args = @(
    "-std=gnu++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-Isrc",
    "-Ithird_party/pdfium/include",
    $src,
    "src/clrop/json.cpp",
    "-o",
    $exe
)

Push-Location -LiteralPath $repoRoot
try {
    Write-Host "Compiling clrop file safety tests..." -ForegroundColor Cyan
    & $compiler.Source @args
    if ($LASTEXITCODE -ne 0) {
        throw "compile failed"
    }

    Write-Host "Running clrop file safety tests..." -ForegroundColor Cyan
    $oldPath = $env:PATH
    $oldTemp = $env:TEMP
    $oldTmp = $env:TMP
    try {
        $env:PATH = "$compilerDir;$oldPath"
        $env:TEMP = $testTemp
        $env:TMP = $testTemp
        & $exe
        if ($LASTEXITCODE -ne 0) {
            throw "test failed"
        }
    }
    finally {
        $env:PATH = $oldPath
        $env:TEMP = $oldTemp
        $env:TMP = $oldTmp
    }

    Write-Host "All clrop file safety tests passed." -ForegroundColor Green
}
finally {
    Pop-Location
}
