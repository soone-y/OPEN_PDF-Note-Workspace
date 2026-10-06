[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$testDir = Join-Path $repoRoot "out\tests\startup_instance"
$compiler = Get-Command g++ -ErrorAction Stop
New-Item -ItemType Directory -Force -Path $testDir | Out-Null
$testExe = Join-Path $testDir "startup_instance_tests.exe"
Push-Location -LiteralPath $repoRoot
try {
    & $compiler.Source -std=gnu++17 -O2 -Wall -Isrc -municode -static-libgcc -static-libstdc++ `
        tests/unit/startup_instance_tests.cpp src/app/startup_instance.cpp -luser32 -lshell32 -lgdi32 -o $testExe
    if ($LASTEXITCODE -ne 0) { throw "Startup instance test compilation failed." }
    $testLog = Join-Path $testDir "startup_instance.log"
    $testErrorLog = Join-Path $testDir "startup_instance_errors.log"
    $testProcess = Start-Process -FilePath $testExe -WorkingDirectory $testDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $testLog -RedirectStandardError $testErrorLog
    $ownedProcessHandle = $testProcess.Handle
    if (-not $testProcess.WaitForExit(60000)) {
        $testProcess.Kill()
        $testProcess.WaitForExit()
        throw "Startup instance tests timed out; only the owned fixture parent was stopped."
    }
    $testProcess.Refresh()
    if ($null -eq $testProcess.ExitCode -or $testProcess.ExitCode -ne 0) {
        $detail = Get-Content -LiteralPath $testErrorLog -Raw
        throw "Startup instance tests failed (exit=$($testProcess.ExitCode)): $detail"
    }
    Write-Host "[PASS] Startup instance tests" -ForegroundColor Green
}
finally { Pop-Location }
