[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot 'out/tests/import_size'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$testExe = Join-Path $outputDir 'import_size_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -UNDEBUG -static `
    -I (Join-Path $repoRoot 'src') (Join-Path $repoRoot 'tests/unit/import_size_tests.cpp') -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'Import size test compilation failed.' }
$process = Start-Process -FilePath $testExe -WorkingDirectory $outputDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $outputDir 'stdout.log') -RedirectStandardError (Join-Path $outputDir 'stderr.log')
$ownedHandle = $process.Handle
if (-not $process.WaitForExit(30000)) {
    $process.Kill(); $process.WaitForExit()
    throw "Import size tests timed out; only the owned fixture was stopped. Evidence: $outputDir"
}
$process.Refresh()
if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
    throw ('Import size tests failed (exit ' + $process.ExitCode + '): ' + (Get-Content (Join-Path $outputDir 'stderr.log') -Raw))
}
Get-Content (Join-Path $outputDir 'stdout.log')
