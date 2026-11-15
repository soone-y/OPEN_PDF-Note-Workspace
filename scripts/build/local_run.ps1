. (Join-Path $PSScriptRoot 'sample_workspace.ps1')

function Get-LocalRunItem {
    param([string]$Path, [bool]$Directory)

    try { $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop }
    catch [System.Management.Automation.ItemNotFoundException] { return }
    if ($item.PSIsContainer -ne $Directory -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unexpected file type or link at local build path: $Path"
    }
    return $item
}

function Assert-LocalSampleTree {
    param([string]$Path)

    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($Path)
    while ($pending.Count -gt 0) {
        foreach ($item in Get-ChildItem -LiteralPath ($pending.Pop()) -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Cannot reset a sample containing a link: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
}

function Test-LocalRunActive {
    param([string]$RunPath)

    $paths = @((Join-Path $RunPath 'pdf_note_workspace.exe'), (Join-Path $RunPath 'readonly_viewer.exe'))
    foreach ($process in Get-Process -Name 'pdf_note_workspace', 'readonly_viewer' -ErrorAction SilentlyContinue) {
        $imagePath = $process.Path
        if (-not $imagePath -and -not $process.HasExited) {
            throw "Cannot verify application path (PID $($process.Id)). Close the application and retry."
        }
        if ($imagePath -in $paths) {
            return $true
        }
    }
    return $false
}

function Get-LocalRunHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return [Convert]::ToBase64String($sha.ComputeHash($stream)) }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Initialize-LocalSampleWorkspace {
    param([string]$RepositoryRoot, [string]$RunPath, [string]$Locale, [switch]$Lite)

    $sample = Join-Path $RunPath 'sample_workspace'
    if (-not (Get-LocalRunItem $sample $true)) {
        $files = @(Get-SampleWorkspaceFiles -RepositoryRoot $RepositoryRoot -Locale $Locale -Lite:$Lite)
        if (-not $files.Count) { throw "Empty sample workspace template: $Locale" }
        # Publish the tree only after a complete copy. An interrupted copy never
        # occupies the active sample path or touches an existing workspace.
        $stage = Join-Path $RunPath ('.sample-stage-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $stage | Out-Null
        foreach ($file in $files) {
            $target = Join-Path $stage $file.RelativePath
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
            [IO.File]::Copy($file.Source, $target)
        }
        [IO.Directory]::Move($stage, $sample)
        Write-Host "Sample workspace initialized: $sample"
    }

    $setup = Join-Path $RunPath 'pdf_note_workspace_setup.json'
    $legacy = Join-Path $RunPath 'pdf_workspace_setup.json'
    if (-not (Get-LocalRunItem $setup $false) -and -not (Get-LocalRunItem $legacy $false)) {
        $stage = Join-Path $RunPath ('.setup-stage-' + [guid]::NewGuid().ToString('N'))
        [IO.File]::WriteAllText($stage, '{"workspaceRoot":"sample_workspace","workspaceRootMode":"relative","tempExternalLectureDirs":[]}' + [Environment]::NewLine)
        [IO.File]::Move($stage, $setup)
    }
}

function Invoke-LocalRunLayout {
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][ValidateSet('ja', 'en')][string]$Locale,
        [string[]]$Editions = @('Full', 'Lite'),
        [switch]$Clean,
        [switch]$HardClean
    )

    $outRoot = Join-Path $RepositoryRoot 'out'
    if (-not (Get-LocalRunItem $outRoot $true)) { [IO.Directory]::CreateDirectory($outRoot) | Out-Null }
    $lockPath = Join-Path $outRoot 'build.lock'
    $null = Get-LocalRunItem $lockPath $false
    $lock = [IO.File]::Open($lockPath, 'OpenOrCreate', 'Write', 'None')
    try {
        $localeSuffix = if ($Locale -eq 'en') { '_en' } else { '' }
        $artifacts = @('pdfium.dll', 'zlib1.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll',
                       'readonly_viewer.exe', 'pdf_note_workspace.exe')
        $targets = @(foreach ($edition in $Editions) {
            if ($edition -notin @('Full', 'Lite')) { throw "Unsupported local build edition: $edition" }
            $editionSuffix = if ($edition -eq 'Lite') { '_lite' } else { '' }
            $run = Join-Path $outRoot "run$editionSuffix$localeSuffix"
            $sample = Join-Path $run 'sample_workspace'
            $hasRun = $null -ne (Get-LocalRunItem $run $true)
            if ($HardClean) { Write-Host "Sample workspace reset target: $sample" -ForegroundColor Yellow }
            if ($hasRun -and ($Clean -or $HardClean)) {
                if (Test-LocalRunActive $run) { throw "Close the local application and retry: $run" }
                foreach ($name in $artifacts) { $null = Get-LocalRunItem (Join-Path $run $name) $false }
                if ($HardClean -and (Get-LocalRunItem $sample $true)) { Assert-LocalSampleTree $sample }
            }
            [pscustomobject]@{ Run = $run; Sample = $sample; HasRun = $hasRun; IsLite = ($edition -eq 'Lite');
                               Bin = Join-Path $outRoot "bin$editionSuffix$localeSuffix" }
        })

        foreach ($target in $targets) {
            if ($Clean -or $HardClean) {
                if (-not $target.HasRun) { continue }
                foreach ($name in $artifacts) { [IO.File]::Delete((Join-Path $target.Run $name)) }
                if ($HardClean -and (Get-LocalRunItem $target.Sample $true)) {
                    $retired = Join-Path $target.Run ('.sample-reset-' + [guid]::NewGuid().ToString('N'))
                    [IO.Directory]::Move($target.Sample, $retired)
                    try { [IO.Directory]::Delete($retired, $true) }
                    catch { throw "Could not delete all reset sample files. Remaining files: $retired. $($_.Exception.Message)" }
                }
                continue
            }

            if (-not $target.HasRun) { [IO.Directory]::CreateDirectory($target.Run) | Out-Null }
            $viewerBin = Join-Path $outRoot "bin$localeSuffix"
            $null = Get-LocalRunItem $viewerBin $true
            $null = Get-LocalRunItem $target.Bin $true
            $updates = @(foreach ($name in $artifacts) {
                $source = Join-Path $(if ($name -eq 'readonly_viewer.exe') { $viewerBin } else { $target.Bin }) $name
                $dest = Join-Path $target.Run $name
                $inputFile = Get-LocalRunItem $source $false
                if (-not $inputFile) { throw "Local runtime artifact not found: $source" }
                $existing = Get-LocalRunItem $dest $false
                if (-not $existing -or $existing.Length -ne $inputFile.Length -or
                    (Get-LocalRunHash $source) -cne (Get-LocalRunHash $dest)) {
                    [pscustomobject]@{ Source = $source; Destination = $dest; Exists = ($null -ne $existing) }
                }
            })
            if ($updates.Count -and (Test-LocalRunActive $target.Run)) {
                Write-Warning "Local application is running; runtime update deferred until the next build: $($target.Run)"
                continue
            }
            Initialize-LocalSampleWorkspace -RepositoryRoot $RepositoryRoot -RunPath $target.Run -Locale $Locale -Lite:$target.IsLite
            foreach ($update in $updates) {
                $stage = Join-Path $target.Run ('.runtime-stage-' + [guid]::NewGuid().ToString('N'))
                [IO.File]::Copy($update.Source, $stage)
                if ($update.Exists) {
                    # Never write through an existing file, including a hard link.
                    $previous = $stage + '.previous'
                    [IO.File]::Move($update.Destination, $previous)
                    try { [IO.File]::Move($stage, $update.Destination) }
                    catch {
                        [IO.File]::Move($previous, $update.Destination)
                        throw
                    }
                    [IO.File]::Delete($previous)
                } else { [IO.File]::Move($stage, $update.Destination) }
            }
            Write-Host "Local application: $(Join-Path $target.Run 'pdf_note_workspace.exe')" -ForegroundColor Green
        }
    }
    finally { $lock.Dispose() }
}
