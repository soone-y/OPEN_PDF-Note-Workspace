# Fixed, offline resources shared by build, tests and distribution packaging.
function Sync-MicrotexAssets {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$SourceRoot,
          [Parameter(Mandatory)][string]$DestinationRoot,
          [switch]$DryRun)
    $assets = @(
        @{ Source = 'third_party/microtex/LICENSE'; Destination = 'licenses/microtex/LICENSE.txt'; Sha256 = 'bc05c564a978284269047da23db39cdc1db811a88670989dc48cacfd54f203d9' },
        @{ Source = 'third_party/microtex/res/xits/OFL.txt'; Destination = 'licenses/xits/OFL.txt'; Sha256 = '10c83acb7bf240c6e263906e0905c8e419f3483c89141e83243465c221d322ad' },
        @{ Source = 'third_party/microtex/res/xits/FONTLOG.txt'; Destination = 'licenses/xits/FONTLOG.txt'; Sha256 = 'f5b5fb90c0787a134525f435fcc9c78e5c732ff6810c4285a7c2a08a5b815f03' },
        @{ Source = 'third_party/microtex/res/xits/README.txt'; Destination = 'licenses/xits/README.txt'; Sha256 = '67432af9a4ed7fdf43126fc881105a06471d1f308c0e5bb0c1e2136e431775e5' },
        @{ Source = 'third_party/microtex/res/xits/XITSMath-Regular.clm2'; Destination = 'resources/math/xits/XITSMath-Regular.clm2'; Sha256 = '1bf9cbe66a4357d63600e3f8e8415321d9d2f87f1476cd4b7e36bed6534e3042' }
    )
    $destinationBase = [IO.Path]::GetFullPath($DestinationRoot)
    if ((Test-Path -LiteralPath $destinationBase) -and
        ((Get-Item -LiteralPath $destinationBase).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'MicroTeX destination root must be a regular directory'
    }
    foreach ($asset in $assets) {
        $source = Join-Path $SourceRoot $asset.Source
        if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $asset.Sha256) {
            throw "MicroTeX resource is missing or changed: $($asset.Source)"
        }
        $destination = Join-Path $destinationBase $asset.Destination
        if ((Test-Path -LiteralPath $destination) -and
            ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "MicroTeX resource destination is a link: $destination"
        }
        $parent = Split-Path -Parent $destination
        $ancestor = $parent
        while ($ancestor.Length -gt $destinationBase.Length) {
            if ((Test-Path -LiteralPath $ancestor) -and
                ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "MicroTeX resource destination is a link: $ancestor"
            }
            $ancestor = Split-Path -Parent $ancestor
        }
        if ($DryRun) {
            Write-Host "copy-file: $source -> $destination"
            $destination
            continue
        }
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        if (-not (Test-Path -LiteralPath $destination -PathType Leaf) -or
            (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ine $asset.Sha256) {
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ine $asset.Sha256) {
            throw "MicroTeX resource copy verification failed: $destination"
        }
        $destination
    }
}
