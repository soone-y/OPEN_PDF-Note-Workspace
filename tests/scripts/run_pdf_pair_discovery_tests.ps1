[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repo ('out/tests/pdf_pair_discovery/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$exe = Join-Path $output 'pdf_pair_discovery_tests.exe'
Push-Location -LiteralPath $repo
try {
    & $compiler -std=gnu++17 -O2 -Wall -Wextra -DCLROP_READ_ONLY_BUILD -municode -Isrc -Ithird_party/pdfium/include `
        tests/unit/pdf_pair_discovery_tests.cpp src/clrop/pair_discovery.cpp src/clrop/json.cpp `
        third_party/pdfium/lib/pdfium.dll.lib -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Discovery test compile failed: $output" }
    $previousPath = $env:PATH
    try {
        $env:PATH = "$(Join-Path $repo 'third_party/pdfium/bin');$(Split-Path -Parent $compiler);$previousPath"
        $process = Start-Process -FilePath $exe -ArgumentList ('"' + (Join-Path $output 'fixture') + '"') `
            -WorkingDirectory $output -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $output 'test.log') -RedirectStandardError (Join-Path $output 'errors.log')
        $ownedHandle = $process.Handle
        if (-not $process.WaitForExit(60000)) {
            $process.Kill(); $process.WaitForExit()
            throw "Discovery test timed out; only its owned process was stopped: $output"
        }
        $process.Refresh()
        Get-Content -LiteralPath (Join-Path $output 'test.log')
        if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
            Get-Content -LiteralPath (Join-Path $output 'errors.log')
            throw "Discovery test failed: $output"
        }
    } finally { $env:PATH = $previousPath }
    Write-Host "Evidence: $output"
} finally { Pop-Location }
