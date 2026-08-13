[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$uiAutomationScript = Join-Path $PSScriptRoot "run_ui_automation_fault_tests.ps1"
if (-not (Test-Path -LiteralPath $uiAutomationScript)) {
    throw "Missing workspace configuration recovery automation script: $uiAutomationScript"
}

Write-Host "Running workspace configuration recovery tests..." -ForegroundColor Cyan
& $uiAutomationScript -ConfigRecoveryOnly
if (-not $?) {
    throw "workspace configuration recovery tests failed."
}

Write-Host "All workspace configuration recovery tests passed." -ForegroundColor Green
