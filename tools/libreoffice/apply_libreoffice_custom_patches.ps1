param(
    [string]$SourceDir = $env:PDF_NOTE_LO_SOURCE_DIR,
    [string]$PatchExe = "C:\msys64\usr\bin\patch.exe"
)

$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$PatchDir = Join-Path $RepoRoot "third_party\libreoffice\custom_build\patches"

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    throw "LibreOffice source directory is not set. Pass -SourceDir or set PDF_NOTE_LO_SOURCE_DIR."
}

if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
    throw "LibreOffice source directory not found: $SourceDir"
}

if (-not (Test-Path -LiteralPath $PatchDir -PathType Container)) {
    throw "LibreOffice patch directory not found: $PatchDir"
}

$patches = Get-ChildItem -LiteralPath $PatchDir -Filter "*.patch" | Sort-Object Name
if ($patches.Count -eq 0) {
    throw "No LibreOffice patch files found in: $PatchDir"
}

function Test-GitApply {
    param(
        [string]$SourceDir,
        [string]$PatchPath,
        [switch]$Reverse
    )

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    if ($Reverse) {
        & git -C $SourceDir apply --reverse --check $PatchPath *> $null
    } else {
        & git -C $SourceDir apply --check $PatchPath *> $null
    }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    return ($exitCode -eq 0)
}

function Test-PatchApply {
    param(
        [string]$SourceDir,
        [string]$PatchPath,
        [switch]$Reverse
    )

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location -LiteralPath $SourceDir
    try {
        $args = @("--batch", "--forward", "--dry-run", "-p1", "-i", $PatchPath)
        if ($Reverse) {
            $args = @("--batch", "--reverse", "--dry-run", "-p1", "-i", $PatchPath)
        }
        & $PatchExe @args *> $null
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $ErrorActionPreference = $previousErrorActionPreference
    }
    return ($exitCode -eq 0)
}

function Test-PatchAlreadyAppliedHint {
    param(
        [string]$SourceDir,
        [string]$PatchPath
    )

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location -LiteralPath $SourceDir
    try {
        $output = & $PatchExe "--batch" "--forward" "--dry-run" "-p1" "-i" $PatchPath 2>&1 | Out-String
    }
    finally {
        Pop-Location
        $ErrorActionPreference = $previousErrorActionPreference
    }

    return ($output -match "Reversed \(or previously applied\) patch detected" -or
            $output -match "which already exists")
}

$sourceIsGitRepo = Test-Path -LiteralPath (Join-Path $SourceDir ".git") -PathType Container
if (-not $sourceIsGitRepo -and -not (Test-Path -LiteralPath $PatchExe -PathType Leaf)) {
    throw "patch.exe not found: $PatchExe"
}

foreach ($patch in $patches) {
    if ($sourceIsGitRepo) {
        if (Test-GitApply -SourceDir $SourceDir -PatchPath $patch.FullName) {
            & git -C $SourceDir apply $patch.FullName
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to apply patch: $($patch.Name)"
            }
            Write-Host "applied:$($patch.Name)"
            continue
        }

        if (Test-GitApply -SourceDir $SourceDir -PatchPath $patch.FullName -Reverse) {
            Write-Host "already-applied:$($patch.Name)"
            continue
        }

        throw "Patch cannot be applied cleanly: $($patch.Name)"
    } else {
        if (Test-PatchApply -SourceDir $SourceDir -PatchPath $patch.FullName) {
            Push-Location -LiteralPath $SourceDir
            try {
                & $PatchExe "--batch" "--forward" "-p1" "-i" $patch.FullName
                if ($LASTEXITCODE -ne 0) {
                    throw "Failed to apply patch: $($patch.Name)"
                }
            }
            finally {
                Pop-Location
            }
            Write-Host "applied:$($patch.Name)"
            continue
        }

        if (Test-PatchApply -SourceDir $SourceDir -PatchPath $patch.FullName -Reverse) {
            Write-Host "already-applied:$($patch.Name)"
            continue
        }

        if (Test-PatchAlreadyAppliedHint -SourceDir $SourceDir -PatchPath $patch.FullName) {
            Write-Host "already-applied:$($patch.Name)"
            continue
        }

        throw "Patch cannot be applied cleanly: $($patch.Name)"
    }
}
