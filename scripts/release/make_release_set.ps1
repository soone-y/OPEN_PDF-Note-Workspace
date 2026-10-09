[CmdletBinding()]
param(
    [string]$OutBaseDir = "..\\PDF-Note-ReleaseSet",
    [string]$NamePrefix = "pdf_note_workspace_release_set",
    [string]$ReleaseSetName = "",
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
    [string]$PublicSnapshotSource = "",
    [switch]$SnapshotOnly,
    [switch]$DryRun,
    [switch]$Lite,
    [switch]$DeferPostCreationValidation,
    [ValidateSet("ja", "en")]
    [string]$Locale = "ja"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Lite) {
    throw "release set は通常版と Lite版を一組で作成するため、-Lite は指定できません。Lite版だけの開発用梱包は scripts\\release\\pack_release.ps1 -Lite を使用してください。"
}

$scriptRoot = $PSScriptRoot
if (-not $scriptRoot) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$scriptsRoot = Split-Path -Parent $scriptRoot
$repoRoot = Split-Path -Parent $scriptsRoot
if (-not $repoRoot) {
    $repoRoot = $scriptRoot
}
$script:releaseDetailLogPath = ""

function Write-Info([string]$Message) { Write-Host $Message -ForegroundColor Cyan }

function Initialize-ReleaseDetailLog([string]$SetRoot) {
    $logDirectory = Join-Path $repoRoot "out\logs\release_set"
    $logName = "release_set_{0}_{1}.log" -f $Locale, (Get-Date -Format "yyyyMMdd_HHmmss")
    $candidatePath = Join-Path $logDirectory $logName
    try {
        New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
        New-Item -ItemType File -Force -Path $candidatePath | Out-Null
        $script:releaseDetailLogPath = $candidatePath
        Add-Content -LiteralPath $script:releaseDetailLogPath -Encoding UTF8 -Value @(
            "== Release Set ==",
            ("started: {0}" -f (Get-Date).ToString("o")),
            ("locale: {0}" -f $Locale),
            ("release_set: {0}" -f $SetRoot),
            ""
        )
        Write-Info "Release set detail log: $script:releaseDetailLogPath"
    }
    catch {
        $script:releaseDetailLogPath = ""
        Write-Warning "release set 詳細ログを作成できませんでした。処理は継続します: $($_.Exception.Message)"
    }
}

function Append-ReleaseDetailLog([AllowNull()]$Line) {
    if ([string]::IsNullOrWhiteSpace($script:releaseDetailLogPath)) {
        return
    }
    try {
        Add-Content -LiteralPath $script:releaseDetailLogPath -Encoding UTF8 -Value $(if ($null -eq $Line) { "" } else { [string]$Line })
    }
    catch {
        Write-Warning "release set 詳細ログへ書き込めませんでした。処理は継続します: $($_.Exception.Message)"
        $script:releaseDetailLogPath = ""
    }
}

function Write-ReleaseFailureLogTail {
    if ([string]::IsNullOrWhiteSpace($script:releaseDetailLogPath) -or -not (Test-Path -LiteralPath $script:releaseDetailLogPath)) {
        return
    }
    $tailCount = 80
    Write-Host "release set の詳細ログ: $script:releaseDetailLogPath" -ForegroundColor Yellow
    Write-Host "---- release set failure log tail ($tailCount lines) ----" -ForegroundColor Yellow
    Get-Content -LiteralPath $script:releaseDetailLogPath -Tail $tailCount | ForEach-Object { Write-Host $_ }
    Write-Host "---- end release set failure log tail ----" -ForegroundColor Yellow
}

function Invoke-ReleasePythonGate {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$ScriptPath,
        [string[]]$Arguments = @()
    )

    if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
        throw "$Name が見つかりません: $ScriptPath"
    }
    Append-ReleaseDetailLog ("> python {0} {1}" -f $ScriptPath, ($Arguments -join " "))
    $savedErrorActionPreference = $ErrorActionPreference
    $restoreNativeCommandErrorPreference = $false
    $previousNativeCommandErrorPreference = $false
    if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $restoreNativeCommandErrorPreference = $true
        $previousNativeCommandErrorPreference = $PSNativeCommandUseErrorActionPreference
        $PSNativeCommandUseErrorActionPreference = $false
    }
    $exitCode = 0
    try {
        $ErrorActionPreference = "Continue"
        if ([string]::IsNullOrWhiteSpace($script:releaseDetailLogPath)) {
            & python $ScriptPath @Arguments
        }
        else {
            & python $ScriptPath @Arguments 2>&1 |
                ForEach-Object {
                    Write-Host $_
                    $_
                } |
                Out-File -LiteralPath $script:releaseDetailLogPath -Append -Encoding UTF8 -Width 4096
        }
        $exitCode = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
        if ($restoreNativeCommandErrorPreference) {
            $PSNativeCommandUseErrorActionPreference = $previousNativeCommandErrorPreference
        }
    }
    if ($exitCode -ne 0) {
        throw "$Name に失敗しました（終了コード: $exitCode）。"
    }
}

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

function Copy-DirectoryStrict([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Public snapshot source directory is missing: $Source"
    }
    $sourceFull = (Resolve-Path -LiteralPath $Source -ErrorAction Stop).Path.TrimEnd('\')
    $destinationFull = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\')
    if ($sourceFull.Equals($destinationFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Public snapshot source and destination must differ: $sourceFull"
    }
    if (Test-Path -LiteralPath $Destination) {
        throw "Public snapshot destination already exists: $Destination"
    }
    if ($DryRun) {
        Write-Info "[dry-run] copy public snapshot: $sourceFull -> $destinationFull"
        return
    }
    $destinationParent = Split-Path -Parent $destinationFull
    Ensure-Directory $destinationParent
    Copy-Item -LiteralPath $sourceFull -Destination $destinationFull -Recurse -Force -ErrorAction Stop
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

function Get-RepoVersionLabel {
    $versionFile = Join-Path $repoRoot "REPO_VERSION.txt"
    if (-not (Test-Path -LiteralPath $versionFile)) {
        return ""
    }
    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    return (Convert-ToSafeLabel -Value $version)
}

function New-ReleaseSetFolderName([string]$Prefix, [string]$Locale) {
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    $version = Get-RepoVersionLabel
    if ([string]::IsNullOrWhiteSpace($version)) {
        return "${Prefix}_${Locale}_${stamp}"
    }
    return "${Prefix}_${version}_${Locale}_${stamp}"
}

function Get-DistributionZipName([string]$Version, [ValidateSet("ja", "en")][string]$Locale, [ValidateSet("full", "lite")][string]$Edition) {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        throw "配布 ZIP 名に必要な版番号を取得できません。REPO_VERSION.txt を確認してください。"
    }
    return "pdf_note_workspace_${Version}_${Locale}_${Edition}.zip"
}

function Get-DistributionArchiveRootName([string]$Version) {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        throw "配布 ZIP 内のルート名に必要な版番号を取得できません。REPO_VERSION.txt を確認してください。"
    }
    return "PDF-Note-Workspace-$Version"
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
    try {
        Move-Item -LiteralPath $Source -Destination $Destination -ErrorAction Stop
    }
    catch {
        # Windows のプロセス排他ロック等の場合、Copy + Remove で安全フォールバック
        Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
        Remove-Item -LiteralPath $Source -Recurse -Force -ErrorAction SilentlyContinue
    }
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

function Assert-ReleaseSetZipRoots([object]$ZipRoots) {
    if ($null -eq $ZipRoots) {
        throw "Release set manifest ZIP root metadata is missing."
    }
    foreach ($property in $ZipRoots.PSObject.Properties) {
        if ([string]::IsNullOrWhiteSpace([string]$property.Value)) {
            throw "Release set manifest ZIP root '$($property.Name)' is empty."
        }
    }
}

Push-Location -LiteralPath $repoRoot
try {
    $folderName = if ([string]::IsNullOrWhiteSpace($ReleaseSetName)) {
        New-ReleaseSetFolderName -Prefix $NamePrefix -Locale $Locale
    }
    else {
        Convert-ToSafeLabel -Value $ReleaseSetName
    }
    if ([string]::IsNullOrWhiteSpace($folderName)) {
        throw "Release set directory name is empty or invalid."
    }
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
    $distributionArchiveRootName = Get-DistributionArchiveRootName -Version (Get-RepoVersionLabel)
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
    Initialize-ReleaseDetailLog -SetRoot $setRoot

    if (-not $SnapshotOnly) {
        $packScript = Join-Path $scriptRoot "pack_release.ps1"
        if (-not (Test-Path -LiteralPath $packScript)) {
            throw "Missing pack script: $packScript"
        }

        $packArgsBase = @{
            OutBaseDir = $stagingBaseRel
            NamePrefix = "release"
            Checksums = $Checksums
            Locale = $Locale
        }
        if ($Zip) { $packArgsBase["Zip"] = $true }
        if ($IncludeWorkspace) { $packArgsBase["IncludeWorkspace"] = $true }
        if (-not [string]::IsNullOrWhiteSpace($WorkspacePath)) { $packArgsBase["WorkspacePath"] = $WorkspacePath }
        if ($NoSetupJson) { $packArgsBase["NoSetupJson"] = $true }
        if ($NoSampleWorkspace) { $packArgsBase["NoSampleWorkspace"] = $true }
        if (-not [string]::IsNullOrWhiteSpace($LibreOfficeRuntimePath)) { $packArgsBase["LibreOfficeRuntimePath"] = $LibreOfficeRuntimePath }
        if ($SkipFreshnessCheck) { $packArgsBase["SkipFreshnessCheck"] = $true }
        if ($DryRun) { $packArgsBase["DryRun"] = $true }

        if (-not $Lite) {
            # Build Full Version
            Write-Info "Packing Full version..."
            & $packScript @packArgsBase
            if (-not $?) {
                throw "pack_release.ps1 (Full) failed."
            }
        }

        # Build Lite Version
        $packArgsLite = $packArgsBase.Clone()
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
            
            if ($Lite) {
                if ($liteStagedDirs.Count -ne 1 -or $stagedDirs.Count -ne 1) {
                    throw "Expected exactly one Lite staged release directory under $stagingBaseDir."
                }
            } else {
                if ($fullStagedDirs.Count -ne 1 -or $liteStagedDirs.Count -ne 1 -or $stagedDirs.Count -ne 2) {
                    throw "Expected exactly one Full and one Lite staged release directory under $stagingBaseDir."
                }
            }

            if (-not $Lite) {
                $fullStagedDir = $fullStagedDirs[0]
                $releaseComponentName = $fullStagedDir.Name
                Move-ItemStrict -Source $fullStagedDir.FullName -Destination (Join-Path $setRoot $releaseComponentName)
            }
            $liteStagedDir = $liteStagedDirs[0]
            $releaseLiteComponentName = $liteStagedDir.Name
            Move-ItemStrict -Source $liteStagedDir.FullName -Destination (Join-Path $setRoot $releaseLiteComponentName)

            if ($Zip) {
                $stagedZips = @(Get-ChildItem -LiteralPath $stagingBaseDir -File -Force | Where-Object { $_.Extension -ieq ".zip" })
                $stagedZipNames = @()
                $destinationZipNames = @()
                if ($Lite) {
                    $stagedZipNames = @(($releaseLiteComponentName + ".zip"))
                    $releaseZipComponentName = $null
                    $releaseLiteZipComponentName = Get-DistributionZipName -Version (Get-RepoVersionLabel) -Locale $Locale -Edition "lite"
                    $destinationZipNames = @($releaseLiteZipComponentName)
                } else {
                    $stagedZipNames = @(
                        ($releaseComponentName + ".zip"),
                        ($releaseLiteComponentName + ".zip")
                    )
                    $releaseZipComponentName = Get-DistributionZipName -Version (Get-RepoVersionLabel) -Locale $Locale -Edition "full"
                    $releaseLiteZipComponentName = Get-DistributionZipName -Version (Get-RepoVersionLabel) -Locale $Locale -Edition "lite"
                    $destinationZipNames = @($releaseZipComponentName, $releaseLiteZipComponentName)
                }
                $unexpectedZips = @($stagedZips | Where-Object { $_.Name -notin $stagedZipNames })
                if ($stagedZips.Count -ne $stagedZipNames.Count -or $unexpectedZips.Count -ne 0) {
                    throw "Expected ZIP files for staged releases under $stagingBaseDir."
                }
                for ($index = 0; $index -lt $stagedZipNames.Count; $index++) {
                    $stagedZipPath = Join-Path $stagingBaseDir $stagedZipNames[$index]
                    Move-ItemStrict -Source $stagedZipPath -Destination (Join-Path $setRoot $destinationZipNames[$index])
                }
            }
            Remove-DirectoryIfEmpty -Path $stagingBaseDir
        }
    }

    $snapshotScript = Join-Path $scriptRoot "export_public_snapshot.ps1"
    if (-not (Test-Path -LiteralPath $snapshotScript)) { throw "Missing public snapshot entry script: $snapshotScript" }
    $snapshotArgs = @("--dest", $publicSnapshotDir)
    $releasePublicAllowlist = Join-Path $repoRoot "docs\internal\public_repo_release_allowlist_2026-08-24.txt"
    $releaseArtifactManifest = Join-Path $repoRoot "docs\internal\public_repo_release_artifact_manifest_2026-08-12.tsv"
    if ([string]::IsNullOrWhiteSpace($PublicAllowlist)) {
        if (-not (Test-Path -LiteralPath $releasePublicAllowlist -PathType Leaf)) {
            throw "Missing release public allowlist: $releasePublicAllowlist"
        }
        $snapshotArgs += @("--allowlist", $releasePublicAllowlist)
    }
    else {
        $snapshotArgs += @("--allowlist", $PublicAllowlist)
    }
    if (-not (Test-Path -LiteralPath $releaseArtifactManifest -PathType Leaf)) {
        throw "Missing release public artifact manifest: $releaseArtifactManifest"
    }
    $snapshotArgs += @("--artifact-manifest", $releaseArtifactManifest)
    if (-not [string]::IsNullOrWhiteSpace($PublicGitignoreTemplate)) { $snapshotArgs += @("--gitignore-template", $PublicGitignoreTemplate) }
    if ([string]::IsNullOrWhiteSpace($PublicSnapshotSource)) {
        if ($DryRun) { $snapshotArgs += "--dry-run" }
        & $snapshotScript @snapshotArgs
        if (-not $?) { throw "export_public_snapshot.ps1 failed with exit code $LASTEXITCODE" }
    }
    else {
        Write-Info "Reusing the frozen public snapshot: $PublicSnapshotSource"
        Copy-DirectoryStrict -Source $PublicSnapshotSource -Destination $publicSnapshotDir
    }

    if (-not $DryRun) {
        # Release invariant: every public file submitted by publish.ps1 must be
        # generated and frozen in this release set before its manifest and
        # confirmation word are created. Submit must never transform snapshot content.
        $snapshotReadme = Join-Path $publicSnapshotDir "README.md"
        $pagesBuildScript = Join-Path $publicSnapshotDir "site\github\scripts\build_public_site.py"
        $pagesValidationScript = Join-Path $publicSnapshotDir "site\github\scripts\validate_public_site.py"
        foreach ($requiredPath in @($snapshotReadme, $pagesBuildScript, $pagesValidationScript)) {
            if (-not (Test-Path -LiteralPath $requiredPath)) { throw "Missing release snapshot preparation input: $requiredPath" }
        }
        # The snapshot exporter excludes Python caches.  Keep that invariant while
        # generating frozen Pages output inside the snapshot: bytecode is a local
        # runtime artifact and can otherwise differ between the ja/en creations.
        # A JA/EN pair reuses the already frozen JA snapshot verbatim. Rebuilding
        # Pages after that copy would duplicate work and could make the two sets
        # differ; validate the copied output instead.
        if ([string]::IsNullOrWhiteSpace($PublicSnapshotSource)) {
            & python -B $pagesBuildScript --replace --documentation-portal
            if ($LASTEXITCODE -ne 0) { throw "GitHub Pages snapshot build failed." }
        }
        if (-not $DeferPostCreationValidation) {
            $pagesOutput = Join-Path $publicSnapshotDir "site\github\output\public"
            & python -B $pagesValidationScript --site $pagesOutput
            if ($LASTEXITCODE -ne 0) { throw "GitHub Pages snapshot validation failed." }
        }
        $snapshotContentGateScript = Join-Path $repoRoot "tools\release_checks\public_snapshot_content_gate.py"
        Invoke-ReleasePythonGate -Name "公開snapshot内容検査" -ScriptPath $snapshotContentGateScript -Arguments @("--snapshot", $publicSnapshotDir)
    }

    if ($releaseNotesTarget) {
        Copy-FileStrict -Source $releaseNotesSource -Destination $releaseNotesTarget
    }

    $manifest = [PSCustomObject]@{
        created_at = (Get-Date).ToString("o")
        app_version = (Get-RepoVersionLabel)
        locale = $Locale
        name = $folderName
        components = [PSCustomObject]@{
            release = $releaseComponentName
            release_lite = $releaseLiteComponentName
            public_snapshot = "public_snapshot"
            release_zip = $releaseZipComponentName
            release_lite_zip = $releaseLiteZipComponentName
            release_notes = $(if ($releaseNotesTarget) { [System.IO.Path]::GetFileName($releaseNotesTarget) } else { $null })
        }
        zip_roots = [PSCustomObject]@{
            release_zip = $distributionArchiveRootName
            release_lite_zip = $distributionArchiveRootName
        }
        commands = [PSCustomObject]@{
            pack_release = "./pack_release.ps1"
        }
    }
    Write-JsonFile -Destination $setManifestPath -Value $manifest
    if (-not $DryRun) {
        Assert-ReleaseSetManifestComponents -SetRoot $setRoot -Components $manifest.components
        Assert-ReleaseSetZipRoots -ZipRoots $manifest.zip_roots
        $integrityGateScript = Join-Path $repoRoot "tools\release_checks\release_set_integrity_gate.py"
        $allowlistForManifest = if ([string]::IsNullOrWhiteSpace($PublicAllowlist)) { $releasePublicAllowlist } else { $PublicAllowlist }
        $integrityArgs = @(
            $integrityGateScript, "--release-set", $setRoot,
            "--write-snapshot-manifest", "--allowlist", $allowlistForManifest,
            "--artifact-manifest", $releaseArtifactManifest
        )
        if ($DeferPostCreationValidation) { $integrityArgs += "--skip-validation-after-write" }
        Invoke-ReleasePythonGate -Name "release set 整合性検査" -ScriptPath $integrityGateScript -Arguments $integrityArgs[1..($integrityArgs.Count - 1)]
        if (-not $DeferPostCreationValidation -and -not $SnapshotOnly -and -not $Lite) {
            $licenseGateScript = Join-Path $repoRoot "tools\release_checks\release_license_gate.py"
            Invoke-ReleasePythonGate -Name "release ライセンス検査" -ScriptPath $licenseGateScript -Arguments @("--release-set", $setRoot)
            $textGateScript = Join-Path $repoRoot "tools\release_checks\release_text_gate.py"
            Invoke-ReleasePythonGate -Name "release テキスト検査" -ScriptPath $textGateScript -Arguments @("--release-set", $setRoot)
            $localeContentGateScript = Join-Path $repoRoot "tools\release_checks\release_locale_content_gate.py"
            Invoke-ReleasePythonGate -Name "release 言語別内容検査" -ScriptPath $localeContentGateScript -Arguments @("--release-set", $setRoot)
        }
        if ($DeferPostCreationValidation) {
            Write-Info "Release-set validation was deferred to the publish caller; this set must not be used until that validation passes."
        }
    }

    Write-Info "Done."
    # Machine-readable handoff for callers. Emit only after every requested
    # creation-side operation has completed successfully.
    Write-Output "RELEASE_SET_PATH=$setRoot"
}
catch {
    Append-ReleaseDetailLog ("error: {0}" -f $_.Exception.Message)
    Write-ReleaseFailureLogTail
    throw
}
finally {
    Pop-Location
}
