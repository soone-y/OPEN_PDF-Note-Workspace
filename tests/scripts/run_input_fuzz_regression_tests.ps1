[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$source = Join-Path $repoRoot "tests\unit\input_fuzz_regression_tests.cpp"
$exe = Join-Path $outDir "input_fuzz_regression_tests.exe"
$md4cObject = Join-Path $outDir "input_fuzz_regression_md4c.obj"
$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) { throw "g++ not found in PATH." }
$compilerDir = Split-Path -Parent $compiler.Source

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source -x c -std=c99 -O2 -Wall -DMD4C_USE_UTF16 -Ithird_party/md4c/src `
        -c third_party/md4c/src/md4c.c -o $md4cObject
    if ($LASTEXITCODE -ne 0) { throw "MD4C compile failed" }

    & $compiler.Source -std=gnu++17 -O2 -Wall -Isrc -Ithird_party/md4c/src `
        -Ithird_party/pdfium/include $source src/clrop/json.cpp src/note/note_model.cpp `
        src/note/note_md4c_adapter.cpp src/note/note_parser.cpp $md4cObject `
        third_party/pdfium/lib/pdfium.dll.lib -lole32 -lwindowscodecs -o $exe
    if ($LASTEXITCODE -ne 0) { throw "input fuzz regression compile failed" }

    $oldPath = $env:PATH
    try {
        $env:PATH = "$(Join-Path $repoRoot 'third_party\pdfium\bin');$compilerDir;$oldPath"
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $exe
        $startInfo.WorkingDirectory = $repoRoot
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        if (-not $process.Start()) { throw "input fuzz regression process did not start" }
        if (-not $process.WaitForExit(60000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "input fuzz regression test timed out after 60 seconds"
        }
        $exitCode = $process.ExitCode
        if ($exitCode -ne 0) { throw "input fuzz regression test failed (exit=$exitCode)" }
        Write-Host "Input fuzz regression tests passed (CLROP, annotation history, note, PDFium, WIC; 256 deterministic mutations each)." -ForegroundColor Green
    }
    finally {
        $env:PATH = $oldPath
    }
}
finally {
    Pop-Location
}
