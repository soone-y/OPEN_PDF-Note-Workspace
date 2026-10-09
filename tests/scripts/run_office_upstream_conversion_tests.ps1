[CmdletBinding()]
param(
    [string]$SourceDir = "",
    [string]$Soffice = "third_party\libreoffice\custom_runtime\instdir\program\soffice.com",
    [string]$BaselineSoffice = "",
    [string]$OutputDir = "",
    [int]$TimeoutSec = 180,
    [string]$Group = "",
    [string[]]$Sample = @(),
    [switch]$Acceptance,
    [switch]$ListGroups,
    [switch]$Preflight,
    [ValidateRange(1, 8)][int]$Workers = 1,
    [switch]$PairedConversions,
    [Nullable[int]]$FixedTime = $null
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pythonExe = (Get-Command python -ErrorAction Stop).Source
$tool = Join-Path $repoRoot "tools\libreoffice\libreoffice_upstream_conversion_test.py"

Push-Location -LiteralPath $repoRoot
try {
    $arguments = @($tool, "--soffice", $Soffice, "--timeout", "$TimeoutSec", "--workers", "$Workers")
    if (-not [string]::IsNullOrWhiteSpace($SourceDir)) { $arguments += @("--source-dir", $SourceDir) }
    if (-not [string]::IsNullOrWhiteSpace($Group)) {
        foreach ($name in $Group.Split(',')) { $arguments += @("--group", $name.Trim()) }
    }
    foreach ($path in $Sample) { $arguments += @("--sample", $path) }
    if ($Acceptance) { $arguments += "--acceptance" }
    if ($PairedConversions) { $arguments += "--paired-conversions" }
    if ($null -ne $FixedTime) { $arguments += @("--fixed-time", "$FixedTime") }
    if ($ListGroups) { $arguments += "--list-groups" }
    if ($Preflight) { $arguments += "--preflight" }
    if (-not [string]::IsNullOrWhiteSpace($BaselineSoffice)) {
        $arguments += @("--baseline-soffice", $BaselineSoffice)
    }
    if (-not [string]::IsNullOrWhiteSpace($OutputDir)) {
        $arguments += @("--output-dir", $OutputDir)
    }
    & $pythonExe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Upstream Office conversion check failed or was incomplete (exit=$LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}
