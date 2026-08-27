[CmdletBinding()]
param(
    [ValidateRange(1, 20)]
    [int]$Iterations = 3,
    [string[]]$UiAutomationArguments = @(),
    [switch]$SkipPngExport,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$appPath = Join-Path $repoRoot "out\bin\pdf_note_workspace.exe"
$uiAutomationScript = Join-Path $PSScriptRoot "run_ui_automation_fault_tests.ps1"
$appVerifier = (Get-Command appverif.exe -ErrorAction SilentlyContinue).Source
$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$currentPrincipal = [Security.Principal.WindowsPrincipal]::new($currentIdentity)
$isAdministrator = $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path -LiteralPath $appPath)) {
    throw "Memory diagnostics requires a freshly built application: $appPath"
}
if (-not (Test-Path -LiteralPath $uiAutomationScript)) {
    throw "UI automation script not found: $uiAutomationScript"
}
if ([string]::IsNullOrWhiteSpace($appVerifier)) {
    throw "Application Verifier (appverif.exe) was not found. Install the Windows SDK test tools before running this check."
}
if (-not $DryRun -and -not $isAdministrator) {
    throw "Run this script from an elevated Administrator PowerShell. Application Verifier persists settings in HKLM, and elevation is required both to enable and to remove them."
}

$targetName = Split-Path -Leaf $appPath
$verifierKeys = @(
    "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$targetName",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$targetName"
)

if ($DryRun) {
    Write-Host "[DRY RUN] Would require an elevated Administrator PowerShell."
    Write-Host "[DRY RUN] Would refuse to overwrite existing Application Verifier settings for $targetName."
    Write-Host "[DRY RUN] Would enable Application Verifier Basics for $targetName."
    Write-Host "[DRY RUN] Would run UI automation $Iterations time(s)."
    Write-Host "[DRY RUN] Would delete only the Application Verifier settings for $targetName."
    exit 0
}

foreach ($key in $verifierKeys) {
    if (Test-Path -LiteralPath $key) {
        throw "Application Verifier or another debugger setting already exists for $targetName ($key). This test will not overwrite or remove an existing setting. Remove or preserve that setting manually, then retry."
    }
}

function Invoke-AppVerifier {
    param([Parameter(Mandatory)][string[]]$Arguments)

    & $appVerifier @Arguments
    if (-not $?) {
        throw "Application Verifier invocation failed: $($Arguments -join ' ')"
    }
}

$configured = $false
$childUiAutomationArguments = [System.Collections.Generic.List[string]]::new()
foreach ($argument in $UiAutomationArguments) {
    $childUiAutomationArguments.Add($argument)
}
if ($SkipPngExport) {
    $childUiAutomationArguments.Add("-SkipPngExport")
}
try {
    # /verify enables the documented Basics set, including full-page heap,
    # handle, COM/RPC, and lock checks, for this executable name only.
    Invoke-AppVerifier -Arguments @("/verify", $targetName)
    foreach ($key in $verifierKeys) {
        if (-not (Test-Path -LiteralPath $key)) {
            throw "Application Verifier did not create the expected setting: $key"
        }
    }
    $configured = $true

    for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
        Write-Host "Running Application Verifier UI automation ($iteration/$Iterations)..." -ForegroundColor Cyan
        & powershell -NoProfile -ExecutionPolicy Bypass -File $uiAutomationScript @childUiAutomationArguments
        if ($LASTEXITCODE -ne 0) {
            throw "UI automation failed under Application Verifier (iteration $iteration, exit=$LASTEXITCODE)."
        }
    }
}
finally {
    if ($configured) {
        # Settings are persisted by Windows. appverif.exe's documented delete
        # command can report success while leaving these IFEO keys behind, so
        # remove the exact keys that this script confirmed were absent before
        # it enabled verification. This never overwrites a pre-existing setting.
        foreach ($key in $verifierKeys) {
            if (Test-Path -LiteralPath $key) {
                Remove-Item -LiteralPath $key -Recurse -Force
            }
            if (Test-Path -LiteralPath $key) {
                throw "Application Verifier settings were not removed: $key"
            }
        }
    }
}

Write-Host "Application Verifier memory diagnostics passed ($Iterations UI automation iteration(s))." -ForegroundColor Green
