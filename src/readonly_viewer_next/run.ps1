[CmdletBinding()]
param([ValidateSet('ja', 'en')][string]$Locale = 'ja', [string]$Path)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$exe = Join-Path $repoRoot "out/readonly_viewer_next/$Locale/bin/readonly_viewer_next.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Run src/readonly_viewer_next/build.ps1 first.' }
$arguments = @()
if ($Path) {
    $resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $arguments = @('"' + $resolvedPath + '"')
}
# Explicit user launcher: this is the visible interactive reader the user requested.
Start-Process -FilePath $exe -ArgumentList ($arguments + @('--prototype')) -WorkingDirectory (Split-Path -Parent $exe)
