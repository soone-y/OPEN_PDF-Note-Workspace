[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$fixture = Join-Path $repoRoot ('out/tests/identity_scope_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
foreach ($directory in @('workspace', 'registered', 'outside')) {
    New-Item -ItemType Directory -Path (Join-Path $fixture $directory) | Out-Null
}
[IO.File]::WriteAllText((Join-Path $fixture 'outside/escape.pdf'), 'outside bytes')
New-Item -ItemType Junction -Path (Join-Path $fixture 'registered/junction') -Target (Join-Path $fixture 'outside') | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$exe = Join-Path $fixture 'identity_scope_tests.exe'
Push-Location -LiteralPath $repoRoot
try {
    & $compiler -std=gnu++17 -O2 -Wall -Wextra -Isrc -Ithird_party/pdfium/include `
        tests/unit/workspace_identity_scope_tests.cpp src/workspace/workspace_resource_identity.cpp `
        src/note/note_identity.cpp src/note/note_identity_store.cpp src/clrop/hash.cpp `
        third_party/pdfium/lib/pdfium.dll.lib -lole32 -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Identity scope tests failed to compile.' }
    $previousPath = $env:PATH
    try {
        $env:PATH = "$(Join-Path $repoRoot 'third_party/pdfium/bin');$(Split-Path -Parent $compiler);$previousPath"
        foreach ($phase in @('', '--verify')) {
            if ($phase) { & $exe $fixture $phase } else { & $exe $fixture }
            if ($LASTEXITCODE -ne 0) { throw "Identity scope phase failed: $phase" }
        }
    } finally { $env:PATH = $previousPath }
    Write-Host "Identity scope tests passed. Evidence: $fixture"
} finally { Pop-Location }
