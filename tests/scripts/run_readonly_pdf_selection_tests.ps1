[CmdletBinding()]
param([ValidateSet('ja','en')][string]$Locale = 'ja')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$run = Join-Path $repoRoot ('out/tests/readonly_pdf_selection/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$exe = Join-Path $run 'readonly_pdf_region_selection_tests.exe'
$fixture = Join-Path $run 'selection.pdf'
$compiler = Get-Command g++ -ErrorAction Stop
$generated = Join-Path $repoRoot ('out/generated_locale/' + $Locale)
if (-not (Test-Path -LiteralPath (Join-Path $generated 'locale_catalog.generated.h'))) { throw 'Explicit locale build required first.' }
Push-Location $repoRoot
try {
    & $compiler.Source -std=c++17 -O1 -municode -DCLROP_READ_ONLY_BUILD -DMD4C_USE_UTF16 `
        "-DPDF_NOTE_LOCALE_EN=$([int]($Locale -eq 'en'))" -Isrc -Ithird_party/pdfium/include "-I$generated" `
        tests/unit/readonly_pdf_region_selection_tests.cpp src/readonly_viewer/pdf_region_selection.cpp `
        src/core/localization.cpp src/theme/built_in_theme.cpp src/clrop/json.cpp `
        third_party/pdfium/lib/pdfium.dll.lib -lgdi32 -luser32 -lmsimg32 -lshell32 -lole32 -luuid -lcomctl32 -luxtheme -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Reader selection test compilation failed.' }
    $bin = Join-Path $repoRoot $(if ($Locale -eq 'en') { 'out/bin_en' } else { 'out/bin' })
    foreach ($name in @('pdfium.dll','libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $bin $name) -Destination (Join-Path $run $name)
    }
    & python -c "import fitz,sys; d=fitz.open(); p=d.new_page(width=595,height=842); p.insert_text((100,110),'ALPHA BETA GAMMA',fontsize=16); p.insert_text((100,150),'SECOND LINE',fontsize=16); p.insert_text((330,110),'OUTSIDE',fontsize=16); p.insert_text((100,190),'\u65e5\u672c\u8a9e\u3067\u9078\u629e',fontname='japan',fontsize=16); d.save(sys.argv[1]); d.close()" $fixture
    if ($LASTEXITCODE -ne 0) { throw 'Selection PDF fixture preparation failed.' }
    $before = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    & $exe $fixture
    if ($LASTEXITCODE -ne 0) { throw 'Reader selection tests failed.' }
    if ((Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash -ne $before) { throw 'Reader changed its original PDF fixture.' }
    @{ locale=$Locale; originals_unchanged=$true; clipboard_api_sink=$true; os_clipboard_tested=$false; foreground_mouse_tested=$false } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding UTF8
    Write-Host "PASS: locale=$Locale; original unchanged; clipboard API sink; OS clipboard not tested; $run"
} finally { Pop-Location }
