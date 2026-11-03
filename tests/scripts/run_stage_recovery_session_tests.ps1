[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('ja','en')][string]$Locale)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runRoot=Join-Path $repoRoot ('out/tests/stage_recovery_session/'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $runRoot
$utf8=[System.Text.UTF8Encoding]::new($false)
function Snapshot {
    $paths=@(& rg --files src locales scripts/build tools/localization tools/recovery_demo)
    if($LASTEXITCODE -ne 0){throw 'Cannot enumerate recovery test inputs.'}
    $paths+=@('REPO_VERSION.txt','tests/fixtures/ui_automation_session/sample.pdf','tests/scripts/run_stage_recovery_session_tests.ps1')
    @($paths | Sort-Object -Unique | ForEach-Object { [pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash} })
}
function Run-Host([string]$mode) {
    $info=[System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName=Join-Path $runRoot 'recovery_demo.exe'
    $info.Arguments=$mode+' "'+$runRoot+'"'+$(if($mode -eq '--prepare'){' all'}else{''})
    $info.WorkingDirectory=$runRoot;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($key in @($info.Environment.Keys)){if($key.StartsWith('PDF_NOTE_',[StringComparison]::OrdinalIgnoreCase)){$null=$info.Environment.Remove($key)}}
    $profile=Join-Path $runRoot 'workspace/__pdf_note_workspace__/__tmp__/test_process'
    $null=New-Item -ItemType Directory -Force -Path $profile
    foreach($key in @('TEMP','TMP','APPDATA','LOCALAPPDATA','USERPROFILE','HOME')){$info.Environment[$key]=$profile}
    $child=[System.Diagnostics.Process]::Start($info)
    try {
        $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
        if(-not $child.WaitForExit(120000)){throw "Recovery test timeout: $mode"}
        $log=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
        [System.IO.File]::WriteAllText((Join-Path $runRoot ($mode.Substring(2)+'.log')),$log,$utf8)
        if($child.ExitCode -ne 0){throw "Recovery test failed: $mode; see $runRoot"}
        return $log
    } finally {if(-not $child.HasExited){$child.Kill();$null=$child.WaitForExit(5000)};$child.Dispose()}
}
Push-Location -LiteralPath $repoRoot
try {
    $before=@(Snapshot)
    & scripts/build/build_workspace.ps1 -Locale $Locale *> (Join-Path $runRoot 'build.log')
    if($LASTEXITCODE -ne 0){throw "Production build failed: $runRoot/build.log"}
    $objects=Join-Path $repoRoot $(if($Locale -eq 'en'){'out/obj_en'}else{'out/obj'})
    $bin=Join-Path $repoRoot $(if($Locale -eq 'en'){'out/bin_en'}else{'out/bin'})
    $signature=Get-Content -LiteralPath (Join-Path $objects 'build_signature.json') -Raw | ConvertFrom-Json
    if($signature.Locale -cne $Locale -or $signature.Edition -cne 'Full'){throw 'Wrong production build signature.'}
    $compiler=(Get-Command g++ -ErrorAction Stop).Source
    $compiled=@()
    foreach($name in @('demo_host','demo_ui','demo_reconnect','demo_composition','demo_monitor')) {
        $object=Join-Path $runRoot ($name+'.o')
        & $compiler -std=gnu++17 @($signature.Includes) @($signature.Flags) -c ('tools/recovery_demo/'+$name+'.cpp') -o $object *> (Join-Path $runRoot ($name+'_compile.log'))
        if($LASTEXITCODE -ne 0){throw "Recovery test host compilation failed: $name; $runRoot"}
        $compiled+=$object
    }
    $exe=Join-Path $runRoot 'recovery_demo.exe';$arguments=[System.Collections.Generic.List[string]]::new();$removed=0
    $lines=@(Get-Content -LiteralPath (Join-Path $objects 'link_args.rsp'))
    for($i=0;$i -lt $lines.Count;++$i){$line=$lines[$i]
        if($i -eq 1){$arguments.Add('"'+$exe+'"')}
        elseif($line -match 'src_file_output_file_output_stage\.o"$'){foreach($object in $compiled){$arguments.Add('"'+$object+'"')};++$removed}
        elseif($line -eq '"-mwindows"'){$arguments.Add('"-mconsole"')}
        else{$arguments.Add($line)}
    }
    if($removed -ne 1){throw 'Production stage object must be replaced exactly once.'}
    $response=Join-Path $runRoot 'link_args.rsp';[System.IO.File]::WriteAllLines($response,$arguments,$utf8)
    & $compiler ('@'+$response) *> (Join-Path $runRoot 'link.log')
    if($LASTEXITCODE -ne 0){throw "Recovery test link failed: $runRoot/link.log"}
    foreach($name in @('pdfium.dll','libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll','zlib1.dll')){Copy-Item -LiteralPath (Join-Path $bin $name) -Destination (Join-Path $runRoot $name)}
    Copy-Item -LiteralPath tests/fixtures/ui_automation_session/sample.pdf -Destination (Join-Path $runRoot 'fixture.pdf')
    [System.IO.File]::WriteAllText((Join-Path $runRoot 'owned.txt'),"pdf-note-recovery-demo-v1`n",$utf8)
    $null=Run-Host '--prepare';$log=Run-Host '--app-selftest'
    $required=@('annotation_conflict','note_conflict','annotation_changed','note_changed','truncated_journal','missing_journal','pdf_mismatch','write_locked','commit_failed','annotation_missing','note_missing','pdf_missing','note_external_conflict')
    $actual=@([regex]::Matches($log,'(?m)^PASS app-session (\w+)\r?$') | ForEach-Object {$_.Groups[1].Value})
    if($actual.Count -ne 13 -or (Compare-Object ($required | Sort-Object) ($actual | Sort-Object))){throw 'Required recovery session case set is incomplete.'}
    $boundaries=@('commit-retry','commit-failed-rescue','published-pdf-stage','empty-pdf-rescue','unreadable-metadata','parent-note-write')
    foreach($boundary in $boundaries){if($log -notmatch ('(?m)^PASS app-boundary '+[regex]::Escape($boundary)+'\r?$')){throw "Required recovery boundary missing: $boundary"}}
    $after=@(Snapshot)
    if(($before | ConvertTo-Json -Compress) -cne ($after | ConvertTo-Json -Compress)){throw 'Recovery test inputs changed during verification; rerun.'}
    $receipt=@{format='stage-recovery-session-v1';locale=$Locale;cases=$actual;boundaries=$boundaries;inputs=$after;exeSha256=(Get-FileHash -LiteralPath $exe).Hash;uiVerified=$false}
    [System.IO.File]::WriteAllText((Join-Path $runRoot 'result.json'),($receipt | ConvertTo-Json -Depth 6),$utf8)
    Write-Host "PASS: 13 production recovery sessions and 6 boundaries ($Locale). Evidence: $runRoot"
} finally {Pop-Location}
