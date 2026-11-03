[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repoRoot ('out/tests/pdf_worker_render/' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $output
$exe = Join-Path $output 'pdf_worker_render_tests.exe'
& g++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $repoRoot 'src') `
    -I (Join-Path $repoRoot 'third_party/pdfium/include') `
    (Join-Path $repoRoot 'tests/unit/pdf_worker_render_tests.cpp') `
    (Join-Path $repoRoot 'third_party/pdfium/lib/pdfium.dll.lib') -lgdi32 -lmsimg32 `
    -static-libgcc -static-libstdc++ -o $exe
if ($LASTEXITCODE -ne 0) { throw 'PDF worker render test compilation failed.' }
$previousPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $repoRoot 'third_party/pdfium/bin');$previousPath"
    $process = Start-Process -FilePath $exe -WorkingDirectory $output -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'result.log') -RedirectStandardError (Join-Path $output 'errors.log')
    $ownedHandle = $process.Handle
    if (-not $process.WaitForExit(30000)) {
        $process.Kill(); $process.WaitForExit()
        throw "PDF worker render test timeout: $output"
    }
    $process.Refresh()
    Get-Content -LiteralPath (Join-Path $output 'result.log')
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        Get-Content -LiteralPath (Join-Path $output 'errors.log')
        throw "PDF worker render tests failed: $output"
    }
    Write-Host "Evidence: $output"
} finally { $env:PATH = $previousPath }
