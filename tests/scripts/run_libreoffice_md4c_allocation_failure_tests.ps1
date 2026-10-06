[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Md4cSource
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourcePath = (Resolve-Path -LiteralPath $Md4cSource -ErrorAction Stop).Path
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf) -or
    [IO.Path]::GetFileName($sourcePath) -ne 'md4c.c') {
    throw 'Md4cSource must name the actual LibreOffice unpacked md4c.c'
}
$compiler = Get-Command gcc -ErrorAction Stop
$outDir = Join-Path $repoRoot 'out/tests'
$exe = Join-Path $outDir 'libreoffice_md4c_allocation_failure_tests.exe'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$sourceDefine = '-DMD4C_TEST_SOURCE="' + $sourcePath.Replace('\', '/') + '"'
Push-Location -LiteralPath $repoRoot
try {
    # LibreOffice builds the default UTF-8 mode, unlike the app's UTF-16 copy.
    & $compiler.Source -std=c99 -O2 -Wall -Wextra -DMD4C_TEST_LIBREOFFICE `
        $sourceDefine tests/unit/md4c_allocation_failure_tests.c -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'LibreOffice MD4C allocation test compile failed' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'LibreOffice MD4C allocation cleanup regression failed' }
}
finally { Pop-Location }
