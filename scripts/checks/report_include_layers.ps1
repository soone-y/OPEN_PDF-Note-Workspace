# Regenerate include dependency + layer violation report.
param(
    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$python = "python"
$visualizer = Join-Path $RepoRoot "tools/metrics/cpp_include_visualizer.py"
$layers = Join-Path $RepoRoot "tools/metrics/layers.json"
$src = Join-Path $RepoRoot "src"
$out = Join-Path $RepoRoot "docs/internal/reports/build_依存関係調査レポート_2026-05-22.md"

& $python $visualizer $src --include-root $src --layers $layers --report $out
Write-Host ("Wrote {0}" -f $out)
