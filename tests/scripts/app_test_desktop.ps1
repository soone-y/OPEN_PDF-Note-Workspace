# Shared entry boundary for GUI test runners. Re-entry preserves bound values,
# owns the full child tree and shows a passive notice before using the input desktop.
function Invoke-AppTestDesktop {
    param([Parameter(Mandatory)][string]$ScriptPath,
          [Parameter(Mandatory)][System.Collections.IDictionary]$Parameters,
          [int]$TimeoutSeconds = 1800, [switch]$PrivateClipboard, [switch]$Interactive)
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $launcher = Join-Path $root 'tools/dev/isolated_app_test.py'
    $context = & python -B $launcher --check-context
    if ($LASTEXITCODE -ne 0) { throw 'Cannot verify app-test desktop binding.' }
    if ($context -eq 'interactive') { return $false }
    if ($context -eq 'isolated' -and -not $Interactive -and
        (-not $PrivateClipboard -or $env:PDF_NOTE_TEST_STATION -ne 'WinSta0')) { return $false }
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
    $command = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new(`$false); `$OutputEncoding=[Console]::OutputEncoding; `$ErrorActionPreference='Stop'; try { `$bound=[Management.Automation.PSSerializer]::Deserialize([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$serialized'))); & $literalPath @bound; if (-not `$?) { exit 1 } } catch { [Console]::Error.WriteLine(`$_.ToString()); [Console]::Error.WriteLine(`$_.ScriptStackTrace); exit 1 }"
    $dispatch = Join-Path $root ('out/tests/desktop_launch_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dispatch -ErrorAction Stop | Out-Null
    $bootstrap = Join-Path $dispatch 'bootstrap.ps1'
    [IO.File]::WriteAllText($bootstrap, $command, [Text.UTF8Encoding]::new($true))
    $powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
    $mode = @()
    if ($PrivateClipboard) { $mode += '--private-clipboard' }
    if ($Interactive) { $mode += '--interactive' }
    $previousEncoding = [Console]::OutputEncoding
    $previousErrorAction = $ErrorActionPreference
    try {
        [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
        $ErrorActionPreference = 'Continue'
        & python -B $launcher --cwd (Get-Location).Path --timeout $TimeoutSeconds @mode -- $powershell -NoProfile -ExecutionPolicy Bypass -OutputFormat Text -File $bootstrap 2>&1 |
            ForEach-Object {
                if ($_ -is [Management.Automation.ErrorRecord]) { Write-Host $_.Exception.Message }
                else { Write-Host $_.ToString() }
            }
        $ErrorActionPreference = $previousErrorAction
        if ($LASTEXITCODE -ne 0) { throw "Isolated app-test runner failed (exit=$LASTEXITCODE): $ScriptPath" }
    } finally {
        $ErrorActionPreference = $previousErrorAction
        [Console]::OutputEncoding = $previousEncoding
    }
    return $true
}
