[CmdletBinding()]
param(
    [switch]$ConfigOnly,
    [switch]$LogContractOnly,
    [switch]$ConfigRecoveryOnly,
    [switch]$ConfigUnknownFieldOnly,
    [switch]$SettingsBundleOnly,
    [switch]$HelpVisibilityOnly,
    [switch]$DialogOwnerVisibilityOnly,
    [switch]$OutputExportOnly,
    [switch]$TargetSessionOnly,
    [switch]$PdfOnly,
    [switch]$PdfiumRawOnly,
    [switch]$MathRenderOnly,
    [switch]$RenderedClickOnly,
    [switch]$ImeGeometryOnly,
    [switch]$OpenWorkOnly,
    [ValidateRange(0, 5000)]
    [int]$OpenWorkNoteLines = 0,
    [switch]$InputAssistOnly,
    [switch]$SkipPngExport,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,80}$')]
    [string]$ArtifactName = "ui_automation",
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,80}$')]
    [string]$ExecutableName = "pdf_note_workspace",
    [ValidateRange(30, 600)]
    [int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$focusedModeSwitches = @(
    $ConfigOnly, $LogContractOnly, $ConfigRecoveryOnly, $ConfigUnknownFieldOnly,
    $SettingsBundleOnly, $HelpVisibilityOnly, $DialogOwnerVisibilityOnly,
    $OutputExportOnly, $TargetSessionOnly, $PdfOnly, $PdfiumRawOnly,
    $MathRenderOnly, $InputAssistOnly, $OpenWorkOnly
)
$selectedFocusedModes = @($focusedModeSwitches | Where-Object { $_ })
if ($selectedFocusedModes.Count -gt 1) {
    throw "Only one focused UI automation mode may be used at once."
}
$fullSuite = $selectedFocusedModes.Count -eq 0
# Only these isolated configuration/dialog probes intentionally omit the
# reusable session fixture. Keep this distinct from the full-suite decision.
$requiresSessionFixture = -not ($ConfigOnly -or $ConfigRecoveryOnly -or
    $ConfigUnknownFieldOnly -or $SettingsBundleOnly -or $HelpVisibilityOnly -or
    $DialogOwnerVisibilityOnly)
if ($SkipPngExport -and -not $OutputExportOnly) {
    throw "-SkipPngExport may only be used together with -OutputExportOnly."
}
if ($RenderedClickOnly -and -not $MathRenderOnly) {
    throw "-RenderedClickOnly requires -MathRenderOnly and selects only its click/insertion/undo probe."
}
if ($ImeGeometryOnly -and (-not $MathRenderOnly -or $RenderedClickOnly)) {
    throw "-ImeGeometryOnly requires -MathRenderOnly and cannot be combined with -RenderedClickOnly."
}
if ($OpenWorkNoteLines -ne 0 -and -not $OpenWorkOnly) {
    throw "-OpenWorkNoteLines requires -OpenWorkOnly and changes only its disposable note fixture."
}

function Assert-PdfTextboxReeditFontTrace {
    param([string]$TraceText)
    if ($TraceText -notmatch '(?m)^pdf_textbox:presets_theme_ok cases=4$') {
        throw 'Textbox A/B preset application and inactive-slot isolation were not verified across native and themed UI.'
    }
    if ($TraceText -notmatch '(?m)^pdf_textbox:reedit_hover_ok cases=432$') {
        throw 'Textbox reedit hover did not verify insertion positions and read-only state.'
    }
    if ($TraceText -notmatch '(?m)^pdf_textbox:geometry_ok cases=192$') {
        throw 'PDF textbox geometry did not verify all font/size/zoom and unclipped reference-ink cases.'
    }
    if ($TraceText -notmatch '(?m)^pdf_textbox:reedit_font_ok cases=6 cycles=48$' -or
        [regex]::Matches($TraceText, '(?m)^pdf_textbox:reedit_font_case_ok$').Count -ne 6 -or
        [regex]::Matches($TraceText, '(?m)^pdf_textbox:reedit_font_observation .* valid=1$').Count -ne 48) {
        throw 'PDF textbox reedit did not preserve font size through every repeated edit case. Rebuild the executable before rerunning.'
    }
}

function Assert-ImeGeometryTrace {
    param([string]$TraceText, [switch]$Focused)
    if ([regex]::Matches($TraceText, '(?m)^math_render:ime_candidate_requests_valid=1$').Count -lt 3 -or
        [regex]::Matches($TraceText, '(?m)^math_render:ime_character_query valid=1 ').Count -lt 2 -or
        $TraceText -notmatch '(?m)^math_render:ime_offscreen_candidate_unchanged=1 ' -or
        $TraceText -notmatch '(?m)^math_render:ime_visible_prerequisite_recovery=1$' -or
        $TraceText -notmatch '(?m)^math_render:ime_heading_band_valid=1 ') {
        throw "IME geometry automation did not observe every native/final/heading/offscreen request check. Rebuild the executable before rerunning."
    }
    if ($Focused -and
        ($TraceText -notmatch '(?m)^automation:ime_geometry_ok$' -or
         $TraceText -notmatch '(?m)^math_render:ime_geometry_only$')) {
        throw "Focused IME geometry automation did not complete its selected subset."
    }
}

. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
$privateClipboard = $fullSuite
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters -PrivateClipboard:$privateClipboard) { return }
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$binDir = Join-Path $repoRoot "out\\bin"
$exePath = Join-Path $binDir ($ExecutableName + ".exe")
# Each explicit run alias owns its fixture and evidence, so focused checks
# can retain previous results and avoid another runner's active workspace.
$outDir = Join-Path $repoRoot ("out\tests\" + $ArtifactName)
$workspaceRoot = Join-Path $outDir "workspace"
$fixtureSessionSource = Join-Path $repoRoot "tests\fixtures\ui_automation_session"
$resultFile = Join-Path $outDir "ui_automation_result.txt"
$traceFile = "$resultFile.trace.txt"
$noteStageDir = Join-Path $workspaceRoot "__pdf_note_workspace__\__tmp__\__stage__\note"
$outputExportDir = Join-Path $workspaceRoot "__pdf_note_workspace__\__tmp__\ui_automation_output"
$workspaceConfigPath = Join-Path $workspaceRoot "workspace.json"
# Keep the standard watchdog unchanged; longer suites can explicitly select
# a bounded budget without disabling timeout cleanup or any assertion.
$timeoutSec = $TimeoutSeconds

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Executable not found: $exePath"
}
if ($requiresSessionFixture -and -not (Test-Path -LiteralPath $fixtureSessionSource)) {
    throw "Fixture session not found: $fixtureSessionSource"
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (((Get-Item -LiteralPath $outDir).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Refusing to use a reparse-point automation output directory."
}
if (Test-Path -LiteralPath $workspaceRoot) {
    $automationTarget = [System.IO.Path]::GetFullPath($workspaceRoot)
    $automationParent = [System.IO.Path]::GetFullPath($outDir) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $automationTarget.StartsWith($automationParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        (((Get-Item -LiteralPath $workspaceRoot).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Refusing to remove an automation workspace outside its owned output directory."
    }
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
} elseif ($MathRenderOnly -or $InputAssistOnly -or $OpenWorkOnly) {
    # The focused final-render probe keeps a local, disposable event trace so
    # a failed publication can be diagnosed without the clipboard suite.
    $workspaceJson = '{"classesDir":".","noteRenderEnabled":true,"noteRawOnly":false,"noteRenderMath":true,"debugLogPreviewTrace":true}'
}
Set-Content -LiteralPath (Join-Path $workspaceRoot "workspace.json") -Encoding UTF8 -Value $workspaceJson
$expectedCorruptWorkspaceJson = if ($ConfigRecoveryOnly) {
    Get-Content -LiteralPath (Join-Path $workspaceRoot "workspace.json") -Raw -Encoding UTF8
} else {
    ""
}
if ($requiresSessionFixture) {
    $fixtureDestination = Join-Path $workspaceRoot "lecture1\session1"
    # The bundled first-session note has raw markup wider than its composed
    # surface. Maximization can hide the final horizontal bar while native
    # RichEdit still needs it; short synthetic notes do not exercise this.
    # Keep this script ASCII-compatible with Windows PowerShell 5.1.
    $resizeSampleRelative = [regex]::Unescape('release_assets/sample_workspace/ja/01_\u8b1b\u7fa9\u30b5\u30f3\u30d7\u30eb/\u7b2c01\u56de_\u57fa\u672c\u64cd\u4f5c/\u30ce\u30fc\u30c8_\u57fa\u672c\u64cd\u4f5c.clro')
    $resizeSample = Join-Path $repoRoot $resizeSampleRelative
    Copy-Item -LiteralPath $resizeSample -Destination (Join-Path $fixtureDestination "note_resize_sample.clro")
    Get-ChildItem -LiteralPath $fixtureSessionSource -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $fixtureDestination -Recurse -Force
    }
    $fixturePdfSource = Join-Path $fixtureDestination "sample.pdf"
    $fixturePdfAlt = Join-Path $fixtureDestination "sample2.pdf"
    if ((Test-Path -LiteralPath $fixturePdfSource) -and -not (Test-Path -LiteralPath $fixturePdfAlt)) {
        Copy-Item -LiteralPath $fixturePdfSource -Destination $fixturePdfAlt -Force
    }
    if ($OpenWorkOnly -and $OpenWorkNoteLines -gt 0) {
        $longNote = [System.Text.StringBuilder]::new()
        for ($lineIndex = 0; $lineIndex -lt $OpenWorkNoteLines; ++$lineIndex) {
            [void]$longNote.Append("Open work fixture line $lineIndex : unchanged text across selection.`r`n")
        }
        Set-Content -LiteralPath (Join-Path $fixtureDestination "note1.txt") -Encoding UTF8 -NoNewline -Value $longNote.ToString()
        Add-Content -LiteralPath (Join-Path $fixtureDestination "note_math.md") -Encoding UTF8 -Value $longNote.ToString()
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

# Every UI mode owns its app directory as well as its workspace.
# Keep the developer's setup and running copy outside the test boundary.
$sourceExe = $exePath
$sourceBinDir = $binDir
# Preserve the application's existing two-ancestor bundled-runtime lookup.
$binDir = Join-Path (Join-Path $repoRoot "out") ("app_" + $ArtifactName + "_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $binDir -ErrorAction Stop | Out-Null
$exePath = Join-Path $binDir ([IO.Path]::GetFileName($sourceExe))
Copy-Item -LiteralPath $sourceExe -Destination $exePath -ErrorAction Stop
foreach ($dll in Get-ChildItem -LiteralPath $sourceBinDir -Filter "*.dll" -File) {
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $binDir $dll.Name) -ErrorAction Stop
}
if ((Get-FileHash -LiteralPath $sourceExe -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash) {
    throw "The isolated automation executable differs from its build source."
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
    "PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_UNKNOWN_FIELD_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_UNKNOWN_FIELD_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_HELP_VISIBILITY_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_HELP_VISIBILITY_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_DIALOG_OWNER_VISIBILITY_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_DIALOG_OWNER_VISIBILITY_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_OUTPUT_EXPORT_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_OUTPUT_EXPORT_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_SKIP_PNG_EXPORT" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_SKIP_PNG_EXPORT", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_TARGET_SESSION_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_TARGET_SESSION_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_PDF_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_PDF_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_PDFIUM_RAW_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_PDFIUM_RAW_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_INPUT_ASSIST_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_INPUT_ASSIST_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_MATH_RENDER_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_MATH_RENDER_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_RENDERED_CLICK_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_RENDERED_CLICK_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_IME_GEOMETRY_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_IME_GEOMETRY_ONLY", "Process")
    "PDF_NOTE_SMALL_UI_AUTOMATION_OPEN_WORK_ONLY" = [Environment]::GetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_OPEN_WORK_ONLY", "Process")
}

function Restore-Env {
    foreach ($entry in $savedEnv.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }
}

$backgroundOnly = $ConfigOnly -or $LogContractOnly -or $ConfigRecoveryOnly -or $ConfigUnknownFieldOnly -or $PdfiumRawOnly
$executionMode = if ($backgroundOnly) { "background; foreground acquisition is a failure" } else { "UI-dependent checks on the verified test desktop" }
Write-Host "Running UI automation tests ($executionMode)..." -ForegroundColor Cyan
try {
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION", "1", "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", $workspaceRoot, "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_RESULT_FILE", $resultFile, "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_INSTANCE_SUFFIX", $instanceSuffix, "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_ONLY", $(if ($ConfigOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_LOG_CONTRACT_ONLY", $(if ($LogContractOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_RECOVERY_ONLY", $(if ($ConfigRecoveryOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_UNKNOWN_FIELD_ONLY", $(if ($ConfigUnknownFieldOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY", $(if ($SettingsBundleOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_HELP_VISIBILITY_ONLY", $(if ($HelpVisibilityOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_DIALOG_OWNER_VISIBILITY_ONLY", $(if ($DialogOwnerVisibilityOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_OUTPUT_EXPORT_ONLY", $(if ($OutputExportOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_SKIP_PNG_EXPORT", $(if ($SkipPngExport) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_TARGET_SESSION_ONLY", $(if ($TargetSessionOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_PDF_ONLY", $(if ($PdfOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_PDFIUM_RAW_ONLY", $(if ($PdfiumRawOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_INPUT_ASSIST_ONLY", $(if ($InputAssistOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_MATH_RENDER_ONLY", $(if ($MathRenderOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_RENDERED_CLICK_ONLY", $(if ($RenderedClickOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_IME_GEOMETRY_ONLY", $(if ($ImeGeometryOnly) { "1" } else { $null }), "Process")
    [Environment]::SetEnvironmentVariable("PDF_NOTE_SMALL_UI_AUTOMATION_OPEN_WORK_ONLY", $(if ($OpenWorkOnly) { "1" } else { $null }), "Process")

    if ($backgroundOnly) {
        $backgroundRunner = Join-Path $repoRoot "tests\python\run_background_app.py"
        & python -B $backgroundRunner --cwd $binDir --timeout $timeoutSec -- $exePath
        if ($LASTEXITCODE -ne 0) {
            throw "Background automation failed (exit=$LASTEXITCODE)."
        }
    } else {
        # CreateProcess inherits the verified desktop. The app deliberately shows
        # its main window there; no shell-mediated launch may escape to the input desktop.
        $start = New-Object System.Diagnostics.ProcessStartInfo
        $start.FileName = $exePath; $start.WorkingDirectory = $binDir
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true
        $proc = [Diagnostics.Process]::Start($start)
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
            if ((Test-Path -LiteralPath $resultFile -PathType Leaf) -and
                (Get-Item -LiteralPath $resultFile).Length -le 64KB) {
                Get-Content -LiteralPath $resultFile -TotalCount 12 | ForEach-Object { Write-Host $_ }
            }
            throw "UI automation app exit code was $($proc.ExitCode). Result: $resultFile; trace: $traceFile"
        }
    }
    if (-not (Test-Path -LiteralPath $resultFile)) {
        throw "UI automation result file was not created."
    }

    $result = Get-Content -LiteralPath $resultFile -Raw
    if (-not $result.StartsWith("OK")) {
        throw ("UI automation reported failure:`n{0}" -f $result.Trim())
    }
    if ($MathRenderOnly -or $OpenWorkOnly) {
        # A frame published before WM_PAINT is not sufficient evidence: a
        # failed paint withdraws it and silently switches to native raw.
        # Include startup's empty view as well as the opened-note scenario.
        $paintTracePath = Join-Path $workspaceRoot "__pdf_note_workspace__\__log__\preview_trace.log"
        if (-not (Test-Path -LiteralPath $paintTracePath)) {
            throw "The focused rendering test did not create its required paint trace."
        }
        if (Select-String -LiteralPath $paintTracePath -Pattern '\[FinalNoteRenderPaint\] failed' -Quiet) {
            throw "A published final frame failed actual paint; see the local preview trace."
        }
        if (Select-String -LiteralPath $paintTracePath -Pattern '\[FinalNoteGeometryReplay\] exhausted' -Quiet) {
            throw "Final scrollbar/client geometry did not converge; see the local preview trace."
        }
    }
    $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
    if ($fullSuite -and $traceText -notmatch '(?m)^automation:empty_clro_delete_ok new=1 unsaved_blocked=1 saved=1$') {
        throw 'Empty CLRO deletion did not verify newly created, unsaved and explicitly saved notes.'
    }
    if ($RenderedClickOnly -and ($traceText -notmatch "(?m)^math_render:block_title_clicks_ok$" -or
        $traceText -notmatch "(?m)^math_render:fullwidth_click_insertion_undo_ok$")) {
        throw "Rendered click automation did not verify container and code titles. Rebuild the executable."
    }
    $expectedExecution = if ($backgroundOnly) { "background_hidden" } else { "foreground_allowed" }
    if ($traceText -notmatch "(?m)^automation:execution_$expectedExecution$") {
        throw "UI automation did not confirm the required execution mode: $expectedExecution."
    }
    if ($traceText -notmatch "(?m)^automation:toolbar_fonts_ok$") {
        throw "UI automation did not verify the common font of every toolbar control."
    }
    if ($fullSuite -and $traceText -notmatch "(?m)^automation:organize_dirty_annotations_ok$") {
        throw "UI automation did not verify unsaved annotation preservation during file organization."
    }
    if ($PdfOnly -or $fullSuite) {
        if ($traceText -notmatch '(?m)^pdf_ime:prerequisite native_control=1 native_context=1$') {
            throw 'PDF IME prerequisites were not available or not observed; this is not a completed PDF/IME check.'
        }
        Assert-PdfTextboxReeditFontTrace -TraceText $traceText
        if ($traceText -notmatch '(?m)^annotation_shortcuts:independent_ok cases=34$') {
            throw 'Direct annotation shortcuts did not complete all hidden/disabled tool cases and persistence checks.'
        }
        if ($traceText -notmatch '(?m)^annotation_shortcuts:selector_ok cases=688$') {
            throw 'Shortcut key catalog did not round-trip all 86 keys and eight modifier combinations through the persistence parser.'
        }
        if ($traceText -notmatch '(?m)^application_shortcuts:policy_ok cases=26$') {
            throw 'Shared main/reader shortcut surface, modifier, repeat and text-input policy checks did not complete.'
        }
        if ($traceText -notmatch '(?m)^application_shortcuts:close_guard_ok cases=8$') {
            throw 'Closing shortcut safety-modifier and retired-alias checks did not complete.'
        }
        if ($traceText -notmatch '(?m)^application_shortcuts:page_number_ok cases=9$') {
            throw 'Page-number validation and failure output-preservation checks did not complete.'
        }
        if ($traceText -notmatch '(?m)^pdf_rotation:canonical_edit_save_undo_export_ok cases=4$' -or
            [regex]::Matches($traceText, '(?m)^pdf_rotation:case_ok turn=[0-3]$').Count -ne 4) {
            throw 'Page rotation editing, sidecar, undo/redo or PDF output checks did not complete.'
        }
        if ($traceText -notmatch '(?m)^pdf_rotation:all_pages_ok pages=(\d+)$' -or [int]$Matches[1] -lt 2) {
            throw 'All-page rotation, reset, cache rebuild or annotation preservation checks did not complete.'
        }
        if ([regex]::Matches($traceText, '(?m)^pdf_annotation_display:app_ok turn=[0-3] cache_hits=2 bytes=\d+ probe_ms=\d+$').Count -ne 4) {
            throw 'Annotation display and text coverage reuse did not complete in every rotation.'
        }
        if ([regex]::Matches($traceText, '(?m)^pdf_marker_display:app_ok turn=[0-3] rect=1 quads=1 colors=3$').Count -ne 4) {
            throw 'Text marker multiply composition did not preserve dark text in every rotation and geometry.'
        }
        if ([regex]::Matches($traceText, '(?m)^pdf_marker_modes:app_ok turn=[0-3] legacy=1 multiply=1 v1=1 v2=1 omitted=1$').Count -ne 4) {
            throw 'Marker display choices or version-aware migration did not complete in every rotation.'
        }
        if ($traceText -notmatch '(?m)^annotation_blend_switch:app_ok toolbar=1 inspector=1 v2=1 undo=1 render=1$') {
            throw 'Annotation blend toolbar, inspector, persistence, undo or rendering did not complete.'
        }
        if ($traceText -notmatch '(?m)^automation:pdf_active_edit_save_ok$') {
            throw 'PDF active edit and background-assist staging check did not complete.'
        }
        if ($traceText -notmatch '(?m)^pdf_line:chained_style_ok cases=24 segments=72$') {
            throw "PDF Ctrl/Enter placement did not preserve the selected line type and dash pattern."
        }
    }
    if ($InputAssistOnly -and $traceText -notmatch "(?m)^automation:input_assist_ok$") {
        throw "Input assist automation completion marker was not found."
    }
    if ($InputAssistOnly) {
        foreach ($marker in @("notice_timer_failure_recovery_ok", "notice_click_dismissal_ok", "all_guidance_dismissal_ok", "notice_owner_isolation_ok", "font_geometry_matrix_ok", "real_ime_audio_os_dpi_not_observed")) {
            if ($traceText -notmatch ("(?m)^input_assist:" + [regex]::Escape($marker) + '$')) {
                throw "Input assist automation did not complete its required check: $marker. Rebuild the executable before rerunning."
            }
        }
        if ([regex]::Matches($traceText, '(?m)^input_assist:font_geometry_case_ok ').Count -ne 27 -or
            [regex]::Matches($traceText, '(?m)^input_assist:notice_font_fit_ok ').Count -ne 3) {
            throw "Input assist font/geometry matrix or scaled notice checks were incomplete."
        }
        Write-Host "Real IME operation, audible silence, and OS DPI changes were not observed by this automated suite." -ForegroundColor Yellow
    }
    if (($OpenWorkOnly -or $fullSuite) -and
        ($traceText -notmatch "(?m)^automation:open_work_ok$" -or
         $traceText -notmatch "(?m)^open_work:content_stage_undo_redo_ok$" -or
         $traceText -notmatch "(?m)^open_work:failed_note_load_metadata_ok$" -or
         $traceText -notmatch "(?m)^open_work:rejected_note_open_result_ok$" -or
         $traceText -notmatch "(?m)^open_work:external_reload_retry_ok$" -or
         $traceText -notmatch "(?m)^open_work:external_copy_retry_ok$" -or
         $traceText -notmatch "(?m)^open_work:source_protection_close_ok$" -or
         $traceText -notmatch "(?m)^open_work:source_protection_recovery_ok$" -or
         $traceText -notmatch "(?m)^open_work:parse_reuse_stage_external_ok$" -or
         $traceText -notmatch "(?m)^open_work:pdf_reopen_history_failure_ok$" -or
         $traceText -notmatch "(?m)^open_work:paired_auto_open_ok$" -or
         $traceText -notmatch "(?m)^open_work:paired_pdf_failure_ok$" -or
         $traceText -notmatch "(?m)^open_work:paired_note_failure_ok$" -or
         $traceText -notmatch "(?m)^open_work:paired_long_complex_ok$" -or
         $traceText -notmatch "(?m)^open_work:pdf_worker_concurrency_ok$" -or
         $traceText -notmatch "(?m)^automation:pdf_worker_faults_ok render=1 publish=1 recovery=1$" -or
         $traceText -notmatch "(?m)^open_work:startup_restore_ok$" -or
         $traceText -notmatch "(?m)^open_work:pdf_initial_viewport_ok$")) {
        throw "Open work automation did not verify checkpoint counts and content/history preservation."
    }
    if ($RenderedClickOnly -and
        ($traceText -notmatch "(?m)^automation:rendered_click_ok$" -or
         $traceText -notmatch "(?m)^math_render:rendered_click_only$" -or
         $traceText -notmatch "(?m)^math_render:rendered_click_insertion_ok$")) {
        throw "Rendered click-only automation did not verify insertion and undo."
    }
    if ($MathRenderOnly -and -not $RenderedClickOnly -and -not $ImeGeometryOnly -and $traceText -notmatch "(?m)^automation:math_render_ok$") {
        throw "Final rendering automation did not complete its math/resize responsiveness probe."
    }
    if ($MathRenderOnly -and -not $RenderedClickOnly) {
        Assert-ImeGeometryTrace -TraceText $traceText -Focused:$ImeGeometryOnly
    }
    if ($fullSuite -and
        (-not (Test-Path -LiteralPath $noteStageDir) -or
         -not (Get-ChildItem -LiteralPath $noteStageDir -File -ErrorAction SilentlyContinue))) {
        throw "UI automation did not preserve the staged-exit note diff."
    }
    if ($fullSuite) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:output_export_ok$") {
            throw "UI automation did not complete the output export scenario."
        }
        $pdfExport = Join-Path $outputExportDir "export.pdf"
        $pngExport = Join-Path $outputExportDir "page.png"
        $expectedExports = @($pdfExport,
                            (Join-Path $outputExportDir "note.txt"),
                            (Join-Path $outputExportDir "note.md"),
                            (Join-Path $outputExportDir "note.html"))
        if (-not $SkipPngExport) {
            $expectedExports += $pngExport
        }
        foreach ($path in $expectedExports) {
            if (-not (Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0) {
                throw "UI automation export artifact is missing or empty: $path"
            }
        }
        $pdfBytes = [System.IO.File]::ReadAllBytes($pdfExport)
        if ($pdfBytes.Length -lt 5 -or [System.Text.Encoding]::ASCII.GetString($pdfBytes, 0, 5) -ne "%PDF-") {
            throw "UI automation PDF export does not have a PDF signature."
        }
        if (-not $SkipPngExport) {
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
    if (($fullSuite -or $ConfigOnly) -and $result -notmatch "(?m)^automation:workspace_config_roundtrip_ok$") {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:workspace_config_roundtrip_ok$") {
            throw "UI automation did not complete the workspace configuration round-trip scenario."
        }
    }
    if ($ConfigOnly) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:office_worker_faults_ok launch=1 work=1 cancel=1 polling=1$") {
            throw "UI automation did not complete owned Office worker failure/cancellation checks."
        }
        if ($traceText -notmatch "(?m)^automation:write_retry_ok$") {
            throw "UI automation did not complete the theme and temporary-path write retry scenario."
        }
        if ($traceText -notmatch "(?m)^automation:temp_path_worker_ok count=128 stale=1 cancel=1 busy=1 annotation_state=1$") {
            throw "UI automation did not complete the batch temporary-path worker and stale-result checks."
        }
    }
    if ($LogContractOnly) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:app_log_contract_ok$") {
            throw "UI automation did not complete the app-log contract scenario."
        }
        $logDir = Join-Path $workspaceRoot "__pdf_note_workspace__\__log__"
        $token = "automation-log-contract"
        foreach ($name in @("preview_trace.log", "crash.log")) {
            $path = Join-Path $logDir $name
            if (-not (Test-Path -LiteralPath $path) -or
                (Get-Content -LiteralPath $path -Raw -Encoding UTF8) -notmatch [regex]::Escape($token)) {
                throw "Enabled app log did not contain the contract token: $name"
            }
        }
        foreach ($name in @("switch_timing.log", "startup_watchdog.log", "office_conversion.log")) {
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
        $escapeFiles = Get-ChildItem -LiteralPath (Join-Path $workspaceRoot "__pdf_note_workspace__\__escape__") -File -ErrorAction SilentlyContinue
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
    if ($ConfigUnknownFieldOnly) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:workspace_config_unknown_field_ok$") {
            throw "UI automation did not complete the workspace configuration unknown-field preservation scenario."
        }
    }
    if ($SettingsBundleOnly) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:settings_bundle_ok$") {
            throw "UI automation did not complete the settings bundle scenario."
        }
        $escapeDir = Join-Path $workspaceRoot "__pdf_note_workspace__\__escape__"
        $bundleBackups = Get-ChildItem -LiteralPath $escapeDir -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "settings_import_*" -and (Test-Path -LiteralPath (Join-Path $_.FullName "manifest.txt")) }
        if (-not $bundleBackups) {
            throw "Settings bundle import did not leave a recovery backup."
        }
    }
    $expectsHelpVisibility = $HelpVisibilityOnly -or $fullSuite
    if ($expectsHelpVisibility) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:help_visibility_ok$") {
            if ($traceText -notmatch "(?m)^automation:help_visibility_(ok|failed)$") {
                throw "The executable does not contain the help-visibility automation. Rebuild out\\bin\\pdf_note_workspace.exe, then run this test again."
            }
            throw "UI automation did not keep the main window visible while closing help."
        }
    }
    $expectsDialogOwnerVisibility = $DialogOwnerVisibilityOnly -or $fullSuite
    if ($expectsDialogOwnerVisibility) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:dialog_owner_visibility_ok$") {
            if ($traceText -notmatch "(?m)^automation:dialog_owner_visibility_(ok|failed)$") {
                throw "The executable does not contain the dialog-owner visibility automation. Rebuild out\\bin\\pdf_note_workspace.exe, then run this test again."
            }
            throw "UI automation detected that the main window disappeared, was destroyed, or was minimized while a dialog was open."
        }
    }

    if ($OutputExportOnly -or $fullSuite) {
        $traceText = if (Test-Path -LiteralPath $traceFile) { Get-Content -LiteralPath $traceFile -Raw } else { "" }
        if ($traceText -notmatch "(?m)^automation:output_export_ok$") {
            throw "UI automation did not complete the output export scenario."
        }
        & python -B (Join-Path $repoRoot "tests/python/pdf_annotation_interop_check.py") `
            --merged-pdf (Join-Path $outputExportDir "annotations_merged.pdf")
        if ($LASTEXITCODE -ne 0) { throw "Independent merged PDF annotation verification failed." }
        if ($traceText -notmatch '(?m)^automation:shape_outline_export_ok cases=8$') {
            throw "UI automation did not verify every dashed shape export."
        }
        & python -B (Join-Path $repoRoot "tests/python/pdf_annotation_interop_check.py") `
            --shape-fixtures $outputExportDir
        if ($LASTEXITCODE -ne 0) { throw "Independent dashed shape annotation verification failed." }
    }
    if ($PdfOnly -or $fullSuite) {
        & python -B (Join-Path $repoRoot "tests/python/pdf_page_rotation_check.py") `
            (Join-Path $workspaceRoot "__pdf_note_workspace__/__tmp__/rotation_probe")
        if ($LASTEXITCODE -ne 0) { throw 'Independent page rotation verification failed.' }
    }
    Write-Host "[PASS] UI automation" -ForegroundColor Green
}
finally {
    Restore-Env
    Write-Host "UI automation evidence: $resultFile; trace: $traceFile"
}

if ($RenderedClickOnly) {
    Write-Host "Rendered click/insertion/undo subset passed; full rendering suite was not selected." -ForegroundColor Green
} elseif ($ImeGeometryOnly) {
    Write-Host "IME request geometry subset passed; full rendering suite was not selected." -ForegroundColor Green
} else {
    Write-Host "All selected UI automation tests passed." -ForegroundColor Green
}
