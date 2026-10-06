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
$functionAst = @(
    $publishAst.FindAll(
        {
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq "Get-PreviousPublishedVersion"
        },
        $true
    )
)
if ($functionAst.Count -ne 1) {
    throw "Expected exactly one Get-PreviousPublishedVersion function in publish.ps1."
}
. ([scriptblock]::Create($functionAst[0].Extent.Text))

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

$fixtureDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("pdf_note_publish_version_history_" + [guid]::NewGuid().ToString("N"))
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

    $selectedVersion = Get-PreviousPublishedVersion -RecordsDirectory $fixtureDirectory
    Assert-Equal -Actual $selectedVersion -Expected "0.9.503" -Message "The latest submitted pair checklist must be selected over legacy and pending records."

    $nonPairFixtures = @(
        Get-ChildItem -LiteralPath $fixtureDirectory -File |
            Where-Object { $_.Name -notlike "*0.9.503*" }
    )
    foreach ($fixture in $nonPairFixtures) {
        Remove-Item -LiteralPath $fixture.FullName
    }
    $selectedVersion = Get-PreviousPublishedVersion -RecordsDirectory $fixtureDirectory
    Assert-Equal -Actual $selectedVersion -Expected "0.9.503" -Message "A current paired checklist must supply its version from its body, not its filename."

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
}
finally {
    if (Test-Path -LiteralPath $fixtureDirectory) {
        Remove-Item -LiteralPath $fixtureDirectory -Recurse -Force
    }
}

Write-Host "Publish version history tests passed."
