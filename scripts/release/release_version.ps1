# Local release preparation only. Submitted versions are consumed even while
# the GitHub Release is still a draft; confirmation alone does not consume one.
function Get-PreviousSubmittedVersion {
    param([Parameter(Mandatory)][string]$RecordsDirectory)

    if (-not (Test-Path -LiteralPath $RecordsDirectory)) { return "" }
    $candidates = @(foreach ($record in Get-ChildItem -LiteralPath $RecordsDirectory -File -Filter "*.txt") {
        if ($record.Name -notlike "publish_公開前確認文書_*" -and $record.Name -notlike "PUBLISH_CHECKLIST_*") { continue }
        $content = Get-Content -LiteralPath $record.FullName -Raw -Encoding UTF8
        if ($content -notmatch '(?m)^(?:再)?提出結果:') { continue }
        $match = [regex]::Match($content, '(?m)^版番号 / タグ:\s*v(?<version>[0-9]+\.[0-9]+\.[0-9]+)(?:-(?:ja|en))?\s*$')
        if (-not $match.Success) {
            # Compatibility for historical checklists without a version line.
            $match = [regex]::Match($record.Name, 'release_set_(?<version>[0-9]+\.[0-9]+\.[0-9]+)_')
        }
        if (-not $match.Success) { throw "Submitted record has no valid version: $($record.FullName)" }
        $version = $match.Groups['version'].Value
        [PSCustomObject]@{ Text = $version; Number = [version]$version }
    })
    $latest = $candidates | Sort-Object Number -Descending | Select-Object -First 1
    if ($null -eq $latest) { return "" }
    return $latest.Text
}

function Enter-ReleaseVersion {
    param([Parameter(Mandatory)][string]$RepositoryRoot)

    # Own the release/version decision through finalization. Briefly take the
    # build lock before changing the version; child builds acquire it normally.
    # A read lease then prevents version edits until all locales are frozen.
    # Atomic replacement retains the old bytes in out/ on write failure/crash.
    $outRoot = Join-Path $RepositoryRoot 'out'
    New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    $releaseLock = $null
    $buildLock = $null
    $versionLease = $null
    try {
        $releaseLock = [System.IO.File]::Open((Join-Path $outRoot 'release_version.lock'), 'OpenOrCreate', 'Write', 'None')
        $buildLock = [System.IO.File]::Open((Join-Path $outRoot 'build.lock'), 'OpenOrCreate', 'Write', 'None')
        $versionPath = Join-Path $RepositoryRoot 'REPO_VERSION.txt'
        $originalBytes = [System.IO.File]::ReadAllBytes($versionPath)
        $current = (Get-Content -LiteralPath $versionPath -Raw -Encoding UTF8).Trim()
        if ($current -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "Invalid REPO_VERSION.txt: expected X.Y.Z." }
        $currentNumber = [version]$current
        $previous = Get-PreviousSubmittedVersion -RecordsDirectory (Join-Path $RepositoryRoot 'docs/internal/archive/operations/publish_records')
        $selected = $current
        if ($previous) {
            if ($currentNumber -lt [version]$previous) { throw "Repository version $current is older than submitted version $previous. Update REPO_VERSION.txt." }
            if ($currentNumber -eq [version]$previous) {
                $parts = $current.Split('.')
                $next = [long]$parts[2] + 1
                if ($next -gt [int]::MaxValue) { throw "Version segment overflow. Update REPO_VERSION.txt." }
                $selected = "$($parts[0]).$($parts[1]).$($next.ToString('D' + $parts[2].Length))"
            }
        }
        if ($selected -ne $current) {
            $temporaryPath = Join-Path $RepositoryRoot ('.release_version_' + [guid]::NewGuid().ToString('N') + '.tmp')
            $backupDirectory = Join-Path $outRoot 'logs/release/version_backups'
            New-Item -ItemType Directory -Force -Path $backupDirectory | Out-Null
            $backupPath = Join-Path $backupDirectory ([guid]::NewGuid().ToString('N') + '.txt')
            try {
                [System.IO.File]::WriteAllText($temporaryPath, $selected + "`n", (New-Object System.Text.UTF8Encoding($false)))
                $now = [System.IO.File]::ReadAllBytes($versionPath)
                if ([Convert]::ToBase64String($now) -cne [Convert]::ToBase64String($originalBytes)) { throw 'REPO_VERSION.txt changed during version selection.' }
                [System.IO.File]::Replace($temporaryPath, $versionPath, $backupPath)
            }
            finally {
                if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath -Force }
            }
            Write-Host "提出済みの版番号 $previous から $selected に更新しました。原本の退避先: $backupPath" -ForegroundColor Cyan
        }
        $versionLease = [System.IO.File]::Open($versionPath, 'Open', 'Read', 'Read')
        if ((Get-Content -LiteralPath $versionPath -Raw -Encoding UTF8).Trim() -cne $selected) { throw 'REPO_VERSION.txt changed before the release version was fixed.' }
        Write-Host "Release版番号を確定しました: $selected" -ForegroundColor Cyan
        return [PSCustomObject]@{ Version = $selected; ReleaseLock = $releaseLock; VersionLease = $versionLease }
    }
    catch {
        if ($null -ne $versionLease) { $versionLease.Dispose() }
        if ($null -ne $releaseLock) { $releaseLock.Dispose() }
        throw
    }
    finally {
        if ($null -ne $buildLock) { $buildLock.Dispose() }
    }
}

function Exit-ReleaseVersion {
    param([AllowNull()]$Lease)
    if ($null -ne $Lease) {
        $Lease.VersionLease.Dispose()
        $Lease.ReleaseLock.Dispose()
    }
}
