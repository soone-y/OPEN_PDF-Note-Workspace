[CmdletBinding()]
param([string]$AppExecutable = "", [ValidateRange(30, 600)][int]$TimeoutSeconds = 240,
      [switch]$FontBoundaryOnly, [switch]$RegroupedOnly, [switch]$ExternalOnly,
      [ValidateSet("ja", "en")][string]$Locale = "ja")
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SettingsWorkflowProcessExit {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetExitCodeProcess(IntPtr handle, out uint exitCode);
}
'@
if ($FontBoundaryOnly -and $RegroupedOnly) { throw "Select one workflow subset." }
if ($ExternalOnly -and ($FontBoundaryOnly -or $RegroupedOnly)) { throw "Select one workflow subset." }
if ($Locale -eq "en" -and -not $RegroupedOnly) { throw "English UI checks require -RegroupedOnly; the full workflow remains JA-only." }
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $FontBoundaryOnly -and -not $RegroupedOnly -and -not $ExternalOnly) {
    & (Join-Path $PSScriptRoot 'run_workspace_identity_scope_tests.ps1')
}
$outDir = Join-Path $repoRoot "out/tests"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$binarySource = if ($AppExecutable) { (Get-Item -LiteralPath $AppExecutable -ErrorAction Stop).FullName }
    else { Join-Path $repoRoot $(if ($Locale -eq "en") { "out/bin_en/pdf_note_workspace.exe" } else { "out/bin/pdf_note_workspace.exe" }) }
if (-not (Test-Path -LiteralPath $binarySource -PathType Leaf)) { throw "Build the current $Locale Full app first: $binarySource" }
$buildInfo = "$binarySource.buildinfo.txt"
if (-not (Test-Path -LiteralPath $buildInfo)) { throw "Missing build-info: $buildInfo" }
$buildMetadata = Get-Content -LiteralPath $buildInfo -Raw
if ($buildMetadata -notmatch "(?m)^locale\s+$Locale\s*$" -or $buildMetadata -notmatch '(?m)^edition\s+full\s*$') {
    throw "The real UI workflow driver requires matching locale and Full build-info."
}
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$fixture = Join-Path $outDir ("settings_workflow_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $fixture -ErrorAction Stop | Out-Null
if (-not $FontBoundaryOnly) {
    $guideUnit = Join-Path $fixture "empty_view_guide_tests.exe"
    $guideLog = Join-Path $fixture "empty_view_guide_tests.log"
    & $compiler -std=c++17 -Wall -Wextra -Werror -static-libgcc -static-libstdc++ `
        -I (Join-Path $repoRoot "src") (Join-Path $repoRoot "tests/unit/empty_view_guide_tests.cpp") `
        (Join-Path $repoRoot "src/ui/empty_view_guide.cpp") -lgdi32 -o $guideUnit
    if ($LASTEXITCODE -ne 0) { throw "Empty guide component tests did not compile. Evidence: $fixture" }
    & $guideUnit *> $guideLog
    if ($LASTEXITCODE -ne 0) { Get-Content $guideLog; throw "Empty guide component tests failed. Evidence: $fixture" }
    $guideSummary = Get-Content $guideLog -Raw
    if ($guideSummary -notmatch '^Empty view guide: [1-9][0-9]* checks passed\s*$') {
        throw "Empty guide component completion marker missing. Evidence: $fixture"
    }
    Write-Host $guideSummary.Trim()
}
$driver = Join-Path $fixture "settings_workflow_app_tests.exe"
& $compiler -std=c++17 -Wall -Wextra -Werror -static-libgcc -static-libstdc++ `
    -I (Join-Path $repoRoot "src") -I (Join-Path $repoRoot "third_party/pdfium/include") `
    (Join-Path $repoRoot "tests/integration/settings_workflow_app_tests.cpp") `
    (Join-Path $repoRoot "src/clrop/json.cpp") -o $driver
if ($LASTEXITCODE -ne 0) { throw "Settings workflow driver compilation failed." }
# Never delete/reuse past test evidence. All app/setup/import/output paths stay
# under this new fixture; the driver's nested job kills its children on timeout.
$appDir = Join-Path $fixture "app"
$ownedAppName = 'pdf_note_settings_workflow_' + [IO.Path]::GetFileName($fixture) + '.exe'
New-Item -ItemType Directory -Path $appDir -ErrorAction Stop | Out-Null
# A unique image name also isolates other-package startup diagnostics, whose
# process scan is session-wide while window enumeration is desktop-local.
Copy-Item -LiteralPath $binarySource -Destination (Join-Path $appDir $ownedAppName)
Get-ChildItem -LiteralPath (Split-Path -Parent $binarySource) -Filter "*.dll" -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $appDir
}
# Verify the actual isolated bytes, not just the source's name/locale. A build
# being replaced during copying or a missing runtime must fail before startup.
$artifacts = [regex]::Matches($buildMetadata, '(?m)^artifact\t([^\t\r\n]+)\t([0-9a-fA-F]{64})\s*$')
if ($artifacts.Count -lt 2) { throw "Build-info has no complete runtime artifact set." }
$hasExecutable = $false
foreach ($artifact in $artifacts) {
    $name = $artifact.Groups[1].Value
    if ([IO.Path]::GetFileName($name) -ne $name) { throw "Invalid artifact name in build-info: $name" }
    $isExecutable = $name -eq [IO.Path]::GetFileName($binarySource)
    if ($isExecutable) { $hasExecutable = $true }
    $copied = Join-Path $appDir $(if ($isExecutable) { $ownedAppName } else { $name })
    if (-not (Test-Path -LiteralPath $copied -PathType Leaf) -or
        (Get-FileHash -LiteralPath $copied -Algorithm SHA256).Hash -ne $artifact.Groups[2].Value) {
        throw "Missing/changed copied runtime artifact: $name. Evidence: $fixture"
    }
}
if (-not $hasExecutable) { throw "Build-info does not identify the selected executable." }
Copy-Item -LiteralPath $buildInfo -Destination (Join-Path $fixture "source.buildinfo.txt")
$oldRoot = [Environment]::GetEnvironmentVariable("WORKSPACE_MEMO_APP_TEST_ROOT", "Process")
$stdout = Join-Path $fixture "driver.stdout.txt"
$stderr = Join-Path $fixture "driver.stderr.txt"
$receipt = Join-Path $fixture "execution.txt"
$logWarningBytes = 1MB
$logLimitBytes = 16MB
function Get-DriverLogBytes {
    $total = [int64]0
    foreach ($path in @($stdout, $stderr)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $total += (Get-Item -LiteralPath $path).Length
        }
    }
    return $total
}
function Write-DriverFailureTail {
    foreach ($path in @($stdout, $stderr)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Write-Host "Last 30 lines: $path"
            Get-Content -LiteralPath $path -Tail 30 | ForEach-Object { Write-Host $_ }
        }
    }
}
@("execution=verified_test_desktop; real UI and child messages are required",
  "test_station=$env:PDF_NOTE_TEST_STATION", "test_desktop=$env:PDF_NOTE_TEST_DESKTOP",
  "interactive_notice=$env:PDF_NOTE_TEST_INTERACTIVE",
  "source_executable=$binarySource", "owned_executable=$ownedAppName", "copied_executable_sha256=$((Get-FileHash -LiteralPath (Join-Path $appDir $ownedAppName) -Algorithm SHA256).Hash)",
  "driver_sha256=$((Get-FileHash -LiteralPath $driver -Algorithm SHA256).Hash)",
  "git_head=$(& git -C $repoRoot rev-parse HEAD)", "power_loss_test=false; explicit process termination at durable checkpoints") |
    Set-Content -LiteralPath $receipt -Encoding UTF8
Write-Host "Settings/workspace workflow tests: verified test desktop; disposable children only. Evidence: $fixture"
try {
    [Environment]::SetEnvironmentVariable("WORKSPACE_MEMO_APP_TEST_ROOT", $fixture, "Process")
    $startOptions = @{ FilePath = $driver; WorkingDirectory = $repoRoot; WindowStyle = "Hidden";
        RedirectStandardOutput = $stdout; RedirectStandardError = $stderr; PassThru = $true }
    if ($FontBoundaryOnly) { $startOptions.ArgumentList = @("--font-boundary-only") }
    if ($RegroupedOnly) { $startOptions.ArgumentList = @("--regrouped-only") }
    if ($ExternalOnly) { $startOptions.ArgumentList = @("--external-only") }
    $process = Start-Process @startOptions
    $processHandle = $process.Handle
    if (-not $processHandle) { throw "Settings workflow process handle unavailable at start. Evidence: $fixture" }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
        $process.WaitForExit(1000) | Out-Null
        if ((Get-DriverLogBytes) -gt $logLimitBytes) {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
            throw "Settings workflow log exceeded 16 MiB; test stopped without truncating evidence. Evidence: $fixture"
        }
    }
    if (-not $process.HasExited) {
        $process.Kill(); $process.WaitForExit(5000) | Out-Null
        throw "Settings workflow tests timed out; job-owned children were terminated. Evidence: $fixture"
    }
    $process.WaitForExit()
    [uint32]$processExitCode = 259
    if (-not [SettingsWorkflowProcessExit]::GetExitCodeProcess($processHandle, [ref]$processExitCode) -or
        $processExitCode -eq 259) {
        throw "Settings workflow exit status unavailable. Evidence: $fixture"
    }
    $logBytes = Get-DriverLogBytes
    if ($logBytes -gt $logLimitBytes) {
        throw "Settings workflow log exceeded 16 MiB. Evidence: $fixture"
    }
    if ($logBytes -gt $logWarningBytes) {
        Write-Warning "Settings workflow log exceeds 1 MiB ($logBytes bytes). Evidence: $fixture"
    }
    if ($processExitCode -ne 0) {
        Write-DriverFailureTail
        throw "Settings workflow tests failed (exit=$processExitCode). Evidence: $fixture"
    }
    $driverOutput = Get-Content -LiteralPath $stdout -Raw
    if (-not $FontBoundaryOnly -and -not $ExternalOnly -and $driverOutput -notmatch '\[PASS\] marker display defaults / cancel / failed save / restart / old data preserved') {
        throw "Marker display default workflow completion marker missing. Evidence: $fixture"
    }
    $markers = if ($ExternalOnly) { @() } elseif ($RegroupedOnly) { @("[PASS] six-group navigation / drafts / close choices / quick preferences / validation / restart") } elseif ($FontBoundaryOnly) { @("[PASS] overlong configured note font starts and exits normally") } else {
        @("[PASS] multi-tab cancel/revert/explicit failure/palette auto-save/restart",
          "[PASS] six-group navigation / drafts / close choices / quick preferences / validation / restart",
          "[PASS] bottom-pane persistent default / temporary View / dialog precedence / failure / restart",
          "[PASS] opening preferences / cancel / revert / restart / actual note creation / invalid values",
          "[PASS] overlong configured note font starts and exits normally",
          "[PASS] multi-file settings kill/restart / exact partial-save contract")
    }
    if ($ExternalOnly) { $markers = @() }
    if (-not $FontBoundaryOnly -and -not $RegroupedOnly) {
        foreach ($placement in @("root", "separate")) {
            if (-not $ExternalOnly) { $markers += "[PASS] native import/edit/organize/restart/output / $placement" }
            $markers += "[PASS] external organize / paired rename / undo / redo / restart / $placement"
            foreach ($point in @("operation_before_moves", "operation_after_first_move", "operation_after_all_moves", "operation_committed")) {
                if (-not $ExternalOnly) { $markers += "[PASS] real kill/restart $point / $placement" }
                $markers += "[PASS] external real kill/restart $point / $placement"
            }
        }
    }
    if (-not $FontBoundaryOnly -and -not $ExternalOnly) {
        $markers = @($markers) + @("[PASS] central guide / modes / rollback / custom bindings / scrolling / restart / PDF transition")
    }
    foreach ($marker in $markers) {
        if ([regex]::Matches($driverOutput, [regex]::Escape($marker)).Count -ne 1) {
            throw "Required workflow marker missing/duplicated: $marker. Evidence: $fixture"
        }
    }
    $summary = [regex]::Match($driverOutput, 'Settings/workspace workflows: [1-9][0-9]* checks passed\.')
    if (-not $summary.Success) {
        throw "Workflow driver did not report completed checks. Evidence: $fixture"
    }
    Write-Host $summary.Value
    Write-Host "Driver output: $logBytes bytes; stdout: $stdout; stderr: $stderr"
    if (-not $FontBoundaryOnly -and -not $RegroupedOnly -and -not $ExternalOnly) {
        & python -B (Join-Path $repoRoot "tests/python/verify_settings_workflow_exports.py") $fixture
        if ($LASTEXITCODE -ne 0) { throw "Independent PDF verification failed. Evidence: $fixture" }
    }
    Write-Host "[PASS] Settings/workspace workflows (process termination is not a physical power-loss test)."
} finally {
    [Environment]::SetEnvironmentVariable("WORKSPACE_MEMO_APP_TEST_ROOT", $oldRoot, "Process")
    Write-Host "Workflow logs: $stdout; $stderr; receipt: $receipt"
}
