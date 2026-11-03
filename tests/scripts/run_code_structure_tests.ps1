[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$previousErrorActionPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = "Continue"
    & python -B (Join-Path $repoRoot "tests/python/test_code_structure.py") 2>&1 | ForEach-Object {
        $message = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { [string]$_ }
        if (-not [string]::IsNullOrWhiteSpace($message)) { Write-Host $message }
    }
    $testExitCode = $LASTEXITCODE
} finally { $ErrorActionPreference = $previousErrorActionPreference }
if ($testExitCode -ne 0) { throw "C++ structure fixture tests failed (exit $testExitCode); dependencies are mandatory for this selected step." }
