[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDir = Join-Path $repoRoot "out/tests/write_checks"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$testExe = Join-Path $outputDir "write_checks_tests.exe"
& $compiler -std=c++17 -O2 -Wall -Wextra -municode -static-libgcc -static-libstdc++ -I (Join-Path $repoRoot "src") `
    (Join-Path $repoRoot "tests/unit/write_checks_tests.cpp") `
    (Join-Path $repoRoot "src/diagnostics/write_checks.cpp") -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Write checks compilation failed." }
Push-Location $outputDir
try {
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "Write checks tests failed." }
    $environmentExe = Join-Path $outputDir "office_work_paths_tests.exe"
    & $compiler -std=c++17 -O2 -Wall -Wextra -municode -static-libgcc -static-libstdc++ -I (Join-Path $repoRoot "src") `
        (Join-Path $repoRoot "tests/unit/office_work_paths_tests.cpp") -o $environmentExe
    if ($LASTEXITCODE -ne 0) { throw "Office child environment compilation failed." }
    & $environmentExe
    if ($LASTEXITCODE -ne 0) { throw "Office child environment tests failed." }
    $uiExe = Join-Path $outputDir "write_checks_ui_tests.exe"
    & $compiler -std=c++17 -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE -static-libgcc -static-libstdc++ `
        -I (Join-Path $repoRoot "src") -I (Join-Path $repoRoot "third_party/pdfium/include") `
        (Join-Path $repoRoot "tests/unit/write_checks_ui_tests.cpp") `
        (Join-Path $repoRoot "src/ui/dialogs/write_checks_dialog.cpp") `
        (Join-Path $repoRoot "src/diagnostics/write_checks.cpp") -lcomctl32 -lgdi32 -o $uiExe
    if ($LASTEXITCODE -ne 0) { throw "Write checks UI compilation failed." }
    $process = Start-Process -FilePath $uiExe -WorkingDirectory $outputDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $outputDir "ui_stdout.log") -RedirectStandardError (Join-Path $outputDir "ui_stderr.log")
    $ownedHandle = $process.Handle
    if (-not $process.WaitForExit(30000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Write checks UI tests timed out; only the owned fixture was stopped."
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        $detail = Get-Content -LiteralPath (Join-Path $outputDir "ui_stderr.log") -Raw
        throw "Write checks UI tests failed: $detail"
    }
    Get-Content -LiteralPath (Join-Path $outputDir "ui_stdout.log")
}
finally { Pop-Location }
