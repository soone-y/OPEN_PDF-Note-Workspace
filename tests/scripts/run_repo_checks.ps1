[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipReadOnlyViewerBuild,
    [switch]$SkipAtomicWrite,
    [switch]$SkipPathSafety,
    [switch]$SkipFaultInjection,
    [switch]$SkipNoteParserTests,
    [switch]$SkipMermaidSubsetParserTests,
    [switch]$SkipClropJsonDirectParseTests,
    [switch]$SkipClropFileSafetyTests,
    [switch]$SkipDocxSpaceProtectionTests,
    [switch]$SkipRuntimeDependencyTests,
    [switch]$SkipThemeSwitchingTests,
    [switch]$SkipWorkspaceConfigTests,
    [switch]$SkipWorkspaceConfigRecoveryTests,
    [switch]$SkipAppLogContractTests,
    [switch]$SkipUiAutomation,
    [switch]$SkipPythonToolTests,
    [switch]$SkipCodebaseValidation,
    [switch]$SkipSafetyScan,
    [switch]$SkipLibreOfficeRuntimeGate,
    [switch]$SkipBinaryArtifactScan,
    [switch]$IncludeOfficeConversionTests,
    [string]$OfficeSoffice = "",
    [switch]$KeepOfficeConversionOutputs,
    [switch]$Rebuild,
    [switch]$Release,
    [switch]$VerboseOutput,
    [int]$FailureTailLines = 80,
    [string]$LogDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$fullBuildScript = Join-Path $repoRoot "full_build.ps1"
$fullReleaseScript = Join-Path $repoRoot "full_release.ps1"
$workspaceBuildScript = Join-Path $repoRoot "scripts\build\build_workspace.ps1"
$atomicWriteScript = Join-Path $PSScriptRoot "run_atomic_write_tests.ps1"
$pathSafetyScript = Join-Path $PSScriptRoot "run_path_safety_tests.ps1"
$faultInjectionScript = Join-Path $PSScriptRoot "run_fault_injection_tests.ps1"
$noteParserScript = Join-Path $PSScriptRoot "run_note_parser_tests.ps1"
$mermaidSubsetParserScript = Join-Path $PSScriptRoot "run_mermaid_subset_parser_tests.ps1"
$clropJsonDirectParseScript = Join-Path $PSScriptRoot "run_clrop_json_direct_parse_tests.ps1"
$clropFileSafetyScript = Join-Path $PSScriptRoot "run_clrop_file_safety_tests.ps1"
$docxSpaceProtectionScript = Join-Path $PSScriptRoot "run_docx_space_protection_tests.ps1"
$runtimeDependencyScript = Join-Path $PSScriptRoot "run_runtime_dependency_tests.ps1"
$themeSwitchingScript = Join-Path $PSScriptRoot "run_theme_switching_tests.ps1"
$workspaceConfigTestScript = Join-Path $PSScriptRoot "run_workspace_config_tests.ps1"
$workspaceConfigRecoveryTestScript = Join-Path $PSScriptRoot "run_workspace_config_recovery_tests.ps1"
$appLogContractTestScript = Join-Path $PSScriptRoot "run_app_log_contract_tests.ps1"
$officeConversionFixtureScript = Join-Path $PSScriptRoot "run_office_conversion_fixture_tests.ps1"
$uiAutomationScript = Join-Path $PSScriptRoot "run_ui_automation_fault_tests.ps1"
$pythonToolTestScript = Join-Path $repoRoot "tests\python\test_python_tools.py"
$codebaseValidationScript = Join-Path $repoRoot "tests\python\validate_codebase.py"
$safetyScanIgnoreFile = Join-Path $repoRoot "tests\config\safety_scan_ignore_globs.txt"
$binaryScanScript = Join-Path $repoRoot "tools\release_checks\binary_scan.py"
$libreOfficeRuntimeGateScript = Join-Path $repoRoot "tools\release_checks\libreoffice_runtime_gate.py"
$binaryOutputDir = Join-Path $repoRoot "out/bin"
$liteBinaryOutputDir = Join-Path $repoRoot "out/bin_lite"
$appVersionPath = Join-Path $repoRoot "APP_VERSION.txt"
$versionTrackedDocumentationPaths = @(
    (Join-Path $repoRoot "README.md"),
    (Join-Path $repoRoot "docs/public/README.md"),
    (Join-Path $repoRoot "Document/Index.md"),
    (Join-Path $repoRoot "Document/How_to_Build.md"),
    (Join-Path $repoRoot "Document/What_is_File_Formats.md"),
    (Join-Path $repoRoot "Document/How_to_Save_and_Recovery.md"),
    (Join-Path $repoRoot "Document/How_to_Setup.md"),
    (Join-Path $repoRoot "Document/How_to_Troubleshoot.md"),
    (Join-Path $repoRoot "Document/How_to_Use.md")
)
$appBuildInfoManifestPath = Join-Path $binaryOutputDir "pdf_note_workspace.exe.buildinfo.txt"
$liteAppBuildInfoManifestPath = Join-Path $liteBinaryOutputDir "pdf_note_workspace.exe.buildinfo.txt"
$readOnlyViewerBuildInfoManifestPath = Join-Path $binaryOutputDir "readonly_viewer.exe.buildinfo.txt"
$libreOfficeCustomRuntimeDir = Join-Path $repoRoot "third_party/libreoffice/custom_runtime/instdir"
$powershellExe = (Get-Command powershell -ErrorAction SilentlyContinue).Source
$pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
$repoCheckLogRoot = $LogDir
if ([string]::IsNullOrWhiteSpace($repoCheckLogRoot)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $repoCheckLogRoot = Join-Path $repoRoot "out/logs/repo_checks/$stamp"
}
$script:CurrentStepLogPath = ""
$script:CurrentStepName = ""

function Get-SafeLogFileName {
    param([Parameter(Mandatory)][string]$Name)
    $safe = $Name.ToLowerInvariant() -replace '[^a-z0-9]+', '_'
    $safe = $safe.Trim('_')
    if ([string]::IsNullOrWhiteSpace($safe)) {
        $safe = "step"
    }
    return "$safe.log"
}

function Append-StepLog {
    param([AllowNull()]$Line)
    if ([string]::IsNullOrWhiteSpace($script:CurrentStepLogPath)) {
        return
    }
    $text = if ($null -eq $Line) { "" } else { [string]$Line }
    Add-Content -LiteralPath $script:CurrentStepLogPath -Value $text -Encoding UTF8
}

function Write-StepLogHeader {
    param([Parameter(Mandatory)][string]$Name)
    Append-StepLog ("== {0} ==" -f $Name)
    Append-StepLog ("started: {0}" -f (Get-Date).ToString("o"))
    Append-StepLog ("cwd: {0}" -f (Get-Location).Path)
    Append-StepLog ""
}

function Write-StepLogFooter {
    param(
        [Parameter(Mandatory)][string]$Status,
        [Parameter(Mandatory)][TimeSpan]$Elapsed
    )
    Append-StepLog ""
    Append-StepLog ("status: {0}" -f $Status)
    Append-StepLog ("elapsed_sec: {0}" -f $Elapsed.TotalSeconds.ToString("F3", [System.Globalization.CultureInfo]::InvariantCulture))
    Append-StepLog ("finished: {0}" -f (Get-Date).ToString("o"))
}

function Write-FailureTail {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $tailCount = [Math]::Max(1, $FailureTailLines)
    Write-Host ("---- failure log tail ({0} lines): {1}" -f $tailCount, $Path) -ForegroundColor Yellow
    Get-Content -LiteralPath $Path -Tail $tailCount | ForEach-Object { Write-Host $_ }
    Write-Host "---- end failure log tail" -ForegroundColor Yellow
}

function Invoke-LoggedCommand {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$FailureMessage = "command failed"
    )

    Append-StepLog ("> {0} {1}" -f $FilePath, ($Arguments -join " "))
    $savedErrorActionPreference = $ErrorActionPreference
    $restoreNativeCommandErrorPreference = $false
    $previousNativeCommandErrorPreference = $false
    if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $restoreNativeCommandErrorPreference = $true
        $previousNativeCommandErrorPreference = $PSNativeCommandUseErrorActionPreference
        $PSNativeCommandUseErrorActionPreference = $false
    }
    $exitCode = 0
    try {
        $ErrorActionPreference = "Continue"
        & $FilePath @Arguments 2>&1 |
            ForEach-Object {
                if ($VerboseOutput) {
                    Write-Host $_
                }
                $_
            } |
            Out-File -LiteralPath $script:CurrentStepLogPath -Append -Encoding UTF8 -Width 4096
        $exitCode = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
        if ($restoreNativeCommandErrorPreference) {
            $PSNativeCommandUseErrorActionPreference = $previousNativeCommandErrorPreference
        }
    }
    if ($exitCode -ne 0) {
        throw ("{0}: {1} (exit={2})" -f $FailureMessage, $FilePath, $exitCode)
    }
}

function Invoke-Step {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][scriptblock]$Action
    )

    New-Item -ItemType Directory -Force -Path $repoCheckLogRoot | Out-Null
    $script:CurrentStepName = $Name
    $script:CurrentStepLogPath = Join-Path $repoCheckLogRoot (Get-SafeLogFileName $Name)
    if (Test-Path -LiteralPath $script:CurrentStepLogPath) {
        Remove-Item -LiteralPath $script:CurrentStepLogPath -Force
    }
    New-Item -ItemType File -Force -Path $script:CurrentStepLogPath | Out-Null
    Write-StepLogHeader -Name $Name
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host ""
    Write-Host ("== {0} ==" -f $Name) -ForegroundColor Cyan
    Write-Host ("log: {0}" -f $script:CurrentStepLogPath) -ForegroundColor DarkCyan
    try {
        & $Action
        $sw.Stop()
        Write-StepLogFooter -Status "PASS" -Elapsed $sw.Elapsed
        Write-Host ("[PASS] {0} ({1}s)" -f $Name, [Math]::Round($sw.Elapsed.TotalSeconds, 1)) -ForegroundColor Green
    }
    catch {
        $sw.Stop()
        Append-StepLog ("error: {0}" -f $_.Exception.Message)
        Write-StepLogFooter -Status "FAIL" -Elapsed $sw.Elapsed
        Write-Host ("[FAIL] {0} ({1}s)" -f $Name, [Math]::Round($sw.Elapsed.TotalSeconds, 1)) -ForegroundColor Red
        Write-Host ("detail log: {0}" -f $script:CurrentStepLogPath) -ForegroundColor Yellow
        Write-FailureTail -Path $script:CurrentStepLogPath
        throw
    }
    finally {
        $script:CurrentStepLogPath = ""
        $script:CurrentStepName = ""
    }
}

function Assert-ScriptExists {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing required script: $Path"
    }
}

function Assert-BuildInfoManifest {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$ExeName,
        [Parameter(Mandatory)][string]$ExpectedVersion,
        [string]$ExpectedEdition = ""
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Build info manifest not found: $Path"
    }

    $text = Get-Content -LiteralPath $Path -Raw
    if ($text -notmatch "(?m)^format`tpdf-note-build-info-v1`r?$") {
        throw "Build info manifest is missing the expected format line: $Path"
    }
    $escapedVersion = [regex]::Escape($ExpectedVersion)
    if ($text -notmatch ("(?m)^version`t{0}`r?$" -f $escapedVersion)) {
        throw "Build info manifest version does not match APP_VERSION.txt ('$ExpectedVersion'): $Path"
    }
    if ($text -notmatch "(?m)^build_timestamp`t.+`r?$") {
        throw "Build info manifest is missing the build timestamp line: $Path"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedEdition)) {
        $escapedEdition = [regex]::Escape($ExpectedEdition)
        if ($text -notmatch ("(?m)^edition`t{0}`r?$" -f $escapedEdition)) {
            throw "Build info manifest edition does not match '$ExpectedEdition': $Path"
        }
    }

    $manifestDir = Split-Path -Parent $Path
    $artifactRecords = [regex]::Matches($text, "(?m)^artifact`t([^`t`r`n]+)`t([0-9a-fA-F]{64})`r?$")
    if ($artifactRecords.Count -eq 0) {
        throw "Build info manifest contains no artifact SHA-256 entries: $Path"
    }
    $foundExpectedExe = $false
    foreach ($record in $artifactRecords) {
        $artifactName = $record.Groups[1].Value
        $expectedHash = $record.Groups[2].Value.ToLowerInvariant()
        if ([IO.Path]::GetFileName($artifactName) -ne $artifactName -or $artifactName -eq "." -or $artifactName -eq "..") {
            throw "Build info manifest contains an unsafe artifact name '$artifactName': $Path"
        }
        $artifactPath = Join-Path $manifestDir $artifactName
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            throw "Build info manifest artifact is missing: $artifactPath"
        }
        $actualHash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Build info manifest SHA-256 mismatch for '$artifactName': $Path"
        }
        if ($artifactName -eq $ExeName) {
            $foundExpectedExe = $true
        }
    }
    if (-not $foundExpectedExe) {
        throw "Build info manifest does not contain an artifact entry for ${ExeName}: $Path"
    }
}

function Get-AppVersionForVerification {
    if (-not (Test-Path -LiteralPath $appVersionPath)) {
        throw "App version file not found: $appVersionPath"
    }
    $version = (Get-Content -LiteralPath $appVersionPath -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($version)) {
        throw "App version file is empty: $appVersionPath"
    }
    return $version
}

function Assert-VersionTrackedDocumentation {
    param([Parameter(Mandatory)][string]$ExpectedVersion)

    foreach ($path in $versionTrackedDocumentationPaths) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Version-tracked documentation file not found: $path"
        }
        $text = Get-Content -LiteralPath $path -Raw
        if ($text -notmatch "(?m)^対象アプリ版: __APP_VERSION__\r?$") {
            throw "Version-tracked documentation is missing the __APP_VERSION__ marker: $path"
        }
    }
}

function Invoke-ChildPowerShellScript {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [string[]]$Arguments = @()
    )

    if ([string]::IsNullOrWhiteSpace($powershellExe)) {
        throw "powershell.exe not found in PATH."
    }

    $cmdArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $ScriptPath
    ) + $Arguments

    Invoke-LoggedCommand -FilePath $powershellExe -Arguments $cmdArgs -FailureMessage "script failed"
}

function Invoke-PythonScript {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [string[]]$Arguments = @()
    )

    if ([string]::IsNullOrWhiteSpace($pythonExe)) {
        throw "python.exe not found in PATH."
    }

    Invoke-LoggedCommand -FilePath $pythonExe -Arguments (@($ScriptPath) + $Arguments) -FailureMessage "python script failed"
}

function Invoke-PythonUnittestFile {
    param([Parameter(Mandatory)][string]$TestFile)

    if ([string]::IsNullOrWhiteSpace($pythonExe)) {
        throw "python.exe not found in PATH."
    }

    Invoke-LoggedCommand -FilePath $pythonExe -Arguments @("-m", "unittest", $TestFile) -FailureMessage "python unittest failed"
}

function Invoke-RipgrepScan {
    param(
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][string]$Pattern,
        [string[]]$AllowLinePatterns = @(),
        [string[]]$IncludeGlobs = @(
            "*.c", "*.cc", "*.cpp", "*.cxx", "*.c++",
            "*.h", "*.hh", "*.hpp", "*.hxx", "*.ipp", "*.inl",
            "*.inc", "*.cppinc", "*.ps1"
        )
    )

    $args = @(
        "-n",
        "-S"
    )
    foreach ($glob in $IncludeGlobs) {
        $args += @("--glob", $glob)
    }
    $ignoreGlobs = @()
    if (Test-Path -LiteralPath $safetyScanIgnoreFile) {
        $ignoreGlobs = Get-Content -LiteralPath $safetyScanIgnoreFile |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and -not $_.StartsWith("#") }
    }
    foreach ($glob in $ignoreGlobs) {
        $args += @("--glob", "!$glob")
    }
    $args += @($Pattern, ".")

    $hits = @(& rg @args 2>&1)
    $exitCode = $LASTEXITCODE

    switch ($exitCode) {
        0 {
            $violations = @()
            foreach ($hit in $hits) {
                $allowed = $false
                foreach ($allowPattern in $AllowLinePatterns) {
                    if ($hit -match $allowPattern) {
                        $allowed = $true
                        break
                    }
                }
                if (-not $allowed) {
                    $violations += $hit
                }
            }
            if ($violations.Count -eq 0) {
                Write-Host ("[OK] {0} ({1} allowed hits)" -f $Label, $hits.Count) -ForegroundColor Green
                Append-StepLog ("[OK] {0} ({1} allowed hits)" -f $Label, $hits.Count)
                $hits | ForEach-Object { Append-StepLog ("allowed: {0}" -f $_) }
                return
            }
            Write-Host ("[NG] {0}" -f $Label) -ForegroundColor Red
            Append-StepLog ("[NG] {0}" -f $Label)
            $violations | ForEach-Object { Write-Host $_ }
            $violations | ForEach-Object { Append-StepLog $_ }
            if ($AllowLinePatterns.Count -gt 0 -and $violations.Count -lt $hits.Count) {
                Append-StepLog ("allowed_hits: {0}" -f ($hits.Count - $violations.Count))
            }
            throw ("Safety scan failed: {0}" -f $Label)
        }
        1 {
            Write-Host ("[OK] {0}" -f $Label) -ForegroundColor Green
            Append-StepLog ("[OK] {0}" -f $Label)
        }
        default {
            Append-StepLog ("ripgrep failed during '{0}' (exit={1})" -f $Label, $exitCode)
            $hits | ForEach-Object { Append-StepLog $_ }
            throw ("ripgrep failed during '{0}' (exit={1})`n{2}" -f $Label, $exitCode, ($hits -join [Environment]::NewLine))
        }
    }
}

function Assert-FileOutputSystemDialogPolicy {
    $relativePath = "src\file_output\file_output.cpp"
    $path = Join-Path $repoRoot $relativePath
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing source file for system save dialog policy: $relativePath"
    }

    $source = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    $picker = $source.IndexOf("PickSavePathWithSystemDialog")
    $choice = $source.IndexOf("const SavePathPromptResult choice = PromptSavePath")
    $explicitBranch = $source.IndexOf("if (choice == SavePathPromptResult::OpenSystemDialog)", $choice)
    $pickerCall = $source.IndexOf("return PickSavePathWithSystemDialog", $explicitBranch)
    if ($picker -lt 0 -or $choice -lt 0 -or $explicitBranch -lt $choice -or $pickerCall -lt $explicitBranch) {
        throw "System save dialog policy violation: file_output must reach PickSavePathWithSystemDialog only after the explicit OpenSystemDialog choice."
    }

    Write-Host "[OK] System save dialog explicit-choice policy" -ForegroundColor Green
    Append-StepLog "[OK] System save dialog explicit-choice policy"
}

function Invoke-SafetyScans {
    if (-not (Get-Command rg -ErrorAction SilentlyContinue)) {
        throw "rg not found in PATH."
    }

    $schemaUrlAllowList = @(
        'https?://openoffice\.org/',
        'https?://www\.w3\.org/',
        'https?://schemas\.openxmlformats\.org/',
        'https?://purl\.org/'
    )

    # AGENTS.md permits OS standard file dialogs only for explicit user file operations.
    # Keep this allow-list narrow so accidental shell dialog additions still fail the scan.
    $shellFileDialogAllowList = @(
        '\\src\\readonly_viewer\\main\.cpp:\d+:.*\b(?:IFileOpenDialog|CLSID_FileOpenDialog)\b',
        '\\src\\readonly_viewer\\pdf_preview_panel\.cpp:\d+:.*\b(?:IFileOpenDialog|CLSID_FileOpenDialog)\b',
        '\\src\\readonly_viewer\\pdf_preview_panel\.cpp:\d+:.*\b(?:IFileSaveDialog|CLSID_FileSaveDialog)\b',
        '\\src\\ui\\lists\\main_local_path_browser\.cppinc:\d+:.*\b(?:IFileOpenDialog|CLSID_FileOpenDialog)\b',
        '\\src\\workspace\\workspace_actions\.cpp:\d+:.*\b(?:IFileSaveDialog|CLSID_FileSaveDialog)\b',
        '\\src\\file_output\\file_output\.cpp:\d+:.*\b(?:IFileSaveDialog|CLSID_FileSaveDialog)\b'
    )

    $scanRules = @(
        [pscustomobject]@{
            Label = "Network API scan"
            IncludeGlobs = @(
                "*.c", "*.cc", "*.cpp", "*.cxx", "*.c++",
                "*.h", "*.hh", "*.hpp", "*.hxx", "*.ipp", "*.inl",
                "*.inc", "*.cppinc", "*.ps1"
            )
            Pattern = '\b(?:[Ww]in[Hh]ttp|[Ww]in[Ii]net|[Uu]rlmon|[Ww]eb[Ss]ocket|URLDownloadToFile[A-Z]*|InternetOpen[A-Z]*|InternetConnect[A-Z]*|HttpOpenRequest[A-Z]*|WSAStartup|WSASocket[A-Z]*|curl_(?:easy|multi))\b|\b(?:connect|send|recv|socket)\s*\('
            AllowLinePatterns = @()
        },
        [pscustomobject]@{
            Label = "Network URL literal scan"
            IncludeGlobs = @(
                "*.c", "*.cc", "*.cpp", "*.cxx", "*.c++",
                "*.h", "*.hh", "*.hpp", "*.hxx", "*.ipp", "*.inl",
                "*.inc", "*.cppinc", "*.ps1"
            )
            Pattern = 'https?://'
            AllowLinePatterns = $schemaUrlAllowList
        },
        [pscustomobject]@{
            Label = "Sound API scan"
            IncludeGlobs = @(
                "*.c", "*.cc", "*.cpp", "*.cxx", "*.c++",
                "*.h", "*.hh", "*.hpp", "*.hxx", "*.ipp", "*.inl",
                "*.inc", "*.cppinc", "*.ps1"
            )
            Pattern = '\b(?:PlaySound[A-Z]*|MessageBeep|Beep)\s*\('
            AllowLinePatterns = @()
        },
        [pscustomobject]@{
            Label = "Dialog API scan"
            IncludeGlobs = @(
                "*.c", "*.cc", "*.cpp", "*.cxx", "*.c++",
                "*.h", "*.hh", "*.hpp", "*.hxx", "*.ipp", "*.inl",
                "*.inc", "*.cppinc", "*.ps1"
            )
            Pattern = '\b(?:MessageBox[A-Z]*|DialogBox[A-Z]*|CreateDialog[A-Z]*|TaskDialog|TaskDialogIndirect)\s*\('
            AllowLinePatterns = @()
        },
        [pscustomobject]@{
            Label = "Forbidden Shell File Dialog API scan"
            IncludeGlobs = @(
                "*.c", "*.cc", "*.cpp", "*.cxx", "*.c++",
                "*.h", "*.hh", "*.hpp", "*.hxx", "*.ipp", "*.inl",
                "*.inc", "*.cppinc", "*.ps1"
            )
            Pattern = '\b(?:IFileOpenDialog|CLSID_FileOpenDialog|IFileSaveDialog|CLSID_FileSaveDialog|GetOpenFileName[A-Z]*|GetSaveFileName[A-Z]*|SHBrowseForFolder[A-Z]*)\b'
            AllowLinePatterns = $shellFileDialogAllowList
        },
        [pscustomobject]@{
            Label = "Python network API scan"
            IncludeGlobs = @("*.py")
            Pattern = '^\s*(?:from|import)\s+(?:urllib|urllib3|requests|httpx|aiohttp|http\.client|socket|websocket|webbrowser|ftplib|smtplib|imaplib|poplib|telnetlib|xmlrpc\.client)\b|^\s*from\s+http\s+import\s+client\b|\b(?:urllib\.request|urllib3\.|requests\.(?:get|post|put|patch|delete|request)|httpx\.|aiohttp\.|http\.client|socket\.(?:socket|create_connection)|asyncio\.(?:open_connection|start_server)|websocket\.|webbrowser\.|ftplib\.|smtplib\.|imaplib\.|poplib\.|telnetlib\.|xmlrpc\.client)'
            AllowLinePatterns = @()
        }
    )

    foreach ($rule in $scanRules) {
        Invoke-RipgrepScan `
            -Label $rule.Label `
            -IncludeGlobs $rule.IncludeGlobs `
            -Pattern $rule.Pattern `
            -AllowLinePatterns $rule.AllowLinePatterns
    }
    Assert-FileOutputSystemDialogPolicy
}

function Invoke-BinaryArtifactScan {
    if (-not (Test-Path -LiteralPath $binaryOutputDir) -or -not (Test-Path -LiteralPath $liteBinaryOutputDir)) {
        if ($SkipBuild) {
            Write-Host "[SKIP] Binary Artifact Network Import Scan: Full or Lite output does not exist and build was skipped."
            return
        }
        throw "Missing Full or Lite build output directory for binary artifact scan."
    }
    if ($SkipBuild) {
        Write-Host "[INFO] Build was skipped; scanning existing Full and Lite artifacts only."
    }

    Invoke-PythonScript -ScriptPath $binaryScanScript -Arguments @(
        "--include", "out/bin",
        "--include", "out/bin_lite",
        "--imports-only",
        "--imported-dll", "winhttp.dll",
        "--imported-dll", "wininet.dll",
        "--imported-dll", "urlmon.dll",
        "--imported-dll", "ws2_32.dll",
        "--imported-dll", "wsock32.dll",
        "--imported-dll", "websocket.dll",
        "--fail-on-import",
        "--fail-on-unparseable-pe"
    )
}

function Invoke-LibreOfficeRuntimeGate {
    if (-not (Test-Path -LiteralPath $libreOfficeCustomRuntimeDir)) {
        throw "Missing LibreOffice custom runtime directory: $libreOfficeCustomRuntimeDir"
    }

    Invoke-PythonScript -ScriptPath $libreOfficeRuntimeGateScript -Arguments @(
        "--image", $libreOfficeCustomRuntimeDir
    )
}

$repoCheckExitCode = 0
Push-Location -LiteralPath $repoRoot
try {
    Assert-ScriptExists -Path $atomicWriteScript
    Assert-ScriptExists -Path $pathSafetyScript
    Assert-ScriptExists -Path $faultInjectionScript
    Assert-ScriptExists -Path $noteParserScript
    Assert-ScriptExists -Path $mermaidSubsetParserScript
    Assert-ScriptExists -Path $clropJsonDirectParseScript
    Assert-ScriptExists -Path $clropFileSafetyScript
    Assert-ScriptExists -Path $docxSpaceProtectionScript
    Assert-ScriptExists -Path $runtimeDependencyScript
    Assert-ScriptExists -Path $themeSwitchingScript
    Assert-ScriptExists -Path $workspaceConfigTestScript
    Assert-ScriptExists -Path $appLogContractTestScript
    Assert-ScriptExists -Path $officeConversionFixtureScript
    Assert-ScriptExists -Path $uiAutomationScript
    Assert-ScriptExists -Path $pythonToolTestScript
    Assert-ScriptExists -Path $codebaseValidationScript
    Assert-ScriptExists -Path $safetyScanIgnoreFile
    Assert-ScriptExists -Path $binaryScanScript
    Assert-ScriptExists -Path $libreOfficeRuntimeGateScript

    if (-not $SkipBuild) {
        Assert-ScriptExists -Path $fullBuildScript
        Assert-ScriptExists -Path $fullReleaseScript
        Assert-ScriptExists -Path $workspaceBuildScript
        Invoke-Step -Name "Build" -Action {
            if ($Release) {
                if ($SkipReadOnlyViewerBuild) {
                    throw "-Release cannot be combined with -SkipReadOnlyViewerBuild because a full release always includes every application."
                }
                if ($Rebuild -or $VerboseOutput) {
                    throw "Use full_release.ps1 directly for a complete release; -Rebuild and -VerboseOutput are not supported with -Release."
                }
                Invoke-ChildPowerShellScript -ScriptPath $fullReleaseScript
                return
            }

            if ($SkipReadOnlyViewerBuild) {
                $workspaceBuildArgs = @("-Edition", "Full")
                if ($Rebuild) { $workspaceBuildArgs += "-Rebuild" }
                if ($VerboseOutput) { $workspaceBuildArgs += "-VerboseOutput" }
                Invoke-ChildPowerShellScript -ScriptPath $workspaceBuildScript -Arguments $workspaceBuildArgs
                return
            }

            $fullBuildArgs = @()
            if ($Rebuild) { $fullBuildArgs += "-Rebuild" }
            if ($VerboseOutput) { $fullBuildArgs += "-VerboseOutput" }
            Invoke-ChildPowerShellScript -ScriptPath $fullBuildScript -Arguments $fullBuildArgs
        }

    }

    Invoke-Step -Name "Version Consistency" -Action {
        $expectedVersion = Get-AppVersionForVerification
        Assert-VersionTrackedDocumentation -ExpectedVersion $expectedVersion
        Assert-BuildInfoManifest -Path $appBuildInfoManifestPath -ExeName "pdf_note_workspace.exe" -ExpectedVersion $expectedVersion -ExpectedEdition "full"
        Assert-BuildInfoManifest -Path $liteAppBuildInfoManifestPath -ExeName "pdf_note_workspace.exe" -ExpectedVersion $expectedVersion -ExpectedEdition "lite"
        if (-not $SkipReadOnlyViewerBuild) {
            Assert-BuildInfoManifest -Path $readOnlyViewerBuildInfoManifestPath -ExeName "readonly_viewer.exe" -ExpectedVersion $expectedVersion
        }
    }

    if (-not $SkipAtomicWrite) {
        Invoke-Step -Name "Atomic Write Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $atomicWriteScript
        }
    }

    if (-not $SkipPathSafety) {
        Invoke-Step -Name "Path Safety Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $pathSafetyScript
        }
    }

    if (-not $SkipFaultInjection) {
        Invoke-Step -Name "Fault Injection Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $faultInjectionScript
        }
    }

    if (-not $SkipNoteParserTests) {
        Invoke-Step -Name "Note Parser Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $noteParserScript
        }
    }

    if (-not $SkipMermaidSubsetParserTests) {
        Invoke-Step -Name "Mermaid Subset Parser Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $mermaidSubsetParserScript
        }
    }

    if (-not $SkipClropJsonDirectParseTests) {
        Invoke-Step -Name "Clrop JSON Direct Parse Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $clropJsonDirectParseScript
        }
    }

    if (-not $SkipClropFileSafetyTests) {
        Invoke-Step -Name "Clrop File Safety Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $clropFileSafetyScript
        }
    }

    if (-not $SkipDocxSpaceProtectionTests) {
        Invoke-Step -Name "DOCX Space Protection Tests" -Action {
            $docxArgs = @()
            if ($IncludeOfficeConversionTests -and -not [string]::IsNullOrWhiteSpace($OfficeSoffice)) {
                $docxArgs += @("-Soffice", $OfficeSoffice)
                if ($KeepOfficeConversionOutputs) { $docxArgs += "-KeepLibreOfficeSmoke" }
            }
            Invoke-ChildPowerShellScript -ScriptPath $docxSpaceProtectionScript -Arguments $docxArgs
        }
    }

    if (-not $SkipRuntimeDependencyTests) {
        Invoke-Step -Name "Runtime Dependency Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $runtimeDependencyScript
        }
    }

    if (-not $SkipThemeSwitchingTests) {
        Invoke-Step -Name "Theme Switching Source Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $themeSwitchingScript
        }
    }

    if (-not $SkipWorkspaceConfigTests) {
        Invoke-Step -Name "Workspace Configuration Round-Trip Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $workspaceConfigTestScript
        }
    }

    if (-not $SkipWorkspaceConfigRecoveryTests) {
        Invoke-Step -Name "Workspace Configuration Recovery Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $workspaceConfigRecoveryTestScript
        }
    }

    if (-not $SkipAppLogContractTests) {
        Invoke-Step -Name "App Log Contract Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $appLogContractTestScript
        }
    }

    if (-not $SkipUiAutomation) {
        Invoke-Step -Name "UI Automation Rollback Tests" -Action {
            Invoke-ChildPowerShellScript -ScriptPath $uiAutomationScript
        }
    }

    if ($IncludeOfficeConversionTests) {
        Invoke-Step -Name "Office Conversion Fixture Tests" -Action {
            $officeArgs = @()
            if (-not [string]::IsNullOrWhiteSpace($OfficeSoffice)) {
                $officeArgs += @("-Soffice", $OfficeSoffice)
            }
            if ($KeepOfficeConversionOutputs) { $officeArgs += "-Keep" }
            Invoke-ChildPowerShellScript -ScriptPath $officeConversionFixtureScript -Arguments $officeArgs
        }
    }

    if (-not $SkipPythonToolTests) {
        Invoke-Step -Name "Python Tool Tests" -Action {
            Invoke-PythonUnittestFile -TestFile $pythonToolTestScript
        }
    }

    if (-not $SkipCodebaseValidation) {
        Invoke-Step -Name "Codebase Validation" -Action {
            Invoke-PythonScript -ScriptPath $codebaseValidationScript
        }
    }

    if (-not $SkipSafetyScan) {
        Invoke-Step -Name "Safety Scans" -Action {
            Invoke-SafetyScans
        }
    }

    if (-not $SkipLibreOfficeRuntimeGate) {
        Invoke-Step -Name "LibreOffice Runtime Gate" -Action {
            Invoke-LibreOfficeRuntimeGate
        }
    }

    if (-not $SkipBinaryArtifactScan) {
        Invoke-Step -Name "Binary Artifact Network Import Scan" -Action {
            Invoke-BinaryArtifactScan
        }
    }

    Write-Host ""
    Write-Host "All selected checks passed." -ForegroundColor Green
}
catch {
    $repoCheckExitCode = 1
    Write-Host ""
    Write-Host ("Repo checks failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
}
finally {
    Pop-Location
}

if ($repoCheckExitCode -ne 0) {
    exit $repoCheckExitCode
}
