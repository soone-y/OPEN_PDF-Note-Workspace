[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$publishScriptPath = Join-Path $repoRoot "publish.ps1"
if (-not (Test-Path -LiteralPath $publishScriptPath)) {
    throw "Missing publish script: $publishScriptPath"
}

$tokens = $null
$parseErrors = $null
$publishAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $publishScriptPath,
    [ref]$tokens,
    [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
    throw "publish.ps1 could not be parsed: $($parseErrors[0].Message)"
}
. (Join-Path $repoRoot 'scripts/release/release_version.ps1')

foreach ($helperName in @("Get-Sha256", "Get-EffectiveAllowlistEntries", "Get-AllowlistDiffLines")) {
    $helperAst = @($publishAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq $helperName
    }, $true))
    if ($helperAst.Count -ne 1) { throw "Expected exactly one $helperName function." }
    . ([scriptblock]::Create($helperAst[0].Extent.Text))
}

function Assert-Equal {
    param(
        [Parameter(Mandatory)][string]$Actual,
        [Parameter(Mandatory)][string]$Expected,
        [Parameter(Mandatory)][string]$Message
    )
    if ($Actual -ne $Expected) {
        throw "$Message Expected '$Expected', got '$Actual'."
    }
}

function New-ChecklistFixture {
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][bool]$IncludeVersionLine,
        [Parameter(Mandatory)][bool]$HasUncheckedItems,
        [Parameter(Mandatory)][bool]$HasSubmissionResult,
        [Parameter(Mandatory)][datetime]$LastWriteTime
    )

    $lines = @( "公開前チェックリスト（ローカル専用）" )
    if ($IncludeVersionLine) {
        $lines += "版番号 / タグ: v$Version"
    }
    if ($HasUncheckedItems) {
        $lines += "- [ ] 手作業の確認が残っている。"
    }
    else {
        $lines += "- [x] 全確認済み。"
    }
    if ($HasSubmissionResult) {
        $lines += "提出結果: GitHub Release draft created"
    }

    $path = Join-Path $Directory $Name
    Set-Content -LiteralPath $path -Value $lines -Encoding UTF8
    (Get-Item -LiteralPath $path).LastWriteTime = $LastWriteTime
}

$fixtureBase = Join-Path $repoRoot 'out/tests'
New-Item -ItemType Directory -Path $fixtureBase -Force | Out-Null
$fixtureDirectory = Join-Path $fixtureBase ("pdf_note_publish_version_history_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $fixtureDirectory | Out-Null
try {
    $baseTime = Get-Date "2026-08-27T00:00:00"
    New-ChecklistFixture -Directory $fixtureDirectory `
        -Name "publish_公開前確認文書_pdf_note_workspace_release_set_0.9.501_legacy.txt" `
        -Version "0.9.501" -IncludeVersionLine $false -HasUncheckedItems $false -HasSubmissionResult $false `
        -LastWriteTime $baseTime.AddMinutes(1)
    New-ChecklistFixture -Directory $fixtureDirectory `
        -Name "publish_公開前確認文書_pdf_note_workspace_release_0.9.503_pair_ja-en_pair.txt" `
        -Version "0.9.503" -IncludeVersionLine $true -HasUncheckedItems $true -HasSubmissionResult $true `
        -LastWriteTime $baseTime.AddMinutes(2)
    New-ChecklistFixture -Directory $fixtureDirectory `
        -Name "publish_公開前確認文書_pdf_note_workspace_release_0.9.504_pending_ja-en_pair.txt" `
        -Version "0.9.504" -IncludeVersionLine $true -HasUncheckedItems $true -HasSubmissionResult $false `
        -LastWriteTime $baseTime.AddMinutes(3)

    $selectedVersion = Get-PreviousSubmittedVersion -RecordsDirectory $fixtureDirectory
    Assert-Equal -Actual $selectedVersion -Expected "0.9.503" -Message "The latest submitted pair checklist must be selected over legacy and pending records."

    $nonPairFixtures = @(
        Get-ChildItem -LiteralPath $fixtureDirectory -File |
            Where-Object { $_.Name -notlike "*0.9.503*" }
    )
    foreach ($fixture in $nonPairFixtures) {
        Remove-Item -LiteralPath $fixture.FullName
    }
    $selectedVersion = Get-PreviousSubmittedVersion -RecordsDirectory $fixtureDirectory
    Assert-Equal -Actual $selectedVersion -Expected "0.9.503" -Message "A current paired checklist must supply its version from its body, not its filename."

    New-ChecklistFixture -Directory $fixtureDirectory -Name 'PUBLISH_CHECKLIST_confirmed_only.txt' `
        -Version '0.9.900' -IncludeVersionLine $true -HasUncheckedItems $false -HasSubmissionResult $false `
        -LastWriteTime $baseTime.AddMinutes(5)
    Assert-Equal -Actual (Get-PreviousSubmittedVersion $fixtureDirectory) -Expected '0.9.503' -Message 'Confirmation alone must not consume a version.'
    New-ChecklistFixture -Directory $fixtureDirectory -Name 'PUBLISH_CHECKLIST_older_touched.txt' `
        -Version '0.9.502' -IncludeVersionLine $true -HasUncheckedItems $false -HasSubmissionResult $true `
        -LastWriteTime $baseTime.AddMinutes(6)
    Assert-Equal -Actual (Get-PreviousSubmittedVersion $fixtureDirectory) -Expected '0.9.503' -Message 'Version ordering must not depend on file modification time.'

    $versionRoot = Join-Path $fixtureDirectory 'repository'
    $versionRecords = Join-Path $versionRoot 'docs/internal/archive/operations/publish_records'
    New-Item -ItemType Directory -Path $versionRecords -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $fixtureDirectory 'publish_公開前確認文書_pdf_note_workspace_release_0.9.503_pair_ja-en_pair.txt') -Destination $versionRecords
    $versionPath = Join-Path $versionRoot 'REPO_VERSION.txt'
    Set-Content -LiteralPath $versionPath -Value '0.9.503' -Encoding UTF8
    $lease = Enter-ReleaseVersion -RepositoryRoot $versionRoot
    try {
        Assert-Equal -Actual $lease.Version -Expected '0.9.504' -Message 'Release preparation must advance a submitted version.'
        $blocked = $false
        try { $unexpected = Enter-ReleaseVersion -RepositoryRoot $versionRoot; Exit-ReleaseVersion $unexpected } catch { $blocked = $true }
        if (-not $blocked) { throw 'Concurrent release preparation was accepted.' }
        $blocked = $false
        try { [System.IO.File]::WriteAllText($versionPath, '0.9.900') } catch { $blocked = $true }
        if (-not $blocked) { throw 'The version must remain read-only until both locales are frozen.' }
        Assert-Equal -Actual ((Get-Content $versionPath -Raw).Trim()) -Expected '0.9.504' -Message 'Read sharing must allow child build inputs.'
    }
    finally { Exit-ReleaseVersion $lease }
    $lease = Enter-ReleaseVersion $versionRoot
    try { Assert-Equal -Actual $lease.Version -Expected '0.9.504' -Message 'A retry must retain its unsubmitted version.' }
    finally { Exit-ReleaseVersion $lease }
    Set-Content -LiteralPath $versionPath -Value '0.9.600' -Encoding UTF8
    $lease = Enter-ReleaseVersion $versionRoot
    try { Assert-Equal -Actual $lease.Version -Expected '0.9.600' -Message 'An explicitly advanced version must be retained.' }
    finally { Exit-ReleaseVersion $lease }
    foreach ($invalid in @('0.9.502', 'invalid')) {
        Set-Content -LiteralPath $versionPath -Value $invalid -Encoding UTF8
        $blocked = $false
        try { $unexpected = Enter-ReleaseVersion $versionRoot; Exit-ReleaseVersion $unexpected } catch { $blocked = $true }
        if (-not $blocked) { throw "Invalid/stale version was accepted: $invalid" }
        Assert-Equal -Actual ((Get-Content $versionPath -Raw).Trim()) -Expected $invalid -Message 'Rejected inputs must remain unchanged.'
    }
    Set-Content -LiteralPath $versionPath -Value '0.9.503' -Encoding UTF8
    foreach ($lockedPath in @((Join-Path $versionRoot 'out/build.lock'), $versionPath)) {
        $lock = [System.IO.File]::Open($lockedPath, 'OpenOrCreate', 'Read', 'Read')
        try {
            $blocked = $false
            try { $unexpected = Enter-ReleaseVersion $versionRoot; Exit-ReleaseVersion $unexpected } catch { $blocked = $true }
            if (-not $blocked) { throw "Locked input accepted: $lockedPath" }
            Assert-Equal -Actual ((Get-Content $versionPath -Raw).Trim()) -Expected '0.9.503' -Message 'Lock/replace failure must preserve original version.'
        }
        finally { $lock.Dispose() }
    }
    $backups = @(Get-ChildItem (Join-Path $versionRoot 'out/logs/release/version_backups') -File)
    if ($backups.Count -ne 1) { throw 'Expected exactly one atomic version replacement backup.' }
    Assert-Equal -Actual ((Get-Content $backups[0].FullName -Raw).Trim()) -Expected '0.9.503' -Message 'Atomic replacement must retain the original bytes.'

    $policyRoot = Join-Path $fixtureDirectory "internal"
    $policyArchive = Join-Path $policyRoot "archive\operations"
    New-Item -ItemType Directory -Path $policyArchive -Force | Out-Null
    $currentName = "public_repo_release_allowlist_2026-08-24.txt"
    $currentPolicy = Join-Path $policyRoot $currentName
    Set-Content -LiteralPath $currentPolicy -Value @("# current", "README.md", "docs/ja/", "docs/en/") -Encoding UTF8
    $diffLines = @(Get-AllowlistDiffLines -CurrentAllowlistName $currentName -CurrentAllowlistPath $currentPolicy)
    if (-not ($diffLines -match "初回または履歴未保持")) {
        throw "A policy without archive history must retain first-policy handling."
    }
    $baselineName = "public_repo_release_allowlist_2026-07-28.txt"
    $baselinePolicy = Join-Path $policyArchive $baselineName
    Set-Content -LiteralPath $baselinePolicy -Value @("# historical", "README.md", "old.md") -Encoding UTF8
    $ignoredDirectory = Join-Path $policyArchive "unrelated"
    New-Item -ItemType Directory -Path $ignoredDirectory | Out-Null
    Set-Content -LiteralPath (Join-Path $ignoredDirectory "public_repo_release_allowlist_2026-08-23.txt") -Value "unexpected.md" -Encoding UTF8
    $diffText = (@(Get-AllowlistDiffLines -CurrentAllowlistName $currentName -CurrentAllowlistPath $currentPolicy)) -join "`n"
    foreach ($expected in @($baselineName, (Get-Sha256 $baselinePolicy), (Get-Sha256 $currentPolicy), "- 追加: 2 件", "- 削除: 1 件", "  - + docs/en/", "  - - old.md")) {
        if (-not $diffText.Contains($expected)) { throw "Missing archive policy comparison evidence: $expected" }
    }
    if ($diffText.Contains("unexpected.md")) { throw "Policy history lookup must not recurse into unrelated trees." }

    $checklistAst = @($publishAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq "New-PairChecklist"
    }, $true))
    if ($checklistAst.Count -ne 1) { throw "Expected exactly one New-PairChecklist function." }
    $global:publishChecklistOutputPath = Join-Path $fixtureDirectory "new_pair_checklist.txt"
    $global:publishChecklistAllowlistPath = $currentPolicy
    function Assert-UnifiedReleasePair { param([object[]]$Items) }
    function Get-PairChecklistPath { param([object[]]$Items) return $global:publishChecklistOutputPath }
    function Assert-ChildPath { param([string]$Root, [string]$RelativePath, [string]$Label) return $global:publishChecklistAllowlistPath }
    function Get-ReleaseTag { param([object]$Manifest) return "v$($Manifest.app_version)" }
    function New-ReleaseNotesText { param([object[]]$Items) return "release notes" }
    function Write-Info { param([string]$Message) }
    function Write-Warn { param([string]$Message) }
    $checklistFunctionPath = Join-Path $fixtureDirectory "new_pair_checklist_function.ps1"
    Set-Content -LiteralPath $checklistFunctionPath -Value $checklistAst[0].Extent.Text -Encoding UTF8
    . $checklistFunctionPath
    $checklistItems = @(
        [PSCustomObject]@{
            SetRoot = "C:\\release\\ja"
            Result = [PSCustomObject]@{
                Manifest = [PSCustomObject]@{ locale = "ja"; app_version = "1.2.3" }
                SnapshotAllowlistName = $currentName
                SnapshotFiles = 1
                SnapshotTreeHash = "tree"
                SnapshotAllowlistHash = "allowlist"
                SnapshotArtifactManifestHash = "artifact"
                FullZipHash = "ja-full"
                LiteZipHash = "ja-lite"
            }
        },
        [PSCustomObject]@{
            SetRoot = "C:\\release\\en"
            Result = [PSCustomObject]@{
                Manifest = [PSCustomObject]@{ locale = "en"; app_version = "1.2.3" }
                SnapshotAllowlistName = $currentName
                SnapshotFiles = 1
                SnapshotTreeHash = "tree"
                SnapshotAllowlistHash = "allowlist"
                SnapshotArtifactManifestHash = "artifact"
                FullZipHash = "en-full"
                LiteZipHash = "en-lite"
            }
        }
    )
    New-PairChecklist -Items $checklistItems -Repository "owner/repository" | Out-Null
    New-PairChecklist -Items $checklistItems -Repository "owner/repository" | Out-Null
    $checklistText = Get-Content -LiteralPath $global:publishChecklistOutputPath -Raw -Encoding UTF8
    if ($checklistText.Contains("System.Object[]")) {
        throw "New pair checklist must write each allowlist-diff line rather than an array placeholder."
    }
    foreach ($expected in @("--- ALLOWLIST DIFF BEGIN ---", "- 追加: 2 件", "  - + docs/en/", "--- ALLOWLIST DIFF END ---")) {
        if (-not $checklistText.Contains($expected)) { throw "New pair checklist is missing allowlist-diff evidence: $expected" }
    }
    if (@([regex]::Matches($checklistText, '(?m)^--- ALLOWLIST DIFF BEGIN ---\r?$')).Count -ne 1) {
        throw "An unconfirmed checklist must retain exactly one allowlist-diff section on repeated Confirm attempts."
    }
}
finally {
    Remove-Variable -Name publishChecklistOutputPath -Scope Global -ErrorAction SilentlyContinue
    Remove-Variable -Name publishChecklistAllowlistPath -Scope Global -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $fixtureDirectory) {
        if (-not ([System.IO.Path]::GetFullPath($fixtureDirectory)).StartsWith(([System.IO.Path]::GetFullPath($fixtureBase) + [System.IO.Path]::DirectorySeparatorChar), [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup path.' }
        Remove-Item -LiteralPath $fixtureDirectory -Recurse -Force
    }
}

Write-Host "Publish version history tests passed."
