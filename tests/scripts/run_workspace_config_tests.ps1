[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$uiAutomationScript = Join-Path $PSScriptRoot "run_ui_automation_fault_tests.ps1"
if (-not (Test-Path -LiteralPath $uiAutomationScript)) {
    throw "Missing workspace configuration automation script: $uiAutomationScript"
}

Write-Host "Running workspace configuration round-trip tests..." -ForegroundColor Cyan
& $uiAutomationScript -ConfigOnly
if (-not $?) {
    throw "workspace configuration round-trip tests failed."
}

Write-Host "All workspace configuration round-trip tests passed." -ForegroundColor Green
