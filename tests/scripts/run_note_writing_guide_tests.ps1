[CmdletBinding()]
param([ValidateSet("ja", "en")][string]$Locale = "ja", [ValidateRange(30, 300)][int]$TimeoutSeconds = 120)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$binDir = Join-Path $repoRoot $(if ($Locale -eq "en") { "out/bin_en" } else { "out/bin" })
$fixture = Join-Path $repoRoot ("out/tests/note guide_" + $Locale + "_" + [char]0x65e5 + [char]0x672c + "_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $fixture -ErrorAction Stop | Out-Null
[IO.File]::WriteAllText((Join-Path $fixture 'owned_fixture.txt'), "note_writing_guide_fixture_v1`n", [Text.Encoding]::ASCII)
# Spaces and Unicode exercise command-line quoting. Never reuse/delete a run.
$appDir = Join-Path $fixture "app"
New-Item -ItemType Directory -Path $appDir -ErrorAction Stop | Out-Null
foreach ($executable in @("pdf_note_workspace.exe", "readonly_viewer.exe")) {
    $source = Join-Path $binDir $executable
    $metadata = Get-Content -LiteralPath "$source.buildinfo.txt" -Raw -ErrorAction Stop
    if ($metadata -notmatch ("(?m)^locale\s+" + [regex]::Escape($Locale) + '\s*$')) { throw "Build locale mismatch: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $appDir $executable)
    $artifacts = [regex]::Matches($metadata, '(?m)^artifact\t([^\t\r\n]+)\t([0-9a-fA-F]{64})\s*$')
    if ($artifacts.Count -lt 2) { throw "Missing artifact receipt: $source" }
    $observedExe = $false
    foreach ($artifact in $artifacts) {
        $name = $artifact.Groups[1].Value
        if ([IO.Path]::GetFileName($name) -ne $name) { throw "Invalid artifact receipt path: $name" }
        if ($name -eq $executable) { $observedExe = $true }
        elseif ($name.EndsWith(".dll", [StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath (Join-Path $binDir $name) -Destination (Join-Path $appDir $name)
        } else { throw "Unexpected runtime artifact: $name" }
        if ((Get-FileHash -LiteralPath (Join-Path $appDir $name)).Hash -ne $artifact.Groups[2].Value) {
            throw "Copied executable/runtime differs from build receipt: $name"
        }
    }
    if (-not $observedExe) { throw "Build receipt does not identify executable: $source" }
    Copy-Item -LiteralPath "$source.buildinfo.txt" -Destination (Join-Path $fixture "$executable.buildinfo.txt")
}
Copy-Item -LiteralPath (Join-Path $repoRoot "docs/$Locale") -Destination (Join-Path $appDir "docs") -Recurse
Copy-Item -LiteralPath (Join-Path $binDir "LICENSE.md") -Destination $appDir
Copy-Item -LiteralPath (Join-Path $binDir "licenses") -Destination $appDir -Recurse
python -B (Join-Path $repoRoot "tools/dev/prepare_readonly_viewer_next_licenses.py") --locale $Locale --variant standard --output $appDir --check
if ($LASTEXITCODE -ne 0) { throw "Isolated viewer license check failed." }
$driver = Join-Path $fixture "note_writing_guide_app_tests.exe"
& g++ -std=c++17 -municode -Wall -Wextra -Werror -Wno-unused-function -static-libgcc -static-libstdc++ `
    -I (Join-Path $repoRoot "src") -I (Join-Path $repoRoot "third_party/pdfium/include") `
    (Join-Path $repoRoot "tests/integration/note_writing_guide_app_tests.cpp") -o $driver
if ($LASTEXITCODE -ne 0) { throw "Writing guide integration driver compilation failed." }
$stdout = Join-Path $fixture "stdout.txt"
$stderr = Join-Path $fixture "stderr.txt"
Write-Host "Guide app test ($Locale): foreground allowed; owned main and viewer copies only. Evidence: $fixture"
$process = Start-Process -FilePath $driver -ArgumentList @(('"' + $fixture + '"'), $Locale) `
    -WorkingDirectory $repoRoot -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    $process.Kill(); $process.WaitForExit(5000) | Out-Null
    throw "Guide test timed out; job-owned children terminated. Evidence: $fixture"
}
Get-Content -LiteralPath $stdout | Write-Host
Get-Content -LiteralPath $stderr | Write-Host
if ($process.ExitCode -ne 0) { throw "Guide app test failed (exit=$($process.ExitCode)). Evidence: $fixture" }
$report = Get-Content -LiteralPath $stdout -Raw
foreach ($marker in @("[PASS] guide button opens actual locale Markdown", "[PASS] missing guide and missing viewer", "[PASS] owned shutdown and original bytes unchanged")) {
    if (-not $report.Contains($marker)) { throw "Required guide observation missing: $marker" }
}
Write-Host "PASS: writing guide launch, missing assets and data preservation. Real IME, OS DPI changes and audible silence were not observed."
