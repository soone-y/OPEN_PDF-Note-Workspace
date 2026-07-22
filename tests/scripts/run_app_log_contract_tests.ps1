[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$uiAutomationScript = Join-Path $PSScriptRoot "run_ui_automation_fault_tests.ps1"
if (-not (Test-Path -LiteralPath $uiAutomationScript)) {
    throw "Missing app-log automation script: $uiAutomationScript"
}

Write-Host "Running app-log contract tests..." -ForegroundColor Cyan
& $uiAutomationScript -LogContractOnly
if (-not $?) {
    throw "app-log contract tests failed."
}

Write-Host "All app-log contract tests passed." -ForegroundColor Green
