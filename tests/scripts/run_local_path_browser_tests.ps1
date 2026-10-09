[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot "out/tests/local_path_browser"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$testExe = Join-Path $outputDir "local_path_browser_tests.exe"
& $compiler -std=c++17 -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE -static-libgcc -static-libstdc++ `
    -I (Join-Path $repoRoot "src") (Join-Path $repoRoot "tests/unit/local_path_browser_tests.cpp") `
    -lcomctl32 -lgdi32 -lole32 -lshell32 -luuid -limm32 -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Local path browser tests compilation failed." }
$process = Start-Process -FilePath $testExe -WorkingDirectory $outputDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $outputDir "stdout.log") -RedirectStandardError (Join-Path $outputDir "stderr.log")
$ownedHandle = $process.Handle
if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    $process.WaitForExit()
    throw "Local path browser tests timed out; only the owned fixture was stopped."
}
$process.Refresh()
if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
    $detail = Get-Content -LiteralPath (Join-Path $outputDir "stderr.log") -Raw
    throw "Local path browser tests failed: $detail"
}
Get-Content -LiteralPath (Join-Path $outputDir "stdout.log")
