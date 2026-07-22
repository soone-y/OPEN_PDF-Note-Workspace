[CmdletBinding()]
param(
    [string]$SourceRepo = "",
    [Parameter(Mandatory = $true)]
    [string]$OutputBundle,
    [string]$Allowlist = "",
    [string]$GitignoreTemplate = "",
    [string]$TempRoot = "",
    [string]$PublicBranch = "main",
    [string]$PublicAuthorName = "Public Maintainer",
    [string]$PublicAuthorEmail = "public@example.invalid",
    [switch]$BucketCommitTimes,
    [switch]$PreserveMergeCommits,
    [switch]$KeepRepo,
    [switch]$SkipMetadataSanitization,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = $PSScriptRoot
if (-not $scriptRoot) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$scriptsRoot = Split-Path -Parent $scriptRoot
$repoRoot = Split-Path -Parent $scriptsRoot

function Write-Info([string]$Message) { Write-Host $Message -ForegroundColor Cyan }

function Resolve-AbsolutePath([string]$Path, [string]$BasePath) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Path must not be empty."
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Test-IsSameOrChildPath([string]$ParentPath, [string]$CandidatePath) {
    $parentFull = [System.IO.Path]::GetFullPath($ParentPath).TrimEnd('\', '/')
    $candidateFull = [System.IO.Path]::GetFullPath($CandidatePath).TrimEnd('\', '/')
    if ($candidateFull -eq $parentFull) {
        return $true
    }
    return $candidateFull.StartsWith($parentFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}

function Remove-FileIfExists([string]$TargetPath) {
    if (-not (Test-Path -LiteralPath $TargetPath)) {
        return
    }
    if ($DryRun) {
        Write-Info "[dry-run] remove existing bundle: $TargetPath"
        return
    }
    Remove-Item -LiteralPath $TargetPath -Force
}

function Invoke-GitBundleCreate([string]$RepoPath, [string]$BundlePath, [string]$BranchName) {
    if ($DryRun) {
        Write-Info ("[dry-run] git -C {0} bundle create `"{1}`" {2}" -f $RepoPath, $BundlePath, $BranchName)
        return
    }
    Push-Location -LiteralPath $RepoPath
    try {
        & git bundle create $BundlePath $BranchName
        if ($LASTEXITCODE -ne 0) {
            throw "git bundle create failed."
        }
    }
    finally {
        Pop-Location
    }
}

$sourceRepoPath = if ([string]::IsNullOrWhiteSpace($SourceRepo)) { $repoRoot } else { Resolve-AbsolutePath -Path $SourceRepo -BasePath $repoRoot }
$bundlePath = Resolve-AbsolutePath -Path $OutputBundle -BasePath $repoRoot
$tempRootPath = if ([string]::IsNullOrWhiteSpace($TempRoot)) {
    Join-Path ([System.IO.Path]::GetTempPath()) "pdf-note-public-history"
}
else {
    Resolve-AbsolutePath -Path $TempRoot -BasePath $repoRoot
}

$timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss_fff") + "_" + [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
$tempRepoPath = Join-Path $tempRootPath ("public_history_repo_" + $timestamp)
$createScript = Join-Path $scriptRoot "create_public_history.ps1"

if (-not (Test-Path -LiteralPath $createScript)) {
    throw "Missing public history creation script: $createScript"
}
if (Test-IsSameOrChildPath -ParentPath $sourceRepoPath -CandidatePath $tempRootPath) {
    throw "Temporary root for bundle export must be outside the source repository: $tempRootPath"
}

$bundleParent = Split-Path -Parent $bundlePath
if (-not [string]::IsNullOrWhiteSpace($bundleParent) -and -not (Test-Path -LiteralPath $bundleParent)) {
    if ($DryRun) {
        Write-Info "[dry-run] mkdir: $bundleParent"
    }
    else {
        New-Item -ItemType Directory -Force -Path $bundleParent | Out-Null
    }
}

Remove-FileIfExists -TargetPath $bundlePath

$createArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $createScript,
    "-OutputRepo", $tempRepoPath,
    "-ReplaceOutput",
    "-PublicBranch", $PublicBranch,
    "-PublicAuthorName", $PublicAuthorName,
    "-PublicAuthorEmail", $PublicAuthorEmail
)
if (-not [string]::IsNullOrWhiteSpace($SourceRepo)) {
    $createArgs += @("-SourceRepo", $SourceRepo)
}
if (-not [string]::IsNullOrWhiteSpace($Allowlist)) {
    $createArgs += @("-Allowlist", $Allowlist)
}
if (-not [string]::IsNullOrWhiteSpace($GitignoreTemplate)) {
    $createArgs += @("-GitignoreTemplate", $GitignoreTemplate)
}
if (-not [string]::IsNullOrWhiteSpace($TempRoot)) {
    $createArgs += @("-TempRoot", $TempRoot)
}
if ($SkipMetadataSanitization) {
    $createArgs += "-SkipMetadataSanitization"
}
if ($BucketCommitTimes) {
    $createArgs += "-BucketCommitTimes"
}
if ($PreserveMergeCommits) {
    $createArgs += "-PreserveMergeCommits"
}
if ($DryRun) {
    $createArgs += "-DryRun"
}

Write-Info "Bundle output: $bundlePath"
Write-Info "Temporary public-history repo: $tempRepoPath"

try {
    & powershell @createArgs
    if ($LASTEXITCODE -ne 0) {
        throw "create_public_history.ps1 failed."
    }

    Invoke-GitBundleCreate -RepoPath $tempRepoPath -BundlePath $bundlePath -BranchName $PublicBranch

    if (-not $KeepRepo) {
        if ($DryRun) {
            Write-Info "[dry-run] remove temporary public-history repo after bundle creation"
        }
        elseif (Test-Path -LiteralPath $tempRepoPath) {
            Remove-Item -LiteralPath $tempRepoPath -Recurse -Force
        }
    }

    Write-Info "Public-history bundle created successfully."
    Write-Info "Import examples:"
    Write-Info ("  git clone `"{0}`" imported-public-repo" -f $bundlePath)
    Write-Info ("  git -C <existing-public-repo> fetch `"{0}`" {1}:{1}" -f $bundlePath, $PublicBranch)
}
catch {
    if (-not $DryRun -and (Test-Path -LiteralPath $tempRepoPath)) {
        Write-Info "Temporary public-history repo was kept for inspection:"
        Write-Info ("  {0}" -f $tempRepoPath)
    }
    throw
}
