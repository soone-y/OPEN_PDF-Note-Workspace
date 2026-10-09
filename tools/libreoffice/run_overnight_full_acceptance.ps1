[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,
    [Parameter(Mandatory = $true)]
    [string]$BaselineSoffice,
    [Parameter(Mandatory = $true)]
    [string]$Soffice,
    [string]$OutputParent = "",
    [ValidateRange(1, 4)]
    [int]$Workers = 2,
    [int]$TimeoutSec = 180,
    [int]$FixedTime = 1767323045
)

<#
  Overnight full acceptance runner.

  Safety contract:
    * Reads the source and both runtimes only; it never deletes or modifies them.
    * Requires a new, non-existing output directory for every run.
    * Runs preflight before conversion and stops on any missing/hash-mismatched input.
    * Keeps raw results and logs when conversion fails or is interrupted.
    * Does not invoke runtime-removal, sanitizer, release, or publish commands.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\")).Path
$python = (Get-Command python -ErrorAction Stop).Source
$runner = Join-Path $repoRoot "tools\libreoffice\libreoffice_upstream_conversion_test.py"

function Resolve-RequiredFile([string]$Path, [string]$Name) {
    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Name is not a file: $resolved"
    }
    return $resolved
}

$source = (Resolve-Path -LiteralPath $SourceDir -ErrorAction Stop).Path
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "SourceDir is not a directory: $source"
}
$baseline = Resolve-RequiredFile $BaselineSoffice "BaselineSoffice"
$candidate = Resolve-RequiredFile $Soffice "Soffice"
$runner = Resolve-RequiredFile $runner "conversion runner"

if ([string]::IsNullOrWhiteSpace($OutputParent)) {
    $OutputParent = Join-Path $repoRoot ".local\repo_resource\tmp\lo_overnight_acceptance"
}
$parent = [System.IO.Path]::GetFullPath($OutputParent)
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$output = Join-Path $parent "run_$stamp"
if (Test-Path -LiteralPath $output) {
    throw "Refusing to reuse an existing output directory: $output"
}
$log = Join-Path $parent "run_${stamp}.log"
$metadata = Join-Path $parent "run_${stamp}.json"

$oldPythonUtf8 = $env:PYTHONUTF8
$env:PYTHONUTF8 = "1"
$started = [DateTimeOffset]::Now
$command = @(
    $runner, "--source-dir", $source, "--soffice", $candidate,
    "--baseline-soffice", $baseline, "--timeout", "$TimeoutSec",
    "--workers", "$Workers", "--acceptance", "--fixed-time", "$FixedTime",
    "--output-dir", $output
)
$record = [ordered]@{
    started = $started.ToString("o")
    source = $source
    baseline_soffice = $baseline
    candidate_soffice = $candidate
    output = $output
    log = $log
    workers = $Workers
    timeout_seconds = $TimeoutSec
    fixed_time = $FixedTime
    deletion_performed = $false
    command = $command
}
[IO.File]::WriteAllText($metadata, ($record | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))

$exitCode = 2
try {
    Start-Transcript -LiteralPath $log -Force | Out-Null
    Write-Host "Starting read-only LibreOffice full acceptance run."
    Write-Host "Output: $output"
    Write-Host "The source and runtimes are protected; interruption leaves evidence in place."

    # Preflight validates the pinned 3,256-input catalog without generating PDFs.
    & $python $runner --source-dir $source --soffice $candidate `
        --baseline-soffice $baseline --acceptance --preflight `
        --fixed-time $FixedTime
    if ($LASTEXITCODE -ne 0) {
        throw "Preflight failed (exit=$LASTEXITCODE); conversion was not started."
    }

    & $python @command
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Full acceptance conversion failed or was incomplete (exit=$exitCode). Raw output is retained at $output."
    }
    $report = Join-Path $output "report.json"
    if (-not (Test-Path -LiteralPath $report -PathType Leaf)) {
        throw "Formal report.json is missing; this run is not accepted."
    }
    $summary = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if ($summary.summary.acceptance_passed -ne $true) {
        throw "Formal report exists but acceptance_passed is not true. Inspect $report."
    }
    Write-Host "FULL ACCEPTANCE PASSED: $report"
    $exitCode = 0
}
finally {
    try { Stop-Transcript | Out-Null } catch { }
    if ($null -eq $oldPythonUtf8) { Remove-Item Env:PYTHONUTF8 -ErrorAction SilentlyContinue }
    else { $env:PYTHONUTF8 = $oldPythonUtf8 }
    $record.finished = [DateTimeOffset]::Now.ToString("o")
    $record.exit_code = $exitCode
    $record.report_present = Test-Path -LiteralPath (Join-Path $output "report.json") -PathType Leaf
    [IO.File]::WriteAllText($metadata, ($record | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
}
exit $exitCode
