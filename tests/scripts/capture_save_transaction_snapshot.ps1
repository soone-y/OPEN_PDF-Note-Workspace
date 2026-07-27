param(
    [Parameter(Mandatory = $false)]
    [string]$WorkspaceRoot = ".",

    [Parameter(Mandatory = $false)]
    [string]$Label = "snapshot"
)

$ErrorActionPreference = "Stop"

function Resolve-PathSafe([string]$p) {
    try {
        return (Resolve-Path -LiteralPath $p).Path
    } catch {
        return [System.IO.Path]::GetFullPath($p)
    }
}

$root = Resolve-PathSafe $WorkspaceRoot
$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $root "__resource__\__tmp__\__test_logs__"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$outFile = Join-Path $outDir ("save_tx_{0}_{1}.log" -f $Label, $ts)

$stageRoot = Join-Path $root "__resource__\__tmp__\__stage__"
$backupRoot = Join-Path $root "__resource__\__escape__\backup"

@(
    "timestamp: $((Get-Date).ToString("o"))"
    "workspace_root: $root"
    "stage_root: $stageRoot"
    "backup_root: $backupRoot"
    ""
    "[stage_files]"
) | Out-File -FilePath $outFile -Encoding utf8

if (Test-Path -LiteralPath $stageRoot) {
    Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            "{0}`t{1}`t{2}" -f $_.LastWriteTime.ToString("o"), $_.Length, $_.FullName
        } | Out-File -FilePath $outFile -Append -Encoding utf8
} else {
    "stage root not found." | Out-File -FilePath $outFile -Append -Encoding utf8
}

@(
    ""
    "[backup_files]"
) | Out-File -FilePath $outFile -Append -Encoding utf8

if (Test-Path -LiteralPath $backupRoot) {
    Get-ChildItem -LiteralPath $backupRoot -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            "{0}`t{1}`t{2}" -f $_.LastWriteTime.ToString("o"), $_.Length, $_.FullName
        } | Out-File -FilePath $outFile -Append -Encoding utf8
} else {
    "backup root not found." | Out-File -FilePath $outFile -Append -Encoding utf8
}

@(
    ""
    "[meta_preview]"
) | Out-File -FilePath $outFile -Append -Encoding utf8

if (Test-Path -LiteralPath $stageRoot) {
    Get-ChildItem -LiteralPath $stageRoot -Recurse -File -Filter "*.meta.txt" |
        Sort-Object FullName |
        ForEach-Object {
            ">>> $($_.FullName)" | Out-File -FilePath $outFile -Append -Encoding utf8
            Get-Content -LiteralPath $_.FullName -Encoding utf8 |
                Out-File -FilePath $outFile -Append -Encoding utf8
        }
}

Write-Host "saved: $outFile"
