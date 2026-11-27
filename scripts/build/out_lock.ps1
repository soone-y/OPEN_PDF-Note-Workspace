# Reader/writer lock over out/build.lock for builds and GUI tests that share
# one working tree. Builds take it exclusively (they replace out/bin); GUI test
# runners take it shared, so several tests may run together while a build waits
# until they finish, and a test waits for a running build. File sharing modes
# provide the lock: the OS releases it when a holder exits or crashes.
# Holder notes live in out/locks/ only to explain a wait; they never grant it.

function Get-OutLockHolders {
    param([Parameter(Mandatory)][string]$OutRoot)
    $dir = Join-Path $OutRoot 'locks'
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    $holders = @()
    foreach ($note in Get-ChildItem -LiteralPath $dir -Filter '*.txt' -File -ErrorAction SilentlyContinue) {
        $holderPid = 0
        if (-not [int]::TryParse(($note.BaseName -split '_')[0], [ref]$holderPid)) { continue }
        if (-not (Get-Process -Id $holderPid -ErrorAction SilentlyContinue)) { continue }
        $holders += (Get-Content -LiteralPath $note.FullName -Raw -ErrorAction SilentlyContinue).Trim()
    }
    return $holders
}

function Enter-OutLock {
    param(
        [Parameter(Mandatory)][string]$OutRoot,
        [Parameter(Mandatory)][ValidateSet('Exclusive', 'Shared')][string]$Mode,
        [Parameter(Mandatory)][string]$Label,
        [int]$TimeoutSeconds = 1800
    )
    if (-not (Test-Path -LiteralPath $OutRoot)) {
        New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
    }
    $override = [Environment]::GetEnvironmentVariable('PDF_NOTE_OUT_LOCK_TIMEOUT_SECONDS')
    $parsed = 0
    if ($override -and [int]::TryParse($override, [ref]$parsed) -and $parsed -ge 0) { $TimeoutSeconds = $parsed }
    $lockPath = Join-Path $OutRoot 'build.lock'
    $access = if ($Mode -eq 'Exclusive') { [IO.FileAccess]::Write } else { [IO.FileAccess]::Read }
    $share = if ($Mode -eq 'Exclusive') { [IO.FileShare]::None } else { [IO.FileShare]::Read }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $announced = $false
    while ($true) {
        try {
            $stream = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, $access, $share)
            break
        } catch {
            # Windows PowerShell 5 wraps the sharing violation in MethodInvocationException.
            $sharing = $_.Exception
            while ($sharing -and -not ($sharing -is [System.IO.IOException])) { $sharing = $sharing.InnerException }
            if (-not $sharing) { throw }
            if ([DateTime]::UtcNow -ge $deadline) {
                $holders = @(Get-OutLockHolders -OutRoot $OutRoot)
                $detail = if ($holders.Count) { ' Holders: ' + ($holders -join ' | ') } else { '' }
                throw "Timed out after $TimeoutSeconds s waiting for out/build.lock ($Mode, $Label).$detail"
            }
            if (-not $announced) {
                $holders = @(Get-OutLockHolders -OutRoot $OutRoot)
                $detail = if ($holders.Count) { $holders -join ' | ' } else { 'holder unknown' }
                Write-Host "Waiting for out/build.lock ($Mode, $Label): $detail" -ForegroundColor Yellow
                $announced = $true
            }
            Start-Sleep -Milliseconds 1000
        }
    }
    $notes = Join-Path $OutRoot 'locks'
    New-Item -ItemType Directory -Force -Path $notes | Out-Null
    $notePath = Join-Path $notes ('{0}_{1}.txt' -f $PID, [guid]::NewGuid().ToString('N'))
    $note = '{0} {1} pid={2} since={3}' -f $Mode.ToLowerInvariant(), $Label, $PID, (Get-Date -Format 's')
    try { [IO.File]::WriteAllText($notePath, $note) } catch { $notePath = $null }
    if ($announced) { Write-Host "Acquired out/build.lock ($Mode, $Label)." -ForegroundColor DarkCyan }
    return [pscustomobject]@{ Stream = $stream; NotePath = $notePath }
}

function Exit-OutLock {
    param($Lock)
    if ($null -eq $Lock) { return }
    if ($Lock.NotePath) { Remove-Item -LiteralPath $Lock.NotePath -Force -ErrorAction SilentlyContinue }
    if ($Lock.Stream) { $Lock.Stream.Dispose() }
}

# Records which tree state the binaries in out/bin were built from, outside the
# distributed build-info receipt. Failure to read git never fails a build.
function Write-OutBuildSource {
    param([Parameter(Mandatory)][string]$OutRoot, [Parameter(Mandatory)][string]$RepoRoot,
          [Parameter(Mandatory)][string]$Artifact)
    try {
        $head = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
        $status = @(& git -C $RepoRoot status --porcelain --untracked-files=normal 2>$null)
        $diff = (& git -C $RepoRoot diff HEAD 2>$null) -join "`n"
        $sha = [Security.Cryptography.SHA256]::Create()
        $digest = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($diff + "`n" + ($status -join "`n"))) |
            Select-Object -First 6 | ForEach-Object { $_.ToString('x2') })
        $line = '{0}  artifact={1} head={2} uncommitted_paths={3} tree_digest={4}' -f
            (Get-Date -Format 's'), $Artifact, $(if ($head) { $head } else { 'unknown' }), $status.Count, $digest
        $paths = $status | Select-Object -First 20 | ForEach-Object { '  ' + $_ }
        $dir = Join-Path $OutRoot 'build_source'
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        [IO.File]::WriteAllText((Join-Path $dir ($Artifact + '.txt')), (@($line) + $paths -join "`r`n") + "`r`n")
    } catch {
        Write-Host "Build source record skipped: $($_.Exception.Message)" -ForegroundColor DarkYellow
    }
}
