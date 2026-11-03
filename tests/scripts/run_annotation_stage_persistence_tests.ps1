[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$ArtifactName = 'pdf_note_workspace_stage_persistence',
    # A caller may select a shorter evidence root for a deep source snapshot.
    # Every run still creates a fresh GUID directory and never clears old data.
    [string]$OutputRoot = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$evidenceRoot = if ($OutputRoot) {
    if ($OutputRoot -notmatch '^[A-Za-z]:[\\/]') { throw 'OutputRoot must be an absolute local drive path.' }
    [System.IO.Path]::GetFullPath($OutputRoot)
} else { Join-Path $repoRoot 'out/tests/annotation_stage_persistence' }
$runRoot = Join-Path $evidenceRoot ([Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $runRoot
$results = [System.Collections.Generic.List[object]]::new()
$expectedScenarios = 28
$testExe = Join-Path $runRoot 'stage_persistence_host.exe'
$priorTemp = $env:TEMP; $priorTmp = $env:TMP; $priorPath = $env:PATH
$sourceBefore = @(); $sourceAfter = @()
$head = ''; $version = ''; $fatal = ''
$utf8 = [System.Text.UTF8Encoding]::new($false)

function Source-Snapshot {
    $paths = @(& rg --files src locales scripts/build tools/localization third_party/md4c/src third_party/pdfium/include third_party/zlib/include)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate test/build inputs.' }
    $paths += @('REPO_VERSION.txt', 'third_party/pdfium/lib/pdfium.dll.lib', 'third_party/pdfium/bin/pdfium.dll', 'third_party/zlib/lib/libz.dll.a',
        'tests/integration/annotation_stage_persistence_tests.cpp', 'tests/integration/annotation_stage_write_pause.cpp',
        'tests/scripts/run_annotation_stage_persistence_tests.ps1', 'tests/fixtures/ui_automation_session/sample.pdf')
    foreach ($path in ($paths | Sort-Object -Unique)) {
        [pscustomobject]@{ path=$path; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
}
function Protected-Snapshot([string]$workspace) {
    $map = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $workspace -Recurse -File)) {
        # Published originals must stay at their exact path and byte identity.
        if ($file.Extension -eq '.pdf' -or $file.Extension -eq '.clrop') {
            $map[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        }
    }
    return $map
}
function Recovery-PayloadSnapshot([string]$workspace) {
    $payloads = @()
    foreach ($file in (Get-ChildItem -LiteralPath $workspace -Recurse -File)) {
        # A rejected checkpoint can be quarantined under __escape__. Preserve its
        # type, filename, and bytes while allowing only that owned relocation.
        $kind = if ($file.Name -like '*.clrop.stage') { 'checkpoint' }
                elseif ($file.Name -like '*.clrop.stage.meta.txt') { 'metadata' }
                elseif ($file.Name -like 'clropj_*.json') { 'journal' }
                else { '' }
        if ($kind) {
            $payloads += "$kind|$($file.Name)|$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
        }
    }
    return @($payloads | Sort-Object)
}
function Assert-Protected([hashtable]$before, [string]$workspace) {
    $after = Protected-Snapshot $workspace
    if ($before.Count -ne $after.Count) { throw 'Published original file inventory changed.' }
    foreach ($path in $before.Keys) {
        if (-not $after.ContainsKey($path) -or $before[$path] -cne $after[$path]) {
            throw "Published original bytes changed: $path"
        }
    }
}
function Start-Host([string]$mode, [string]$workspace, [string]$signal = '') {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $testExe
    $info.Arguments = '"' + $mode + '" "' + $workspace + '"'
    $info.WorkingDirectory = $workspace
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.EnvironmentVariables['TEMP'] = $workspace
    $info.EnvironmentVariables['TMP'] = $workspace
    $null = $info.EnvironmentVariables.Remove('PDF_NOTE_STAGE_TEST_PAUSE_FILE')
    if ($signal) { $info.EnvironmentVariables['PDF_NOTE_STAGE_TEST_PAUSE_FILE'] = $signal }
    $process = [System.Diagnostics.Process]::new(); $process.StartInfo = $info
    if (-not $process.Start()) { throw 'Owned test child failed to start.' }
    [pscustomobject]@{ process=$process; stdout=$process.StandardOutput.ReadToEndAsync(); stderr=$process.StandardError.ReadToEndAsync() }
}
function Stop-OwnedChild($process) {
    if (-not $process.HasExited) {
        $process.Kill()
        if (-not $process.WaitForExit(5000)) { throw 'Owned test child did not stop after termination.' }
    }
}
function Finish-Host($child, [string]$log, [int]$timeoutMs = 30000) {
    try {
        if (-not $child.process.WaitForExit($timeoutMs)) {
            Stop-OwnedChild $child.process
            throw 'Owned stage test child timed out; this is not a pass.'
        }
        $text = $child.stdout.GetAwaiter().GetResult() + $child.stderr.GetAwaiter().GetResult()
        [System.IO.File]::WriteAllText($log, $text, $utf8)
        if ($child.process.ExitCode -ne 0) { throw "Stage phase failed (exit=$($child.process.ExitCode)); see $log" }
    } finally {
        Stop-OwnedChild $child.process
        $child.process.Dispose()
    }
}
function Phase([string]$mode, [string]$workspace) {
    $log = Join-Path (Split-Path -Parent $workspace) ($mode + '.log')
    Finish-Host (Start-Host $mode $workspace) $log
}
function Fixture([string]$name, [string]$seed = 'seed') {
    $folder = Join-Path $runRoot ('cases/' + $name)
    $workspace = Join-Path $folder 'workspace'
    $null = New-Item -ItemType Directory -Path $workspace
    $pdf = Join-Path $repoRoot 'tests/fixtures/ui_automation_session/sample.pdf'
    Copy-Item -LiteralPath $pdf -Destination (Join-Path $workspace 'sample.pdf')
    [System.IO.File]::WriteAllText((Join-Path $workspace 'stage-test-owned.txt'), 'Disposable stage persistence test workspace.', $utf8)
    Phase $seed $workspace
    if ((Get-FileHash -LiteralPath $pdf).Hash -cne (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.pdf')).Hash) { throw 'Seed changed its PDF copy.' }
    return $workspace
}
function Journal-Files([string]$workspace) {
    @(Get-ChildItem -LiteralPath (Join-Path $workspace '__pdf_note_workspace__/__tmp__/__stage__/clrop_journal') -Filter 'clropj_*.json' -File | Sort-Object Name)
}
function Reject-And-Preserve([string]$workspace) {
    $before = Protected-Snapshot $workspace
    $recoveryBefore = @(Recovery-PayloadSnapshot $workspace)
    try {
        Phase 'reject' $workspace
        $stageDirectory = Join-Path $workspace '__pdf_note_workspace__/__tmp__/__stage__/clrop'
        $activeCheckpoints = @()
        if (Test-Path -LiteralPath $stageDirectory) {
            $activeCheckpoints = @(Get-ChildItem -LiteralPath $stageDirectory -File -Filter '*.clrop.stage')
        }
        if ($activeCheckpoints.Count -gt 0) {
            Phase 'integrate-rejected-input' $workspace
        } else {
            # A corrupt checkpoint is rejected by moving it to the owned escape
            # directory. There is then no active input to integrate; requiring a
            # later resolver lookup would mistake this successful quarantine for
            # a test-host failure.
            $escapeDirectory = Join-Path $workspace '__pdf_note_workspace__/__escape__'
            $quarantined = @()
            if (Test-Path -LiteralPath $escapeDirectory) {
                $quarantined = @(Get-ChildItem -LiteralPath $escapeDirectory -File -Filter '*.clrop.stage')
            }
            if ($quarantined.Count -eq 0) { throw 'Rejected checkpoint was neither retained nor safely quarantined.' }
        }
    } finally {
        Assert-Protected $before $workspace
        $recoveryAfter = @(Recovery-PayloadSnapshot $workspace)
        if (($recoveryBefore | ConvertTo-Json -Compress) -cne ($recoveryAfter | ConvertTo-Json -Compress)) {
            throw 'Recovery payload bytes changed or were lost during rejection.'
        }
    }
}
function Case([string]$name, [scriptblock]$body) {
    $start = Get-Date
    try {
        & $body
        $results.Add([pscustomobject]@{name=$name;status='PASS';error='';seconds=((Get-Date)-$start).TotalSeconds})
        Write-Host "PASS: $name"
    } catch {
        $results.Add([pscustomobject]@{name=$name;status='FAIL';error=$_.Exception.Message;seconds=((Get-Date)-$start).TotalSeconds})
        Write-Host "FAIL: $name / $($_.Exception.Message)"
    }
}

Push-Location -LiteralPath $repoRoot
try {
    $env:TEMP = $runRoot; $env:TMP = $runRoot
    $compiler = Get-Command g++ -ErrorAction Stop
    $env:PATH = (Join-Path $repoRoot 'third_party/pdfium/bin') + ';' + (Split-Path -Parent $compiler.Source) + ';' + $priorPath
    $head = (& git rev-parse HEAD).Trim(); if ($LASTEXITCODE -ne 0) { throw 'Cannot identify HEAD.' }
    $version = (Get-Content -LiteralPath 'REPO_VERSION.txt' -Raw).Trim()
    & git status --short --branch | Set-Content -LiteralPath (Join-Path $runRoot 'status_before.txt') -Encoding UTF8
    $buildDeadline = [DateTime]::UtcNow.AddMinutes(5)
    $waitingReported = $false
    do {
        # Respect another developer's build. Never delete its lock or stop its
        # process. Capture inputs after waiting, immediately before our build.
        $ready = $false
        try {
            $probe = [System.IO.File]::Open((Join-Path $repoRoot 'out/build.lock'),
                [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
            $probe.Dispose(); $ready = $true
        } catch [System.IO.IOException] { }
        if (-not $ready) {
            if (-not $waitingReported) { Write-Host 'Waiting for the existing build to finish (bounded to five minutes).'; $waitingReported=$true }
            if ([DateTime]::UtcNow -ge $buildDeadline) { throw 'Existing build remained busy; stage integration was not tested.' }
            Start-Sleep -Milliseconds 250
            continue
        }
        $sourceBefore = @(Source-Snapshot)
        & scripts/build/build_workspace.ps1 -ArtifactName $ArtifactName -Edition Full -Locale ja *> (Join-Path $runRoot 'build.log')
        if ($LASTEXITCODE -eq 0) { break }
        $buildText = Get-Content -LiteralPath (Join-Path $runRoot 'build.log') -Raw
        if ($buildText -notmatch 'Another build is currently running' -or [DateTime]::UtcNow -ge $buildDeadline) {
            throw "Current JA Full build failed; see $(Join-Path $runRoot 'build.log')"
        }
        Start-Sleep -Milliseconds 250
    } while ($true)
    $sourceAfter = @(Source-Snapshot)
    if (($sourceBefore | ConvertTo-Json -Compress) -cne ($sourceAfter | ConvertTo-Json -Compress)) { throw 'Test/build inputs changed during build; no current-version PASS can be claimed.' }
    $objectRoot = Join-Path $repoRoot ('out/obj_' + $ArtifactName)
    $signature = Get-Content -LiteralPath (Join-Path $objectRoot 'build_signature.json') -Raw | ConvertFrom-Json
    if ($signature.Edition -ne 'Full' -or $signature.Locale -ne 'ja') { throw 'Wrong build edition/locale.' }
    $hostObject = Join-Path $runRoot 'stage_host.o'; $pauseObject = Join-Path $runRoot 'write_pause.o'
    foreach ($entry in @(
        @{source='tests/integration/annotation_stage_persistence_tests.cpp';object=$hostObject;log='host_compile.log'},
        @{source='tests/integration/annotation_stage_write_pause.cpp';object=$pauseObject;log='pause_compile.log'})) {
        & $compiler.Source -std=gnu++17 @($signature.Includes) @($signature.Flags) -c $entry.source -o $entry.object *> (Join-Path $runRoot $entry.log)
        if ($LASTEXITCODE -ne 0) { throw "Test host compilation failed; see $($entry.log)" }
    }
    $arguments = [System.Collections.Generic.List[string]]::new()
    $removed = 0
    $lines = @(Get-Content -LiteralPath (Join-Path $objectRoot 'link_args.rsp'))
    for ($index=0; $index -lt $lines.Count; ++$index) {
        $line = $lines[$index]
        if ($index -eq 1) { $arguments.Add('"' + $testExe + '"') }
        elseif ($line -match 'src_file_output_file_output_stage\.o"$') {
            $arguments.Add('"' + $hostObject + '"'); $arguments.Add('"' + $pauseObject + '"'); ++$removed
        }
        elseif ($line -eq '"-mwindows"') { $arguments.Add('"-mconsole"') }
        else { $arguments.Add($line) }
    }
    if ($removed -ne 1) { throw 'Required real stage object replacement was not found.' }
    $arguments.Add('"-ladvapi32"')
    $response = Join-Path $runRoot 'link_args.rsp'
    [System.IO.File]::WriteAllLines($response, $arguments, $utf8)
    & $compiler.Source ('@' + $response) *> (Join-Path $runRoot 'host_link.log')
    if ($LASTEXITCODE -ne 0) { throw 'Test host link failed.' }

    Case 'background_assist_round_trip_and_stage_comparison' {
        $workspace = Fixture 'background_assist'
        $pdfBefore = (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.pdf') -Algorithm SHA256).Hash
        Phase 'background-assist' $workspace
        if ((Get-FileHash -LiteralPath (Join-Path $workspace 'sample.pdf') -Algorithm SHA256).Hash -cne $pdfBefore) {
            throw 'Background-assist round-trip changed the PDF original.'
        }
    }
    Case 'modern_restart_restore' {
        $workspace = Fixture 'modern_restart_restore'
        $before = Protected-Snapshot $workspace
        Phase 'load' $workspace
        Assert-Protected $before $workspace
    }
    Case 'legacy_12_checkpoint_6_journal_restart_restore' {
        $workspace = Fixture 'legacy_restart_restore' 'seed-legacy'
        $before = Protected-Snapshot $workspace
        Phase 'load-legacy' $workspace
        Assert-Protected $before $workspace
    }
    Case 'truncated_last_journal_rejects_partial_replay' {
        $workspace = Fixture 'truncated_journal'
        $path = (Journal-Files $workspace)[-1].FullName
        $bytes = [System.IO.File]::ReadAllText($path)
        [System.IO.File]::WriteAllText($path, $bytes.Substring(0, [int]($bytes.Length/2)), $utf8)
        Reject-And-Preserve $workspace
    }
    Case 'wrong_typed_journal_field_rejected' {
        $workspace = Fixture 'wrong_typed_journal'
        $path = (Journal-Files $workspace)[-1].FullName
        $bytes = [System.IO.File]::ReadAllText($path)
        $changed = $bytes.Replace('"font":"Segoe UI"', '"font":123')
        if ($bytes -ceq $changed) { throw 'Required typed-field fixture was not found.' }
        [System.IO.File]::WriteAllText($path, $changed, $utf8)
        Reject-And-Preserve $workspace
    }
    Case 'missing_first_journal_rejected' {
        $workspace = Fixture 'missing_first_journal'
        $path = (Journal-Files $workspace)[0].FullName
        # Retain evidence outside the workspace rather than delete a segment.
        Move-Item -LiteralPath $path -Destination (Join-Path (Split-Path -Parent $workspace) 'held_first_journal.json')
        Reject-And-Preserve $workspace
    }
    Case 'reordered_journal_payloads_rejected' {
        $workspace = Fixture 'reordered_journal'
        $paths = Journal-Files $workspace
        $one = [System.IO.File]::ReadAllText($paths[0].FullName); $two = [System.IO.File]::ReadAllText($paths[1].FullName)
        [System.IO.File]::WriteAllText($paths[0].FullName, $two, $utf8)
        [System.IO.File]::WriteAllText($paths[1].FullName, $one, $utf8)
        Reject-And-Preserve $workspace
    }
    Case 'unknown_precision_rejects_load_save_integration' {
        $workspace = Fixture 'unknown_precision'
        $path = (Get-ChildItem -LiteralPath $workspace -Recurse -File -Filter '*.clrop.stage.meta.txt')[0].FullName
        $bytes = [System.IO.File]::ReadAllText($path)
        $changed = $bytes.Replace('annotation_numeric_precision=17', 'annotation_numeric_precision=19')
        if ($bytes -ceq $changed) { throw 'Required precision fixture was not found.' }
        [System.IO.File]::WriteAllText($path, $changed, $utf8)
        $before = Protected-Snapshot $workspace
        Reject-And-Preserve $workspace
        Phase 'save-unsupported' $workspace
        Assert-Protected $before $workspace
    }
    Case 'duplicate_precision_metadata_rejected' {
        $workspace = Fixture 'duplicate_precision'
        $path = (Get-ChildItem -LiteralPath $workspace -Recurse -File -Filter '*.clrop.stage.meta.txt')[0].FullName
        [System.IO.File]::AppendAllText($path, "annotation_numeric_precision=17`n", $utf8)
        Reject-And-Preserve $workspace
    }
    Case 'corrupt_checkpoint_rejected' {
        $workspace = Fixture 'corrupt_checkpoint'
        $path = (Get-ChildItem -LiteralPath $workspace -Recurse -File -Filter '*.clrop.stage')[0].FullName
        [System.IO.File]::WriteAllText($path, '{"version":1', $utf8)
        Reject-And-Preserve $workspace
    }
    Case 'checkpoint_write_denial_keeps_previous_recovery_and_dirty_edit' {
        $workspace = Fixture 'checkpoint_write_denial'
        $before = Protected-Snapshot $workspace
        $directory = (Get-ChildItem -LiteralPath $workspace -Recurse -File -Filter '*.clrop.stage')[0].DirectoryName
        $acl = (Get-Acl -LiteralPath $directory).Sddl
        Phase 'save-denied' $workspace
        if ($acl -cne (Get-Acl -LiteralPath $directory).Sddl) { throw 'Owned stage directory ACL was not restored.' }
        Assert-Protected $before $workspace
        Phase 'load' $workspace
    }
    Case 'journal_write_denial_keeps_previous_recovery' {
        $workspace = Fixture 'journal_write_denial'
        $before = Protected-Snapshot $workspace
        $directory = Join-Path $workspace '__pdf_note_workspace__/__tmp__/__stage__/clrop_journal'
        $acl = (Get-Acl -LiteralPath $directory).Sddl
        Phase 'journal-denied' $workspace
        if ($acl -cne (Get-Acl -LiteralPath $directory).Sddl) { throw 'Owned journal directory ACL was not restored.' }
        Assert-Protected $before $workspace
        Phase 'load' $workspace
    }
    Case 'integration_lock_failure_keeps_original_and_recovery' {
        $workspace = Fixture 'integration_lock_failure'
        $before = Protected-Snapshot $workspace
        Phase 'integrate-denied' $workspace
        Assert-Protected $before $workspace
        Phase 'load' $workspace
    }
    Case 'integration_reopens_final_edit_and_retains_original_backup' {
        $workspace = Fixture 'integration_success'
        $original = (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.clrop')).Hash
        $pdf = (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.pdf')).Hash
        Phase 'integrate' $workspace
        if ($pdf -cne (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.pdf')).Hash) { throw 'Integration changed PDF bytes.' }
        $backupRoot = Join-Path $workspace '__pdf_note_workspace__/__escape__/backup'
        $matching = @(Get-ChildItem -LiteralPath $backupRoot -Recurse -File | Where-Object { (Get-FileHash -LiteralPath $_.FullName).Hash -ceq $original })
        if ($matching.Count -eq 0) { throw 'No byte-identical original CLROP backup was retained.' }
    }
    foreach ($mode in @(
        'retention-sync-denied', 'retention-sync-success',
        'retention-worker-denied', 'retention-worker-success',
        'retention-note-sync-denied', 'retention-note-sync-success',
        'retention-note-worker-denied', 'retention-note-worker-success',
        'retention-note-sync-commit-denied', 'retention-note-worker-commit-denied',
        'retention-restore-denied', 'retention-restore-success')) {
        Case $mode {
            $workspace = Fixture $mode
            $pdf = (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.pdf')).Hash
            Phase $mode $workspace
            if ($pdf -cne (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.pdf')).Hash) {
                throw 'Backup retention scenario changed PDF bytes.'
            }
        }
    }
    Case 'termination_after_real_partial_temp_write_recovers_previous_complete_state' {
        $workspace = Fixture 'termination_during_journal_write'
        $before = Protected-Snapshot $workspace
        $signal = Join-Path $workspace 'paused_after_write.txt'
        $child = Start-Host 'crash-write' $workspace $signal
        try {
            $deadline = [DateTime]::UtcNow.AddSeconds(15)
            while (-not (Test-Path -LiteralPath $signal) -and -not $child.process.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 25 }
            if (-not (Test-Path -LiteralPath $signal) -or $child.process.HasExited) { throw 'Real partial-write boundary was not observed.' }
            $receipt = [System.IO.File]::ReadAllText($signal)
            if ($receipt -notmatch 'after_real_WriteFile_before_flush_and_replace\r?\nbytes=1048576') { throw 'Partial-write receipt is incomplete or wrong.' }
            Stop-OwnedChild $child.process
        } finally {
            Stop-OwnedChild $child.process
            $receipt = if (Test-Path -LiteralPath $signal) { [System.IO.File]::ReadAllText($signal) } else { 'No partial-write boundary receipt.' }
            [System.IO.File]::WriteAllText((Join-Path (Split-Path -Parent $workspace) 'crash-write.log'),
                $child.stdout.GetAwaiter().GetResult() + $child.stderr.GetAwaiter().GetResult() + $receipt, $utf8)
            $child.process.Dispose()
        }
        $partial = @(Get-ChildItem -LiteralPath $workspace -Recurse -File -Filter '*.tmp' | Where-Object { $_.Length -eq 1048576 })
        if ($partial.Count -ne 1) { throw 'The actual partial temporary journal was not retained for observation.' }
        Assert-Protected $before $workspace
        Phase 'load' $workspace
        Assert-Protected $before $workspace
    }
    # This is an ordinary required-success assertion. A known bug remains FAIL;
    # expected-failure handling must not turn it into a safety certification.
    Case 'modern_bbox_cancellation_near_zero_valid_journal_restore' {
        $workspace = Fixture 'modern_bbox_cancellation' 'seed-cancellation'
        $before = Protected-Snapshot $workspace
        try { Phase 'load-cancellation' $workspace }
        finally { Assert-Protected $before $workspace }
    }
    $sourceAfter = @(Source-Snapshot)
    if (($sourceBefore | ConvertTo-Json -Compress) -cne ($sourceAfter | ConvertTo-Json -Compress)) { throw 'Test/build inputs changed during execution; results do not certify the final worktree.' }
} catch {
    $fatal = $_.Exception.Message
} finally {
    $report = [ordered]@{
        status=$(if ($fatal -or $results.Count -ne $expectedScenarios -or @($results | Where-Object status -eq 'FAIL').Count) { 'FAIL' } else { 'PASS' })
        expectedScenarios=$expectedScenarios;completedScenarios=$results.Count
        version=$version;head=$head;artifactName=$ArtifactName;testHost=$testExe
        testHostSha256=$(if (Test-Path -LiteralPath $testExe) { (Get-FileHash -LiteralPath $testExe).Hash } else { '' })
        appBinarySha256=$(if (Test-Path -LiteralPath (Join-Path $repoRoot ('out/bin/' + $ArtifactName + '.exe'))) { (Get-FileHash -LiteralPath (Join-Path $repoRoot ('out/bin/' + $ArtifactName + '.exe'))).Hash } else { '' })
        scope='JA Full production stage/CLROP/note save and backup restore functions in separate test-host processes; no normal GUI startup claim'
        crashInjection='test-only pause after the first real 1 MiB journal temp write, before flush/replace; owned child termination'
        sourceBefore=$sourceBefore;sourceAfter=$sourceAfter;fatal=$fatal;cases=@($results.ToArray())
        untested=@('normal GUI startup recovery prompt and user selection', 'all crash boundaries', 'actual power loss', 'actual full disk', 'manual audible silence', 'EN/Lite/release distribution acceptance')
    }
    $report | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $runRoot 'report.json') -Encoding UTF8
    $env:TEMP=$priorTemp; $env:TMP=$priorTmp; $env:PATH=$priorPath
    Pop-Location
}
Write-Host "Report: $(Join-Path $runRoot 'report.json')"
$failures = @($results | Where-Object status -eq 'FAIL').Count
Write-Host "Stage persistence scenarios: $($results.Count - $failures) passed, $failures failed."
if ($fatal) { throw $fatal }
if ($failures) { throw 'Stage persistence regression failed; all case results were retained.' }
if ($results.Count -ne $expectedScenarios) { throw 'Required stage persistence scenarios were not all executed.' }
