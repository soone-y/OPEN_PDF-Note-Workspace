[CmdletBinding()]
param([ValidateSet('ja', 'en')][string]$Locale = 'ja',
      [ValidatePattern('^[A-Za-z0-9._-]*$')][string]$ArtifactName = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
try {
    $buildLock = [IO.File]::Open((Join-Path $repoRoot 'out/build.lock'), 'OpenOrCreate', 'Write', 'None')
} catch { throw 'A build or linked-app test is active. Wait for it to finish.' }
try {
    $suffix = if ($Locale -eq 'en') { '_en' } else { '' }
    $objectDir = Join-Path $repoRoot $(if ($ArtifactName) { "out/obj_${ArtifactName}${suffix}" } else { "out/obj${suffix}" })
    $binDir = Join-Path $repoRoot $(if ($Locale -eq 'en') { 'out/bin_en' } else { 'out/bin' })
    $response = Join-Path $objectDir 'link_args.rsp'
    if (-not (Test-Path -LiteralPath $response)) { throw 'Build the selected locale before running this app test.' }
    $outputDir = Join-Path $repoRoot ('out/tests/list_presentation/' + $Locale + '_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $outputDir | Out-Null
    $testObject = Join-Path $outputDir 'list_presentation_app_tests.o'
    $testExe = Join-Path $outputDir 'list_presentation_app_tests.exe'
    & g++ -std=c++17 -O2 -Wall -Wextra -Werror -UNDEBUG -I (Join-Path $repoRoot 'src') `
        -I (Join-Path $repoRoot 'third_party/pdfium/include') -c `
        (Join-Path $repoRoot 'tests/unit/list_presentation_app_tests.cpp') -o $testObject
    if ($LASTEXITCODE -ne 0) { throw "List test compilation failed. Evidence: $outputDir" }
    Push-Location $repoRoot
    try {
        & g++ "@$response" $testObject -mconsole -o $testExe
        if ($LASTEXITCODE -ne 0) { throw "List test linking failed. Evidence: $outputDir" }
    } finally { Pop-Location }
    foreach ($name in @('pdfium.dll', 'zlib1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $binDir $name) -Destination $outputDir
    }
    $process = Start-Process -FilePath $testExe -WorkingDirectory $outputDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $outputDir 'stdout.log') -RedirectStandardError (Join-Path $outputDir 'stderr.log')
    $ownedHandle = $process.Handle
    if (-not $process.WaitForExit(60000)) {
        $process.Kill(); $process.WaitForExit()
        throw "List test timed out; only the owned fixture was stopped. Evidence: $outputDir"
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        throw ('List test failed (exit ' + $process.ExitCode + '; evidence: ' + $outputDir + '): ' + (Get-Content (Join-Path $outputDir 'stderr.log') -Raw))
    }
    $testOutput = Get-Content (Join-Path $outputDir 'stdout.log') -Raw
    if ($testOutput -notmatch '(?m)^List presentation PASS: 4 x 1000 entries,') { throw "Required list cases were not observed: $outputDir" }
    if ($testOutput -notmatch '(?m)^Layout presentation PASS:') { throw "Required live-layout cases were not observed: $outputDir" }
    if ($testOutput -notmatch '(?m)^List blank-area PASS:') { throw "Required blank-area pixel cases were not observed: $outputDir" }
    if ($testOutput -notmatch '(?m)^List scroll PASS:') { throw "Required precision-scroll and viewport cases were not observed: $outputDir" }
    if ($testOutput -notmatch '(?m)^List context reload PASS:') { throw "Required directory reload cases were not observed: $outputDir" }
    Write-Output $testOutput.TrimEnd()
    Write-Host "Retained fixture and logs: $outputDir"
    Write-Host 'Real control/data/owner-draw paths were checked; physical scrolling, DPI changes and audio were not observed.'
} finally { $buildLock.Dispose() }
