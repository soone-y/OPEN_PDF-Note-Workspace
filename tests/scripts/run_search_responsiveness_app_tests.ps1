[CmdletBinding()]
param([ValidateSet('ja', 'en')][string]$Locale = 'ja',
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$ArtifactName = 'pdf_note_workspace')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot ('out/tests/search_responsiveness/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $outputDir | Out-Null
Write-Host "Evidence and logs: $outputDir"
try {
    $buildLock = [IO.File]::Open((Join-Path $repoRoot 'out/build.lock'), 'OpenOrCreate', 'Write', 'None')
} catch { throw 'A build or linked-app test is active. Wait for it to finish.' }
try {
    $suffix = if ($Locale -eq 'en') { '_en' } else { '' }
    $objectDir = Join-Path $repoRoot "out/obj_${ArtifactName}${suffix}"
    $binDir = Join-Path $repoRoot $(if ($Locale -eq 'en') { 'out/bin_en' } else { 'out/bin' })
    $response = Join-Path $objectDir 'link_args.rsp'
    $original = Get-Content -LiteralPath $response
    $searchObject = $original | Where-Object { $_ -match '[/\\]src_search_search\.o"?$' }
    if (@($searchObject).Count -ne 1) { throw 'Expected exactly one production search object.' }
    $testResponse = Join-Path $outputDir 'link.rsp'
    $original | Where-Object { $_ -ne $searchObject } | Set-Content -Encoding utf8 $testResponse
    $testObject = Join-Path $outputDir 'test.o'
    $testExe = Join-Path $outputDir 'test.exe'
    & g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -UNDEBUG -municode -I (Join-Path $repoRoot 'src') `
        -I (Join-Path $repoRoot 'third_party/pdfium/include') -c `
        (Join-Path $repoRoot 'tests/unit/search_responsiveness_app_tests.cpp') -o $testObject *> (Join-Path $outputDir 'compile.log')
    if ($LASTEXITCODE -ne 0) { throw "Search test compilation failed: $outputDir/compile.log" }
    Push-Location $repoRoot
    try {
        & g++ "@$testResponse" $testObject -mconsole -o $testExe *> (Join-Path $outputDir 'link.log')
        if ($LASTEXITCODE -ne 0) { throw "Search test linking failed: $outputDir/link.log" }
    } finally { Pop-Location }
    foreach ($name in @('pdfium.dll', 'zlib1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $binDir $name) -Destination $outputDir
    }
    $process = Start-Process -FilePath $testExe -WorkingDirectory $outputDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $outputDir 'stdout.log') -RedirectStandardError (Join-Path $outputDir 'stderr.log')
    $ownedHandle = $process.Handle
    if (-not $process.WaitForExit(60000)) {
        $process.Kill(); $process.WaitForExit()
        throw "Search test timed out; owned process stopped. Evidence: $outputDir"
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) { throw "Search test failed (exit $($process.ExitCode)): $outputDir/stderr.log" }
    $output = Get-Content (Join-Path $outputDir 'stdout.log') -Raw
    foreach ($marker in @('Search failed-PDF budget PASS:', 'Search publication PASS:', 'Search note worker PASS:', 'Search worker lifecycle PASS:', 'Search responsiveness PASS:')) {
        if (!$output.Contains($marker)) { throw "Required marker missing: $marker; evidence: $outputDir" }
    }
    Write-Host $output.TrimEnd()
    Write-Host 'Physical input latency, DPI and audible silence were not observed.'
} finally { $buildLock.Dispose() }
