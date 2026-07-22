[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipUiAutomation,
    [switch]$SkipCodebaseValidation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$powershellExe = (Get-Command powershell -ErrorAction Stop).Source
$pythonExe = (Get-Command python -ErrorAction Stop).Source
$workspaceBuildScript = Join-Path $repoRoot "scripts\build\build_workspace.ps1"
$noteParserScript = Join-Path $PSScriptRoot "run_note_parser_tests.ps1"
$uiAutomationScript = Join-Path $PSScriptRoot "run_ui_automation_fault_tests.ps1"
$codebaseValidationScript = Join-Path $repoRoot "tests\python\validate_codebase.py"

function Invoke-Check {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][scriptblock]$Action
    )

    Write-Host ""
    Write-Host ("== {0} ==" -f $Name) -ForegroundColor Cyan
    & $Action
    Write-Host ("[PASS] {0}" -f $Name) -ForegroundColor Green
}

function Invoke-PowerShellScript {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string[]]$Arguments = @()
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Required script was not found: $Path"
    }
    & $powershellExe -NoProfile -ExecutionPolicy Bypass -File $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Script failed: $Path (exit=$LASTEXITCODE)"
    }
}

Push-Location -LiteralPath $repoRoot
try {
    Invoke-Check -Name "Note text boundaries and kernel history" -Action {
        Invoke-PowerShellScript -Path $noteParserScript -Arguments @("-ArtifactName", "note_undo_redo_tests")
    }

    if (-not $SkipCodebaseValidation) {
        Invoke-Check -Name "Codebase validation" -Action {
            & $pythonExe $codebaseValidationScript
            if ($LASTEXITCODE -ne 0) {
                throw "Codebase validation failed (exit=$LASTEXITCODE)"
            }
        }
    }

    if (-not $SkipBuild) {
        Invoke-Check -Name "Full app build" -Action {
            Invoke-PowerShellScript -Path $workspaceBuildScript -Arguments @("-Edition", "Full")
        }
    }

    if (-not $SkipUiAutomation) {
        Invoke-Check -Name "Undo/redo UI automation" -Action {
            Invoke-PowerShellScript -Path $uiAutomationScript
        }
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "All selected undo/redo checks passed." -ForegroundColor Green
