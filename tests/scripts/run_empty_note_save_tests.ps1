[CmdletBinding()]
param([string]$AppExecutable = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot 'out/tests'
$null = New-Item -ItemType Directory -Force -Path $outputDir
$driver = Join-Path $outputDir 'empty_note_save_tests.exe'
& g++ -std=c++17 -Wall -Wextra -Werror -static-libgcc -static-libstdc++ -I (Join-Path $repoRoot 'src') `
    (Join-Path $repoRoot 'tests/integration/empty_note_save_tests.cpp') -o $driver
if ($LASTEXITCODE -ne 0) { throw 'Empty note driver compilation failed.' }
$binary = if ($AppExecutable) { (Get-Item -LiteralPath $AppExecutable -ErrorAction Stop).FullName }
    else { Join-Path $repoRoot 'out/bin/pdf_note_workspace.exe' }
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw 'Build the current application before this test.' }
$fixture = Join-Path $outputDir ('empty_note_app_' + [guid]::NewGuid().ToString('N'))
$appDir = Join-Path $fixture 'app'
$null = New-Item -ItemType Directory -Path $appDir
$ownedAppName = 'pdf_note_empty_' + [IO.Path]::GetFileName($fixture) + '.exe'
Copy-Item -LiteralPath $binary -Destination (Join-Path $appDir $ownedAppName)
Get-ChildItem -LiteralPath (Split-Path -Parent $binary) -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $appDir
}
$priorRoot = [Environment]::GetEnvironmentVariable('EMPTY_NOTE_APP_TEST_ROOT', 'Process')
Push-Location -LiteralPath $repoRoot
try {
    [Environment]::SetEnvironmentVariable('EMPTY_NOTE_APP_TEST_ROOT', $fixture, 'Process')
    & $driver
    if ($LASTEXITCODE -ne 0) { throw 'Empty note actual-app tests failed.' }
} finally {
    [Environment]::SetEnvironmentVariable('EMPTY_NOTE_APP_TEST_ROOT', $priorRoot, 'Process')
    Pop-Location
}
