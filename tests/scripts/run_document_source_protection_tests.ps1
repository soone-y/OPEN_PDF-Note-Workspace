[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/document_source_protection/' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $output
$exe = Join-Path $output 'document_source_protection_tests.exe'
$compiler = Get-Command g++ -ErrorAction Stop
Push-Location -LiteralPath $repoRoot
try {
    & python tools/localization/generate_locale_catalog.py --locale ja --ja locales/ja.json --localized locales/ja.json `
        --output (Join-Path $output 'locale_catalog.generated.h') --report (Join-Path $output 'locale_report.json')
    if ($LASTEXITCODE -ne 0) { throw 'Document source protection test locale generation failed.' }
    & $compiler.Source -std=gnu++17 -O2 -Wall -Wextra -DPDF_NOTE_SOURCE_PROTECTION_TESTING=1 -Isrc "-I$output" -municode -static-libgcc -static-libstdc++ `
        tests/unit/document_source_protection_tests.cpp src/workspace/workspace_write_lock.cpp src/core/localization.cpp -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Document source protection test build failed.' }
    $process = Start-Process -FilePath $exe -WorkingDirectory $output -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'test.log') -RedirectStandardError (Join-Path $output 'errors.log')
    $ownedHandle = $process.Handle
    if (-not $process.WaitForExit(45000)) {
        $process.Kill(); $process.WaitForExit()
        throw 'Document source protection test timed out; only its owned process was stopped.'
    }
    $process.Refresh()
    Get-Content -LiteralPath (Join-Path $output 'test.log')
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        Get-Content -LiteralPath (Join-Path $output 'errors.log')
        throw "Document source protection tests failed (exit=$($process.ExitCode)); evidence: $output"
    }
    Write-Host "PASS: document source protection / $output"
} finally { Pop-Location }
