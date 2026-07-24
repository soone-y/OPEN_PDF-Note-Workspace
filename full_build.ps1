[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$Clean,
    [switch]$VerboseOutput,
    [switch]$Lite
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-FullBuildStep {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [string]$Edition = "",
        [switch]$ForceRebuild
    )

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "Missing build script: $ScriptPath"
    }

    $powerShellExe = (Get-Process -Id $PID -ErrorAction Stop).Path
    if ([string]::IsNullOrWhiteSpace($powerShellExe)) {
        throw "Unable to determine the current PowerShell executable."
    }
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath)
    if ($Rebuild -or $ForceRebuild) { $arguments += "-Rebuild" }
    if ($Clean) { $arguments += "-Clean" }
    if ($VerboseOutput) { $arguments += "-VerboseOutput" }
    if (-not [string]::IsNullOrWhiteSpace($Edition)) { $arguments += @("-Edition", $Edition) }

    & $powerShellExe @arguments
    if ($LASTEXITCODE -ne 0) {
        exit ([int]$LASTEXITCODE)
    }
}

function Test-BuildConfigurationIsNewer {
    param(
        [Parameter(Mandatory)][string]$ArtifactPath,
        [Parameter(Mandatory)][string[]]$ConfigurationPaths
    )

    if (-not (Test-Path -LiteralPath $ArtifactPath)) {
        return $true
    }

    $artifactTime = (Get-Item -LiteralPath $ArtifactPath).LastWriteTimeUtc
    foreach ($path in $ConfigurationPaths) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Missing build configuration input: $path"
        }
        if ((Get-Item -LiteralPath $path).LastWriteTimeUtc -gt $artifactTime) {
            return $true
        }
    }
    return $false
}

$workspaceBuildScript = Join-Path $PSScriptRoot "scripts/build/build_workspace.ps1"
$readOnlyViewerBuildScript = Join-Path $PSScriptRoot "scripts/build/build_readonly_viewer.ps1"
$buildSourcesManifest = Join-Path $PSScriptRoot "scripts/build/build_sources.json"
$fullArtifactPath = Join-Path $PSScriptRoot "out/bin/pdf_note_workspace.exe"
$liteArtifactPath = Join-Path $PSScriptRoot "out/bin_lite/pdf_note_workspace.exe"
$readOnlyViewerArtifactPath = Join-Path $PSScriptRoot "out/bin/readonly_viewer.exe"
$workspaceConfigurationInputs = @($workspaceBuildScript, $buildSourcesManifest)
$readOnlyViewerConfigurationInputs = @($readOnlyViewerBuildScript, $buildSourcesManifest)

if ($Lite) {
    Write-Host "Building Lite distributable application (Release configuration)..." -ForegroundColor Cyan
    Invoke-FullBuildStep -ScriptPath $workspaceBuildScript -Edition "Lite" -ForceRebuild:(Test-BuildConfigurationIsNewer -ArtifactPath $liteArtifactPath -ConfigurationPaths $workspaceConfigurationInputs)
    Invoke-FullBuildStep -ScriptPath $readOnlyViewerBuildScript -ForceRebuild:(Test-BuildConfigurationIsNewer -ArtifactPath $readOnlyViewerArtifactPath -ConfigurationPaths $readOnlyViewerConfigurationInputs)
} else {
    Write-Host "Building all distributable applications (Release configuration)..." -ForegroundColor Cyan
    Invoke-FullBuildStep -ScriptPath $workspaceBuildScript -ForceRebuild:(Test-BuildConfigurationIsNewer -ArtifactPath $fullArtifactPath -ConfigurationPaths $workspaceConfigurationInputs)
    Invoke-FullBuildStep -ScriptPath $workspaceBuildScript -Edition "Lite" -ForceRebuild:(Test-BuildConfigurationIsNewer -ArtifactPath $liteArtifactPath -ConfigurationPaths $workspaceConfigurationInputs)
    Invoke-FullBuildStep -ScriptPath $readOnlyViewerBuildScript -ForceRebuild:(Test-BuildConfigurationIsNewer -ArtifactPath $readOnlyViewerArtifactPath -ConfigurationPaths $readOnlyViewerConfigurationInputs)
}

exit 0
