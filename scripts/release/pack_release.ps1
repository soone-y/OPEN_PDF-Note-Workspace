[CmdletBinding()]
param(
    [string]$OutBaseDir = "out/release",
    [string]$NamePrefix = "pdf_note_workspace",
    [switch]$Zip,
    [switch]$Checksums = $true,
    [switch]$NoChecksums,
    [switch]$IncludeWorkspace,
    [string]$WorkspacePath = "",
    [switch]$NoSetupJson,
    [switch]$NoSampleWorkspace,
    [switch]$NoLibreOfficeRuntime,
    [string]$LibreOfficeRuntimePath = "",
    [switch]$SkipFreshnessCheck,
    [switch]$DryRun,
    [switch]$TestOnly,
    [switch]$Lite,
    [ValidateSet("ja", "en")]
    [string]$Locale = "ja"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($NoLibreOfficeRuntime -and -not $Lite) {
    throw "A standard release must include the LibreOffice conversion runtime. Use -Lite for a conversion-free release."
}
if ($TestOnly -and $Zip) {
    throw "-TestOnly は非ZIP確認出力専用です。-Zip は指定できません。"
}
if ($NoChecksums -and -not $TestOnly) {
    throw "-NoChecksums は -TestOnly の内部確認出力だけで使用できます。"
}
if ($NoChecksums) {
    $Checksums = $false
}
$includeLibreOfficeRuntime = -not $Lite

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
function Write-Warn([string]$Message) { Write-Host $Message -ForegroundColor Yellow }

function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
        }
        finally {
            $sha.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-InputItems([string[]]$Paths, [string[]]$ExcludePaths = @()) {
    $excludeRoots = @(
        $ExcludePaths |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            ForEach-Object { [System.IO.Path]::GetFullPath($_).TrimEnd('\') }
    )
    $items = @()
    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }
        $entry = Get-Item -LiteralPath $path
        if ($entry.PSIsContainer) {
            $items += Get-ChildItem -LiteralPath $path -Recurse -File -Force
        }
        else {
            $items += $entry
        }
    }
    return @($items | Where-Object {
        $itemFullPath = [System.IO.Path]::GetFullPath($_.FullName)
        foreach ($excludeRoot in $excludeRoots) {
            if ($itemFullPath.Equals($excludeRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
                $itemFullPath.StartsWith($excludeRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
                return $false
            }
        }
        return $true
    })
}

function Assert-ArtifactFresh([string]$ArtifactPath, [string[]]$InputPaths, [string[]]$ExcludePaths = @()) {
    if (-not (Test-Path -LiteralPath $ArtifactPath)) {
        throw "Missing build artifact: $ArtifactPath"
    }

    $artifactTime = (Get-Item -LiteralPath $ArtifactPath).LastWriteTimeUtc
    foreach ($item in (Get-InputItems -Paths $InputPaths -ExcludePaths $ExcludePaths)) {
        if ($item.FullName -eq $ArtifactPath) {
            continue
        }
        if ($item.LastWriteTimeUtc -gt $artifactTime) {
            throw ("Build artifact is stale. Re-run build.ps1 (or scripts\build\build_workspace.ps1) before packaging. Newer input: {0}" -f $item.FullName)
        }
    }
}

function Assert-BuildInfoEdition([string]$BuildInfoPath, [string]$ExpectedEdition) {
    $content = Get-Content -LiteralPath $BuildInfoPath -Raw
    $expectedLine = "edition`t" + $ExpectedEdition.ToLowerInvariant()
    if ($content -notmatch ("(?m)^" + [regex]::Escape($expectedLine) + "\r?$")) {
        throw "Build info manifest edition does not match '$ExpectedEdition': $BuildInfoPath"
    }
}

function Assert-BuildInfoLocale([string]$BuildInfoPath, [string]$ExpectedLocale) {
    $content = Get-Content -LiteralPath $BuildInfoPath -Raw
    $expectedLine = "locale`t" + $ExpectedLocale
    if ($content -notmatch ("(?m)^" + [regex]::Escape($expectedLine) + "\r?$")) {
        throw "Build info manifest locale does not match '$ExpectedLocale': $BuildInfoPath"
    }
}

function Ensure-Directory([string]$Path) {
    if ($DryRun) {
        Write-Info "[dry-run] mkdir: $Path"
        return
    }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Copy-File([string]$Source, [string]$Dest) {
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing file: $Source"
    }
    $destDir = Split-Path -Parent $Dest
    if ($destDir) {
        Ensure-Directory $destDir
    }
    if ($DryRun) {
        Write-Info "[dry-run] copy: $Source -> $Dest"
        return
    }
    Copy-Item -Force -LiteralPath $Source -Destination $Dest
}

function Copy-DirectoryContents([string]$SourceDir, [string]$DestDir) {
    if (-not (Test-Path -LiteralPath $SourceDir)) {
        throw "Missing directory: $SourceDir"
    }
    if ($DryRun) {
        Write-Info "[dry-run] copy-dir: $SourceDir -> $DestDir"
        return
    }
    Ensure-Directory $DestDir
    Get-ChildItem -LiteralPath $SourceDir -Force | ForEach-Object {
        Copy-Item -Force -Recurse -LiteralPath $_.FullName -Destination (Join-Path $DestDir $_.Name)
    }
}

function Write-TextFile([string]$DestPath, [string]$Value, [string]$Encoding = "UTF8") {
    $destDir = Split-Path -Parent $DestPath
    if ($destDir) {
        Ensure-Directory $destDir
    }
    if ($DryRun) {
        Write-Info "[dry-run] write text: $DestPath"
        return
    }
    Set-Content -LiteralPath $DestPath -Value $Value -Encoding $Encoding
}

function Get-RepoVersion {
    $versionPath = Join-Path $repoRoot "REPO_VERSION.txt"
    if (-not (Test-Path -LiteralPath $versionPath)) {
        throw "Repo version file not found: $versionPath"
    }
    $version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($version)) {
        throw "Repo version file is empty: $versionPath"
    }
    return $version
}

function Get-DistributionArchiveRootName([string]$RepoVersion) {
    if ([string]::IsNullOrWhiteSpace($RepoVersion)) {
        throw "ZIP 内の配布ルート名に必要な版番号を取得できません。REPO_VERSION.txt を確認してください。"
    }
    return "PDF-Note-Workspace-$RepoVersion"
}

function New-DistributionZip([string]$SourceDir, [string]$DestinationZip, [string]$ArchiveRootName) {
    if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
        throw "ZIP 化する配布ディレクトリが見つかりません: $SourceDir"
    }
    if ($ArchiveRootName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw "ZIP 内の配布ルート名が不正です: $ArchiveRootName"
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sourceRoot = [System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\', '/')
    $temporaryZip = $DestinationZip + "." + [System.Guid]::NewGuid().ToString("N") + ".tmp"
    $archive = $null
    try {
        $archive = [System.IO.Compression.ZipFile]::Open($temporaryZip, [System.IO.Compression.ZipArchiveMode]::Create)
        $archive.CreateEntry($ArchiveRootName + "/") | Out-Null
        foreach ($file in @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\', '/').Replace('\', '/')
            if ([string]::IsNullOrWhiteSpace($relative) -or $relative.StartsWith("../") -or $relative.Contains("/../")) {
                throw "配布 ZIP の相対パスが不正です: $($file.FullName)"
            }
            $entry = $archive.CreateEntry(
                ($ArchiveRootName + "/" + $relative),
                [System.IO.Compression.CompressionLevel]::Optimal
            )
            $entry.LastWriteTime = $file.LastWriteTime
            $input = $null
            $output = $null
            try {
                $input = [System.IO.File]::OpenRead($file.FullName)
                $output = $entry.Open()
                $input.CopyTo($output)
            }
            finally {
                if ($output) { $output.Dispose() }
                if ($input) { $input.Dispose() }
            }
        }
        $archive.Dispose()
        $archive = $null
        [System.IO.File]::Move($temporaryZip, $DestinationZip)
    }
    catch {
        if ($archive) { $archive.Dispose() }
        if (Test-Path -LiteralPath $temporaryZip) {
            Remove-Item -Force -LiteralPath $temporaryZip
        }
        throw
    }
}

function Apply-RepoVersionMarkers([string]$DocsDir, [string]$RepoVersion) {
    $marker = "(ZIP配布物ではここにバージョンが記載されます)"
    if ($DryRun) {
        Write-Info "[dry-run] apply $marker in release documentation: $DocsDir"
        return
    }
    $documents = @(Get-ChildItem -LiteralPath $DocsDir -File -Filter "*.md")
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    foreach ($document in $documents) {
        $content = [System.IO.File]::ReadAllText($document.FullName, [System.Text.Encoding]::UTF8)
        $markerCount = ([regex]::Matches($content, [regex]::Escape($marker))).Count
        if ($markerCount -eq 0) {
            continue
        }
        if ($markerCount -ne 1) {
            throw "A release-version marker may appear at most once in a document: $($document.FullName)"
        }
        [System.IO.File]::WriteAllText($document.FullName, $content.Replace($marker, $RepoVersion), $utf8)
    }
}

function Copy-ReleaseTopDocuments([string]$ReleaseRoot, [string]$RepositoryRoot, [ValidateSet("ja", "en")][string]$Locale) {
    # Every distributable has the same bilingual top-level entry documents.
    # Locale-specific user documents are copied under docs/ before this runs.
    $sources = @(
        @{ Source = "README.md"; Destination = "README.md" },
        @{ Source = "LICENSE.md"; Destination = "LICENSE.md" },
        @{ Source = ".github\SECURITY.md"; Destination = "SECURITY.md" }
    )
    foreach ($entry in $sources) {
        $destination = Join-Path $ReleaseRoot $entry.Destination
        Copy-File -Source (Join-Path $RepositoryRoot $entry.Source) -Dest $destination
        if ($DryRun) {
            continue
        }
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        $content = [System.IO.File]::ReadAllText($destination, [System.Text.Encoding]::UTF8)
        # A package contains one selected docs tree as docs/. The common root
        # README links to the public documentation map and both source trees;
        # all three targets resolve to the selected documentation entry here.
        $content = $content.Replace("](DOCUMENTATION.md)", "](docs/README.md)")
        $content = $content.Replace("](docs/ja/README.md)", "](docs/README.md)")
        $content = $content.Replace("](docs/en/README.md)", "](docs/README.md)")
        $content = $content.Replace("](THIRD_PARTY_NOTICES.md)", "](docs/legal/THIRD_PARTY_NOTICES.md)")
        [System.IO.File]::WriteAllText($destination, $content, $utf8)
    }
}

function Write-ReleaseSetupJson([string]$DestPath, [string]$WorkspaceRootRelativePath) {
    $json = @(
        "{",
        ('  "workspaceRoot": "{0}",' -f $WorkspaceRootRelativePath.Replace('\', '/')),
        '  "workspaceRootMode": "relative",',
        '  "tempExternalLectureDirs": []',
        "}"
    ) -join "`r`n"

    $destDir = Split-Path -Parent $DestPath
    if ($destDir) {
        Ensure-Directory $destDir
    }
    if ($DryRun) {
        Write-Info "[dry-run] write sanitized setup: $DestPath"
        return
    }
    Set-Content -LiteralPath $DestPath -Value $json -Encoding UTF8
}

function Get-CommandPath([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) { return "" }
    if ($cmd.Source) { return $cmd.Source }
    return ""
}

function Try-Resolve-MinGwDll([string]$DllName) {
    $gpp = Get-CommandPath "g++"
    if (-not $gpp) { return "" }
    $toolchainBinDir = Split-Path -Parent $gpp
    $candidate = Join-Path $toolchainBinDir $DllName
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    return ""
}

function Get-RelativePathWithinRelease([string]$BaseDir, [string]$TargetPath) {
    $baseFull = [System.IO.Path]::GetFullPath($BaseDir).TrimEnd('\') + '\'
    $targetFull = [System.IO.Path]::GetFullPath($TargetPath)
    $baseUri = New-Object System.Uri($baseFull)
    $targetUri = New-Object System.Uri($targetFull)
    $relative = $baseUri.MakeRelativeUri($targetUri).ToString()
    return [System.Uri]::UnescapeDataString($relative).Replace('/', '\')
}

function Get-LibreOfficeRuntimeImageDirs([string]$ReleaseRoot) {
    if (-not (Test-Path -LiteralPath $ReleaseRoot)) {
        return @()
    }
    $runtimeMarkers = @(
        Get-ChildItem -LiteralPath $ReleaseRoot -Recurse -File -Force |
            Where-Object { $_.Name -ieq "soffice.com" -or $_.Name -ieq "mergedlo.dll" }
    )
    if ($runtimeMarkers.Count -eq 0) {
        return @()
    }

    $programDirs = @($runtimeMarkers | ForEach-Object { $_.DirectoryName } | Sort-Object -Unique)
    return @($programDirs | ForEach-Object { Split-Path -Parent $_ } | Sort-Object -Unique)
}

function Assert-NoProhibitedLibreOfficeRuntime([string]$ReleaseRoot) {
    $imageDirs = @(Get-LibreOfficeRuntimeImageDirs -ReleaseRoot $ReleaseRoot)
    if ($imageDirs.Count -eq 0) {
        Write-Info "LibreOffice executable conversion runtime is not included in this release."
        return
    }

    $python = Get-CommandPath "python"
    if (-not $python) {
        throw "python.exe is required to validate a bundled LibreOffice conversion runtime."
    }
    $gateScript = Join-Path $repoRoot "tools\release_checks\libreoffice_runtime_gate.py"
    if (-not (Test-Path -LiteralPath $gateScript)) {
        throw "LibreOffice runtime validation script not found: $gateScript"
    }

    foreach ($imageDir in $imageDirs) {
        & $python $gateScript "--image" $imageDir
        if ($LASTEXITCODE -ne 0) {
            throw "Release contains a LibreOffice runtime with prohibited safety indicators: $imageDir"
        }
    }
}

function Sanitize-LibreOfficeRuntimeForRelease([string]$ImageDir) {
    if ($DryRun) {
        Write-Info "[dry-run] sanitize LibreOffice release runtime: $ImageDir"
        return
    }

    $python = Get-CommandPath "python"
    if (-not $python) {
        throw "python.exe is required to sanitize a bundled LibreOffice conversion runtime."
    }
    $sanitizeScript = Join-Path $repoRoot "tools\release_checks\sanitize_libreoffice_runtime_release.py"
    if (-not (Test-Path -LiteralPath $sanitizeScript)) {
        throw "LibreOffice runtime sanitization script not found: $sanitizeScript"
    }

    & $python $sanitizeScript "--image" $ImageDir
    if ($LASTEXITCODE -ne 0) {
        throw "LibreOffice runtime sanitization failed: $ImageDir"
    }
}

function Copy-LibreOfficeRuntimeLicenseArtifacts([string]$ReleaseRoot, [string]$LicensesDir) {
    $imageDirs = @(Get-LibreOfficeRuntimeImageDirs -ReleaseRoot $ReleaseRoot)
    if ($imageDirs.Count -eq 0) {
        return
    }
    if ($imageDirs.Count -gt 1) {
        throw "Multiple LibreOffice runtime image directories found in release; license copying must be reviewed explicitly."
    }

    $imageDir = $imageDirs[0]
    foreach ($loDoc in @("license.txt", "LICENSE.html", "NOTICE")) {
        $loDocPath = Join-Path $imageDir $loDoc
        if (-not (Test-Path -LiteralPath $loDocPath)) {
            throw "Bundled LibreOffice runtime is missing required license/notice file: $loDocPath"
        }
        Copy-File -Source $loDocPath -Dest (Join-Path $LicensesDir ("libreoffice\" + $loDoc))
    }

    $customBuildDir = Join-Path $repoRoot "third_party\libreoffice\custom_build"
    if (Test-Path -LiteralPath $customBuildDir) {
        $customBuildLicenseDir = Join-Path $LicensesDir "libreoffice\custom_build"
        $options = Join-Path $customBuildDir "communication_free_options.input"
        if (Test-Path -LiteralPath $options) {
            Copy-File -Source $options -Dest (Join-Path $customBuildLicenseDir "communication_free_options.input")
        }
        $reductionManifest = Join-Path $customBuildDir "release_reduction_manifest.json"
        if (Test-Path -LiteralPath $reductionManifest) {
            Copy-File -Source $reductionManifest -Dest (Join-Path $customBuildLicenseDir "release_reduction_manifest.json")
        }
        $patchDir = Join-Path $customBuildDir "patches"
        if (Test-Path -LiteralPath $patchDir) {
            Copy-DirectoryContents -SourceDir $patchDir -DestDir (Join-Path $customBuildLicenseDir "patches")
        }
    }
}

function Copy-ReleaseSampleWorkspace([string]$SampleDest, [string]$Locale, [switch]$Lite) {
    $sampleSource = Join-Path $repoRoot ("release_assets\sample_workspace\" + $Locale)
    if (-not (Test-Path -LiteralPath $sampleSource)) {
        throw "Release sample workspace template not found: $sampleSource"
    }

    # Keep the course container in the package. Flattening its sessions into
    # sample_workspace hid the course -> session structure from users.
    $conversionSession = if ($Locale -eq "ja") {
        Join-Path $sampleSource "01_講義サンプル\第04回_Office変換"
    }
    else {
        Join-Path $sampleSource "01_Lecture_Samples\Session_04_Office_Conversion"
    }

    Ensure-Directory $SampleDest
    Get-ChildItem -LiteralPath $sampleSource -Recurse -File -Force | ForEach-Object {
        $sourceFile = $_.FullName
        if ($Lite -and ($sourceFile.Equals($conversionSession, [System.StringComparison]::OrdinalIgnoreCase) -or
                $sourceFile.StartsWith($conversionSession.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase))) {
            return
        }
        $relativePath = $sourceFile.Substring($sampleSource.TrimEnd('\').Length).TrimStart('\')
        Copy-File -Source $sourceFile -Dest (Join-Path $SampleDest $relativePath)
    }
}

function New-ReleaseFolderName([string]$Prefix) {
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    return "${Prefix}_${stamp}"
}

Push-Location -LiteralPath $repoRoot
try {
    $repoVersion = Get-RepoVersion
    $binSuffix = if ($Locale -eq "en") { "_en" } else { "" }
    $binDir = Join-Path $repoRoot ("out\bin" + $binSuffix)
    $appBinDir = if ($Lite) { Join-Path $repoRoot ("out\bin_lite" + $binSuffix) } else { $binDir }
    $exeName = "pdf_note_workspace.exe"
    $exePath = Join-Path $appBinDir $exeName
    $exeBuildInfoName = $exeName + ".buildinfo.txt"
    $exeBuildInfoPath = Join-Path $appBinDir $exeBuildInfoName
    $readOnlyViewerExeName = "readonly_viewer.exe"
    $readOnlyViewerExePath = Join-Path $binDir $readOnlyViewerExeName
    $readOnlyViewerBuildInfoName = $readOnlyViewerExeName + ".buildinfo.txt"
    $readOnlyViewerBuildInfoPath = Join-Path $binDir $readOnlyViewerBuildInfoName

    if (-not (Test-Path -LiteralPath $exePath)) {
        throw "Executable not found: $exePath"
    }
    if (-not (Test-Path -LiteralPath $readOnlyViewerExePath)) {
        throw "Read-only viewer executable not found: $readOnlyViewerExePath"
    }
    if (-not (Test-Path -LiteralPath $exeBuildInfoPath)) {
        throw "Build info manifest not found: $exeBuildInfoPath"
    }
    if (-not (Test-Path -LiteralPath $readOnlyViewerBuildInfoPath)) {
        throw "Read-only viewer build info manifest not found: $readOnlyViewerBuildInfoPath"
    }
    Assert-BuildInfoEdition -BuildInfoPath $exeBuildInfoPath -ExpectedEdition $(if ($Lite) { "Lite" } else { "Full" })
    Assert-BuildInfoLocale -BuildInfoPath $exeBuildInfoPath -ExpectedLocale $Locale
    Assert-BuildInfoLocale -BuildInfoPath $readOnlyViewerBuildInfoPath -ExpectedLocale $Locale
    if (-not $SkipFreshnessCheck) {
        Assert-ArtifactFresh -ArtifactPath $exePath -InputPaths @(
            (Join-Path $repoRoot "src"),
            (Join-Path $repoRoot "scripts\build\build_workspace.ps1"),
            (Join-Path $repoRoot "scripts\build\build_sources.json"),
            (Join-Path $repoRoot "locales")
        ) -ExcludePaths @(
            (Join-Path $repoRoot "src\readonly_viewer")
        )
        Assert-ArtifactFresh -ArtifactPath $readOnlyViewerExePath -InputPaths @(
            (Join-Path $repoRoot "src\readonly_viewer"),
            (Join-Path $repoRoot "src\theme"),
            (Join-Path $repoRoot "src\clrop"),
            (Join-Path $repoRoot "src\resources"),
            (Join-Path $repoRoot "scripts\build\build_readonly_viewer.ps1"),
            (Join-Path $repoRoot "scripts\build\build_sources.json")
        )
    }

    if ([System.IO.Path]::IsPathRooted($OutBaseDir)) {
        $outBase = [System.IO.Path]::GetFullPath($OutBaseDir)
    } else {
        $outBase = Join-Path $repoRoot $OutBaseDir
    }
    if ($Lite) {
        $NamePrefix = $NamePrefix + "_Lite"
    }
    $folderName = New-ReleaseFolderName -Prefix $NamePrefix
    $outDir = Join-Path $outBase $folderName
    $archiveRootName = Get-DistributionArchiveRootName -RepoVersion $repoVersion
    $docsDir = Join-Path $outDir "docs"
    $licensesDir = Join-Path $outDir "licenses"

    Write-Info "Release output: $outDir"
    Ensure-Directory $outDir
    Ensure-Directory $docsDir
    Ensure-Directory $licensesDir

    if ($TestOnly) {
        $testOnlyNotice = (@(
            "TEST ONLY - NOT A RELEASE SET OR DISTRIBUTABLE",
            "This unpacked output was created by release.ps1 -Test for local verification.",
            "Do not zip, publish, submit, or distribute this directory.",
            "Create a normal release set with release.ps1 before any publication."
        ) -join [Environment]::NewLine)
        Write-TextFile -DestPath (Join-Path $outDir "TEST_ONLY_DO_NOT_PUBLISH.txt") -Value $testOnlyNotice
    }

    Copy-File -Source $exePath -Dest (Join-Path $outDir $exeName)
    Copy-File -Source $readOnlyViewerExePath -Dest (Join-Path $outDir $readOnlyViewerExeName)
    Copy-File -Source $exeBuildInfoPath -Dest (Join-Path $outDir $exeBuildInfoName)
    Copy-File -Source $readOnlyViewerBuildInfoPath -Dest (Join-Path $outDir $readOnlyViewerBuildInfoName)

    $pdfiumBundled = Join-Path $repoRoot "third_party\pdfium\bin\pdfium.dll"
    if (-not (Test-Path -LiteralPath $pdfiumBundled -PathType Leaf)) {
        throw "Audited PDFium DLL not found: third_party/pdfium/bin/pdfium.dll"
    }
    Copy-File -Source $pdfiumBundled -Dest (Join-Path $outDir "pdfium.dll")

    $runtimeDlls = @(
        "libstdc++-6.dll",
        "libgcc_s_seh-1.dll",
        "libwinpthread-1.dll"
    )

    foreach ($dll in $runtimeDlls) {
        $dllFromBin = Join-Path $appBinDir $dll
        if (Test-Path -LiteralPath $dllFromBin) {
            Copy-File -Source $dllFromBin -Dest (Join-Path $outDir $dll)
            continue
        }

        $resolved = Try-Resolve-MinGwDll -DllName $dll
        if ($resolved) {
            Write-Warn "$dll not found in out/bin; using toolchain DLL: $resolved"
            Copy-File -Source $resolved -Dest (Join-Path $outDir $dll)
            continue
        }

        throw "Required runtime DLL not found: $dll (not in out/bin and not found near g++)."
    }

    $zlibBundled = Join-Path $repoRoot "third_party\zlib\bin\zlib1.dll"
    if (-not (Test-Path -LiteralPath $zlibBundled -PathType Leaf)) {
        throw "Audited zlib DLL not found: third_party/zlib/bin/zlib1.dll"
    }
    Copy-File -Source $zlibBundled -Dest (Join-Path $outDir "zlib1.dll")

    if ($includeLibreOfficeRuntime) {
        $loRuntime = $LibreOfficeRuntimePath
        if ([string]::IsNullOrWhiteSpace($loRuntime)) {
            $loRuntime = Join-Path $repoRoot "third_party\libreoffice\custom_runtime\instdir"
        }
        if (-not (Test-Path -LiteralPath $loRuntime)) {
            throw "Verified LibreOffice conversion runtime was not found: $loRuntime. Use -NoLibreOfficeRuntime only for a conversion-disabled release."
        }
        $soffice = Join-Path $loRuntime "program\soffice.com"
        if (-not (Test-Path -LiteralPath $soffice)) {
            throw "LibreOffice runtime is missing program\soffice.com: $loRuntime"
        }
        Write-Info "Including gated LibreOffice conversion runtime."
        # Keep the distributable layout shallow.  LibreOffice has long registry
        # component names, and the former nested runtime root could exceed the
        # legacy Windows extraction path limit under ordinary user folders.
        $releaseLoRuntime = Join-Path $outDir "lo"
        Copy-DirectoryContents -SourceDir $loRuntime -DestDir $releaseLoRuntime
        Sanitize-LibreOfficeRuntimeForRelease -ImageDir $releaseLoRuntime
    }

    $documentSource = Join-Path $repoRoot ("docs\" + $Locale)
    Copy-DirectoryContents -SourceDir $documentSource -DestDir $docsDir

    $releaseReadme = Join-Path $documentSource "README.md"
    if (Test-Path -LiteralPath $releaseReadme) {
        Copy-File -Source $releaseReadme -Dest (Join-Path $docsDir "README.md")
    }
    else {
        $repoReadme = Join-Path $repoRoot "README.md"
        if (Test-Path -LiteralPath $repoReadme) {
            Copy-File -Source $repoReadme -Dest (Join-Path $docsDir "README.md")
        }
    }
    Apply-RepoVersionMarkers -DocsDir $docsDir -RepoVersion $repoVersion
    Copy-ReleaseTopDocuments -ReleaseRoot $outDir -RepositoryRoot $repoRoot -Locale $Locale
    $docsContents = @(
        "Release documentation contents",
        "",
        "- README.md: canonical documentation for this release language.",
        "- legal/LICENSE.md: legal text for this release language.",
        "- legal/THIRD_PARTY_NOTICES.md: third-party notice for this release language.",
        "- Top-level README.md, LICENSE.md, and SECURITY.md are bilingual common documents."
    ) -join "`r`n"
    Write-TextFile -DestPath (Join-Path $docsDir "CONTENTS.txt") -Value $docsContents -Encoding UTF8

    $pdfiumLicDir = Join-Path $repoRoot "third_party\pdfium\licenses"
    if (Test-Path -LiteralPath $pdfiumLicDir) {
        Copy-DirectoryContents -SourceDir $pdfiumLicDir -DestDir (Join-Path $licensesDir "pdfium")
    }
    $pdfiumPackageLicense = Join-Path $repoRoot "third_party\pdfium\LICENSE"
    if (Test-Path -LiteralPath $pdfiumPackageLicense) {
        Copy-File -Source $pdfiumPackageLicense -Dest (Join-Path $licensesDir "pdfium\LICENSE")
    }

    $md4cLicense = Join-Path $repoRoot "third_party\md4c\LICENSE.md"
    if (Test-Path -LiteralPath $md4cLicense) {
        Copy-File -Source $md4cLicense -Dest (Join-Path $licensesDir "md4c\LICENSE.md")
    }
    else {
        Write-Warn "third_party/md4c/LICENSE.md not found; MD4C license may be missing from release."
    }

    $zlibLicense = Join-Path $repoRoot "third_party\zlib\LICENSE"
    if (Test-Path -LiteralPath $zlibLicense) {
        Copy-File -Source $zlibLicense -Dest (Join-Path $licensesDir "zlib\LICENSE")
    }
    else {
        Write-Warn "third_party/zlib/LICENSE not found; zlib1.dll license may be missing from release."
    }

    $mingwLicDir = Join-Path $repoRoot "third_party\mingw_runtime_licenses\mingw-w64"
    if (Test-Path -LiteralPath $mingwLicDir) {
        Copy-DirectoryContents -SourceDir $mingwLicDir -DestDir (Join-Path $licensesDir "mingw-w64")
    }
    else {
        Write-Warn "third_party/mingw_runtime_licenses/mingw-w64 not found; licenses for MinGW runtime DLLs may be missing."
    }

    $libreOfficeImageDir = Join-Path $repoRoot "third_party\libreoffice\image"
    $libreOfficeFontsDir = Join-Path $libreOfficeImageDir "Fonts"
    if (Test-Path -LiteralPath $libreOfficeFontsDir) {
        $privateFontDestDir = Join-Path $outDir "libreoffice\image\Fonts"
        $privateFontPatterns = @(
            "LiberationSans-*.ttf",
            "LiberationSerif-*.ttf",
            "LiberationMono-*.ttf",
            "Carlito-*.ttf",
            "Caladea-*.ttf",
            "opens___.ttf"
        )
        $copiedLibreOfficeFontCount = 0
        foreach ($pattern in $privateFontPatterns) {
            $fontFiles = @(Get-ChildItem -LiteralPath $libreOfficeFontsDir -File -Filter $pattern)
            foreach ($fontFile in $fontFiles) {
                Copy-File -Source $fontFile.FullName -Dest (Join-Path $privateFontDestDir $fontFile.Name)
                $copiedLibreOfficeFontCount++
            }
        }
        if ($copiedLibreOfficeFontCount -eq 0) {
            Write-Warn "No selected LibreOffice private font files were found for release."
        }
        foreach ($loDoc in @("license.txt", "LICENSE.html", "NOTICE")) {
            $loDocPath = Join-Path $libreOfficeImageDir $loDoc
            if (Test-Path -LiteralPath $loDocPath) {
                Copy-File -Source $loDocPath -Dest (Join-Path $licensesDir ("libreoffice\" + $loDoc))
            }
            else {
                Write-Warn "LibreOffice license/notice file not found: third_party/libreoffice/image/$loDoc"
            }
        }
    }
    else {
        Write-Warn "third_party/libreoffice/image/Fonts not found; bundled private fonts will be missing from release."
    }

    if ($IncludeWorkspace) {
        $ws = $WorkspacePath
        if ([string]::IsNullOrWhiteSpace($ws)) {
            $candidates = @(
                (Join-Path $repoRoot "workspace")
            )
            foreach ($candidate in $candidates) {
                if (Test-Path -LiteralPath $candidate) {
                    $ws = $candidate
                    break
                }
            }
        }
        if ([string]::IsNullOrWhiteSpace($ws) -or -not (Test-Path -LiteralPath $ws)) {
            throw "IncludeWorkspace is set, but no safe default workspace directory was found. Specify -WorkspacePath explicitly."
        }
        Copy-DirectoryContents -SourceDir $ws -DestDir (Join-Path $outDir "workspace")
    }

    if (-not $NoSampleWorkspace) {
        Copy-ReleaseSampleWorkspace -SampleDest (Join-Path $outDir "sample_workspace") -Locale $Locale -Lite:$Lite
    }

    if (-not $NoSetupJson) {
        $defaultWorkspacePath = ""
        if (-not $NoSampleWorkspace) {
            $defaultWorkspacePath = Join-Path $outDir "sample_workspace"
        }
        elseif ($IncludeWorkspace) {
            $defaultWorkspacePath = Join-Path $outDir "workspace"
        }

        if ([string]::IsNullOrWhiteSpace($defaultWorkspacePath)) {
            Write-Warn "No packaged workspace was included; skipping sanitized release setup JSON."
        }
        else {
            Write-Info "Writing sanitized release setup JSON."
            Write-ReleaseSetupJson `
                -DestPath (Join-Path $outDir "pdf_note_workspace_setup.json") `
                -WorkspaceRootRelativePath (Get-RelativePathWithinRelease -BaseDir $outDir -TargetPath $defaultWorkspacePath)
        }
    }

    Copy-LibreOfficeRuntimeLicenseArtifacts -ReleaseRoot $outDir -LicensesDir $licensesDir

    $licensesReadme = @(
        "Release license contents",
        "",
        "- pdfium/: package LICENSE plus PDFium and bundled component license texts.",
        "- md4c/: MIT license text for the vendored Markdown parser.",
        "- mingw-w64/: license texts for libstdc++-6.dll, libgcc_s_seh-1.dll, and libwinpthread-1.dll.",
        "- zlib/: zlib license text for the vendored zlib1.dll.",
        "- libreoffice/: LibreOffice license.txt, LICENSE.html, and NOTICE for bundled private fonts.",
        "  If this release includes the Office conversion runtime, these files are copied from that bundled runtime.",
        "  custom_build/ records the LibreOffice build options, reduction manifest, and patches when a runtime is bundled.",
        "",
        "Read docs/legal/THIRD_PARTY_NOTICES.md first for the summary."
    ) -join "`r`n"
    Write-TextFile -DestPath (Join-Path $licensesDir "README.txt") -Value $licensesReadme -Encoding UTF8

    if ($DryRun -and $includeLibreOfficeRuntime) {
        Write-Info "Dry-run: bundled LibreOffice runtime validation will run after copying in a real release."
    }
    else {
        Assert-NoProhibitedLibreOfficeRuntime -ReleaseRoot $outDir
    }

    if ($Checksums) {
        $manifestPath = Join-Path $outDir "manifest.json"
        $checksumsPath = Join-Path $outDir "checksums.sha256"
        if ($DryRun) {
            Write-Info "[dry-run] write: $manifestPath"
            Write-Info "[dry-run] write: $checksumsPath"
        }
        else {
            $files = Get-ChildItem -LiteralPath $outDir -Recurse -File -Force | Sort-Object FullName
            $entries = @()
            $lines = @()
            foreach ($f in $files) {
                $rel = $f.FullName.Substring($outDir.Length).TrimStart('\', '/')
                $hash = Get-Sha256 -Path $f.FullName
                $entries += [PSCustomObject]@{
                    path = $rel
                    sha256 = $hash
                    bytes = $f.Length
                }
                $lines += ("{0}  {1}" -f $hash, $rel.Replace('\', '/'))
            }

            $manifest = [PSCustomObject]@{
                created_at = (Get-Date).ToString("o")
                name = $folderName
                locale = $Locale
                files = $entries
            }

            $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
            $lines | Set-Content -LiteralPath $checksumsPath -Encoding UTF8
        }
    }

    if ($Zip) {
        $zipPath = Join-Path $outBase ($folderName + ".zip")
        if ($DryRun) {
            Write-Info "[dry-run] zip: $outDir -> $zipPath"
            Write-Info "[dry-run] ZIP root: $archiveRootName"
        }
        else {
            if (Test-Path -LiteralPath $zipPath) {
                throw "ZIP output already exists and will not be overwritten: $zipPath"
            }
            New-DistributionZip -SourceDir $outDir -DestinationZip $zipPath -ArchiveRootName $archiveRootName
        }
    }

    Write-Info "Done."
    Write-Info "Tip: share the folder/zip together with checksums.sha256 for verification."
}
finally {
    Pop-Location
}

