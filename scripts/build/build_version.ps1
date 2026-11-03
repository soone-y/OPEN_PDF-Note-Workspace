function Test-BuildVersionNeedsRefresh {
    param(
        [Parameter(Mandatory)][string]$VersionPath,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$ExecutablePath,
        [Parameter(Mandatory)][string]$BuildInfoPath
    )
    # Missing/ambiguous evidence requires a good compile/link; unreadable
    # evidence fails. Neither path may merely relabel an existing binary.
    $current = [System.IO.File]::ReadAllText($VersionPath).Trim()
    if ($current -cne $Version) { throw 'Repository version changed during build preparation.' }
    if (-not (Test-Path -LiteralPath $ExecutablePath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $BuildInfoPath -PathType Leaf)) { return $true }
    $lines = [System.IO.File]::ReadAllLines($BuildInfoPath)
    if (@($lines | Where-Object { $_ -ceq "format`tpdf-note-build-info-v1" }).Count -ne 1) { return $true }
    $records = @($lines | Where-Object { $_ -match '^version\t' })
    if ($records.Count -ne 1) { return $true }
    $match = [regex]::Match($records[0], '^version\t(?<value>[^\t]+)$')
    if (-not $match.Success -or $match.Groups['value'].Value -cne $Version) { return $true }
    return [System.IO.File]::GetLastWriteTimeUtc($VersionPath) -gt [System.IO.File]::GetLastWriteTimeUtc($ExecutablePath)
}
