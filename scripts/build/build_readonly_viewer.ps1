[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$Clean,
    [switch]$VerboseOutput,
    [string[]]$Files,
    [int]$FailureTailLines = 80
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptStart = Get-Date
$scriptRoot = $PSScriptRoot
if (-not $scriptRoot) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$scriptsRoot = Split-Path -Parent $scriptRoot
$repoRoot = Split-Path -Parent $scriptsRoot
if (-not $repoRoot) {
    $repoRoot = $scriptRoot
}

function Test-ContainsNonAscii {
    param([Parameter(Mandatory)][string]$Value)

    return $Value -match '[^\x00-\x7F]'
}

function Get-PowerShellExePath {
    $cmd = Get-Command powershell -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        return $cmd.Source
    }
    return (Join-Path $PSHOME "powershell.exe")
}

function Invoke-BuildViaAsciiJunction {
    param(
        [Parameter(Mandatory)][string]$RepoRootPath,
        [Parameter(Mandatory)][string]$CurrentScriptPath,
        [Parameter(Mandatory)][hashtable]$ForwardedParameters
    )

    $relayRoot = Join-Path $env:TEMP ("pdf_note_workspace_ascii_build_" + [Guid]::NewGuid().ToString("N"))
    $powerShellExe = Get-PowerShellExePath
    $relativeScriptPath = [System.IO.Path]::GetRelativePath($RepoRootPath, $CurrentScriptPath)
    $relayScriptPath = Join-Path $relayRoot $relativeScriptPath
    $relayArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $relayScriptPath)

    foreach ($entry in $ForwardedParameters.GetEnumerator()) {
        $relayArgs += "-$($entry.Key)"
        if ($entry.Value -is [switch]) {
            continue
        }
        if ($entry.Value -is [System.Array]) {
            foreach ($item in $entry.Value) {
                $relayArgs += [string]$item
            }
            continue
        }
        $relayArgs += [string]$entry.Value
    }

    New-Item -ItemType Junction -Path $relayRoot -Target $RepoRootPath | Out-Null
    try {
        Write-Host ("Non-ASCII repository path detected. Relaying build via ASCII junction: {0}" -f $relayRoot) -ForegroundColor Yellow
        $previousRelayFlag = $env:PDF_NOTE_ASCII_BUILD_ROOT_ACTIVE
        $env:PDF_NOTE_ASCII_BUILD_ROOT_ACTIVE = "1"
        try {
            & $powerShellExe @relayArgs
            $childExitCode = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
            exit $childExitCode
        }
        finally {
            if ($null -eq $previousRelayFlag) {
                Remove-Item env:PDF_NOTE_ASCII_BUILD_ROOT_ACTIVE -ErrorAction SilentlyContinue
            }
            else {
                $env:PDF_NOTE_ASCII_BUILD_ROOT_ACTIVE = $previousRelayFlag
            }
        }
    }
    finally {
        if (Test-Path -LiteralPath $relayRoot) {
            Remove-Item -LiteralPath $relayRoot -Force
        }
    }
}

if ($env:PDF_NOTE_ASCII_BUILD_ROOT_ACTIVE -ne "1" -and (Test-ContainsNonAscii -Value $repoRoot)) {
    Invoke-BuildViaAsciiJunction -RepoRootPath $repoRoot -CurrentScriptPath $MyInvocation.MyCommand.Path -ForwardedParameters $PSBoundParameters
}

$outRoot = Join-Path $repoRoot "out"
$binDir = Join-Path $outRoot "bin"
$logDir = Join-Path $outRoot "logs"
$endTimeLogPath = Join-Path $logDir "build_readonly_viewer_end_time.log"
$buildDetailLogPath = Join-Path $logDir ("build_readonly_viewer_detail_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
$versionFilePath = Join-Path $repoRoot "APP_VERSION.txt"

function Append-BuildDetailLog {
    param([AllowNull()]$Line)
    $text = if ($null -eq $Line) { "" } else { [string]$Line }
    Add-Content -LiteralPath $buildDetailLogPath -Value $text -Encoding UTF8
}

function Write-BuildFailureTail {
    if (-not (Test-Path -LiteralPath $buildDetailLogPath)) {
        return
    }
    $tailCount = [Math]::Max(1, $FailureTailLines)
    Write-Host ("---- failure log tail ({0} lines): {1}" -f $tailCount, $buildDetailLogPath) -ForegroundColor Yellow
    Get-Content -LiteralPath $buildDetailLogPath -Tail $tailCount | ForEach-Object { Write-Host $_ }
    Write-Host "---- end failure log tail" -ForegroundColor Yellow
}

function Invoke-BuildNativeCommand {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    Append-BuildDetailLog ("> {0} {1}" -f $FilePath, ($Arguments -join " "))
    $savedErrorActionPreference = $ErrorActionPreference
    $restoreNativeCommandErrorPreference = $false
    $previousNativeCommandErrorPreference = $false
    if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $restoreNativeCommandErrorPreference = $true
        $previousNativeCommandErrorPreference = $PSNativeCommandUseErrorActionPreference
        $PSNativeCommandUseErrorActionPreference = $false
    }
    try {
        $ErrorActionPreference = "Continue"
        & $FilePath @Arguments 2>&1 |
            ForEach-Object {
                if ($VerboseOutput) {
                    Write-Host $_
                }
                $_
            } |
            Out-File -LiteralPath $buildDetailLogPath -Append -Encoding UTF8 -Width 4096
        return [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
        if ($restoreNativeCommandErrorPreference) {
            $PSNativeCommandUseErrorActionPreference = $previousNativeCommandErrorPreference
        }
    }
}

function Write-EndTimeLog {
    param(
        [Parameter(Mandatory)][DateTime]$StartTime,
        [Parameter(Mandatory)][DateTime]$EndTime
    )

    $durationSec = ($EndTime - $StartTime).TotalSeconds
    $line = "{0}`telapsed_sec={1}" -f $EndTime.ToString("o"), $durationSec.ToString("F3", [System.Globalization.CultureInfo]::InvariantCulture)
    try {
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
        Add-Content -LiteralPath $endTimeLogPath -Value $line -Encoding UTF8
    }
    catch {
        Write-Warning ("Failed to write end time log to '{0}': {1}" -f $endTimeLogPath, $_.Exception.Message)
    }

    Write-Host ("Finished at: {0}" -f $line) -ForegroundColor DarkGray
}

function Get-Sha256Hex {
    param([Parameter(Mandatory)][string]$Text)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $hashBytes = $sha.ComputeHash($bytes)
        return ($hashBytes | ForEach-Object { $_.ToString("x2") }) -join ""
    }
    finally {
        $sha.Dispose()
    }
}

function Get-AppVersion {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing app version file: $Path"
    }

    $version = (Get-Content -LiteralPath $Path -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($version)) {
        throw "App version file is empty: $Path"
    }
    return $version
}

function Copy-FileIfHashDiff {
    param(
        [Parameter(Mandatory)][string]$SourcePath,
        [Parameter(Mandatory)][string]$DestPath,
        [string]$DisplayName = ""
    )

    if (-not (Test-Path -LiteralPath $SourcePath)) {
        return $false
    }

    $shouldCopy = $true
    if (Test-Path -LiteralPath $DestPath) {
        $srcHash = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash
        $dstHash = (Get-FileHash -LiteralPath $DestPath -Algorithm SHA256).Hash
        $shouldCopy = ($srcHash -ne $dstHash)
    }

    if ($shouldCopy) {
        Copy-Item -LiteralPath $SourcePath -Destination $DestPath -Force
        $label = if ([string]::IsNullOrWhiteSpace($DisplayName)) {
            Split-Path -Leaf $DestPath
        }
        else {
            $DisplayName
        }
        Write-Host ("Copied: {0}" -f $label) -ForegroundColor Green
        return $true
    }

    return $false
}

function Sync-ReadOnlyViewerRuntimeArtifacts {
    param(
        [Parameter(Mandatory)][string]$OutputExePath,
        [Parameter(Mandatory)][string]$BinDir,
        [Parameter(Mandatory)][string]$Compiler
    )

    $artifacts = @()
    if (Test-Path -LiteralPath $OutputExePath) {
        $artifacts += $OutputExePath
    }

    $pdfiumSource = "third_party/pdfium/bin/pdfium.dll"
    $pdfiumDest = Join-Path $BinDir "pdfium.dll"
    if (Test-Path -LiteralPath $pdfiumSource) {
        Copy-FileIfHashDiff -SourcePath $pdfiumSource -DestPath $pdfiumDest -DisplayName $pdfiumDest | Out-Null
        if (Test-Path -LiteralPath $pdfiumDest) {
            $artifacts += $pdfiumDest
        }
    }
    else {
        Write-Warning "PDFium DLL not found: $pdfiumSource (Executable will fail to run)"
    }

    $compilerCmd = Get-Command $Compiler -ErrorAction SilentlyContinue
    if ($compilerCmd -and $compilerCmd.Source) {
        $toolchainBinDir = Split-Path -Parent $compilerCmd.Source
        Write-Host ("Syncing MinGW runtime DLLs from: {0}" -f $toolchainBinDir) -ForegroundColor Cyan
        foreach ($dll in @("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")) {
            $src = Join-Path $toolchainBinDir $dll
            $dest = Join-Path $BinDir $dll
            if (Test-Path -LiteralPath $src) {
                Copy-FileIfHashDiff -SourcePath $src -DestPath $dest -DisplayName $dll | Out-Null
                if (Test-Path -LiteralPath $dest) {
                    $artifacts += $dest
                }
            }
            else {
                Write-Warning "MinGW runtime DLL not found in toolchain: $dll (Executable may fail to run)"
            }
        }
    }

    return @($artifacts | Select-Object -Unique)
}

function Write-BuildInfoManifest {
    param(
        [Parameter(Mandatory)][string]$ManifestPath,
        [Parameter(Mandatory)][string]$OutputExePath,
        [Parameter(Mandatory)][string]$Version,
        [string[]]$ArtifactPaths = @()
    )

    if (-not (Test-Path -LiteralPath $OutputExePath)) {
        return
    }

    $buildTimestamp = (Get-Item -LiteralPath $OutputExePath).LastWriteTime.ToString("yyyy-MM-dd HH:mm")
    $lines = @(
        "format`tpdf-note-build-info-v1",
        ("version`t{0}" -f $Version),
        ("build_timestamp`t{0}" -f $buildTimestamp)
    )

    foreach ($artifactPath in @($ArtifactPaths | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $artifactPath)) {
            continue
        }
        $hash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $name = Split-Path -Leaf $artifactPath
        $lines += ("artifact`t{0}`t{1}" -f $name, $hash)
    }

    $content = ($lines -join "`r`n") + "`r`n"
    Set-ContentWithRetry -Path $ManifestPath -Value $content -Encoding UTF8
}

function Set-ContentWithRetry {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Value,
        [Parameter(Mandatory)][string]$Encoding,
        [switch]$NoNewline,
        [int]$Retries = 8,
        [int]$BaseDelayMs = 120
    )

    for ($i = 0; $i -lt $Retries; ++$i) {
        try {
            if ($NoNewline) {
                Set-Content -LiteralPath $Path -Value $Value -Encoding $Encoding -NoNewline
            }
            else {
                Set-Content -LiteralPath $Path -Value $Value -Encoding $Encoding
            }
            return
        }
        catch {
            if ($i + 1 -ge $Retries) {
                throw
            }
            Start-Sleep -Milliseconds ($BaseDelayMs * ($i + 1))
        }
    }
}

function Remove-ItemWithRetry {
    param(
        [Parameter(Mandatory)][string]$Path,
        [int]$Retries = 8,
        [int]$BaseDelayMs = 120
    )

    for ($i = 0; $i -lt $Retries; ++$i) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force
            return
        }
        catch {
            if ($i + 1 -ge $Retries) {
                throw
            }
            Start-Sleep -Milliseconds ($BaseDelayMs * ($i + 1))
        }
    }
}

function Write-BuildOutputSummary {
    param(
        [Parameter(Mandatory)][string]$ExePath,
        [Parameter(Mandatory)][string]$BinDir,
        [Parameter(Mandatory)][string]$LogDir
    )

    Write-Host ("Output directory: {0}" -f $BinDir) -ForegroundColor Cyan
    if (Test-Path -LiteralPath $ExePath) {
        Write-Host ("Executable: {0}" -f $ExePath) -ForegroundColor Green
    }
    else {
        Write-Host ("Executable not found: {0}" -f $ExePath) -ForegroundColor Yellow
    }
    Write-Host ("Build logs: {0}" -f $LogDir) -ForegroundColor DarkCyan
}

Push-Location -LiteralPath $repoRoot
$globalLockStream = $null
try {
    $lockFilePath = Join-Path $outRoot "build.lock"
    if (-not (Test-Path -LiteralPath $outRoot)) {
        New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    }
    try {
        $globalLockStream = [System.IO.File]::Open($lockFilePath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    }
    catch {
        Write-Host "Another build is currently running. Please wait for it to finish." -ForegroundColor Red
        exit 1
    }

    $buildSourcesPath = Join-Path $scriptRoot "build_sources.json"
    if (-not (Test-Path -LiteralPath $buildSourcesPath)) {
        Write-Host "Missing build source definition: $buildSourcesPath" -ForegroundColor Red
        exit 1
    }
    $buildSources = Get-Content -Raw -LiteralPath $buildSourcesPath -Encoding UTF8 | ConvertFrom-Json

    $compiler = "g++"
    $resourceCompiler = "windres"
    $outputExeName = "readonly_viewer.exe"
    $outputExe = Join-Path $binDir $outputExeName
    $buildInfoManifestPath = Join-Path $binDir ($outputExeName + ".buildinfo.txt")
    $appVersion = Get-AppVersion -Path $versionFilePath
    $objDir = Join-Path $outRoot "obj_readonly_viewer"
    $resourceSource = "src/resources/app.rc"
    $resourceHeader = "src/resources/app_resource.h"
    $readOnlyViewerIconSource = "src/resources/icons/readonly_viewer_icon.ico"
    $resourceObject = Join-Path $objDir "src_readonly_viewer_resource.o"

    $sourceFiles = @($buildSources.ReadOnlyViewerSourceFiles)
    if ($sourceFiles.Count -eq 0) {
        Write-Host "No read-only viewer source files are defined in scripts/build/build_sources.json." -ForegroundColor Red
        exit 1
    }

    $missingSources = @($sourceFiles | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($missingSources.Count -gt 0) {
        Write-Host "Missing read-only viewer sources:" -ForegroundColor Red
        foreach ($path in $missingSources) {
            Write-Host "  $path" -ForegroundColor Red
        }
        exit 1
    }

    $resourceInputs = @($resourceSource, $resourceHeader, $readOnlyViewerIconSource)
    $missingResourceInputs = @($resourceInputs | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($missingResourceInputs.Count -gt 0) {
        Write-Host "Missing resource files required for read-only viewer icon:" -ForegroundColor Red
        foreach ($path in $missingResourceInputs) {
            Write-Host "  $path" -ForegroundColor Red
        }
        exit 1
    }

    $includes = @(
        "-Ithird_party/pdfium/include",
        "-Ithird_party/md4c/src",
        "-Isrc"
    )

    $libs = @(
        "third_party/pdfium/lib/pdfium.dll.lib",
        "-lgdi32",
        "-luser32",
        "-lmsimg32",
        "-lshell32",
        "-lole32",
        "-luuid",
        "-lcomctl32",
        "-luxtheme"
    )

    $baseFlags = @(
        "-municode",
        "-mwindows",
        "-Wall",
        "-pipe",
        "-DCLROP_READ_ONLY_BUILD"
    )

    if (-not (Get-Command $compiler -ErrorAction SilentlyContinue)) {
        Write-Host "Compiler '$compiler' not found in PATH." -ForegroundColor Red
        exit 1
    }
    if (-not (Get-Command $resourceCompiler -ErrorAction SilentlyContinue)) {
        Write-Host "Resource compiler '$resourceCompiler' not found in PATH." -ForegroundColor Red
        exit 1
    }

    $configuration = "Release"
    $flags = $baseFlags + @("-O2", "-DNDEBUG")

    Write-Host "Building PDF Read-Only Viewer..." -ForegroundColor Cyan
    Write-Host ("Configuration: {0}" -f $configuration) -ForegroundColor Cyan
    Write-Verbose ("Compiler flags: {0}" -f ($flags -join " "))

    function Remove-BuildArtifacts {
        param([string[]]$PathsToRemove)

        foreach ($path in $PathsToRemove) {
            if (-not (Test-Path -LiteralPath $path)) {
                continue
            }
            $item = Get-Item -LiteralPath $path
            if ($item.PSIsContainer) {
                Remove-ItemWithRetry -Path $path
            }
            else {
                Remove-Item -LiteralPath $path -Force
            }
            Write-Verbose ("Removed {0}" -f $path)
        }
    }

    if ($Clean) {
        Remove-BuildArtifacts -PathsToRemove @($objDir, $outputExe, $buildInfoManifestPath)
        Write-Host "Read-only viewer clean finished." -ForegroundColor Green
        return
    }

    if ($Rebuild -and (Test-Path -LiteralPath $objDir)) {
        Remove-ItemWithRetry -Path $objDir
    }
    if (-not (Test-Path -LiteralPath $objDir)) {
        New-Item -ItemType Directory -Path $objDir | Out-Null
    }
    if (-not (Test-Path -LiteralPath $binDir)) {
        New-Item -ItemType Directory -Path $binDir | Out-Null
    }
    if (-not (Test-Path -LiteralPath $logDir)) {
        New-Item -ItemType Directory -Path $logDir | Out-Null
    }
    New-Item -ItemType File -Force -Path $buildDetailLogPath | Out-Null
    Append-BuildDetailLog "== Read-Only Viewer Build =="
    Append-BuildDetailLog ("started: {0}" -f $scriptStart.ToString("o"))
    Append-BuildDetailLog ("cwd: {0}" -f (Get-Location).Path)
    Append-BuildDetailLog ("configuration: {0}" -f $configuration)
    Append-BuildDetailLog ""
    Write-Host ("Build detail log: {0}" -f $buildDetailLogPath) -ForegroundColor DarkCyan

    $signature = [PSCustomObject]@{
        Compiler         = $compiler
        ResourceCompiler = $resourceCompiler
        Configuration    = $configuration
        Includes         = $includes
        Libs             = $libs
        Flags            = $flags
        SourceFiles      = $sourceFiles
        ResourceFiles    = $resourceInputs
        OutputExe        = $outputExe
    }

    $signatureJson = $signature | ConvertTo-Json -Depth 10 -Compress
    $signatureHash = Get-Sha256Hex -Text $signatureJson
    $signatureJsonPath = Join-Path $objDir "build_signature.json"
    $signatureHashPath = Join-Path $objDir "build_signature.sha256"
    $previousHash = $null
    if (Test-Path -LiteralPath $signatureHashPath) {
        $previousHash = (Get-Content -LiteralPath $signatureHashPath -Raw).Trim()
    }

    # -Rebuild removes the object directory above, so it must force every source
    # to compile even when the persistent build signature itself is unchanged.
    $forceRecompile = $Rebuild
    $forceLink = $Rebuild
    if ($previousHash -ne $signatureHash) {
        $forceRecompile = $true
        $forceLink = $true
        if ($null -eq $previousHash) {
            Write-Host "No previous read-only viewer build signature found; forcing full recompile to avoid missing changes." -ForegroundColor Yellow
        }
        else {
            Write-Host "Read-only viewer build settings changed; forcing full recompile to avoid missing changes." -ForegroundColor Yellow
        }
    }

    function Get-DependencySeparatorIndex {
        param([Parameter(Mandatory)][string]$Line)

        for ($i = 0; $i -lt $Line.Length; ++$i) {
            if ($Line[$i] -ne ':') {
                continue
            }
            $nextIndex = $i + 1
            if ($nextIndex -lt $Line.Length -and [char]::IsWhiteSpace($Line[$nextIndex])) {
                return $i
            }
        }
        return -1
    }

    function Get-DependencyPaths {
        param([Parameter(Mandatory)][string]$DependencyFile)

        if (-not (Test-Path -LiteralPath $DependencyFile)) {
            return @()
        }
        $raw = Get-Content -LiteralPath $DependencyFile -Raw -ErrorAction SilentlyContinue
        if ([string]::IsNullOrWhiteSpace($raw)) {
            return @()
        }
        $singleLine = $raw -replace "\\\r?\n", " "
        $separatorIndex = Get-DependencySeparatorIndex -Line $singleLine
        if ($separatorIndex -lt 0) {
            return @()
        }
        $depsPart = $singleLine.Substring($separatorIndex + 1).Trim()
        if ([string]::IsNullOrWhiteSpace($depsPart)) {
            return @()
        }
        return ($depsPart -split "\s+" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    }

    function Should-Compile {
        param(
            [string]$Source,
            [string]$Object,
            [string]$DependencyFile
        )

        if (-not (Test-Path -LiteralPath $Object)) {
            return $true
        }

        $srcTime = (Get-Item -LiteralPath $Source).LastWriteTime
        $objTime = (Get-Item -LiteralPath $Object).LastWriteTime
        if ($srcTime -gt $objTime) {
            return $true
        }
        if (-not (Test-Path -LiteralPath $DependencyFile)) {
            return $true
        }
        foreach ($dep in (Get-DependencyPaths -DependencyFile $DependencyFile)) {
            if (-not (Test-Path -LiteralPath $dep)) {
                return $true
            }
            if ((Get-Item -LiteralPath $dep).LastWriteTime -gt $objTime) {
                return $true
            }
        }
        return $false
    }

    function Should-CompileResource {
        param(
            [string[]]$Inputs,
            [string]$Object
        )

        if (-not (Test-Path -LiteralPath $Object)) {
            return $true
        }
        $objTime = (Get-Item -LiteralPath $Object).LastWriteTime
        foreach ($input in $Inputs) {
            if (-not (Test-Path -LiteralPath $input)) {
                return $true
            }
            if ((Get-Item -LiteralPath $input).LastWriteTime -gt $objTime) {
                return $true
            }
        }
        return $false
    }

    $objFiles = @()
    $compileTimings = @()
    $totalCompileMs = 0
    $compileCount = 0
    $skippedCount = 0
    $resourceCompiled = $false

    foreach ($src in $sourceFiles) {
        $flatName = $src -replace "[\\/]", "_" -replace "\.(cpp|c)$", ".o"
        $objPath = Join-Path $objDir $flatName
        $depPath = "$objPath.d"
        $objFiles += $objPath

        $isTargetFile = $true
        $forceCompileThis = $false
        if ($Files -and $Files.Count -gt 0) {
            $isTargetFile = $false
            foreach ($f in $Files) {
                if ($src.Replace('\', '/') -match [regex]::Escape($f.Replace('\', '/'))) {
                    $isTargetFile = $true
                    $forceCompileThis = $true
                    break
                }
            }
        }

        if ($isTargetFile -and ($forceRecompile -or $forceCompileThis -or (Should-Compile -Source $src -Object $objPath -DependencyFile $depPath))) {
            if ($VerboseOutput) {
                Write-Host "Compiling: $src" -ForegroundColor Yellow
            }
            $fileCompiler = if ($src -match "\.c$") { "gcc" } else { $compiler }
            $cmdArgs = @("-c", $src, "-o", $objPath, "-MMD", "-MF", $depPath, "-MT", $objPath) + $includes + $flags
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $compileExitCode = Invoke-BuildNativeCommand -FilePath $fileCompiler -Arguments $cmdArgs
            $sw.Stop()
            if ($compileExitCode -ne 0) {
                Write-Host ("failed   {0}s  {1}" -f [Math]::Round($sw.Elapsed.TotalSeconds, 1), $src) -ForegroundColor Red
                Write-BuildFailureTail
                exit 1
            }
            $totalCompileMs += $sw.ElapsedMilliseconds
            $compileCount++
            if ($VerboseOutput) {
                Write-Host ("compiled {0}s  {1}" -f [Math]::Round($sw.Elapsed.TotalSeconds, 1), $src) -ForegroundColor Green
            }
            $compileTimings += [PSCustomObject]@{
                File    = $src
                Seconds = $sw.Elapsed.TotalSeconds
                Status  = "ok"
            }
        }
        else {
            $skippedCount++
            Write-Verbose ("Skipped (up to date) {0}" -f $src)
        }
    }

    if ($forceRecompile -or (Should-CompileResource -Inputs $resourceInputs -Object $resourceObject)) {
        Write-Host "Compiling resource: $resourceSource" -ForegroundColor Yellow
        $resourceArgs = @(
            "--codepage=65001",
            "-I", "src",
            "-I", "src/resources",
            "-D", "CLROP_READ_ONLY_BUILD",
            "-i", $resourceSource,
            "-o", $resourceObject,
            "-O", "coff"
        )
        $resourceExitCode = Invoke-BuildNativeCommand -FilePath $resourceCompiler -Arguments $resourceArgs
        if ($resourceExitCode -ne 0) {
            Write-Host "Read-only viewer resource compile failed." -ForegroundColor Red
            Write-BuildFailureTail
            exit 1
        }
        $resourceCompiled = $true
        Write-Host "compiled resource  $resourceSource" -ForegroundColor Green
    }

    if ($compileTimings.Count -gt 0) {
        $totalSec = [Math]::Round(($totalCompileMs / 1000.0), 1)
        Write-Host ("Compile time: {0}s ({1} files)" -f $totalSec, $compileTimings.Count) -ForegroundColor Cyan
        Write-Host ("Skipped: {0} files already up to date" -f $skippedCount) -ForegroundColor DarkCyan
        $top = $compileTimings | Sort-Object Seconds -Descending | Select-Object -First 5
        Write-Host "Slowest files:" -ForegroundColor DarkCyan
        foreach ($entry in $top) {
            Write-Host ("  {0,5}s  {1}" -f [Math]::Round($entry.Seconds, 1), $entry.File) -ForegroundColor DarkCyan
        }
    }

    $needLink = ($compileCount -gt 0) -or (-not (Test-Path -LiteralPath $outputExe))
    if (-not $needLink -and $resourceCompiled) {
        $needLink = $true
    }
    if (-not $needLink -and (Test-Path -LiteralPath $outputExe)) {
        $exeTime = (Get-Item -LiteralPath $outputExe).LastWriteTime
        foreach ($obj in $objFiles) {
            if (-not (Test-Path -LiteralPath $obj) -or (Get-Item -LiteralPath $obj).LastWriteTime -gt $exeTime) {
                $needLink = $true
                break
            }
        }
        if (-not $needLink -and ((-not (Test-Path -LiteralPath $resourceObject)) -or
            ((Get-Item -LiteralPath $resourceObject).LastWriteTime -gt $exeTime))) {
            $needLink = $true
        }
    }
    if ($forceLink) {
        $needLink = $true
    }

    if ($needLink) {
        $linkObjFiles = @($objFiles)
        $linkObjFiles += $resourceObject
        $linkObjFiles = @($linkObjFiles | Select-Object -Unique)

        $missingLinkInputs = @($linkObjFiles | Where-Object { -not (Test-Path -LiteralPath $_) })
        if ($missingLinkInputs.Count -gt 0) {
            Write-Host "Missing object files required for linking read-only viewer:" -ForegroundColor Red
            foreach ($path in $missingLinkInputs) {
                Write-Host "  $path" -ForegroundColor Red
            }
            exit 1
        }

        Write-Host "Linking read-only viewer..." -ForegroundColor Cyan
        $linkArgs = @("-o", $outputExe) + $linkObjFiles + $libs + $flags
        $linkResponsePath = Join-Path $objDir "link_args.rsp"
        $linkResponseText = (($linkArgs | ForEach-Object {
            '"' + (($_.ToString()) -replace '"', '\"') + '"'
        }) -join "`n") + "`n"
        Set-ContentWithRetry -Path $linkResponsePath -Value $linkResponseText -Encoding ASCII -NoNewline
        $linkExitCode = Invoke-BuildNativeCommand -FilePath $compiler -Arguments @("@$linkResponsePath")
        if ($linkExitCode -ne 0) {
            Write-Host "Read-only viewer link failed." -ForegroundColor Red
            Write-BuildFailureTail
            exit 1
        }
    }

    if ($needLink) {
        Write-Host ("Read-only viewer build successful! ({0} files compiled, {1} skipped)" -f $compileCount, $skippedCount) -ForegroundColor Green
    }
    else {
        Write-Host ("Read-only viewer up to date. ({0} files skipped)" -f $skippedCount) -ForegroundColor Green
    }
    Write-BuildOutputSummary -ExePath $outputExe -BinDir $binDir -LogDir $logDir

    $buildInfoArtifactPaths = Sync-ReadOnlyViewerRuntimeArtifacts -OutputExePath $outputExe -BinDir $binDir -Compiler $compiler
    Write-BuildInfoManifest -ManifestPath $buildInfoManifestPath -OutputExePath $outputExe -Version $appVersion -ArtifactPaths $buildInfoArtifactPaths

    Set-ContentWithRetry -Path $signatureJsonPath -Value $signatureJson -Encoding UTF8
    Set-ContentWithRetry -Path $signatureHashPath -Value $signatureHash -Encoding ASCII -NoNewline
}
finally {
    if ($null -ne $globalLockStream) {
        $globalLockStream.Dispose()
    }
    $end = Get-Date
    Write-EndTimeLog -StartTime $scriptStart -EndTime $end
    Pop-Location
}
