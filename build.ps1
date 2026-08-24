[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$Clean,
    [switch]$VerboseOutput,
    [switch]$Lite,
    [ValidateSet("ja", "en")]
    [string]$Locale = "ja"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Info([string]$Message) {
    Write-Host $Message -ForegroundColor Cyan
}

function Invoke-FullBuildStep {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [string]$Edition = "",
        [Parameter(Mandatory)][string]$Locale,
        [switch]$ForceRebuild
    )

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "必要なビルドスクリプトが見つかりません: $ScriptPath"
    }

    $powerShellExe = (Get-Process -Id $PID -ErrorAction Stop).Path
    if ([string]::IsNullOrWhiteSpace($powerShellExe)) {
        throw "現在実行中の PowerShell を特定できません。PowerShell からもう一度実行してください。"
    }
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath)
    if ($Rebuild -or $ForceRebuild) { $arguments += "-Rebuild" }
    if ($Clean) { $arguments += "-Clean" }
    if ($VerboseOutput) { $arguments += "-VerboseOutput" }
    $arguments += @("-Locale", $Locale)
    if (-not [string]::IsNullOrWhiteSpace($Edition)) { $arguments += @("-Edition", $Edition) }

    & $powerShellExe @arguments
    if ($LASTEXITCODE -ne 0) {
        $target = if ([string]::IsNullOrWhiteSpace($Edition)) { "通常版" } else { "$Edition 版" }
        throw "${target}のビルドに失敗しました（終了コード: $LASTEXITCODE、言語: $Locale）。出力されたエラーと out\\logs を確認してください。"
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
$repoVersionFile = Join-Path $PSScriptRoot "REPO_VERSION.txt"
$binSuffix = if ($Locale -eq "en") { "_en" } else { "" }
$fullArtifactPath = Join-Path $PSScriptRoot ("out/bin{0}/pdf_note_workspace.exe" -f $binSuffix)
$liteArtifactPath = Join-Path $PSScriptRoot ("out/bin_lite{0}/pdf_note_workspace.exe" -f $binSuffix)
$readOnlyViewerArtifactPath = Join-Path $PSScriptRoot ("out/bin{0}/readonly_viewer.exe" -f $binSuffix)
$workspaceConfigurationInputs = @($workspaceBuildScript, $buildSourcesManifest, $repoVersionFile)
$readOnlyViewerConfigurationInputs = @($readOnlyViewerBuildScript, $buildSourcesManifest, $repoVersionFile)

if ($Clean -and $Rebuild) {
    throw "-Clean と -Rebuild は同時に指定できません。成果物を削除するだけなら -Clean、削除後に作り直すなら -Rebuild を指定してください。"
}

if ($Lite) {
    if ($Clean) {
        Write-Info "Lite版と閲覧専用ビューアーのビルド成果物を削除します（言語: $Locale）。"
    }
    else {
        Write-Info "Lite版と閲覧専用ビューアーをビルドします（Release、言語: $Locale）。"
    }
    Invoke-FullBuildStep -ScriptPath $workspaceBuildScript -Edition "Lite" -Locale $Locale
    Invoke-FullBuildStep -ScriptPath $readOnlyViewerBuildScript -Locale $Locale
} else {
    if ($Clean) {
        Write-Info "通常版・Lite版・閲覧専用ビューアーのビルド成果物を削除します（言語: $Locale）。"
    }
    else {
        Write-Info "通常版・Lite版・閲覧専用ビューアーをビルドします（Release、言語: $Locale）。"
    }
    Invoke-FullBuildStep -ScriptPath $workspaceBuildScript -Locale $Locale
    Invoke-FullBuildStep -ScriptPath $workspaceBuildScript -Edition "Lite" -Locale $Locale
    Invoke-FullBuildStep -ScriptPath $readOnlyViewerBuildScript -Locale $Locale
}

if ($Clean) {
    Write-Host "ビルド成果物の削除が完了しました。" -ForegroundColor Green
}
else {
    Write-Host "ビルドが完了しました。" -ForegroundColor Green
}

exit 0
