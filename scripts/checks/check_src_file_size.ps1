# Warn when src C++ sources exceed the modularization threshold (default 2000 lines).
param(
    [int]$ThresholdLines = 2000,
    [string]$Root = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "src")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$exts = @(".cpp", ".cppinc", ".inc", ".h", ".hpp")
$hits = @()

Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
    $exts -contains $_.Extension.ToLowerInvariant()
} | ForEach-Object {
    $lineCount = 0
    foreach ($line in [System.IO.File]::ReadLines($_.FullName)) {
        $lineCount++
    }
    if ($lineCount -gt $ThresholdLines) {
        $rel = $_.FullName.Substring($Root.Length).TrimStart('\', '/')
        $hits += [PSCustomObject]@{
            Path  = $rel
            Lines = $lineCount
        }
    }
}

if ($hits.Count -eq 0) {
    Write-Host ("OK: no src files over {0} lines." -f $ThresholdLines) -ForegroundColor Green
    exit 0
}

Write-Host ("WARNING: {0} src file(s) over {1} lines:" -f $hits.Count, $ThresholdLines) -ForegroundColor Yellow
$hits | Sort-Object Lines -Descending | Format-Table -AutoSize
exit 1
