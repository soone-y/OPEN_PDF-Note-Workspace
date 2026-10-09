[CmdletBinding()]
param([switch]$AppIntegration, [string]$AppExecutable = "")
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($AppExecutable -and -not $AppIntegration) { throw "-AppExecutable requires -AppIntegration." }
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
& (Join-Path $PSScriptRoot "run_artifact_usage_tests.ps1")
if (-not $?) { throw "Artifact capacity test runner failed." }
$outputDir = Join-Path $repoRoot "out/tests"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$resource = Join-Path $outputDir "workspace_memo_tests_resource.o"
& windres --codepage=65001 -I (Join-Path $repoRoot "src") -I (Join-Path $repoRoot "src/resources") `
    -i (Join-Path $repoRoot "src/resources/app.rc") -o $resource -O coff
if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed." }
$testExe = Join-Path $outputDir "workspace_memo_tests.exe"
& $compiler -std=c++17 -Wall -Wextra -DWORKSPACE_MEMO_TESTING -I (Join-Path $repoRoot "src") `
    (Join-Path $repoRoot "tests/unit/workspace_memo_tests.cpp") `
    (Join-Path $repoRoot "src/workspace/workspace_memo_store.cpp") $resource -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Workspace memo test compilation failed." }
Push-Location $repoRoot
try {
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "Workspace memo tests failed." }
    $uiExe = Join-Path $outputDir "workspace_memo_ui_tests.exe"
    & $compiler -std=c++17 -Wall -Wextra -DWORKSPACE_MEMO_TESTING -I (Join-Path $repoRoot "src") `
        -I (Join-Path $repoRoot "third_party/pdfium/include") `
        (Join-Path $repoRoot "tests/unit/workspace_memo_ui_tests.cpp") `
        (Join-Path $repoRoot "src/workspace/workspace_memo.cpp") `
        (Join-Path $repoRoot "src/workspace/workspace_memo_store.cpp") $resource `
        -lcomctl32 -limm32 -lgdi32 -o $uiExe
    if ($LASTEXITCODE -ne 0) { throw "Workspace memo UI compilation failed." }
    & $uiExe
    if ($LASTEXITCODE -ne 0) { throw "Workspace memo UI tests failed." }
    & python (Join-Path $repoRoot "tests/python/test_workspace_memo_integration.py")
    if ($LASTEXITCODE -ne 0) { throw "Workspace memo integration checks failed." }
    if ($AppIntegration) {
        # Build the current JA app first. Copy runtime only to a new fixture;
        # never run against the user's workspace/setup or delete past fixtures.
        $appTests = Join-Path $outputDir "workspace_memo_app_tests.exe"
        # Pin the test driver's C++ runtime. An earlier unrelated DLL on PATH
        # must not prevent the driver from entering main (0xc0000139).
        & $compiler -std=c++17 -Wall -Wextra -static-libgcc -static-libstdc++ -I (Join-Path $repoRoot "src") `
            -I (Join-Path $repoRoot "third_party/pdfium/include") `
            (Join-Path $repoRoot "tests/integration/workspace_memo_app_tests.cpp") `
            (Join-Path $repoRoot "src/clrop/json.cpp") $resource -o $appTests
        if ($LASTEXITCODE -ne 0) { throw "Workspace memo actual-app test compilation failed." }
        $fixture = Join-Path $outputDir ("workspace_memo_app_" + [guid]::NewGuid().ToString("N"))
        $appDir = Join-Path $fixture "app"
        New-Item -ItemType Directory -Path $appDir -Force | Out-Null
        # An explicit build may have a different name when the normal exe is
        # running/locked. Always test a fresh copy with the driver's fixed name.
        $binarySource = if ($AppExecutable) { (Get-Item -LiteralPath $AppExecutable -ErrorAction Stop).FullName }
            else { Join-Path $repoRoot "out/bin/pdf_note_workspace.exe" }
        if (-not (Test-Path -LiteralPath $binarySource -PathType Leaf)) { throw "Application source executable is missing: $binarySource" }
        $binaryDir = Split-Path -Parent $binarySource
        Copy-Item -LiteralPath $binarySource -Destination (Join-Path $appDir "pdf_note_workspace.exe")
        Get-ChildItem -LiteralPath $binaryDir -Filter "*.dll" -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $appDir
        }
        $savedRoot = [Environment]::GetEnvironmentVariable("WORKSPACE_MEMO_APP_TEST_ROOT", "Process")
        try {
            [Environment]::SetEnvironmentVariable("WORKSPACE_MEMO_APP_TEST_ROOT", $fixture, "Process")
            & $appTests
            if ($LASTEXITCODE -ne 0) { throw "Workspace memo actual-application tests failed." }
        } finally {
            [Environment]::SetEnvironmentVariable("WORKSPACE_MEMO_APP_TEST_ROOT", $savedRoot, "Process")
        }
    }
} finally { Pop-Location }
