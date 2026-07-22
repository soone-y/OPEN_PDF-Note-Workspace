[CmdletBinding()]
param(
    [string]$SourceRepo = "",
    [Parameter(Mandatory = $true)]
    [string]$OutputRepo,
    [string]$Allowlist = "",
    [string]$GitignoreTemplate = "",
    [string]$TempRoot = "",
    [string]$PublicBranch = "main",
    [string]$PublicAuthorName = "Public Maintainer",
    [string]$PublicAuthorEmail = "public@example.invalid",
    [switch]$BucketCommitTimes,
    [switch]$PreserveMergeCommits,
    [switch]$ReplaceOutput,
    [switch]$SkipMetadataSanitization,
    [switch]$KeepTemp,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = $PSScriptRoot
if (-not $scriptRoot) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$scriptsRoot = Split-Path -Parent $scriptRoot
$repoRoot = Split-Path -Parent $scriptsRoot

function Write-Info([string]$Message) { Write-Host $Message -ForegroundColor Cyan }
function Write-Warn([string]$Message) { Write-Host $Message -ForegroundColor Yellow }

function Resolve-AbsolutePath([string]$Path, [string]$BasePath) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Path must not be empty."
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Test-IsSameOrChildPath([string]$ParentPath, [string]$CandidatePath) {
    $parentFull = [System.IO.Path]::GetFullPath($ParentPath).TrimEnd('\', '/')
    $candidateFull = [System.IO.Path]::GetFullPath($CandidatePath).TrimEnd('\', '/')
    if ($candidateFull -eq $parentFull) {
        return $true
    }
    return $candidateFull.StartsWith($parentFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-OperationFile([string]$Pattern) {
    $operationsDir = Join-Path $repoRoot "docs\internal\operations"
    $matches = @(Get-ChildItem -LiteralPath $operationsDir -File -Filter $Pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one operations file matching '$Pattern', found $($matches.Count)."
    }
    return $matches[0].FullName
}

function Convert-CharCodesToString([object[]]$Codes) {
    if ($null -eq $Codes -or $Codes.Count -eq 0) {
        return ""
    }
    $chars = New-Object System.Collections.Generic.List[char]
    foreach ($code in $Codes) {
        if ($code -is [System.Array]) {
            foreach ($nested in $code) {
                $chars.Add([char][int]$nested)
            }
            continue
        }
        $chars.Add([char][int]$code)
    }
    return -join $chars.ToArray()
}

function Get-RebucketedLocalDateTime([datetime]$Value) {
    $base = Get-Date -Date $Value -Hour 0 -Minute 0 -Second 0 -Millisecond 0
    $bucketHour = [int]([math]::Floor(($Value.Hour + 1) / 3.0) * 3)
    if ($bucketHour -ge 24) {
        $bucketHour -= 24
        $base = $base.AddDays(1)
    }
    return $base.AddHours($bucketHour)
}

function Invoke-GitCommitWithOptionalBucketedNow {
    param(
        [string]$WorkingDirectory,
        [string[]]$Arguments
    )

    $oldAuthorDate = $env:GIT_AUTHOR_DATE
    $oldCommitterDate = $env:GIT_COMMITTER_DATE
    try {
        if ($BucketCommitTimes) {
            $bucketedNow = Get-RebucketedLocalDateTime -Value (Get-Date)
            $offset = [System.TimeZoneInfo]::Local.GetUtcOffset($bucketedNow)
            $sign = if ($offset.Ticks -lt 0) { "-" } else { "+" }
            $absoluteOffset = if ($offset.Ticks -lt 0) { $offset.Negate() } else { $offset }
            $gitDate = "{0} {1}{2:00}{3:00}" -f $bucketedNow.ToString("yyyy-MM-dd HH:mm:ss"), $sign, $absoluteOffset.Hours, $absoluteOffset.Minutes
            $env:GIT_AUTHOR_DATE = $gitDate
            $env:GIT_COMMITTER_DATE = $gitDate
        }
        Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments $Arguments
    }
    finally {
        if ($null -eq $oldAuthorDate) {
            Remove-Item Env:GIT_AUTHOR_DATE -ErrorAction SilentlyContinue
        }
        else {
            $env:GIT_AUTHOR_DATE = $oldAuthorDate
        }
        if ($null -eq $oldCommitterDate) {
            Remove-Item Env:GIT_COMMITTER_DATE -ErrorAction SilentlyContinue
        }
        else {
            $env:GIT_COMMITTER_DATE = $oldCommitterDate
        }
    }
}

function Remove-DirectoryIfRequested([string]$TargetPath, [string]$SourceRoot) {
    if (-not (Test-Path -LiteralPath $TargetPath)) {
        return
    }
    if (-not $ReplaceOutput) {
        throw "Output directory already exists. Use -ReplaceOutput only after confirming the path is disposable: $TargetPath"
    }
    if (Test-IsSameOrChildPath -ParentPath $SourceRoot -CandidatePath $TargetPath) {
        throw "Refusing to remove a path inside the source repository: $TargetPath"
    }
    if ($DryRun) {
        Write-Info "[dry-run] remove existing output: $TargetPath"
        return
    }
    Remove-Item -LiteralPath $TargetPath -Recurse -Force
}

function Invoke-GitCommand {
    param(
        [string]$WorkingDirectory,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [switch]$CaptureOutput
    )

    if ($DryRun) {
        $renderedArgs = ($Arguments | ForEach-Object {
                if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
            }) -join ' '
        Write-Info ("[dry-run] git -C {0} {1}" -f $WorkingDirectory, $renderedArgs)
        if ($CaptureOutput) {
            return @()
        }
        return
    }

    Push-Location -LiteralPath $WorkingDirectory
    try {
        if ($CaptureOutput) {
            $output = & git @Arguments 2>&1
            if ($LASTEXITCODE -ne 0) {
                throw ("git {0} failed.`n{1}" -f ($Arguments -join ' '), ($output -join "`n"))
            }
            return @($output)
        }

        & git @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw ("git {0} failed." -f ($Arguments -join ' '))
        }
    }
    finally {
        Pop-Location
    }
}

function Read-AllowlistEntries([string]$AllowlistPath) {
    if (-not (Test-Path -LiteralPath $AllowlistPath)) {
        throw "Allowlist file not found: $AllowlistPath"
    }

    $entries = New-Object System.Collections.Generic.List[string]
    foreach ($rawLine in Get-Content -LiteralPath $AllowlistPath -Encoding UTF8) {
        $line = $rawLine.Trim()
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith("#")) {
            continue
        }
        if ($line.StartsWith("/") -or $line.StartsWith([string][char]92)) {
            throw "Allowlist entry must be repository-relative: $line"
        }
        $entries.Add($line.Replace([char]92, [char]47))
    }

    if ($entries.Count -eq 0) {
        throw "Allowlist contains no active entries: $AllowlistPath"
    }

    return $entries
}

function Convert-ToPythonBytesLiteral([string]$Text) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append("b'")
    foreach ($byte in $bytes) {
        [void]$builder.AppendFormat("\x{0:x2}", $byte)
    }
    [void]$builder.Append("'")
    return $builder.ToString()
}

function Build-MessageCallbackCode {
    $pairs = @(
        @{ OldLiteral = "b'\x2f\x63\x3a\x2f\x55\x73\x65\x72\x73\x2f\x73\x6f\x6e\x65\x79\x2f\x50\x44\x46\x2d\x4e\x6f\x74\x65\x2d\x57\x6f\x72\x6b\x73\x70\x61\x63\x65'"; New = "<LOCAL_WORKSPACE>" },
        @{ OldLiteral = "b'\x43\x3a\x2f\x55\x73\x65\x72\x73\x2f\x73\x6f\x6e\x65\x79'"; New = "<LOCAL_USER>" },
        @{ OldLiteral = "b'\x43\x3a\x5c\x55\x73\x65\x72\x73\x5c\x73\x6f\x6e\x65\x79'"; New = "<LOCAL_USER>" },
        @{ OldLiteral = "b'\x73\x6f\x6f\x6e\x65\x2d\x79'"; New = "<ACCOUNT>" },
        @{ OldLiteral = "b'\x73\x6f\x6e\x65\x79'"; New = "<LOCAL_USER>" },
        @{ OldLiteral = "b'\x79\x75\x37\x31\x32'"; New = "<AUTHOR>" },
        @{ OldLiteral = "b'\x32\x31\x31\x39\x32\x33\x34\x33\x38\x2b\x73\x6f\x6f\x6e\x65\x2d\x79\x40\x75\x73\x65\x72\x73\x2e\x6e\x6f\x72\x65\x70\x6c\x79\x2e\x67\x69\x74\x68\x75\x62\x2e\x63\x6f\x6d'"; New = "<AUTHOR_EMAIL>" },
        @{ OldLiteral = "b'\x6c\x6f\x2e\x7a\x69\x70'"; New = "LibreOffice build workspace backup" }
    )

    $callback = "return message"
    foreach ($pair in $pairs) {
        $oldLiteral = $pair.OldLiteral
        $newLiteral = Convert-ToPythonBytesLiteral $pair.New
        $callback += ".replace($oldLiteral, $newLiteral)"
    }
    return $callback
}

function Build-CommitCallbackCode {
    param(
        [switch]$LinearizeMergeCommits,
        [switch]$RebucketCommitTimes,
        [hashtable]$ReparentRootMap,
        [object[]]$RootReparentSpecs
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("import calendar")
    $lines.Add("import datetime")
    $lines.Add("")

    if ($null -ne $RootReparentSpecs -and $RootReparentSpecs.Count -gt 0) {
        foreach ($spec in $RootReparentSpecs) {
            $subjectLiteral = Convert-ToPythonBytesLiteral ([string]$spec.Subject)
            $parentCommit = ([string]$spec.Parent).ToLowerInvariant()
            $lines.Add(("if len(commit.parents) == 0 and commit.message.splitlines() and commit.message.splitlines()[0] == {0}:" -f $subjectLiteral))
            $lines.Add("    commit.parents = [b'$parentCommit']")
            $lines.Add("")
        }
    }

    if ($null -ne $ReparentRootMap -and $ReparentRootMap.Count -gt 0) {
        foreach ($entry in $ReparentRootMap.GetEnumerator()) {
            $rootCommit = $entry.Key.ToLowerInvariant()
            $parentCommit = ([string]$entry.Value).ToLowerInvariant()
            $lines.Add("if commit.original_id == b'$rootCommit':")
            $lines.Add("    commit.parents = [b'$parentCommit']")
            $lines.Add("")
        }
    }

    if ($LinearizeMergeCommits) {
        $lines.Add("if len(commit.parents) > 1:")
        $lines.Add("    commit.parents = commit.parents[:1]")
        $lines.Add("")
    }

    if ($RebucketCommitTimes) {
        $lines.Add("def rebucket_git_date(raw):")
        $lines.Add("    if not raw:")
        $lines.Add("        return raw")
        $lines.Add("    stamp, tz = raw.split(b"" "", 1)")
        $lines.Add("    ts = int(stamp)")
        $lines.Add("    sign = 1 if tz[:1] == b""+"" else -1")
        $lines.Add("    offset_seconds = sign * (int(tz[1:3]) * 3600 + int(tz[3:5]) * 60)")
        $lines.Add("    local_ts = ts + offset_seconds")
        $lines.Add("    local_dt = datetime.datetime.utcfromtimestamp(local_ts)")
        $lines.Add("    bucket_hour = ((local_dt.hour + 1) // 3) * 3")
        $lines.Add("    day_shift = 0")
        $lines.Add("    if bucket_hour >= 24:")
        $lines.Add("        bucket_hour -= 24")
        $lines.Add("        day_shift = 1")
        $lines.Add("    rebucketed_local = datetime.datetime(")
        $lines.Add("        local_dt.year,")
        $lines.Add("        local_dt.month,")
        $lines.Add("        local_dt.day,")
        $lines.Add("        0,")
        $lines.Add("        0,")
        $lines.Add("        0,")
        $lines.Add("    ) + datetime.timedelta(days=day_shift, hours=bucket_hour)")
        $lines.Add("    rebucketed_utc = calendar.timegm(rebucketed_local.timetuple()) - offset_seconds")
        $lines.Add("    return str(rebucketed_utc).encode() + b"" "" + tz")
        $lines.Add("")
        $lines.Add("commit.author_date = rebucket_git_date(commit.author_date)")
        $lines.Add("commit.committer_date = rebucket_git_date(commit.committer_date)")
    }

    if ($lines.Count -le 3) {
        $lines.Add("pass")
    }

    return ($lines -join "`n")
}

function Copy-FileIntoRepo([string]$SourcePath, [string]$DestinationPath) {
    $destDir = Split-Path -Parent $DestinationPath
    if ($destDir -and -not (Test-Path -LiteralPath $destDir)) {
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    }
    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
}

function Write-TempUtf8File([string]$DirectoryPath, [string]$FileName, [string]$Content) {
    $targetPath = Join-Path $DirectoryPath $FileName
    if ($DryRun) {
        Write-Info ("[dry-run] write temp callback file: {0}" -f $targetPath)
        return $targetPath
    }
    if (-not (Test-Path -LiteralPath $DirectoryPath)) {
        New-Item -ItemType Directory -Force -Path $DirectoryPath | Out-Null
    }
    [System.IO.File]::WriteAllText($targetPath, $Content, [System.Text.UTF8Encoding]::new($false))
    return $targetPath
}

function Invoke-GitFilterRepo([string]$WorkingDirectory, [string[]]$AdditionalArgs) {
    Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments ($AdditionalArgs + @("--force"))
}

function Get-GitOutputAllowingNoMatch([string]$WorkingDirectory, [string[]]$Arguments) {
    if ($DryRun) {
        Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments $Arguments -CaptureOutput | Out-Null
        return @()
    }

    Push-Location -LiteralPath $WorkingDirectory
    try {
        $output = & git @Arguments 2>&1
        if ($LASTEXITCODE -gt 1) {
            throw ("git {0} failed.`n{1}" -f ($Arguments -join ' '), ($output -join "`n"))
        }
        return @($output)
    }
    finally {
        Pop-Location
    }
}

function Assert-LicenseHistoryIsSingleRevision([string]$WorkingDirectory, [string]$RelativePath) {
    $commits = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("log", "--format=%H", "--", $RelativePath) -CaptureOutput)
    if ($commits.Count -ne 1) {
        throw "Expected exactly one public-history commit for $RelativePath, found $($commits.Count)."
    }
}

function Assert-NoMergeCommits([string]$WorkingDirectory) {
    $merges = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--all", "--min-parents=2") -CaptureOutput)
    if ($merges.Count -gt 0) {
        throw "Expected linear public history without merge commits, found $($merges.Count).`n$($merges -join "`n")"
    }
}

function Assert-SingleRootCommit([string]$WorkingDirectory) {
    $roots = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--all", "--max-parents=0") -CaptureOutput)
    if ($roots.Count -ne 1) {
        throw "Expected a single connected public-history root, found $($roots.Count).`n$($roots -join "`n")"
    }
}

function Get-SingleCommitBySubject([string]$WorkingDirectory, [string]$Subject) {
    $commits = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("log", "--all", "--format=%H`t%s") -CaptureOutput |
        Where-Object { $_.EndsWith("`t$Subject") } |
        ForEach-Object { ($_ -split "`t", 2)[0] })
    if ($commits.Count -eq 0) {
        return $null
    }
    if ($commits.Count -ne 1) {
        throw "Expected exactly one commit with subject '$Subject', found $($commits.Count)."
    }
    return $commits[0]
}

function Get-SingleRootForCommit([string]$WorkingDirectory, [string]$Commit) {
    $roots = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--max-parents=0", $Commit) -CaptureOutput)
    if ($roots.Count -ne 1) {
        throw "Expected exactly one root reachable from $Commit, found $($roots.Count)."
    }
    return $roots[0]
}

function Get-CommitParents([string]$WorkingDirectory, [string]$Commit) {
    $line = @(
        Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--parents", "-n", "1", $Commit) -CaptureOutput
    )[0]
    $parts = @($line -split ' ' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($parts.Count -le 1) {
        return @()
    }
    return @($parts[1..($parts.Count - 1)])
}

function Get-CommitSubject([string]$WorkingDirectory, [string]$Commit) {
    return @(
        Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("show", "--no-patch", "--format=%s", $Commit) -CaptureOutput
    )[0]
}

function Get-LegacyHistoryReparentSpecs([string]$WorkingDirectory) {
    $attachPdfEditorSubject = "Attach legacy pdf_editor history as provenance"
    $attachPdfNoteSmallSubject = "Attach legacy pdf_note_small history as provenance"
    $attachPdfEditor = Get-SingleCommitBySubject -WorkingDirectory $WorkingDirectory -Subject $attachPdfEditorSubject
    $attachPdfNoteSmall = Get-SingleCommitBySubject -WorkingDirectory $WorkingDirectory -Subject $attachPdfNoteSmallSubject
    if ($null -eq $attachPdfEditor -or $null -eq $attachPdfNoteSmall) {
        return @{}
    }

    $attachPdfEditorParents = @(Get-CommitParents -WorkingDirectory $WorkingDirectory -Commit $attachPdfEditor)
    $attachPdfNoteSmallParents = @(Get-CommitParents -WorkingDirectory $WorkingDirectory -Commit $attachPdfNoteSmall)
    if ($attachPdfEditorParents.Count -lt 2 -or $attachPdfNoteSmallParents.Count -lt 2) {
        Write-Warn "Legacy provenance attach commits are already single-parent after allowlist filtering; skipping explicit legacy-root reparenting."
        return @{}
    }

    $pdfEditorTerminal = $attachPdfEditorParents[1]
    $pdfNoteSmallTerminal = $attachPdfNoteSmallParents[1]
    $pdfEditorRoot = Get-SingleRootForCommit -WorkingDirectory $WorkingDirectory -Commit $pdfEditorTerminal
    $pdfNoteSmallRoot = Get-SingleRootForCommit -WorkingDirectory $WorkingDirectory -Commit $pdfNoteSmallTerminal

    $allRoots = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--all", "--max-parents=0") -CaptureOutput)
    $currentRoots = @($allRoots | Where-Object { $_ -ne $pdfEditorRoot -and $_ -ne $pdfNoteSmallRoot })
    if ($currentRoots.Count -ne 1) {
        throw "Expected exactly one current-repository root after excluding legacy roots, found $($currentRoots.Count)."
    }

    return @(
        @{
            Subject = Get-CommitSubject -WorkingDirectory $WorkingDirectory -Commit $pdfNoteSmallRoot
            Parent = $pdfEditorTerminal
        },
        @{
            Subject = Get-CommitSubject -WorkingDirectory $WorkingDirectory -Commit $currentRoots[0]
            Parent = $pdfNoteSmallTerminal
        }
    )
}

function Get-CommitTimestampMap([string]$WorkingDirectory) {
    $map = @{}
    $lines = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("log", "--all", "--format=%H`t%ct") -CaptureOutput)
    foreach ($line in $lines) {
        $parts = $line -split "`t", 2
        if ($parts.Count -eq 2) {
            $map[$parts[0]] = [long]$parts[1]
        }
    }
    return $map
}

function Get-CommitChildMap([string]$WorkingDirectory) {
    $map = @{}
    $lines = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--children", "--all") -CaptureOutput)
    foreach ($line in $lines) {
        $parts = @($line -split ' ')
        if ($parts.Count -eq 0) {
            continue
        }
        $commit = $parts[0]
        $children = @()
        if ($parts.Count -gt 1) {
            $children = @($parts[1..($parts.Count - 1)] | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        }
        $map[$commit] = $children
    }
    return $map
}

function Get-ComponentTipFromRoot([string]$RootCommit, [hashtable]$ChildMap, [hashtable]$TimestampMap) {
    $visited = New-Object 'System.Collections.Generic.HashSet[string]'
    $queue = New-Object 'System.Collections.Generic.Queue[string]'
    [void]$queue.Enqueue($RootCommit)
    $tip = $RootCommit
    $tipTimestamp = [long]$TimestampMap[$RootCommit]

    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        if (-not $visited.Add($current)) {
            continue
        }

        $currentTimestamp = [long]$TimestampMap[$current]
        if ($currentTimestamp -gt $tipTimestamp) {
            $tip = $current
            $tipTimestamp = $currentTimestamp
        }

        $children = @()
        if ($ChildMap.ContainsKey($current)) {
            $children = @($ChildMap[$current])
        }
        foreach ($child in $children) {
            if (-not $visited.Contains($child)) {
                [void]$queue.Enqueue($child)
            }
        }
    }

    return $tip
}

function Get-SequentialRootReparentMap([string]$WorkingDirectory) {
    $roots = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--all", "--max-parents=0") -CaptureOutput)
    if ($roots.Count -le 1) {
        return @{}
    }

    $timestampMap = Get-CommitTimestampMap -WorkingDirectory $WorkingDirectory
    $childMap = Get-CommitChildMap -WorkingDirectory $WorkingDirectory
    $sortedRoots = @($roots | Sort-Object { [long]$timestampMap[$_] }, { $_ })

    $result = @{}
    for ($index = 1; $index -lt $sortedRoots.Count; $index++) {
        $previousRoot = $sortedRoots[$index - 1]
        $currentRoot = $sortedRoots[$index]
        $previousTip = Get-ComponentTipFromRoot -RootCommit $previousRoot -ChildMap $childMap -TimestampMap $timestampMap
        $result[$currentRoot] = $previousTip
    }

    return $result
}

function Invoke-PublicHistoryScans([string]$WorkingDirectory) {
    $objectPattern = 'Hot_note|examples/reference|examples/fixtures|tests/scratch_fs_test\.exe|docs/internal|\.agents|\.local|out/'
    $privateRegexTerms = @(
        (Convert-CharCodesToString @(115,111,110,101,121)),
        (Convert-CharCodesToString @(115,111,111,110,101,45,121)),
        (Convert-CharCodesToString @(115,111,111,110,101,121)),
        (Convert-CharCodesToString @(115,111,111,110,101,95,121)),
        (Convert-CharCodesToString @(121,117,55,49,50)),
        (Convert-CharCodesToString @(50,49,49,57,50,51,52,51,56)),
        (Convert-CharCodesToString @(85,115,101,114,115,47,115,111,110,101,121)),
        (Convert-CharCodesToString @(85,115,101,114,115,92,92,115,111,110,101,121)),
        (Convert-CharCodesToString @(47,99,58,47,85,115,101,114,115,47,115,111,110,101,121)),
        (Convert-CharCodesToString @(67,58,92,92,85,115,101,114,115,92,92,115,111,110,101,121)),
        (Convert-CharCodesToString @(67,58,47,85,115,101,114,115,47,115,111,110,101,121)),
        (Convert-CharCodesToString @(47,109,110,116,47,99,47,85,115,101,114,115)),
        'lo\.zip'
    )
    $metaPattern = ($privateRegexTerms -join '|')
    $contentPattern = (@(
            (Convert-CharCodesToString @(115,111,110,101,121)),
            (Convert-CharCodesToString @(115,111,111,110,101,45,121)),
            (Convert-CharCodesToString @(115,111,111,110,101,121)),
            (Convert-CharCodesToString @(115,111,111,110,101,95,121)),
            (Convert-CharCodesToString @(121,117,55,49,50)),
            (Convert-CharCodesToString @(50,49,49,57,50,51,52,51,56)),
            (Convert-CharCodesToString @(67,58,92,92,85,115,101,114,115,92,92,115,111,110,101,121)),
            (Convert-CharCodesToString @(67,58,47,85,115,101,114,115,47,115,111,110,101,121)),
            (Convert-CharCodesToString @(47,109,110,116,47,99,47,85,115,101,114,115,47,115,111,110,101,121)),
            'lo\.zip',
            'Hot_note',
            'examples/reference',
            'examples/fixtures'
        ) -join '|')
    $blobPattern = 'source_archives/.*\.tar\.xz|lo\.zip|\.msi$|\.7z$'

    $objectHits = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--objects", "--all") -CaptureOutput | Select-String -Pattern $objectPattern -CaseSensitive:$false)
    if ($objectHits.Count -gt 0) {
        throw "Public history still contains filtered object-name risks.`n$($objectHits | ForEach-Object { $_.Line } | Out-String)"
    }

    $metaHits = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("log", "--all", "--pretty=format:%h`t%H`t%an`t%ae`t%cn`t%ce`t%s", "--date=short") -CaptureOutput | Select-String -Pattern $metaPattern -CaseSensitive:$false)
    if ($metaHits.Count -gt 0) {
        throw "Public history metadata scan found private strings.`n$($metaHits | ForEach-Object { $_.Line } | Out-String)"
    }

    $contentHits = @(Get-GitOutputAllowingNoMatch -WorkingDirectory $WorkingDirectory -Arguments @(
            "grep", "-n", "-I", "-E", $contentPattern, "--",
            ".",
            ":(exclude)scripts/release/create_public_history.ps1"
        ))
    if ($contentHits.Count -gt 0) {
        throw "Tracked content scan found private strings.`n$($contentHits -join "`n")"
    }

    $blobHits = @(Invoke-GitCommand -WorkingDirectory $WorkingDirectory -Arguments @("rev-list", "--objects", "--all") -CaptureOutput | Select-String -Pattern $blobPattern -CaseSensitive:$false)
    if ($blobHits.Count -gt 0) {
        throw "Public history still contains prohibited archive/blob names.`n$($blobHits | ForEach-Object { $_.Line } | Out-String)"
    }

    Assert-LicenseHistoryIsSingleRevision -WorkingDirectory $WorkingDirectory -RelativePath "LICENSE.md"
    Assert-LicenseHistoryIsSingleRevision -WorkingDirectory $WorkingDirectory -RelativePath "LICENSES_INDEX.md"
    Assert-LicenseHistoryIsSingleRevision -WorkingDirectory $WorkingDirectory -RelativePath "THIRD_PARTY_NOTICES.md"
    if (-not $PreserveMergeCommits) {
        Assert-NoMergeCommits -WorkingDirectory $WorkingDirectory
    }
    Assert-SingleRootCommit -WorkingDirectory $WorkingDirectory
}

$sourceRepoPath = if ([string]::IsNullOrWhiteSpace($SourceRepo)) { $repoRoot } else { Resolve-AbsolutePath -Path $SourceRepo -BasePath $repoRoot }
$allowlistPath = if ([string]::IsNullOrWhiteSpace($Allowlist)) {
    Resolve-OperationFile -Pattern "public_repo_demo*.txt"
}
else {
    Resolve-AbsolutePath -Path $Allowlist -BasePath $repoRoot
}
$gitignoreTemplatePath = if ([string]::IsNullOrWhiteSpace($GitignoreTemplate)) {
    Resolve-OperationFile -Pattern "public_repo_gitignore*.gitignore"
}
else {
    Resolve-AbsolutePath -Path $GitignoreTemplate -BasePath $repoRoot
}
$tempRootPath = if ([string]::IsNullOrWhiteSpace($TempRoot)) {
    Join-Path $repoRoot ".local\repo_resource\tmp"
}
else {
    Resolve-AbsolutePath -Path $TempRoot -BasePath $repoRoot
}
$outputRepoPath = Resolve-AbsolutePath -Path $OutputRepo -BasePath $repoRoot

if (-not (Test-Path -LiteralPath $sourceRepoPath)) {
    throw "Source repository path not found: $sourceRepoPath"
}
if (-not (Test-Path -LiteralPath (Join-Path $sourceRepoPath ".git"))) {
    throw "Source path is not a Git repository: $sourceRepoPath"
}
if (Test-IsSameOrChildPath -ParentPath $sourceRepoPath -CandidatePath $outputRepoPath) {
    throw "Output repository must be outside the source repository: $outputRepoPath"
}
if (-not (Test-Path -LiteralPath $gitignoreTemplatePath)) {
    throw "Gitignore template not found: $gitignoreTemplatePath"
}

$gitFilterRepoVersion = & git filter-repo --version 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "git filter-repo is required. Install git-filter-repo before running this script."
}

$allowlistEntries = Read-AllowlistEntries -AllowlistPath $allowlistPath
$licenseDocs = @("LICENSE.md", "LICENSES_INDEX.md", "THIRD_PARTY_NOTICES.md")

$pathFilterArgs = New-Object System.Collections.Generic.List[string]
foreach ($entry in $allowlistEntries) {
    if ($licenseDocs -contains $entry) {
        continue
    }
    if ($entry.Contains("*")) {
        $pathFilterArgs.Add("--path-glob")
        $pathFilterArgs.Add($entry)
        continue
    }
    $pathFilterArgs.Add("--path")
    if ($entry.EndsWith("/")) {
        $pathFilterArgs.Add($entry.TrimEnd("/") + "/")
    }
    else {
        $pathFilterArgs.Add($entry)
    }
}

$timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss_fff") + "_" + [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
$tempRepoPath = Join-Path $tempRootPath ("public_history_build_" + $timestamp)

Write-Info "Source repository: $sourceRepoPath"
Write-Info "Output repository: $outputRepoPath"
Write-Info "Disposable clone: $tempRepoPath"
Write-Info "Allowlist: $allowlistPath"
Write-Info "git-filter-repo: $gitFilterRepoVersion"

if (Test-Path -LiteralPath $outputRepoPath) {
    Remove-DirectoryIfRequested -TargetPath $outputRepoPath -SourceRoot $sourceRepoPath
}
elseif ($DryRun) {
    Write-Info "[dry-run] output path is absent and will be created: $outputRepoPath"
}

if (Test-Path -LiteralPath $tempRepoPath) {
    throw "Temporary path already exists unexpectedly: $tempRepoPath"
}

$movedToOutput = $false

try {
    if ($DryRun) {
        Write-Info "[dry-run] mkdir: $tempRootPath"
        Write-Info "[dry-run] git clone --no-local `"$sourceRepoPath`" `"$tempRepoPath`""
    }
    else {
        New-Item -ItemType Directory -Force -Path $tempRootPath | Out-Null
        & git clone --no-local $sourceRepoPath $tempRepoPath
        if ($LASTEXITCODE -ne 0) {
            throw "git clone failed."
        }
    }

    $remoteNames = Invoke-GitCommand -WorkingDirectory $tempRepoPath -Arguments @("remote") -CaptureOutput
    foreach ($remoteName in $remoteNames) {
        if (-not [string]::IsNullOrWhiteSpace($remoteName)) {
            Invoke-GitCommand -WorkingDirectory $tempRepoPath -Arguments @("remote", "remove", $remoteName.Trim())
        }
    }

    Write-Info "Filtering history to allowlisted public paths."
    $initialFilterArgs = @("filter-repo") + @($pathFilterArgs)
    Invoke-GitFilterRepo -WorkingDirectory $tempRepoPath -AdditionalArgs $initialFilterArgs

    if (-not $SkipMetadataSanitization) {
        Write-Info "Sanitizing commit metadata and risky message fragments."
        $nameCallback = "return " + (Convert-ToPythonBytesLiteral $PublicAuthorName)
        $emailCallback = "return " + (Convert-ToPythonBytesLiteral $PublicAuthorEmail)
        $messageCallback = Build-MessageCallbackCode
        Invoke-GitFilterRepo -WorkingDirectory $tempRepoPath -AdditionalArgs @(
            "filter-repo",
            "--name-callback", $nameCallback,
            "--email-callback", $emailCallback,
            "--message-callback", $messageCallback
        )
    }

    $rootReparentSpecs = @()
    if (-not $PreserveMergeCommits) {
        $rootReparentSpecs = Get-LegacyHistoryReparentSpecs -WorkingDirectory $tempRepoPath
        if ($rootReparentSpecs.Count -gt 0) {
            Write-Info "Reparenting legacy roots into pdf_editor -> pdf_note_small -> current repository order."
            $commitCallback = Build-CommitCallbackCode -RootReparentSpecs $rootReparentSpecs
            $callbackDir = if ($DryRun) { $tempRootPath } else { $tempRepoPath }
            $commitCallbackPath = Write-TempUtf8File -DirectoryPath $callbackDir -FileName "public_history_commit_callback.py" -Content $commitCallback
            Invoke-GitFilterRepo -WorkingDirectory $tempRepoPath -AdditionalArgs @(
                "filter-repo",
                "--commit-callback", $commitCallbackPath
            )
            if (-not $DryRun -and (Test-Path -LiteralPath $commitCallbackPath)) {
                Remove-Item -LiteralPath $commitCallbackPath -Force
            }
        }
    }

    if ((-not $PreserveMergeCommits) -or $BucketCommitTimes) {
        if (-not $PreserveMergeCommits -and $BucketCommitTimes) {
            Write-Info "Linearizing merge commits to first-parent history and rebucketing timestamps."
        }
        elseif (-not $PreserveMergeCommits) {
            Write-Info "Linearizing merge commits to first-parent history for the public repository."
        }
        else {
            Write-Info "Rebucketing author and committer timestamps into 3-hour local-time buckets."
        }
        $commitCallback = Build-CommitCallbackCode -LinearizeMergeCommits:(-not $PreserveMergeCommits) -RebucketCommitTimes:$BucketCommitTimes -ReparentRootMap @{} -RootReparentSpecs @()
        $callbackDir = if ($DryRun) { $tempRootPath } else { $tempRepoPath }
        $commitCallbackPath = Write-TempUtf8File -DirectoryPath $callbackDir -FileName "public_history_commit_callback.py" -Content $commitCallback
        Invoke-GitFilterRepo -WorkingDirectory $tempRepoPath -AdditionalArgs @(
            "filter-repo",
            "--commit-callback", $commitCallbackPath
        )
        if (-not $DryRun -and (Test-Path -LiteralPath $commitCallbackPath)) {
            Remove-Item -LiteralPath $commitCallbackPath -Force
        }
    }

    $reparentRootMap = @{}
    if (-not $PreserveMergeCommits) {
        $reparentRootMap = Get-SequentialRootReparentMap -WorkingDirectory $tempRepoPath
        if ($reparentRootMap.Count -gt 0) {
            Write-Info "Connecting remaining disconnected roots after merge linearization."
            $commitCallback = Build-CommitCallbackCode -ReparentRootMap $reparentRootMap -RootReparentSpecs @()
            $callbackDir = if ($DryRun) { $tempRootPath } else { $tempRepoPath }
            $commitCallbackPath = Write-TempUtf8File -DirectoryPath $callbackDir -FileName "public_history_commit_callback.py" -Content $commitCallback
            Invoke-GitFilterRepo -WorkingDirectory $tempRepoPath -AdditionalArgs @(
                "filter-repo",
                "--commit-callback", $commitCallbackPath
            )
            if (-not $DryRun -and (Test-Path -LiteralPath $commitCallbackPath)) {
                Remove-Item -LiteralPath $commitCallbackPath -Force
            }
        }
    }

    if ($DryRun) {
        Write-Info "[dry-run] copy public .gitignore template into rewritten repository root"
        foreach ($licenseDoc in $licenseDocs) {
            Write-Info ("[dry-run] copy latest reviewed license doc: {0}" -f $licenseDoc)
        }
        Write-Info ("[dry-run] git branch -M {0}" -f $PublicBranch)
        Write-Info "[dry-run] git add .gitignore LICENSE.md LICENSES_INDEX.md THIRD_PARTY_NOTICES.md"
        Write-Info "[dry-run] git commit -m `"Add public repository metadata snapshots`""
        Write-Info "[dry-run] run public-history scans"
        Write-Info ("[dry-run] move disposable clone to output: {0}" -f $outputRepoPath)
        return
    }

    Copy-FileIntoRepo -SourcePath $gitignoreTemplatePath -DestinationPath (Join-Path $tempRepoPath ".gitignore")
    foreach ($licenseDoc in $licenseDocs) {
        Copy-FileIntoRepo -SourcePath (Join-Path $sourceRepoPath $licenseDoc) -DestinationPath (Join-Path $tempRepoPath $licenseDoc)
    }

    Invoke-GitCommand -WorkingDirectory $tempRepoPath -Arguments @("branch", "-M", $PublicBranch)
    Invoke-GitCommand -WorkingDirectory $tempRepoPath -Arguments @("config", "user.name", $PublicAuthorName)
    Invoke-GitCommand -WorkingDirectory $tempRepoPath -Arguments @("config", "user.email", $PublicAuthorEmail)
    Invoke-GitCommand -WorkingDirectory $tempRepoPath -Arguments @("add", ".gitignore", "LICENSE.md", "LICENSES_INDEX.md", "THIRD_PARTY_NOTICES.md")
    Invoke-GitCommitWithOptionalBucketedNow -WorkingDirectory $tempRepoPath -Arguments @("commit", "-m", "Add public repository metadata snapshots")

    Write-Info "Running public-history verification scans."
    Invoke-PublicHistoryScans -WorkingDirectory $tempRepoPath

    $pdfList = Invoke-GitCommand -WorkingDirectory $tempRepoPath -Arguments @("ls-files", "*.pdf") -CaptureOutput
    if ($pdfList.Count -gt 0) {
        Write-Warn "Tracked PDF paths in public history:"
        $pdfList | ForEach-Object { Write-Warn ("  " + $_) }
    }

    $outputParent = Split-Path -Parent $outputRepoPath
    if (-not (Test-Path -LiteralPath $outputParent)) {
        New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
    }
    Move-Item -LiteralPath $tempRepoPath -Destination $outputRepoPath
    $movedToOutput = $true

    Write-Info "Public-history repository created successfully."
    Write-Info "Next checks:"
    Write-Info ("  git -C `"{0}`" status -sb" -f $outputRepoPath)
    Write-Info ("  git -C `"{0}`" log --oneline --decorate -n 10" -f $outputRepoPath)
    Write-Info ("  git -C `"{0}`" remote -v" -f $outputRepoPath)
}
catch {
    Write-Warn $_.Exception.Message
    if (-not $DryRun -and (Test-Path -LiteralPath $tempRepoPath)) {
        Write-Warn "Disposable clone was kept for inspection:"
        Write-Warn "  $tempRepoPath"
    }
    throw
}
finally {
    if (-not $DryRun -and -not $movedToOutput -and -not $KeepTemp) {
        # On failure the disposable clone is intentionally kept for inspection.
    }
}
