[CmdletBinding()]
param(
    [switch]$Lite,
    [switch]$Rebuild,
    [switch]$Clean,
    [switch]$VerboseOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-RequiredScript {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [string[]]$Arguments = @()
    )

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "Missing script: $ScriptPath"
    }

    $powerShellExe = (Get-Process -Id $PID -ErrorAction Stop).Path
    if ([string]::IsNullOrWhiteSpace($powerShellExe)) {
        throw "Unable to determine the current PowerShell executable."
    }
    & $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        exit ([int]$LASTEXITCODE)
    }
}

$fullBuildScript = Join-Path $PSScriptRoot "full_build.ps1"
$releaseSetScript = Join-Path $PSScriptRoot "scripts/release/make_release_set.ps1"

$buildArgs = @()
if ($Lite) { $buildArgs += "-Lite" }
if ($Rebuild) { $buildArgs += "-Rebuild" }
if ($Clean) { $buildArgs += "-Clean" }
if ($VerboseOutput) { $buildArgs += "-VerboseOutput" }

$releaseSetArgs = @()
if ($Lite) { $releaseSetArgs += "-Lite" }

if ($Lite) {
    Write-Host "Creating a Lite distributable release set..." -ForegroundColor Cyan
} else {
    Write-Host "Creating a complete distributable release set..." -ForegroundColor Cyan
}

Invoke-RequiredScript -ScriptPath $fullBuildScript -Arguments $buildArgs
Invoke-RequiredScript -ScriptPath $releaseSetScript -Arguments $releaseSetArgs

exit 0
