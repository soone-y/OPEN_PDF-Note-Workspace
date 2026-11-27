# Shared entry boundary for GUI test runners. Re-entry preserves bound values,
# owns the full child tree and keeps its control visible, including for isolated tests.
function Invoke-AppTestDesktop {
    param([Parameter(Mandatory)][string]$ScriptPath,
          [Parameter(Mandatory)][System.Collections.IDictionary]$Parameters,
          [int]$TimeoutSeconds = 1800, [switch]$UsesClipboard, [switch]$Interactive)
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $launcher = Join-Path $root 'tools/dev/isolated_app_test.py'
    $context = & python -B $launcher --check-context
    if ($LASTEXITCODE -ne 0) { throw 'Cannot verify app-test desktop binding.' }
    if ($context -eq 'interactive') { return $false }
    if ($context -eq 'isolated' -and -not $Interactive -and
        -not $UsesClipboard) { return $false }
    if ($context -ne 'unprotected' -and $context -ne 'isolated') { throw "Unknown app-test execution context: $context" }
    $bound = @{}
    foreach ($entry in $Parameters.GetEnumerator()) {
        $bound[$entry.Key] = if ($entry.Value -is [System.Management.Automation.SwitchParameter]) {
            [bool]$entry.Value
        } else { $entry.Value }
    }
    $serialized = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([Management.Automation.PSSerializer]::Serialize($bound)))
    $literalPath = "'" + $ScriptPath.Replace("'", "''") + "'"
    # Native children inherit these output handles. Use a -File bootstrap:
    # -EncodedCommand emits CLIXML stderr when redirected, even with Text output.
    # The child is Windows PowerShell 5. A PowerShell 7 parent exports its own
    # module paths first, which hides 5.x core modules (e.g. Get-FileHash), so
    # rebuild the 5.x search path from its defaults and the persisted variables.
    $moduleReset = "`$env:PSModulePath=((@([Environment]::GetFolderPath('MyDocuments')+'\WindowsPowerShell\Modules', `$env:ProgramFiles+'\WindowsPowerShell\Modules', `$PSHOME+'\Modules') + ([Environment]::GetEnvironmentVariable('PSModulePath','User') -split ';') + ([Environment]::GetEnvironmentVariable('PSModulePath','Machine') -split ';')) | Where-Object { `$_ } | Select-Object -Unique) -join ';'; "
    $command = $moduleReset + "[Console]::OutputEncoding=[Text.UTF8Encoding]::new(`$false); `$OutputEncoding=[Console]::OutputEncoding; `$ErrorActionPreference='Stop'; try { `$bound=[Management.Automation.PSSerializer]::Deserialize([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$serialized'))); & $literalPath @bound; if (-not `$?) { exit 1 } } catch { [Console]::Error.WriteLine(`$_.ToString()); [Console]::Error.WriteLine(`$_.ScriptStackTrace); exit 1 }"
    $dispatch = Join-Path $root ('out/tests/desktop_launch_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dispatch -ErrorAction Stop | Out-Null
    $bootstrap = Join-Path $dispatch 'bootstrap.ps1'
    [IO.File]::WriteAllText($bootstrap, $command, [Text.UTF8Encoding]::new($true))
    $powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
    $mode = @()
    if ($UsesClipboard) { $mode += '--uses-clipboard' }
    if ($Interactive) { $mode += '--interactive' }
    $previousEncoding = [Console]::OutputEncoding
    $previousErrorAction = $ErrorActionPreference
    # Builds wait while this test uses out/bin; this test waits for a running build.
    . (Join-Path $root 'scripts/build/out_lock.ps1')
    $outLock = Enter-OutLock -OutRoot (Join-Path $root 'out') -Mode Shared -Label ('test ' + [IO.Path]::GetFileName($ScriptPath))
    foreach ($record in @(Get-ChildItem -LiteralPath (Join-Path $root 'out/build_source') -Filter '*.txt' -File -ErrorAction SilentlyContinue)) {
        Write-Host ('Build source: ' + (Get-Content -LiteralPath $record.FullName -TotalCount 1))
    }
    try {
        [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
        $ErrorActionPreference = 'Continue'
        & python -B $launcher --cwd (Get-Location).Path --timeout $TimeoutSeconds --label ([IO.Path]::GetFileName($ScriptPath)) @mode -- $powershell -NoProfile -ExecutionPolicy Bypass -OutputFormat Text -File $bootstrap 2>&1 |
            ForEach-Object {
                if ($_ -is [Management.Automation.ErrorRecord]) { Write-Host $_.Exception.Message }
                else { Write-Host $_.ToString() }
            }
        $ErrorActionPreference = $previousErrorAction
        if ($LASTEXITCODE -eq 130) { exit 130 }
        if ($LASTEXITCODE -ne 0) { throw "Isolated app-test runner failed (exit=$LASTEXITCODE): $ScriptPath" }
    } finally {
        $ErrorActionPreference = $previousErrorAction
        [Console]::OutputEncoding = $previousEncoding
        Exit-OutLock $outLock
    }
    return $true
}
