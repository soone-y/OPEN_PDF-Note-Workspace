[CmdletBinding()]
param(
    [switch]$Lite,
    [switch]$Rebuild,
    [switch]$Clean,
    [switch]$VerboseOutput,
    [switch]$DeferPostCreationValidation,
    [ValidateSet("ja", "en")]
    [string]$Locale = "ja"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

$buildScript = Join-Path $PSScriptRoot "build.ps1"
$releaseSetScript = Join-Path $PSScriptRoot "scripts/release/make_release_set.ps1"

if ($Lite) {
    throw "release.ps1 は通常版と Lite版を一組で作成するため、-Lite は指定できません。Lite版だけをビルド・確認する場合は .\\build.ps1 -Lite -Locale $Locale を使用してください。"
}
if ($Clean -and $Rebuild) {
    throw "-Clean と -Rebuild は同時に指定できません。成果物を削除するだけなら -Clean、削除後にrelease setを作り直すなら -Rebuild を指定してください。"
}
if ($Clean -and $DeferPostCreationValidation) {
    throw "-Clean と -DeferPostCreationValidation は同時に指定できません。-Clean はrelease setを作成しない清掃専用の操作です。"
}

$buildArgs = @()
$buildArgs += @("-Locale", $Locale)
if ($Rebuild) { $buildArgs += "-Rebuild" }
if ($Clean) { $buildArgs += "-Clean" }
if ($VerboseOutput) { $buildArgs += "-VerboseOutput" }

$releaseSetArgs = @()
$releaseSetArgs += @("-Locale", $Locale)
if ($DeferPostCreationValidation) { $releaseSetArgs += "-DeferPostCreationValidation" }

if ($Clean) {
    Write-Info "release set は作成せず、ビルド成果物だけを削除します（言語: $Locale）。"
    Invoke-RequiredScript -ScriptPath $buildScript -Arguments $buildArgs -Description "ビルド成果物の削除"
    Write-Host "清掃が完了しました。release set は作成していません。" -ForegroundColor Green
    exit 0
}

Write-Info "通常版・Lite版を含むrelease setを作成します（言語: $Locale）。"

Invoke-RequiredScript -ScriptPath $buildScript -Arguments $buildArgs -Description "配布用アプリケーションのビルド"
Invoke-RequiredScript -ScriptPath $releaseSetScript -Arguments $releaseSetArgs -Description "release set の作成"

exit 0
