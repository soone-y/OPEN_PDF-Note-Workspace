[CmdletBinding()]
param(
    [string]$InputDir = ".local\修正版_変換検証ファイル一式",
    [string]$Soffice = "third_party\libreoffice\custom_runtime\instdir\program\soffice.com",
    [string]$OutputDir = "",
    [int]$TimeoutSec = 180,
    [int]$Dpi = 144,
    [int]$PixelThreshold = 8,
    [switch]$PreserveHostHome
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
$tool = Join-Path $repoRoot "tools\libreoffice\libreoffice_conversion_quality_test.py"

function Resolve-FromRepo([string]$PathValue) {
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $PathValue))
}

if ([string]::IsNullOrWhiteSpace($pythonExe)) {
    throw "python.exe not found in PATH."
}

$inputPath = Resolve-FromRepo $InputDir
$sofficePath = Resolve-FromRepo $Soffice
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $outputPath = Join-Path $repoRoot ".local\repo_resource\tmp\libreoffice_reconstruction\quality\quality_$stamp"
}
else {
    $outputPath = Resolve-FromRepo $OutputDir
}

foreach ($required in @($tool, $inputPath, $sofficePath)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required path not found: $required"
    }
}
if (Test-Path -LiteralPath $outputPath) {
    throw "Output directory already exists: $outputPath"
}

$arguments = @(
    $tool,
    "--input-dir", $inputPath,
    "--soffice", $sofficePath,
    "--output-dir", $outputPath,
    "--timeout", "$TimeoutSec",
    "--dpi", "$Dpi",
    "--pixel-threshold", "$PixelThreshold"
)
if ($PreserveHostHome) {
    Write-Warning "Host HOME/APPDATA will be preserved for stock LibreOffice compatibility. Use a short ASCII OutputDir."
    $arguments += "--preserve-host-home"
}

Push-Location -LiteralPath $repoRoot
try {
    & $pythonExe @arguments
    $exitCode = $LASTEXITCODE
    Write-Host "Quality report: $(Join-Path $outputPath 'quality_report.json')"
    if ($exitCode -ne 0) {
        throw "Office conversion quality differences were detected (exit=$exitCode)."
    }
}
finally {
    Pop-Location
}
