# The build and package entry points seed from the same locale/edition template.
function Get-SampleWorkspaceFiles {
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][ValidateSet('ja', 'en')][string]$Locale,
        [switch]$Lite
    )

    $releaseAssets = Join-Path $RepositoryRoot 'release_assets'
    $sampleAssets = Join-Path $releaseAssets 'sample_workspace'
    $source = Join-Path $sampleAssets $Locale
    foreach ($path in @($releaseAssets, $sampleAssets, $source)) {
        $item = Get-Item -LiteralPath $path -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unsafe sample workspace template directory: $path"
        }
    }

    # Keep each course directory. Lite omits the Office conversion session.
    $conversionSession = if ($Locale -eq 'ja') {
        '01_講義サンプル\第04回_Office変換'
    } else {
        '01_Lecture_Samples\Session_04_Office_Conversion'
    }
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($source)
    while ($pending.Count -gt 0) {
        foreach ($item in Get-ChildItem -LiteralPath ($pending.Pop()) -Force) {
            $relative = $item.FullName.Substring($source.Length + 1)
            if ($Lite -and $relative -eq $conversionSession) { continue }
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Unsafe sample workspace template entry: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            else { [pscustomobject]@{ Source = $item.FullName; RelativePath = $relative } }
        }
    }
}
