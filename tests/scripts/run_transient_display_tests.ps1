[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot 'out/tests/transient_display'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$testExe = Join-Path $outputDir 'transient_display_tests.exe'
# The existing optional DWM loader uses the Win32 GetProcAddress function cast.
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-error=cast-function-type -static `
    -I (Join-Path $repoRoot 'src') (Join-Path $repoRoot 'tests/unit/transient_display_tests.cpp') -lcomctl32 -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'Transient display tests compilation failed.' }
$process = Start-Process -FilePath $testExe -WorkingDirectory $outputDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $outputDir 'stdout.log') -RedirectStandardError (Join-Path $outputDir 'stderr.log')
$ownedHandle = $process.Handle
if (-not $process.WaitForExit(30000)) {
    $process.Kill(); $process.WaitForExit()
    throw 'Transient display tests timed out; only the owned fixture was stopped.'
}
$process.Refresh()
if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
    throw ('Transient display tests failed (exit ' + $process.ExitCode + '): ' + (Get-Content -LiteralPath (Join-Path $outputDir 'stderr.log') -Raw))
}
Get-Content -LiteralPath (Join-Path $outputDir 'stdout.log')
