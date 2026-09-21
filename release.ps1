[CmdletBinding()]
param(
    [switch]$Lite,
    [switch]$Rebuild,
    [switch]$Clean,
    [switch]$Test,
    [switch]$VerboseOutput,
    [switch]$DeferPostCreationValidation,
    [switch]$AllLocales,
    [ValidateSet("ja", "en")]
    [string]$Locale = "ja",
    [string]$ReleaseSetBaseDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:runLogPath = Join-Path $PSScriptRoot ("out\logs\release\release_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
$script:runTranscriptStarted = $false

function Stop-RunLog {
    if ($script:runTranscriptStarted) {
        try { Stop-Transcript | Out-Null } catch { }
        $script:runTranscriptStarted = $false
    }
}

trap {
    Write-Host "release に失敗しました。実行ログ: $script:runLogPath" -ForegroundColor Yellow
    Stop-RunLog
    throw $_
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $script:runLogPath) | Out-Null
Start-Transcript -LiteralPath $script:runLogPath -Force | Out-Null
$script:runTranscriptStarted = $true
Write-Host "release 実行ログ: $script:runLogPath" -ForegroundColor DarkCyan

function Write-Info([string]$Message) {
    Write-Host $Message -ForegroundColor Cyan
}

function Invoke-RequiredScript {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "$Description に必要なスクリプトが見つかりません: $ScriptPath"
    }

    $powerShellExe = (Get-Process -Id $PID -ErrorAction Stop).Path
    if ([string]::IsNullOrWhiteSpace($powerShellExe)) {
        throw "現在実行中の PowerShell を特定できません。PowerShell からもう一度実行してください。"
    }
    & $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description に失敗しました（終了コード: $LASTEXITCODE、言語: $Locale）。出力されたエラーと out\\logs を確認してください。"
    }
}

function Get-ReleaseSetBaseDirectory {
    if ([string]::IsNullOrWhiteSpace($ReleaseSetBaseDir)) {
        Join-Path (Split-Path -Parent $PSScriptRoot) "PDF-Note-ReleaseSet"
    }
    else {
        [System.IO.Path]::GetFullPath($ReleaseSetBaseDir)
    }
}

function Get-ReleasePairDirectory {
    $versionFile = Join-Path $PSScriptRoot "REPO_VERSION.txt"
    $version = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -Raw -Encoding UTF8).Trim() } else { "unknown" }
    $safeVersion = ($version -replace '[^0-9A-Za-z._-]+', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($safeVersion)) { $safeVersion = "unknown" }
    $base = Get-ReleaseSetBaseDirectory
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    return (Join-Path $base "pdf_note_workspace_release_${safeVersion}_${stamp}")
}

$buildScript = Join-Path $PSScriptRoot "build.ps1"
$workspaceBuildScript = Join-Path $PSScriptRoot "scripts\build\build_workspace.ps1"
$readOnlyViewerBuildScript = Join-Path $PSScriptRoot "scripts\build\build_readonly_viewer.ps1"
$packReleaseScript = Join-Path $PSScriptRoot "scripts\release\pack_release.ps1"
$releaseSetScript = Join-Path $PSScriptRoot "scripts/release/make_release_set.ps1"

if ($Lite) {
    throw "release.ps1 は通常版と Lite版を一組で作成するため、-Lite は指定できません。Lite版だけをビルド・確認する場合は .\\build.ps1 -Lite -Locale $Locale を使用してください。"
}
if ($AllLocales -and $PSBoundParameters.ContainsKey("Locale")) {
    throw "-AllLocales と -Locale は同時に指定できません。両言語なら -AllLocales、単一言語なら -Locale ja または -Locale en を使用してください。"
}
if ($Clean -and $Rebuild) {
    throw "-Clean と -Rebuild は同時に指定できません。成果物を削除するだけなら -Clean、削除後にrelease setを作り直すなら -Rebuild を指定してください。"
}
if ($Clean -and $DeferPostCreationValidation) {
    throw "-Clean と -DeferPostCreationValidation は同時に指定できません。-Clean はrelease setを作成しない清掃専用の操作です。"
}
if ($Test -and ($Lite -or $Clean -or $AllLocales -or $DeferPostCreationValidation)) {
    throw "-Test は JA通常版の非ZIP確認出力専用です。-Lite、-Clean、-AllLocales、-DeferPostCreationValidation は併用できません。"
}
if ($Test -and $PSBoundParameters.ContainsKey("Locale") -and $Locale -ne "ja") {
    throw "-Test は日本語通常版だけを作成します。-Locale en は指定できません。"
}
if ($Test -and -not [string]::IsNullOrWhiteSpace($ReleaseSetBaseDir)) {
    throw "-Test の出力先は PDF-Note-ReleaseSet\\TEST_ONLY_DO_NOT_PUBLISH に固定です。-ReleaseSetBaseDir は指定できません。"
}

if ($Clean) {
    $cleanLocales = if ($AllLocales) { @("ja", "en") } else { @($Locale) }
    foreach ($cleanLocale in $cleanLocales) {
        $buildArgs = @("-Locale", $cleanLocale, "-Clean")
        if ($VerboseOutput) { $buildArgs += "-VerboseOutput" }
        Write-Info "release set は作成せず、ビルド成果物だけを削除します（言語: $cleanLocale）。"
        Invoke-RequiredScript -ScriptPath $buildScript -Arguments $buildArgs -Description "ビルド成果物の削除"
    }
    Write-Host "清掃が完了しました。release set は作成していません。" -ForegroundColor Green
    Stop-RunLog
    exit 0
}

if ($Test) {
    $testOutputBase = Join-Path (Get-ReleaseSetBaseDirectory) "TEST_ONLY_DO_NOT_PUBLISH"
    Write-Info "TEST ONLY: 日本語通常版の非ZIP確認出力を作成します。これはrelease setでも配布物でもpublish候補でもありません。"
    Write-Info "TEST ONLY output base: $testOutputBase"

    $fullBuildArgs = @("-Locale", "ja")
    if ($Rebuild) { $fullBuildArgs += "-Rebuild" }
    if ($VerboseOutput) { $fullBuildArgs += "-VerboseOutput" }
    Invoke-RequiredScript -ScriptPath $workspaceBuildScript -Arguments $fullBuildArgs -Description "TEST ONLY 日本語通常版のビルド"

    $viewerBuildArgs = @("-Locale", "ja")
    if ($Rebuild) { $viewerBuildArgs += "-Rebuild" }
    if ($VerboseOutput) { $viewerBuildArgs += "-VerboseOutput" }
    Invoke-RequiredScript -ScriptPath $readOnlyViewerBuildScript -Arguments $viewerBuildArgs -Description "TEST ONLY 閲覧専用ビューアーのビルド"

    $testPackageArgs = @(
        "-OutBaseDir", $testOutputBase,
        "-NamePrefix", "pdf_note_workspace_TEST_ONLY_DO_NOT_PUBLISH_ja_full",
        "-Locale", "ja",
        "-NoChecksums",
        "-TestOnly"
    )
    Invoke-RequiredScript -ScriptPath $packReleaseScript -Arguments $testPackageArgs -Description "TEST ONLY 非ZIP確認出力の作成"
    Write-Host "TEST ONLY 出力が完了しました。PDF-Note-ReleaseSet\\TEST_ONLY_DO_NOT_PUBLISH 配下は提出・publish・配布に使用できません。" -ForegroundColor Yellow
    Stop-RunLog
    exit 0
}

# Keep this an array even for the default single locale.  A conditional
# expression unwraps a one-item array during assignment; StrictMode would then
# reject the .Count checks below on the resulting string.
$targetLocales = @(
    if ($AllLocales) { "ja"; "en" } else { $Locale }
)
$pairDirectory = ""
$frozenPublicSnapshot = ""
if ($targetLocales.Count -eq 2) {
    $pairDirectory = Get-ReleasePairDirectory
    New-Item -ItemType Directory -Path $pairDirectory -ErrorAction Stop | Out-Null
    Write-Info "JA/EN release set の共通親フォルダ: $pairDirectory"
}

foreach ($targetLocale in $targetLocales) {
    Write-Info "通常版・Lite版を含むrelease setを作成します（言語: $targetLocale）。"
    $buildArgs = @("-Locale", $targetLocale)
    if ($Rebuild) { $buildArgs += "-Rebuild" }
    if ($VerboseOutput) { $buildArgs += "-VerboseOutput" }
    $releaseSetArgs = @("-Locale", $targetLocale)
    if ($DeferPostCreationValidation) { $releaseSetArgs += "-DeferPostCreationValidation" }
    if ($targetLocales.Count -eq 2) {
        $releaseSetArgs += @("-OutBaseDir", $pairDirectory, "-ReleaseSetName", $targetLocale)
        if (-not [string]::IsNullOrWhiteSpace($frozenPublicSnapshot)) {
            $releaseSetArgs += @("-PublicSnapshotSource", $frozenPublicSnapshot)
        }
    }
    Invoke-RequiredScript -ScriptPath $buildScript -Arguments $buildArgs -Description "配布用アプリケーションのビルド"
    Invoke-RequiredScript -ScriptPath $releaseSetScript -Arguments $releaseSetArgs -Description "release set の作成"
    if ($targetLocales.Count -eq 2 -and [string]::IsNullOrWhiteSpace($frozenPublicSnapshot)) {
        $frozenPublicSnapshot = Join-Path (Join-Path $pairDirectory $targetLocale) "public_snapshot"
        if (-not (Test-Path -LiteralPath $frozenPublicSnapshot -PathType Container)) {
            throw "最初の release set の固定 public_snapshot が見つかりません: $frozenPublicSnapshot"
        }
    }
}

if ($targetLocales.Count -eq 2) { Write-Output "RELEASE_PAIR_PATH=$pairDirectory" }

Stop-RunLog
exit 0
