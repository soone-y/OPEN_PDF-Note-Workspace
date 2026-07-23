[CmdletBinding()]
param(
    [string]$Soffice = "",
    [int]$TimeoutSec = 240,
    [switch]$KeepLibreOfficeSmoke
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot "out\tests"
$src = Join-Path $repoRoot "tests\unit\docx_space_protection_tests.cpp"
$exe = Join-Path $outDir "docx_space_protection_tests.exe"
$fixtureDir = Join-Path $repoRoot "tests\fixtures\office_conversion"
$fixture = (Get-ChildItem -LiteralPath $fixtureDir -Filter "*.docx" -File | Select-Object -First 1).FullName
if (-not $fixture) {
    throw "DOCX fixture was not found: $fixtureDir"
}
$asciiFixture = Join-Path $outDir "docx_space_protection_source.docx"
$staged = Join-Path $outDir "docx_space_protection_staged.docx"
$storedFixture = Join-Path $outDir "docx_space_protection_stored.docx"
$storedStaged = Join-Path $outDir "docx_space_protection_stored_staged.docx"
$fontFixture = Join-Path $outDir "docx_space_protection_fonts.docx"
$fontStaged = Join-Path $outDir "docx_space_protection_fonts_staged.docx"
$invalidFixture = Join-Path $outDir "docx_space_protection_invalid.docx"
$zip64LikeFixture = Join-Path $outDir "docx_space_protection_zip64_like.docx"
$oversizedExpansionFixture = Join-Path $outDir "docx_space_protection_oversized_expansion.docx"
$externalRelationshipFixture = Join-Path $outDir "docx_space_protection_external_relationship.docx"
$conversionInputDir = Join-Path $outDir "docx_space_protection_conversion_input"

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    throw "g++ not found in PATH."
}
$compilerDir = Split-Path -Parent $compiler.Source

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function New-DocxFixture {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][System.IO.Compression.CompressionLevel]$CompressionLevel
    )

    $zip = [System.IO.Compression.ZipFile]::Open($Path, "Create")
    try {
        $entry = $zip.CreateEntry("word/document.xml", $CompressionLevel)
        $stream = $entry.Open()
        try {
            $wordNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
            $textStr = 'sample' + [char]0x3000 + [char]0x5C0F + [char]0x30C6 + [char]0x30B9 + [char]0x30C8
            $xmlStr = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="' + $wordNs + '"><w:body><w:p><w:r><w:t>' + $textStr + '</w:t></w:r></w:p></w:body></w:document>'
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($xmlStr)
            $stream.Write($bytes, 0, $bytes.Length)
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $zip.Dispose()
    }
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Remove-Item -LiteralPath $asciiFixture -Force -ErrorAction SilentlyContinue
New-DocxFixture -Path $asciiFixture -CompressionLevel Optimal
Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $storedFixture -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $storedStaged -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $fontFixture -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $fontStaged -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $invalidFixture -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $zip64LikeFixture -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $oversizedExpansionFixture -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $externalRelationshipFixture -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $conversionInputDir -Recurse -Force -ErrorAction SilentlyContinue

$args = @(
    "-std=gnu++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-municode",
    "-Isrc",
    $src,
    "src/office/docx_space_protection.cpp",
    "-lgdi32",
    "-luser32",
    "-lz",
    "-o",
    $exe
)

Push-Location -LiteralPath $repoRoot
try {
    Write-Host "Compiling DOCX space protection tests..." -ForegroundColor Cyan
    & $compiler.Source @args
    if ($LASTEXITCODE -ne 0) {
        throw "compile failed"
    }

    Write-Host "Running DOCX space protection tests..." -ForegroundColor Cyan
    function Invoke-DocxProtectionExe {
        param(
            [Parameter(Mandatory)][string]$Source,
            [Parameter(Mandatory)][string]$Dest,
            [switch]$SuppressErrorOutput
        )
        $oldPath = $env:PATH
        try {
            $env:PATH = "$compilerDir;$oldPath"
            if ($SuppressErrorOutput) {
                & $exe $Source $Dest "--quiet" *> $null
            }
            else {
                & $exe $Source $Dest | ForEach-Object { Write-Host $_ }
            }
            return $LASTEXITCODE
        }
        finally {
            $env:PATH = $oldPath
        }
    }

    if ((Invoke-DocxProtectionExe -Source $asciiFixture -Dest $staged) -ne 0) {
        throw "test executable failed"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    function Read-DocxEntryText {
        param(
            [Parameter(Mandatory)][string]$Path,
            [Parameter(Mandatory)][string]$EntryName
        )
        $zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
        try {
            $entry = $zip.GetEntry($EntryName)
            if ($null -eq $entry) {
                throw "$EntryName was not found in DOCX: $Path"
            }
            $reader = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
            try {
                return $reader.ReadToEnd()
            }
            finally {
                $reader.Dispose()
            }
        }
        finally {
            $zip.Dispose()
        }
    }

    $zip = [System.IO.Compression.ZipFile]::OpenRead($staged)
    try {
        $entry = $zip.GetEntry("word/document.xml")
        if ($null -eq $entry) {
            throw "word/document.xml was not found in staged DOCX"
        }
        $reader = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
        try {
            $xml = $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $zip.Dispose()
    }

    if (-not $xml.Contains([string][char]0x2060)) {
        throw "staged DOCX does not contain U+2060 WORD JOINER"
    }

    $sourceZip = [System.IO.Compression.ZipFile]::OpenRead($asciiFixture)
    try {
        $sourceEntry = $sourceZip.GetEntry("word/document.xml")
        $sourceReader = New-Object System.IO.StreamReader($sourceEntry.Open(), [System.Text.Encoding]::UTF8)
        try {
            $sourceXml = $sourceReader.ReadToEnd()
        }
        finally {
            $sourceReader.Dispose()
        }
    }
    finally {
        $sourceZip.Dispose()
    }

    if ($sourceXml.Contains([string][char]0x2060)) {
        throw "source DOCX was unexpectedly modified"
    }

    Write-Host "Checking DOCX ZIP compatibility boundaries..." -ForegroundColor Cyan
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    function New-DocxFixture {
        param(
            [Parameter(Mandatory)][string]$Path,
            [Parameter(Mandatory)][System.IO.Compression.CompressionLevel]$CompressionLevel
        )

        $zip = [System.IO.Compression.ZipFile]::Open($Path, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            $entry = $zip.CreateEntry("word/document.xml", $CompressionLevel)
            $writer = New-Object System.IO.StreamWriter($entry.Open(), [System.Text.Encoding]::UTF8)
            try {
                $wordNs = "http" + "://schemas.openxmlformats.org/wordprocessingml/2006/main"
                $writer.Write(('<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="{0}"><w:body><w:p><w:r><w:t>sample　小テスト</w:t></w:r></w:p></w:body></w:document>' -f $wordNs))
            }
            finally {
                $writer.Dispose()
            }
        }
        finally {
            $zip.Dispose()
        }
    }

    function New-FontDocxFixture {
        param(
            [Parameter(Mandatory)][string]$Path
        )

        $wordNs = "http" + "://schemas.openxmlformats.org/wordprocessingml/2006/main"
        $drawingNs = "http" + "://schemas.openxmlformats.org/drawingml/2006/main"
        $zip = [System.IO.Compression.ZipFile]::Open($Path, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            $entries = @(
                @{
                    Name = "word/document.xml"
                    Text = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:document xmlns:w="{0}"><w:body><w:p><w:r><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="MS Gothic"/><w:sz w:val="21"/><w:szCs w:val="22"/></w:rPr><w:t>sample　小テスト</w:t></w:r></w:p></w:body></w:document>' -f $wordNs)
                },
                @{
                    Name = "word/styles.xml"
                    Text = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:styles xmlns:w="{0}"><w:style w:type="paragraph" w:styleId="Normal"><w:rPr><w:rFonts w:ascii="Cambria" w:hAnsi="Times New Roman"/><w:sz w:val="24"/></w:rPr></w:style></w:styles>' -f $wordNs)
                },
                @{
                    Name = "word/fontTable.xml"
                    Text = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:fonts xmlns:w="{0}"><w:font w:name="Courier New"/></w:fonts>' -f $wordNs)
                },
                @{
                    Name = "word/theme/theme1.xml"
                    Text = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?><a:theme xmlns:a="{0}"><a:themeElements><a:fontScheme name="Office"><a:majorFont><a:latin typeface="Arial"/></a:majorFont></a:fontScheme></a:themeElements></a:theme>' -f $drawingNs)
                }
            )
            foreach ($item in $entries) {
                $entry = $zip.CreateEntry($item.Name, [System.IO.Compression.CompressionLevel]::Optimal)
                $writer = New-Object System.IO.StreamWriter($entry.Open(), [System.Text.Encoding]::UTF8)
                try {
                    $writer.Write($item.Text)
                }
                finally {
                    $writer.Dispose()
                }
            }
        }
        finally {
            $zip.Dispose()
        }
    }

    function Add-ExternalRelationshipToDocx {
        param(
            [Parameter(Mandatory)][string]$Path
        )
        $zip = [System.IO.Compression.ZipFile]::Open($Path, [System.IO.Compression.ZipArchiveMode]::Update)
        try {
            $entry = $zip.CreateEntry("word/_rels/document.xml.rels", [System.IO.Compression.CompressionLevel]::Optimal)
            $writer = New-Object System.IO.StreamWriter($entry.Open(), [System.Text.Encoding]::UTF8)
            try {
                $relsNs = "http" + "://schemas.openxmlformats.org/package/2006/relationships"
                $externalTarget = "https" + "://example.invalid/resource"
                $writer.Write(('<Relationships xmlns="{0}"><Relationship Id="rIdExternal" Type="urn:test" Target="{1}" TargetMode="External"/></Relationships>' -f $relsNs, $externalTarget))
            }
            finally {
                $writer.Dispose()
            }
        }
        finally {
            $zip.Dispose()
        }
    }

    function Test-InstalledFontFamily {
        param(
            [Parameter(Mandatory)][string]$Name
        )
        try {
            Add-Type -AssemblyName System.Drawing
            $collection = New-Object System.Drawing.Text.InstalledFontCollection
            try {
                foreach ($family in $collection.Families) {
                    if ([string]::Equals($family.Name, $Name, [System.StringComparison]::OrdinalIgnoreCase)) {
                        return $true
                    }
                }
            }
            finally {
                $collection.Dispose()
            }
        }
        catch {
            return $false
        }
        return $false
    }

    function Get-ExpectedFontAfterStaging {
        param(
            [Parameter(Mandatory)][string]$Original,
            [Parameter(Mandatory)][string]$Fallback
        )
        if (Test-InstalledFontFamily -Name $Original) {
            return $Original
        }
        return $Fallback
    }

    function Invoke-DocxProtectionExpectSuccess {
        param(
            [Parameter(Mandatory)][string]$Source,
            [Parameter(Mandatory)][string]$Dest,
            [Parameter(Mandatory)][string]$Label
        )
        $exitCode = Invoke-DocxProtectionExe -Source $Source -Dest $Dest
        if ($exitCode -ne 0) {
            throw "$Label should have succeeded"
        }
        if (-not (Test-Path -LiteralPath $Dest)) {
            throw "$Label did not produce staged DOCX"
        }
    }

    function Invoke-DocxProtectionExpectFailure {
        param(
            [Parameter(Mandatory)][string]$Source,
            [Parameter(Mandatory)][string]$Dest,
            [Parameter(Mandatory)][string]$Label
        )
        Remove-Item -LiteralPath $Dest -Force -ErrorAction SilentlyContinue
        $exitCode = Invoke-DocxProtectionExe -Source $Source -Dest $Dest -SuppressErrorOutput
        if ($exitCode -eq 0) {
            throw "$Label should have failed"
        }
        if (Test-Path -LiteralPath $Dest) {
            throw "$Label produced an output file despite failure"
        }
    }

    New-DocxFixture -Path $storedFixture -CompressionLevel ([System.IO.Compression.CompressionLevel]::NoCompression)
    Invoke-DocxProtectionExpectSuccess -Source $storedFixture -Dest $storedStaged -Label "stored DOCX"

    New-FontDocxFixture -Path $fontFixture
    Invoke-DocxProtectionExpectSuccess -Source $fontFixture -Dest $fontStaged -Label "font handling DOCX"
    $fontDocumentXml = Read-DocxEntryText -Path $fontStaged -EntryName "word/document.xml"
    $fontStylesXml = Read-DocxEntryText -Path $fontStaged -EntryName "word/styles.xml"
    $fontTableXml = Read-DocxEntryText -Path $fontStaged -EntryName "word/fontTable.xml"
    $fontThemeXml = Read-DocxEntryText -Path $fontStaged -EntryName "word/theme/theme1.xml"
    $expectedCalibri = Get-ExpectedFontAfterStaging -Original "Calibri" -Fallback "Carlito"
    $expectedCambria = Get-ExpectedFontAfterStaging -Original "Cambria" -Fallback "Caladea"
    $expectedTimes = Get-ExpectedFontAfterStaging -Original "Times New Roman" -Fallback "Liberation Serif"
    $expectedCourier = Get-ExpectedFontAfterStaging -Original "Courier New" -Fallback "Liberation Mono"
    $expectedArial = Get-ExpectedFontAfterStaging -Original "Arial" -Fallback "Liberation Sans"
    if (-not $fontDocumentXml.Contains(('w:ascii="{0}"' -f $expectedCalibri)) -or
        -not $fontDocumentXml.Contains(('w:hAnsi="{0}"' -f $expectedCalibri)) -or
        -not $fontDocumentXml.Contains('w:eastAsia="MS Gothic"')) {
        throw "staged DOCX document.xml did not preserve available fonts or apply expected fallback while preserving Japanese font"
    }
    if (-not $fontDocumentXml.Contains('w:sz w:val="21"') -or
        -not $fontDocumentXml.Contains('w:szCs w:val="22"')) {
        throw "staged DOCX document.xml did not preserve explicit font sizes"
    }
    if (-not $fontStylesXml.Contains(('w:ascii="{0}"' -f $expectedCambria)) -or
        -not $fontStylesXml.Contains(('w:hAnsi="{0}"' -f $expectedTimes))) {
        throw "staged DOCX styles.xml did not preserve available fonts or apply expected fallback"
    }
    if (-not $fontStylesXml.Contains('w:sz w:val="24"')) {
        throw "staged DOCX styles.xml did not preserve style font size"
    }
    if (-not $fontTableXml.Contains(('w:name="{0}"' -f $expectedCourier))) {
        throw "staged DOCX fontTable.xml did not preserve available font or apply expected fallback"
    }
    if (-not $fontThemeXml.Contains(('typeface="{0}"' -f $expectedArial))) {
        throw "staged DOCX theme XML did not preserve available font or apply expected fallback"
    }
    $sourceFontDocumentXml = Read-DocxEntryText -Path $fontFixture -EntryName "word/document.xml"
    if (-not $sourceFontDocumentXml.Contains('w:ascii="Calibri"') -or
        $sourceFontDocumentXml.Contains('w:ascii="Carlito"')) {
        throw "source font fixture was unexpectedly modified"
    }

    [System.IO.File]::WriteAllBytes($invalidFixture, [byte[]](0x50, 0x4b, 0x03, 0x04, 0x00, 0x00))
    Invoke-DocxProtectionExpectFailure -Source $invalidFixture -Dest (Join-Path $outDir "docx_space_protection_invalid_out.docx") -Label "truncated ZIP"

    $zip64Bytes = [System.IO.File]::ReadAllBytes($asciiFixture)
    for ($i = $zip64Bytes.Length - 22; $i -ge 0; --$i) {
        if ($zip64Bytes[$i] -eq 0x50 -and $zip64Bytes[$i + 1] -eq 0x4b -and $zip64Bytes[$i + 2] -eq 0x05 -and $zip64Bytes[$i + 3] -eq 0x06) {
            $zip64Bytes[$i + 10] = 0xff
            $zip64Bytes[$i + 11] = 0xff
            break
        }
    }
    [System.IO.File]::WriteAllBytes($zip64LikeFixture, $zip64Bytes)
    Invoke-DocxProtectionExpectFailure -Source $zip64LikeFixture -Dest (Join-Path $outDir "docx_space_protection_zip64_like_out.docx") -Label "ZIP64-like DOCX"

    $oversizedBytes = [System.IO.File]::ReadAllBytes($asciiFixture)
    $patchedOversizedEntry = $false
    for ($i = 0; $i -le $oversizedBytes.Length - 46; ++$i) {
        if ($oversizedBytes[$i] -ne 0x50 -or $oversizedBytes[$i + 1] -ne 0x4b -or
            $oversizedBytes[$i + 2] -ne 0x01 -or $oversizedBytes[$i + 3] -ne 0x02) {
            continue
        }
        $nameLength = $oversizedBytes[$i + 28] -bor ($oversizedBytes[$i + 29] -shl 8)
        if ($i + 46 + $nameLength -gt $oversizedBytes.Length) { continue }
        $name = [System.Text.Encoding]::UTF8.GetString($oversizedBytes, $i + 46, $nameLength)
        if ($name -ne "word/document.xml") { continue }
        $declaredSize = 64MB + 1
        for ($byte = 0; $byte -lt 4; ++$byte) {
            $oversizedBytes[$i + 24 + $byte] = [byte](($declaredSize -shr (8 * $byte)) -band 0xff)
        }
        $patchedOversizedEntry = $true
        break
    }
    if (-not $patchedOversizedEntry) {
        throw "word/document.xml central-directory entry was not found"
    }
    [System.IO.File]::WriteAllBytes($oversizedExpansionFixture, $oversizedBytes)
    Invoke-DocxProtectionExpectFailure -Source $oversizedExpansionFixture -Dest (Join-Path $outDir "docx_space_protection_oversized_expansion_out.docx") -Label "oversized Word XML expansion"

    Copy-Item -LiteralPath $storedFixture -Destination $externalRelationshipFixture -Force
    Add-ExternalRelationshipToDocx -Path $externalRelationshipFixture
    Invoke-DocxProtectionExpectFailure -Source $externalRelationshipFixture -Dest (Join-Path $outDir "docx_space_protection_external_relationship_out.docx") -Label "external Office relationship"

    if (-not [string]::IsNullOrWhiteSpace($Soffice)) {
        $resolvedSoffice = if ([System.IO.Path]::IsPathRooted($Soffice)) { $Soffice } else { Join-Path $repoRoot $Soffice }
        if (-not (Test-Path -LiteralPath $resolvedSoffice)) {
            throw "LibreOffice soffice.com not found: $resolvedSoffice"
        }
        $pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
        if ([string]::IsNullOrWhiteSpace($pythonExe)) {
            throw "python.exe not found in PATH."
        }
        $smokeTool = Join-Path $repoRoot "tools\libreoffice\libreoffice_smoke_test.py"
        if (-not (Test-Path -LiteralPath $smokeTool)) {
            throw "Missing tool: $smokeTool"
        }
        New-Item -ItemType Directory -Force -Path $conversionInputDir | Out-Null
        Copy-Item -LiteralPath $staged -Destination (Join-Path $conversionInputDir "app_staged.docx") -Force
        $pptxFixture = (Get-ChildItem -LiteralPath $fixtureDir -Filter "*.pptx" -File | Select-Object -First 1).FullName
        if (-not $pptxFixture) {
            throw "PPTX fixture was not found: $fixtureDir"
        }
        Copy-Item -LiteralPath $pptxFixture -Destination (Join-Path $conversionInputDir "sample.pptx") -Force

        $smokeArgs = @(
            $smokeTool,
            "--repo-root", $repoRoot,
            "--soffice", $resolvedSoffice,
            "--input-dir", $conversionInputDir,
            "--require-docx",
            "--require-pptx",
            "--timeout", "$TimeoutSec",
            "--docx-space-protection", "off"
        )
        if ($KeepLibreOfficeSmoke) {
            $smokeArgs += "--keep"
        }
        Write-Host "Running LibreOffice conversion with app-staged DOCX..." -ForegroundColor Cyan
        & $pythonExe @smokeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "app-staged LibreOffice smoke conversion failed"
        }
    }

    Write-Host "All DOCX space protection tests passed." -ForegroundColor Green
}
finally {
    Pop-Location
}
