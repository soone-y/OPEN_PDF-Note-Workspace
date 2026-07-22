[CmdletBinding()]
param(
    [string]$SampleDir = "tests\fixtures\office_conversion",
    [string]$Soffice = "third_party\libreoffice\custom_runtime\instdir\program\soffice.com",
    [int]$TimeoutSec = 180,
    [switch]$Keep
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
$tool = Join-Path $repoRoot "tools\libreoffice\libreoffice_smoke_test.py"
$samplePath = Join-Path $repoRoot $SampleDir
$sofficePath = if ([System.IO.Path]::IsPathRooted($Soffice)) { $Soffice } else { Join-Path $repoRoot $Soffice }

if ([string]::IsNullOrWhiteSpace($pythonExe)) {
    throw "python.exe not found in PATH."
}
if (-not (Test-Path -LiteralPath $tool)) {
    throw "Missing tool: $tool"
}
if (-not (Test-Path -LiteralPath $samplePath)) {
    throw "Sample directory not found: $samplePath"
}
if (-not (Test-Path -LiteralPath $sofficePath)) {
    throw "LibreOffice soffice.com not found: $sofficePath"
}

$docxCount = @(Get-ChildItem -LiteralPath $samplePath -Recurse -File -Filter "*.docx").Count
$pptxCount = @(Get-ChildItem -LiteralPath $samplePath -Recurse -File -Filter "*.pptx").Count
if ($docxCount -lt 1 -or $pptxCount -lt 1) {
    throw "Place at least one .docx and one .pptx under: $samplePath"
}

$args = @(
    $tool,
    "--repo-root", $repoRoot,
    "--soffice", $sofficePath,
    "--input-dir", $SampleDir,
    "--require-docx",
    "--require-pptx",
    "--timeout", "$TimeoutSec"
)
if ($Keep) {
    $args += "--keep"
}

Push-Location -LiteralPath $repoRoot
try {
    Write-Host "Running Office conversion fixture tests..." -ForegroundColor Cyan
    & $pythonExe @args
    if ($LASTEXITCODE -ne 0) {
        throw "office conversion fixture tests failed"
    }
    Write-Host "All Office conversion fixture tests passed." -ForegroundColor Green
}
finally {
    Pop-Location
}
