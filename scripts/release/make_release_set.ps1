[CmdletBinding()]
param(
    [string]$OutBaseDir = "..\\PDF-Note-ReleaseSet",
    [string]$NamePrefix = "pdf_note_workspace_release_set",
    [switch]$Zip = $true,
    [switch]$Checksums = $true,
    [switch]$IncludeWorkspace,
    [string]$WorkspacePath = "",
    [switch]$NoSetupJson,
    [switch]$NoSampleWorkspace,
    [string]$LibreOfficeRuntimePath = "",
    [switch]$SkipFreshnessCheck,
    [string]$ReleaseNotesPath = "",
    [string]$PublicAllowlist = "",
    [string]$PublicGitignoreTemplate = "",
    [switch]$SnapshotOnly,
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
if (-not $repoRoot) {
    $repoRoot = $scriptRoot
}

function Write-Info([string]$Message) { Write-Host $Message -ForegroundColor Cyan }

function Ensure-Directory([string]$Path) {
    if ($DryRun) {
        Write-Info "[dry-run] mkdir: $Path"
        return
    }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Copy-FileStrict([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing file: $Source"
    }
    $destDir = Split-Path -Parent $Destination
    if ($destDir) {
        Ensure-Directory $destDir
    }
    if ($DryRun) {
        Write-Info "[dry-run] copy: $Source -> $Destination"
        return
    }
    Copy-Item -Force -LiteralPath $Source -Destination $Destination
}

function Write-JsonFile([string]$Destination, [object]$Value) {
    $destDir = Split-Path -Parent $Destination
    if ($destDir) {
        Ensure-Directory $destDir
    }
    if ($DryRun) {
        Write-Info "[dry-run] write json: $Destination"
        return
    }
    $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Destination -Encoding UTF8
}

function Get-RelativeRepoPath([string]$Path) {
    $normalizedRoot = $repoRoot.TrimEnd('\') + '\'
    if ($Path.StartsWith($normalizedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return $Path.Substring($normalizedRoot.Length)
    }
    return $Path
}

function Resolve-OutputBasePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Assert-OutsideRepoRoot([string]$Path) {
    $repoRootFull = [System.IO.Path]::GetFullPath($repoRoot)
    $targetFull = [System.IO.Path]::GetFullPath($Path)
    $comparison = [System.StringComparison]::OrdinalIgnoreCase
    $repoPrefix = $repoRootFull.TrimEnd('\') + '\'
    if ($targetFull.Equals($repoRootFull, $comparison) -or $targetFull.StartsWith($repoPrefix, $comparison)) {
        throw "Release set output must be outside the repository root: $targetFull"
    }
}

function Convert-ToSafeLabel([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }
    return (($Value -replace '[^0-9A-Za-z._-]+', '_').Trim('_'))
}

function Get-AppVersionLabel {
    $versionFile = Join-Path $repoRoot "APP_VERSION.txt"
    if (-not (Test-Path -LiteralPath $versionFile)) {
        return ""
    }
    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    return (Convert-ToSafeLabel -Value $version)
}

function New-ReleaseSetFolderName([string]$Prefix) {
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    $version = Get-AppVersionLabel
    if ([string]::IsNullOrWhiteSpace($version)) {
        return "${Prefix}_${stamp}"
    }
    return "${Prefix}_${version}_${stamp}"
}

function Move-ItemStrict([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing path to move: $Source"
    }
    if (Test-Path -LiteralPath $Destination) {
        throw "Destination already exists: $Destination"
    }
    $destDir = Split-Path -Parent $Destination
    if ($destDir) {
        Ensure-Directory $destDir
    }
    if ($DryRun) {
        Write-Info "[dry-run] move: $Source -> $Destination"
        return
    }
    Move-Item -LiteralPath $Source -Destination $Destination
}

function Remove-DirectoryIfEmpty([string]$Path) {
    if ($DryRun) {
        return
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if ((Get-ChildItem -LiteralPath $Path -Force | Measure-Object).Count -eq 0) {
        Remove-Item -LiteralPath $Path
    }
}

function Assert-ReleaseSetManifestComponents([string]$SetRoot, [object]$Components) {
    foreach ($property in $Components.PSObject.Properties) {
        $relativePath = [string]$property.Value
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            continue
        }
        $componentPath = Join-Path $SetRoot $relativePath
        if (-not (Test-Path -LiteralPath $componentPath)) {
            throw "Release set manifest component '$($property.Name)' does not exist: $componentPath"
        }
    }
}

Push-Location -LiteralPath $repoRoot
try {
    $folderName = New-ReleaseSetFolderName -Prefix $NamePrefix
    $outBasePath = Resolve-OutputBasePath -Path $OutBaseDir
    $setRoot = [System.IO.Path]::GetFullPath((Join-Path $outBasePath $folderName))
    Assert-OutsideRepoRoot -Path $setRoot
    $publicSnapshotDir = Join-Path $setRoot "public_snapshot"
    $setManifestPath = Join-Path $setRoot "release_set_manifest.json"
    $stagingBaseDir = Join-Path $setRoot "_staging_release"
    $stagingBaseRel = Get-RelativeRepoPath -Path $stagingBaseDir

    $releaseNotesSource = ""
    $releaseNotesTarget = ""
    $releaseComponentName = $null
    $releaseLiteComponentName = $null
    $releaseZipComponentName = $null
    $releaseLiteZipComponentName = $null
    if (-not [string]::IsNullOrWhiteSpace($ReleaseNotesPath)) {
        $resolvedNotes = Resolve-Path -LiteralPath $ReleaseNotesPath -ErrorAction Stop
        $releaseNotesSource = $resolvedNotes.Path
        $notesExtension = [System.IO.Path]::GetExtension($releaseNotesSource)
        if ([string]::IsNullOrWhiteSpace($notesExtension)) {
            $notesExtension = ".txt"
        }
        $releaseNotesTarget = Join-Path $setRoot ("RELEASE_NOTES" + $notesExtension)
    }

    Write-Info "Release set output: $setRoot"
    Ensure-Directory $setRoot

    if (-not $SnapshotOnly) {
        $packScript = Join-Path $scriptRoot "pack_release.ps1"
        if (-not (Test-Path -LiteralPath $packScript)) {
            throw "Missing pack script: $packScript"
        }

        # Build Full Version
        $packArgsFull = @{
            OutBaseDir = $stagingBaseRel
            NamePrefix = "release"
            Checksums = $Checksums
        }
        if ($Zip) { $packArgsFull["Zip"] = $true }
        if ($IncludeWorkspace) { $packArgsFull["IncludeWorkspace"] = $true }
        if (-not [string]::IsNullOrWhiteSpace($WorkspacePath)) { $packArgsFull["WorkspacePath"] = $WorkspacePath }
        if ($NoSetupJson) { $packArgsFull["NoSetupJson"] = $true }
        if ($NoSampleWorkspace) { $packArgsFull["NoSampleWorkspace"] = $true }
        if (-not [string]::IsNullOrWhiteSpace($LibreOfficeRuntimePath)) { $packArgsFull["LibreOfficeRuntimePath"] = $LibreOfficeRuntimePath }
        if ($SkipFreshnessCheck) { $packArgsFull["SkipFreshnessCheck"] = $true }
        if ($DryRun) { $packArgsFull["DryRun"] = $true }

        Write-Info "Packing Full version..."
        & $packScript @packArgsFull
        if (-not $?) {
            throw "pack_release.ps1 (Full) failed."
        }

        # Build Lite Version
        $packArgsLite = $packArgsFull.Clone()
        $packArgsLite["Lite"] = $true

        Write-Info "Packing Lite version..."
        & $packScript @packArgsLite
        if (-not $?) {
            throw "pack_release.ps1 (Lite) failed."
        }

        if ($DryRun) {
            Write-Info "[dry-run] finalize staged releases into: $setRoot"
        }
        else {
            $stagedDirs = @(Get-ChildItem -LiteralPath $stagingBaseDir -Directory -Force)
            $fullStagedDirs = @($stagedDirs | Where-Object {
                $_.Name -like "release_*" -and $_.Name -notlike "release_Lite_*"
            })
            $liteStagedDirs = @($stagedDirs | Where-Object { $_.Name -like "release_Lite_*" })
            if ($fullStagedDirs.Count -ne 1 -or $liteStagedDirs.Count -ne 1 -or $stagedDirs.Count -ne 2) {
                throw "Expected exactly one Full and one Lite staged release directory under $stagingBaseDir."
            }

            $fullStagedDir = $fullStagedDirs[0]
            $liteStagedDir = $liteStagedDirs[0]
            $releaseComponentName = $fullStagedDir.Name
            $releaseLiteComponentName = $liteStagedDir.Name
            Move-ItemStrict -Source $fullStagedDir.FullName -Destination (Join-Path $setRoot $releaseComponentName)
            Move-ItemStrict -Source $liteStagedDir.FullName -Destination (Join-Path $setRoot $releaseLiteComponentName)

            if ($Zip) {
                $stagedZips = @(Get-ChildItem -LiteralPath $stagingBaseDir -File -Force | Where-Object { $_.Extension -ieq ".zip" })
                $expectedZipNames = @(
                    ($releaseComponentName + ".zip"),
                    ($releaseLiteComponentName + ".zip")
                )
                $unexpectedZips = @($stagedZips | Where-Object { $_.Name -notin $expectedZipNames })
                if ($stagedZips.Count -ne 2 -or $unexpectedZips.Count -ne 0) {
                    throw "Expected ZIP files for the Full and Lite staged releases under $stagingBaseDir."
                }
                $releaseZipComponentName = $expectedZipNames[0]
                $releaseLiteZipComponentName = $expectedZipNames[1]
                foreach ($zipName in $expectedZipNames) {
                    $stagedZipPath = Join-Path $stagingBaseDir $zipName
                    Move-ItemStrict -Source $stagedZipPath -Destination (Join-Path $setRoot $zipName)
                }
            }
            Remove-DirectoryIfEmpty -Path $stagingBaseDir
        }
    }

    $snapshotScript = Join-Path $scriptRoot "export_public_snapshot.ps1"
    if (-not (Test-Path -LiteralPath $snapshotScript)) {
        throw "Missing public snapshot entry script: $snapshotScript"
    }
    $snapshotArgs = @("--dest", $publicSnapshotDir)
    if (-not [string]::IsNullOrWhiteSpace($PublicAllowlist)) {
        $snapshotArgs += @("--allowlist", $PublicAllowlist)
    }
    if (-not [string]::IsNullOrWhiteSpace($PublicGitignoreTemplate)) {
        $snapshotArgs += @("--gitignore-template", $PublicGitignoreTemplate)
    }
    if ($DryRun) {
        $snapshotArgs += "--dry-run"
    }
    & $snapshotScript @snapshotArgs
    if (-not $?) {
        $exitCode = 1
        if (Test-Path -LiteralPath variable:LASTEXITCODE) {
            $exitCode = $LASTEXITCODE
        }
        throw "export_public_snapshot.ps1 failed with exit code $exitCode"
    }

    if ($releaseNotesTarget) {
        Copy-FileStrict -Source $releaseNotesSource -Destination $releaseNotesTarget
    }

    $manifest = [PSCustomObject]@{
        created_at = (Get-Date).ToString("o")
        app_version = (Get-AppVersionLabel)
        name = $folderName
        components = [PSCustomObject]@{
            release = $releaseComponentName
            release_lite = $releaseLiteComponentName
            public_snapshot = "public_snapshot"
            release_zip = $releaseZipComponentName
            release_lite_zip = $releaseLiteZipComponentName
            release_notes = $(if ($releaseNotesTarget) { [System.IO.Path]::GetFileName($releaseNotesTarget) } else { $null })
        }
        commands = [PSCustomObject]@{
            pack_release = "./pack_release.ps1"
            export_public_snapshot = "./export_public_snapshot.ps1"
        }
    }
    Write-JsonFile -Destination $setManifestPath -Value $manifest
    if (-not $DryRun) {
        Assert-ReleaseSetManifestComponents -SetRoot $setRoot -Components $manifest.components
    }

    Write-Info "Done."
}
finally {
    Pop-Location
}
