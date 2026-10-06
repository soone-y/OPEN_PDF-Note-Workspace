[CmdletBinding()]
param([switch]$AppIntegration)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
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
        & $compiler -std=c++17 -Wall -Wextra -I (Join-Path $repoRoot "src") `
            (Join-Path $repoRoot "tests/integration/workspace_memo_app_tests.cpp") $resource -o $appTests
        if ($LASTEXITCODE -ne 0) { throw "Workspace memo actual-app test compilation failed." }
        $fixture = Join-Path $outputDir ("workspace_memo_app_" + [guid]::NewGuid().ToString("N"))
        $appDir = Join-Path $fixture "app"
        New-Item -ItemType Directory -Path $appDir -Force | Out-Null
        $binaryDir = Join-Path $repoRoot "out/bin"
        Copy-Item -LiteralPath (Join-Path $binaryDir "pdf_note_workspace.exe") -Destination $appDir
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
