[CmdletBinding()]
param(
    [ValidateSet('ja', 'en')][string]$Locale = 'ja',
    [ValidateSet('session_root', 'separate_directories')][string]$Layout = 'session_root'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# The linked fixture and runtime copies must come from one completed build.
# Share the build's lock instead of linking objects while they are being replaced.
try {
    $buildLock = [System.IO.File]::Open((Join-Path $repoRoot 'out/build.lock'),
        [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
} catch { throw 'A build or linked-app test is active. Wait for it to finish before running this test.' }
try {
$objectDir = Join-Path $repoRoot $(if ($Locale -eq 'en') { 'out/obj_en' } else { 'out/obj' })
$binDir = Join-Path $repoRoot $(if ($Locale -eq 'en') { 'out/bin_en' } else { 'out/bin' })
$response = Join-Path $objectDir 'link_args.rsp'
if (-not (Test-Path -LiteralPath $response)) { throw 'Build the selected locale before running this app test.' }
$outputDir = Join-Path $repoRoot ('out/tests/drop_import_app/' + $Locale + '_' + $Layout + '_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $outputDir | Out-Null
$testObject = Join-Path $outputDir 'drop_import_app_tests.o'
$testExe = Join-Path $outputDir 'drop_import_app_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -UNDEBUG -I (Join-Path $repoRoot 'src') `
    -I (Join-Path $repoRoot 'third_party/pdfium/include') -c `
    (Join-Path $repoRoot 'tests/unit/drop_import_app_tests.cpp') -o $testObject
if ($LASTEXITCODE -ne 0) { throw 'D&D app test compilation failed.' }
Push-Location $repoRoot
try {
    & g++ "@$response" $testObject -mconsole -o $testExe
    if ($LASTEXITCODE -ne 0) { throw 'D&D app test linking failed.' }
} finally { Pop-Location }
Copy-Item -LiteralPath (Join-Path $repoRoot 'tests/fixtures/ui_automation_session/sample.pdf') -Destination (Join-Path $outputDir 'fixture.pdf')
foreach ($name in @('pdfium.dll', 'zlib1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $binDir $name) -Destination $outputDir
}
foreach ($phase in @('drop', 'restart')) {
    $startOptions = @{
        FilePath = $testExe; WorkingDirectory = $outputDir; WindowStyle = 'Hidden'; PassThru = $true
        RedirectStandardOutput = Join-Path $outputDir "$phase-stdout.log"
        RedirectStandardError = Join-Path $outputDir "$phase-stderr.log"
    }
    $arguments = @()
    if ($phase -eq 'restart') { $arguments += '--verify-restart' }
    if ($Layout -eq 'separate_directories') { $arguments += '--separate-directories' }
    if ($arguments.Count) { $startOptions.ArgumentList = $arguments }
    $process = Start-Process @startOptions
    $ownedHandle = $process.Handle
    # Include the full 100 MB copy comparison and recursive-import cases in one bounded phase.
    if (-not $process.WaitForExit(60000)) {
        $process.Kill(); $process.WaitForExit()
        throw "D&D app test timed out (only the owned fixture was stopped): $outputDir"
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        throw ('D&D app test failed (exit ' + $process.ExitCode + '; evidence: ' + $outputDir + '): ' + (Get-Content -LiteralPath (Join-Path $outputDir "$phase-stderr.log") -Raw))
    }
    $testOutput = Get-Content -LiteralPath (Join-Path $outputDir "$phase-stdout.log") -Raw
    if ($phase -eq 'drop') {
        if ($testOutput -notmatch '(?m)^Large import PASS: 7 decisions, 3 tiers,') {
            throw "Large import decision verification was not observed: $outputDir"
        }
        $requiredRejections = if ($Layout -eq 'separate_directories') { 33 } else { 32 }
        if ($testOutput -notmatch ('(?m)^D&D app PASS: 4 list targets, ' + $requiredRejections + ' notified rejections,')) {
            throw "Required D&D cases were not observed: $outputDir"
        }
    } elseif ($testOutput -notmatch '(?m)^D&D restart PASS:') {
        throw "Restart verification was not observed: $outputDir"
    }
    Write-Output $testOutput.TrimEnd()
}
Write-Host "Retained fixture: $outputDir"
Write-Host 'WM_DROPFILES control flow was checked; real Explorer pointer dragging, audio, OS DPI and power loss were not observed.'

} finally { $buildLock.Dispose() }
