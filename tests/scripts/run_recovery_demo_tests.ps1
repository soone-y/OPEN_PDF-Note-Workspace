[CmdletBinding()]
param([ValidateSet('ja','en')][string]$Locale='ja',[switch]$SkipBuild)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location -LiteralPath $repoRoot
try {
    if (-not $SkipBuild) {
        & scripts/demo/build_recovery_demo.ps1 -Locale $Locale
        if ($LASTEXITCODE -ne 0) { throw 'Demo build failed.' }
    }
    function Snapshot([string]$folder) {
        @(Get-ChildItem -LiteralPath $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
            [pscustomobject]@{path=$_.FullName;hash=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
        }) | ConvertTo-Json -Compress
    }
    $priorEnvironment=@{}
    foreach ($name in @('TEMP','TMP','HOME','USERPROFILE','APPDATA','LOCALAPPDATA')) { $priorEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
    $fixtureHash=(Get-FileHash -LiteralPath tests/fixtures/ui_automation_session/sample.pdf -Algorithm SHA256).Hash
    $first=& scripts/demo/start_recovery_demo.ps1 -Locale $Locale -Scenario all -PrepareOnly
    if ($first.Mode -cne 'Prepared') { throw 'First demo was not prepared.' }
    $before=Snapshot $first.Path
    $second=& scripts/demo/start_recovery_demo.ps1 -Locale $Locale -Scenario all -PrepareOnly
    if ($second.RunId -ceq $first.RunId -or (Snapshot $first.Path) -cne $before) { throw 'New preparation reused or changed the previous demo.' }
    $cases=Get-Content -LiteralPath (Join-Path $first.Path 'cases.txt')
    $casesTwo=Get-Content -LiteralPath (Join-Path $second.Path 'cases.txt')
    $requiredCases=@('annotation_conflict','note_conflict','annotation_changed','note_changed',
        'truncated_journal','missing_journal','pdf_mismatch','write_locked','commit_failed',
        'annotation_missing','note_missing','pdf_missing','note_external_conflict')
    if (($cases -join '|') -cne ($requiredCases -join '|') -or ($cases -join '|') -cne ($casesTwo -join '|')) { throw 'Required repeatable case set is missing.' }
    $tested=& scripts/demo/start_recovery_demo.ps1 -Locale $Locale -Scenario all -SelfTest
    if ((Get-Content -LiteralPath (Join-Path $tested.Path 'selftest.txt') -Raw) -cne "PASS 13 cases`n") { throw 'Self-test completion marker is missing.' }
    if ((Get-Content -LiteralPath (Join-Path $tested.Path 'composition_selftest.txt') -Raw) -cne "PASS 13 cases`n") { throw 'Composition self-test completion marker is missing.' }
    foreach ($name in $priorEnvironment.Keys) {
        if ([Environment]::GetEnvironmentVariable($name,'Process') -cne $priorEnvironment[$name]) { throw "Parent environment changed: $name" }
    }
    if ((Get-FileHash -LiteralPath tests/fixtures/ui_automation_session/sample.pdf -Algorithm SHA256).Hash -cne $fixtureHash) { throw 'Source PDF fixture changed.' }
    $unexpectedLocale=if ($Locale -eq 'ja') { 'en' } else { 'ja' }
    $rejected=$false
    try { & scripts/demo/start_recovery_demo.ps1 -Locale $unexpectedLocale -Resume $first.RunId } catch { $rejected=$true }
    if (-not $rejected -or (Snapshot $first.Path) -cne $before) { throw 'Wrong-locale resume did not preserve the prior run.' }
    Write-Host "PASS: $Locale repeated fresh preparation, 13 production failure/recovery paths, reconnect, new-file rescue, output presence, draft restart, source preservation, lock retry, parent isolation, and locale rejection. GUI not covered."
} finally { Pop-Location }
