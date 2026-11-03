[CmdletBinding()]
param([ValidateSet('ja', 'en')][string]$Locale = 'ja')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outputRoot = Join-Path $repoRoot "out/readonly_viewer_next/$Locale"
$binDir = Join-Path $outputRoot 'bin'
$objDir = Join-Path $outputRoot 'obj'
$generatedDir = Join-Path $outputRoot 'generated'
$lockStream = $null
Push-Location -LiteralPath $repoRoot
try {
    New-Item -ItemType Directory -Force -Path $binDir, $objDir, $generatedDir | Out-Null
    $lockStream = [IO.File]::Open((Join-Path $outputRoot 'build.lock'), 'OpenOrCreate', 'Write', 'None')
    $compiler = (Get-Command g++ -ErrorAction Stop).Source
    $resourceCompiler = (Get-Command windres -ErrorAction Stop).Source
    $inputs = @('third_party/pdfium/bin/pdfium.dll', 'third_party/pdfium/lib/pdfium.dll.lib',
                'third_party/md4c/src/md4c.c', 'src/resources/app.manifest')
    foreach ($inputPath in $inputs) {
        if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { throw "Missing required input: $inputPath" }
    }
    & python tools/dev/prepare_readonly_viewer_next_licenses.py --locale $Locale --output $binDir
    if ($LASTEXITCODE -ne 0) { throw 'Required prototype license files could not be prepared.' }
    & python tools/localization/prepare_locale_catalog.py --source src --ja locales/ja.json --en locales/en.json --locale $Locale --localized "locales/$Locale.json" --output (Join-Path $generatedDir 'locale_catalog.generated.h') --report (Join-Path $generatedDir 'fallback_report.json') --usage-stamp (Join-Path $generatedDir 'usage.stamp.json') --catalog-stamp (Join-Path $generatedDir 'catalog.stamp.json')
    if ($LASTEXITCODE -ne 0) { throw 'Locale preparation failed.' }
    $definition = Get-Content scripts/build/build_sources.json -Raw | ConvertFrom-Json
    $sources = @($definition.ReadOnlyViewerSourceFiles | Where-Object { $_ -ne 'src/readonly_viewer/main.cpp' })
    $sources += 'src/readonly_viewer_next/main.cpp'
    $flags = @('-std=c++17', '-O2', '-DNDEBUG', '-DCLROP_READ_ONLY_BUILD', '-DUNICODE', '-D_UNICODE', '-Wall', '-Wno-unused-parameter',
               '-Ithird_party/pdfium/include', '-Ithird_party/md4c/src', '-Isrc', "-I$generatedDir")
    $flags += if ($Locale -eq 'en') { '-DPDF_NOTE_LOCALE_EN=1' } else { '-DPDF_NOTE_LOCALE_EN=0' }
    $objects = @()
    foreach ($source in $sources) {
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing source: $source" }
        $object = Join-Path $objDir (($source -replace '[/\\.]', '_') + '.o')
        Write-Host "Compile: $source"
        if ($source.EndsWith('.c')) {
            $cFlags = @($flags | Where-Object { $_ -ne '-std=c++17' }) + @('-x', 'c', '-std=c11', '-DMD4C_USE_UTF16')
            & $compiler @cFlags -c $source -o $object
        } else {
            & $compiler @flags -c $source -o $object
        }
        if ($LASTEXITCODE -ne 0) { throw "Compile failed: $source" }
        $objects += $object
    }
    $resource = Join-Path $objDir 'reader_manifest.rc'
    $manifestPath = (Join-Path $repoRoot 'src/resources/app.manifest').Replace('\', '/')
    Set-Content -LiteralPath $resource -Value ('1 24 "' + $manifestPath + '"') -Encoding ASCII
    $resourceObject = Join-Path $objDir 'reader_manifest.o'
    & $resourceCompiler $resource -O coff -o $resourceObject
    if ($LASTEXITCODE -ne 0) { throw 'Manifest compilation failed.' }
    $candidate = Join-Path $binDir 'readonly_viewer_next.candidate.exe'
    $exePath = Join-Path $binDir 'readonly_viewer_next.exe'
    & $compiler -municode -mwindows @objects $resourceObject third_party/pdfium/lib/pdfium.dll.lib -lgdi32 -luser32 -lmsimg32 -lshell32 -lole32 -luuid -lcomctl32 -luxtheme -o $candidate
    if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
    $toolchainDir = Split-Path -Parent $compiler
    $runtime = @('third_party/pdfium/bin/pdfium.dll')
    $runtime += @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll') | ForEach-Object { Join-Path $toolchainDir $_ }
    foreach ($dll in $runtime) {
        if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw "Missing runtime: $dll" }
        $target = Join-Path $binDir (Split-Path -Leaf $dll)
        if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $dll).Hash -ne (Get-FileHash -LiteralPath $target).Hash) {
            Copy-Item -LiteralPath $dll -Destination $target
        }
    }
    if (Test-Path -LiteralPath $exePath -PathType Leaf) {
        [IO.File]::Replace($candidate, $exePath, (Join-Path $binDir 'readonly_viewer_next.previous.exe'))
    } else {
        [IO.File]::Move($candidate, $exePath)
    }
    $info = @{ prototype = 'readonly_viewer_next'; locale = $Locale; version = (Get-Content REPO_VERSION.txt -Raw).Trim(); sha256 = (Get-FileHash -LiteralPath $exePath).Hash; builtAt = (Get-Date).ToString('o') }
    $info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $binDir 'prototype-build.json') -Encoding UTF8
    Write-Host "Built local prototype: $exePath"
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    if ($lockStream) { $lockStream.Dispose() }
    Pop-Location
}
