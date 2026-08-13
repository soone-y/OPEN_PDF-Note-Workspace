[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$uiAutomationScript = Join-Path $PSScriptRoot "run_ui_automation_fault_tests.ps1"
if (-not (Test-Path -LiteralPath $uiAutomationScript)) {
    throw "Missing workspace configuration automation script: $uiAutomationScript"
}

Write-Host "Running workspace configuration unknown-field preservation tests..." -ForegroundColor Cyan
& $uiAutomationScript -ConfigUnknownFieldOnly
if (-not $?) {
    throw "workspace configuration unknown-field preservation tests failed."
}

Write-Host "All workspace configuration unknown-field preservation tests passed." -ForegroundColor Green
