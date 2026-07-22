[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-RequiredScript {
    param([Parameter(Mandatory)][string]$ScriptPath)

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "Missing script: $ScriptPath"
    }

    $powerShellExe = (Get-Process -Id $PID -ErrorAction Stop).Path
    if ([string]::IsNullOrWhiteSpace($powerShellExe)) {
        throw "Unable to determine the current PowerShell executable."
    }
    & $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath
    if ($LASTEXITCODE -ne 0) {
        exit ([int]$LASTEXITCODE)
    }
}

$fullBuildScript = Join-Path $PSScriptRoot "full_build.ps1"
$releaseSetScript = Join-Path $PSScriptRoot "scripts/release/make_release_set.ps1"

Write-Host "Creating a complete distributable release set..." -ForegroundColor Cyan
Invoke-RequiredScript -ScriptPath $fullBuildScript
Invoke-RequiredScript -ScriptPath $releaseSetScript

exit 0
