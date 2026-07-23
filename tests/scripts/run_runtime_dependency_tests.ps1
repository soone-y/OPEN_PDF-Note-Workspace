[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$binDir = Join-Path $repoRoot "out\bin"
$liteBinDir = Join-Path $repoRoot "out\bin_lite"
$exe = Join-Path $binDir "pdf_note_workspace.exe"
$liteExe = Join-Path $liteBinDir "pdf_note_workspace.exe"
$readOnlyViewerExe = Join-Path $binDir "readonly_viewer.exe"
$zlibDll = Join-Path $binDir "zlib1.dll"
$binaryScan = Join-Path $repoRoot "tools\release_checks\binary_scan.py"
$packRelease = Join-Path $repoRoot "scripts\release\pack_release.ps1"
$loRuntimeSanitizer = Join-Path $repoRoot "tools\release_checks\sanitize_libreoffice_runtime_release.py"

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe"
}
if (-not (Test-Path -LiteralPath $liteExe)) {
    throw "Lite executable not found: $liteExe"
}
if (-not (Test-Path -LiteralPath $readOnlyViewerExe)) {
    throw "Read-only viewer executable not found: $readOnlyViewerExe"
}
if (-not (Test-Path -LiteralPath $zlibDll)) {
    throw "Required zlib runtime DLL not found: $zlibDll"
}
if (-not (Test-Path -LiteralPath $binaryScan)) {
    throw "Missing binary scan tool: $binaryScan"
}
if (-not (Test-Path -LiteralPath $packRelease)) {
    throw "Missing release pack script: $packRelease"
}
if (-not (Test-Path -LiteralPath $loRuntimeSanitizer)) {
    throw "Missing LibreOffice release runtime sanitizer: $loRuntimeSanitizer"
}

$fullBuildInfo = Get-Content -LiteralPath ($exe + ".buildinfo.txt") -Raw
if ($fullBuildInfo -notmatch "(?m)^edition`tfull\r?$") {
    throw "Full executable build manifest is not marked as the full edition."
}
$liteBuildInfo = Get-Content -LiteralPath ($liteExe + ".buildinfo.txt") -Raw
if ($liteBuildInfo -notmatch "(?m)^edition`tlite\r?$") {
    throw "Lite executable build manifest is not marked as the Lite edition."
}

$objdump = (Get-Command objdump -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($objdump)) {
    throw "objdump not found in PATH."
}
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($python)) {
    throw "python.exe not found in PATH."
}

Write-Host "Checking runtime imports..." -ForegroundColor Cyan
$imports = & $objdump -p $exe
if ($LASTEXITCODE -ne 0) {
    throw "objdump failed for $exe"
}
$zlibImportHits = @($imports | Select-String -SimpleMatch "DLL Name: zlib1.dll")
if ($zlibImportHits.Count -eq 0) {
    throw "pdf_note_workspace.exe does not import zlib1.dll"
}
$networkImportDlls = @(
    ("win" + "http.dll"),
    ("win" + "inet.dll"),
    ("url" + "mon.dll"),
    ("ws" + "2_32.dll"),
    ("wsock" + "32.dll"),
    ("web" + "socket.dll")
)
foreach ($networkDll in $networkImportDlls) {
    $networkImportHits = @($imports | Select-String -Pattern ("DLL Name:\s*" + [regex]::Escape($networkDll)))
    if ($networkImportHits.Count -gt 0) {
        throw "Network-capable DLL import found in app executable: $networkDll"
    }
}

Write-Host "Scanning out/bin for network-capable imports..." -ForegroundColor Cyan
& $python $binaryScan `
    --include out/bin `
    --imports-only `
    --imported-dll $networkImportDlls[0] `
    --imported-dll $networkImportDlls[1] `
    --imported-dll $networkImportDlls[2] `
    --imported-dll $networkImportDlls[3] `
    --imported-dll $networkImportDlls[4] `
    --imported-dll $networkImportDlls[5] `
    --fail-on-import
if ($LASTEXITCODE -ne 0) {
    throw "binary network import scan failed"
}

Write-Host "Checking release dry-run includes required runtime artifacts and licenses..." -ForegroundColor Cyan
$dryRun = & powershell -NoProfile -ExecutionPolicy Bypass -File $packRelease -DryRun -SkipFreshnessCheck
if ($LASTEXITCODE -ne 0) {
    throw "pack_release dry-run failed"
}
$readOnlyViewerDryRunHits = @($dryRun | Select-String -SimpleMatch "readonly_viewer.exe")
if ($readOnlyViewerDryRunHits.Count -eq 0) {
    throw "pack_release dry-run did not include readonly_viewer.exe"
}
$zlibDryRunHits = @($dryRun | Select-String -SimpleMatch "zlib1.dll")
if ($zlibDryRunHits.Count -eq 0) {
    throw "pack_release dry-run did not include zlib1.dll"
}
$zlibLicenseDryRunHits = @($dryRun | Select-String -SimpleMatch "licenses\zlib\zlib.txt")
if ($zlibLicenseDryRunHits.Count -eq 0) {
    throw "pack_release dry-run did not include zlib license text"
}
$sampleWorkspaceDryRunHits = @($dryRun | Select-String -SimpleMatch "sample_workspace")
if ($sampleWorkspaceDryRunHits.Count -eq 0) {
    throw "pack_release dry-run did not include sample workspace"
}
$sampleGuidePdf = Join-Path $repoRoot "release_assets\sample_workspace\講義サンプル\第01回_基本操作\使い方_基本操作.pdf"
if (-not (Test-Path -LiteralPath $sampleGuidePdf)) {
    throw "Sample workspace template is missing basic usage guide PDF: $sampleGuidePdf"
}
$sampleSpecPdf = Join-Path $repoRoot "release_assets\sample_workspace\講義サンプル\第02回_最初期構想\PDF学習ワークスペース統合画面構成および基本仕様書.pdf"
if (-not (Test-Path -LiteralPath $sampleSpecPdf)) {
    throw "Sample workspace template is missing specification PDF: $sampleSpecPdf"
}

Write-Host "Checking LibreOffice conversion runtime release contract..." -ForegroundColor Cyan
$defaultRuntime = Join-Path $repoRoot "third_party\libreoffice\custom_runtime\instdir"
if (Test-Path -LiteralPath (Join-Path $defaultRuntime "program\soffice.com")) {
    $loDryRun = & powershell -NoProfile -ExecutionPolicy Bypass -File $packRelease -DryRun -SkipFreshnessCheck
    if ($LASTEXITCODE -ne 0) {
        throw "default pack_release LibreOffice runtime dry-run failed"
    }
    $loRuntimeDryRunHits = @($loDryRun | Select-String -SimpleMatch "libreoffice\custom_runtime\instdir")
    if ($loRuntimeDryRunHits.Count -eq 0) {
        throw "default pack_release dry-run did not include LibreOffice custom runtime"
    }
    $loRuntimeSanitizeDryRunHits = @($loDryRun | Select-String -SimpleMatch "sanitize LibreOffice release runtime")
    if ($loRuntimeSanitizeDryRunHits.Count -eq 0) {
        throw "default pack_release dry-run did not sanitize LibreOffice custom runtime"
    }
}
else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $packRelease -DryRun -SkipFreshnessCheck | Out-Null
    if ($LASTEXITCODE -eq 0) {
        throw "default pack_release unexpectedly succeeded without a LibreOffice runtime"
    }
}

$savedNativeCommandErrorPreference = $null
$restoreNativeCommandErrorPreference = $false
$savedErrorActionPreference = $ErrorActionPreference
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $restoreNativeCommandErrorPreference = $true
    $savedNativeCommandErrorPreference = $PSNativeCommandUseErrorActionPreference
    $PSNativeCommandUseErrorActionPreference = $false
}
try {
    $ErrorActionPreference = "Continue"
    $noRuntimeOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $packRelease -DryRun -SkipFreshnessCheck -NoLibreOfficeRuntime 2>&1
    $noRuntimeExitCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $savedErrorActionPreference
    if ($restoreNativeCommandErrorPreference) {
        $PSNativeCommandUseErrorActionPreference = $savedNativeCommandErrorPreference
    }
}
if ($noRuntimeExitCode -eq 0) {
    throw "standard pack_release unexpectedly allowed a release without the LibreOffice conversion runtime"
}

$liteDryRun = & powershell -NoProfile -ExecutionPolicy Bypass -File $packRelease -DryRun -SkipFreshnessCheck -Lite
if ($LASTEXITCODE -ne 0) {
    throw "Lite pack_release dry-run failed"
}
$liteNameHits = @($liteDryRun | Select-String -Pattern "Release output: .*pdf_note_workspace_Lite_")
if ($liteNameHits.Count -eq 0) {
    throw "Lite pack_release dry-run did not create a Lite-named package"
}
$liteRuntimeHits = @($liteDryRun | Select-String -SimpleMatch "Including gated LibreOffice conversion runtime")
if ($liteRuntimeHits.Count -ne 0) {
    throw "Lite pack_release dry-run unexpectedly included the LibreOffice conversion runtime"
}
$liteDisabledHits = @($liteDryRun | Select-String -SimpleMatch "LibreOffice executable conversion runtime is not included")
if ($liteDisabledHits.Count -eq 0) {
    throw "Lite pack_release dry-run did not exclude the LibreOffice conversion runtime"
}
$liteArtifactHits = @($liteDryRun | Select-String -SimpleMatch "out\bin_lite\pdf_note_workspace.exe")
if ($liteArtifactHits.Count -eq 0) {
    throw "Lite pack_release dry-run did not use the Lite executable artifact"
}

Write-Host "All runtime dependency tests passed." -ForegroundColor Green
