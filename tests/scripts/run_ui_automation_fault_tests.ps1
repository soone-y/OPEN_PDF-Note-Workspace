[CmdletBinding()]
param(
    [switch]$ConfigOnly,
    [switch]$LogContractOnly,
    [switch]$ConfigRecoveryOnly,
    [switch]$ConfigUnknownFieldOnly,
    [switch]$SettingsBundleOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if (@(@($ConfigOnly, $LogContractOnly, $ConfigRecoveryOnly, $ConfigUnknownFieldOnly, $SettingsBundleOnly) | Where-Object { $_ }).Count -gt 1) {
    throw "Only one focused UI automation mode may be used at once."
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$binDir = Join-Path $repoRoot "out\\bin"
$exePath = Join-Path $binDir "pdf_note_workspace.exe"
$outDir = Join-Path $repoRoot "out\tests\ui_automation"
$workspaceRoot = Join-Path $outDir "workspace"
$fixtureSessionSource = Join-Path $repoRoot "tests\fixtures\ui_automation_session"
$resultFile = Join-Path $outDir "ui_automation_result.txt"
$traceFile = "$resultFile.trace.txt"
$noteStageDir = Join-Path $workspaceRoot "__resource__\__tmp__\__stage__\note"
$outputExportDir = Join-Path $workspaceRoot "__resource__\__tmp__\ui_automation_output"
$workspaceConfigPath = Join-Path $workspaceRoot "workspace.json"
$timeoutSec = 120

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Executable not found: $exePath"
}
if (-not ($ConfigOnly -or $ConfigRecoveryOnly -or $ConfigUnknownFieldOnly -or $SettingsBundleOnly) -and -not (Test-Path -LiteralPath $fixtureSessionSource)) {
    throw "Fixture session not found: $fixtureSessionSource"
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (Test-Path -LiteralPath $workspaceRoot) {
    Remove-Item -LiteralPath $workspaceRoot -Force -Recurse
}
New-Item -ItemType Directory -Force -Path (Join-Path $workspaceRoot "lecture1\session1") | Out-Null

$workspaceJson = @'
{
  "classesDir": ".",
  "noteRenderEnabled": true,
  "noteRawOnly": false,
  "noteRenderMath": true
}
'@
if ($ConfigUnknownFieldOnly) {
    $workspaceJson = '{"classesDir":".","language":"en","pdfBitmapBudgetMiB":32,"pointerOffsetX":17,"pointerOffsetY":-13,"debugLogPreviewTrace":true,"debugLogSwitchTiming":true,"debugLogCrash":false,"debugLogStartupWatchdog":false,"debugLogOfficeConversion":false,"futureVersionOption":{"keep":true}}'
} elseif ($ConfigRecoveryOnly) {
    $workspaceJson = '{"classesDir":".","language":"en"'
} elseif ($LogContractOnly) {
    $workspaceJson = @'
{
  "classesDir": ".",
  "debugLogPreviewTrace": true,
  "debugLogSwitchTiming": false,
  "debugLogCrash": true,
  "debugLogStartupWatchdog": false,
  "debugLogOfficeConversion": false
}
'@
}
Set-Content -LiteralPath (Join-Path $workspaceRoot "workspace.json") -Encoding UTF8 -Value $workspaceJson
$expectedCorruptWorkspaceJson = if ($ConfigRecoveryOnly) {
    Get-Content -LiteralPath (Join-Path $workspaceRoot "workspace.json") -Raw -Encoding UTF8
} else {
    ""
}
if (-not ($ConfigOnly -or $ConfigRecoveryOnly -or $ConfigUnknownFieldOnly -or $SettingsBundleOnly)) {
    $fixtureDestination = Join-Path $workspaceRoot "lecture1\session1"
    Get-ChildItem -LiteralPath $fixtureSessionSource -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $fixtureDestination -Recurse -Force
    }
    $fixturePdfSource = Join-Path $fixtureDestination "sample.pdf"
    $fixturePdfAlt = Join-Path $fixtureDestination "sample2.pdf"
    if ((Test-Path -LiteralPath $fixturePdfSource) -and -not (Test-Path -LiteralPath $fixturePdfAlt)) {
        Copy-Item -LiteralPath $fixturePdfSource -Destination $fixturePdfAlt -Force
    }
    # This sentinel is intentionally not a valid Office document. It only exercises the
    # missing-PDF prompt gate; automation must never attempt to display or convert it.
    Set-Content -LiteralPath (Join-Path $workspaceRoot "lecture1\office_prompt_guard.docx") -Encoding UTF8 -Value "UI automation prompt guard sentinel."
}

if (Test-Path -LiteralPath $resultFile) {
    Remove-Item -LiteralPath $resultFile -Force
}
if (Test-Path -LiteralPath $traceFile) {
    Remove-Item -LiteralPath $traceFile -Force
}

$instanceSuffix = "_" + [guid]::NewGuid().ToString("N")
$savedEnv = @{
    "PDF_NOTE_SMALL_UI_AUTOMATION" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION", "Process")
    "PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_RESULT_FILE" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_RESULT_FILE", "Process")
    "PDF_NOTE_SMALL_INSTANCE_SUFFIX" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_INSTANCE_SUFFIX", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_LOG_CONTRACT_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_LOG_CONTRACT_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_RECOVERY_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_RECOVERY_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY", "Process")
}

function Restore-Env {
    foreach ($entry in $savedEnv.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }
}

Write-Host "Running UI automation tests..." -ForegroundColor Cyan
try {
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION", "1", "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", $workspaceRoot, "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_RESULT_FILE", $resultFile, "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_INSTANCE_SUFFIX", $instanceSuffix, "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_ONLY", $(if ($ConfigOnly -or $ConfigUnknownFieldOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_LOG_CONTRACT_ONLY", $(if ($LogContractOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_RECOVERY_ONLY", $(if ($ConfigRecoveryOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY", $(if ($SettingsBundleOnly) { "1" } else { $null }), "Process")

    $proc = Start-Process -FilePath $exePath -WorkingDirectory $binDir -PassThru
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    if (-not $proc.HasExited) {
        try {
            $proc.Kill()
            $proc.WaitForExit()
        } catch {
        }
        throw "UI automation test timed out after $timeoutSec seconds."
    }

    if ($proc.ExitCode -ne 0) {
        throw "UI automation app exit code was $($proc.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $resultFile)) {
        throw "UI automation result file was not created."
    }

    $result = Get-Content -LiteralPath $resultFile -Raw
    if (-not $result.StartsWith("OK")) {
        throw ("UI automation reported failure:`n{0}" -f $result.Trim())
    }
    if (-not ($ConfigOnly -or $LogContractOnly -or $ConfigRecoveryOnly -or $ConfigUnknownFieldOnly -or $SettingsBundleOnly) -and
        (-not (Test-Path -LiteralPath $noteStageDir) -or
         -not (Get-ChildItem -LiteralPath $noteStageDir -File -ErrorAction SilentlyContinue))) {
        throw "UI automation did not preserve the staged-exit note diff."
    }
    if (-not ($ConfigOnly -or $LogContractOnly -or $ConfigRecoveryOnly -or $ConfigUnknownFieldOnly -or $SettingsBundleOnly)) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:output_export_ok$") {
            throw "UI automation did not complete the output export scenario."
        }
        $pdfExport = Join-Path $outputExportDir "export.pdf"
        $pngExport = Join-Path $outputExportDir "page.png"
        foreach ($path in @($pdfExport, $pngExport,
                            (Join-Path $outputExportDir "note.txt"),
                            (Join-Path $outputExportDir "note.md"),
                            (Join-Path $outputExportDir "note.html"))) {
            if (-not (Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0) {
                throw "UI automation export artifact is missing or empty: $path"
            }
        }
        $pdfBytes = [System.IO.File]::ReadAllBytes($pdfExport)
        if ($pdfBytes.Length -lt 5 -or [System.Text.Encoding]::ASCII.GetString($pdfBytes, 0, 5) -ne "%PDF-") {
            throw "UI automation PDF export does not have a PDF signature."
        }
        $pngBytes = [System.IO.File]::ReadAllBytes($pngExport)
        $pngSignature = [byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
        $pngSignatureMatches = $pngBytes.Length -ge $pngSignature.Length
        for ($i = 0; $i -lt $pngSignature.Length -and $pngSignatureMatches; ++$i) {
            $pngSignatureMatches = $pngBytes[$i] -eq $pngSignature[$i]
        }
        if (-not $pngSignatureMatches) {
            throw "UI automation PNG export does not have a PNG signature."
        }
    }
    if (-not $ConfigRecoveryOnly -and -not (Test-Path -LiteralPath $workspaceConfigPath)) {
        throw "UI automation did not leave a workspace.json file."
    }
    if (-not $ConfigRecoveryOnly) {
    try {
        $workspaceConfig = Get-Content -LiteralPath $workspaceConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "UI automation wrote an invalid workspace.json: $($_.Exception.Message)"
    }
    foreach ($property in @(
        "language", "pdfBitmapBudgetMiB", "pointerOffsetX", "pointerOffsetY",
        "debugLogPreviewTrace", "debugLogSwitchTiming", "debugLogCrash",
        "debugLogStartupWatchdog", "debugLogOfficeConversion"
    )) {
        if ($null -eq $workspaceConfig.PSObject.Properties[$property]) {
            throw "UI automation workspace.json is missing required property: $property"
        }
    }
    }
    if (-not ($LogContractOnly -or $ConfigRecoveryOnly -or $SettingsBundleOnly) -and $result -notmatch "(?m)^automation:workspace_config_roundtrip_ok$") {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:workspace_config_roundtrip_ok$") {
            throw "UI automation did not complete the workspace configuration round-trip scenario."
        }
    }
    if ($LogContractOnly) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:app_log_contract_ok$") {
            throw "UI automation did not complete the app-log contract scenario."
        }
        $logDir = Join-Path $workspaceRoot "__resource__\__log__"
        $token = "automation-log-contract"
        foreach ($name in @("preview_trace.log", "crash.log")) {
            $path = Join-Path $logDir $name
            if (-not (Test-Path -LiteralPath $path) -or
                (Get-Content -LiteralPath $path -Raw -Encoding UTF8) -notmatch [regex]::Escape($token)) {
                throw "Enabled app log did not contain the contract token: $name"
            }
        }
        foreach ($name in @("switch_timing.log", "startup_watchdog.log")) {
            $path = Join-Path $logDir $name
            if ((Test-Path -LiteralPath $path) -and
                (Get-Content -LiteralPath $path -Raw -Encoding UTF8) -match [regex]::Escape($token)) {
                throw "Disabled app log contained the contract token: $name"
            }
        }
    }
    if ($ConfigRecoveryOnly) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:workspace_config_recovery_ok$") {
            throw "UI automation did not complete the workspace configuration recovery scenario."
        }
        if (Test-Path -LiteralPath $workspaceConfigPath) {
            throw "Corrupt workspace.json was recreated during the recovery test."
        }
        $escapeFiles = Get-ChildItem -LiteralPath (Join-Path $workspaceRoot "__resource__\__escape__") -File -ErrorAction SilentlyContinue
        if (-not $escapeFiles) {
            throw "Corrupt workspace.json was not quarantined."
        }
        if (-not ($escapeFiles | Where-Object { (Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8) -eq $expectedCorruptWorkspaceJson })) {
            throw "Quarantined workspace.json did not preserve the original corrupt bytes."
        }
    }
    if ($ConfigUnknownFieldOnly -and
        (Get-Content -LiteralPath $workspaceConfigPath -Raw -Encoding UTF8) -notmatch '"futureVersionOption"\s*:\s*\{\s*"keep"\s*:\s*true\s*\}') {
        throw "An unknown workspace.json field was lost despite auto-persist protection."
    }
    if ($SettingsBundleOnly) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:settings_bundle_ok$") {
            throw "UI automation did not complete the settings bundle scenario."
        }
        $escapeDir = Join-Path $workspaceRoot "__resource__\__escape__"
        $bundleBackups = Get-ChildItem -LiteralPath $escapeDir -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "settings_import_*" -and (Test-Path -LiteralPath (Join-Path $_.FullName "manifest.txt")) }
        if (-not $bundleBackups) {
            throw "Settings bundle import did not leave a recovery backup."
        }
    }

    Write-Host "[PASS] UI automation" -ForegroundColor Green
}
finally {
    Restore-Env
}

Write-Host "All UI automation tests passed." -ForegroundColor Green
