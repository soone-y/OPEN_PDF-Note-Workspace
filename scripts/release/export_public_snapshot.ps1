[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ScriptArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = $PSScriptRoot
if (-not $scriptRoot) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$scriptsRoot = Split-Path -Parent $scriptRoot
$repoRoot = Split-Path -Parent $scriptsRoot
if (-not $repoRoot) {
    $repoRoot = $scriptRoot
}

$scriptPath = Join-Path $repoRoot "tools/dev/export_public_snapshot.py"
if (-not (Test-Path -LiteralPath $scriptPath)) {
    throw "Missing snapshot export script: $scriptPath"
}

$pythonLauncher = Get-Command py -ErrorAction SilentlyContinue
if ($null -ne $pythonLauncher) {
    & $pythonLauncher.Source -3 $scriptPath @ScriptArgs
}
else {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $pythonCommand) {
        throw "Python launcher not found. Install 'py' or make 'python' available in PATH."
    }
    & $pythonCommand.Source $scriptPath @ScriptArgs
}

if (Test-Path -LiteralPath variable:LASTEXITCODE) {
    exit $LASTEXITCODE
}
exit 0
