[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/temp_path_worker/' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $output
$exe = Join-Path $output 'temp_path_worker_tests.exe'
Push-Location -LiteralPath $repoRoot
try {
    & python tools/localization/generate_locale_catalog.py --locale ja --ja locales/ja.json --localized locales/ja.json `
        --output (Join-Path $output 'locale_catalog.generated.h') --report (Join-Path $output 'locale_report.json')
    if ($LASTEXITCODE -ne 0) { throw 'Temporary path test locale generation failed.' }
    & g++ -std=gnu++17 -O2 -Wall -Wextra -Isrc "-I$output" -static-libgcc -static-libstdc++ `
        tests/unit/temp_path_worker_tests.cpp src/workspace/temp_path_worker.cpp src/core/localization.cpp -o $exe -luser32
    if ($LASTEXITCODE -ne 0) { throw 'Temporary path worker test build failed.' }
    $process = Start-Process -FilePath $exe -WorkingDirectory $output -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'test.log') -RedirectStandardError (Join-Path $output 'errors.log')
    $ownedHandle = $process.Handle
    if (-not $process.WaitForExit(15000)) {
        $process.Kill(); $process.WaitForExit()
        throw "Temporary path worker test timeout: $output"
    }
    $process.Refresh()
    Get-Content -LiteralPath (Join-Path $output 'test.log')
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        Get-Content -LiteralPath (Join-Path $output 'errors.log')
        throw "Temporary path worker tests failed: $output"
    }
    Write-Host "Evidence: $output"
} finally { Pop-Location }
