[CmdletBinding()]
param([switch]$NativeButtons)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'app_test_desktop.ps1')
if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }


$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Get-SourceText {
    param([Parameter(Mandatory)][string]$RelativePath)

    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing source file: $RelativePath"
    }
    return Get-Content -LiteralPath $path -Raw -Encoding UTF8
}

function Require-Text {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$Needle,
        [Parameter(Mandatory)][string]$Description
    )

    if (-not $Text.Contains($Needle)) {
        throw "Theme-switch regression: $Description (missing: $Needle)"
    }
}

function Forbid-Text {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$Needle,
        [Parameter(Mandatory)][string]$Description
    )

    if ($Text.Contains($Needle)) {
        throw "Theme-switch regression: $Description (forbidden: $Needle)"
    }
}

$themeTypes = Get-SourceText "src/core/theme_types.h"
foreach ($field in @("selectionBg", "selectionText", "accent", "buttonHot", "buttonPressed")) {
    Require-Text $themeTypes $field "ThemeColors must expose $field"
}

$core = Get-SourceText "src/core/app_core.cpp"
Require-Text $core "BroadcastThemeChangedToThreadWindows(owner);" "theme changes must refresh open dialogs"
Require-Text $core "bg = g_theme.selectionBg;" "checked owner-draw buttons must use selectionBg"
Require-Text $core "text = g_theme.selectionText;" "checked owner-draw buttons must use selectionText"
Require-Text $core "ColorVisionThemeFromLegacyToneVariant" "legacy color-vision tendencies must migrate to themes"
Require-Text $core "cfg.colorTone = colorVisionTheme;" "legacy color-vision tendencies must select their theme"
Require-Text $core "!isColorVisionTheme" "color-vision themes must not be transformed by a tendency"
$buttonTheme = Get-SourceText "src/ui/themed_push_buttons.h"
Require-Text $buttonTheme "if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) return true;" "shared drawing must preserve native toggle and dedicated button types"
Require-Text $buttonTheme "message == WM_DRAWITEM" "shared theme registration must also own drawing dispatch"
Require-Text $buttonTheme "button->parent == parent && state->draw(item)" "only managed buttons may bypass the parent's dedicated renderer"
Require-Text $buttonTheme "message == WM_GETDLGCODE" "themed buttons must retain keyboard roles"
Require-Text $buttonTheme "message == BM_SETSTYLE" "dialog focus changes must retain themed drawing"
Require-Text $buttonTheme "ReleaseParent(state->parent);" "button restoration/destruction must release parent routing"
Forbid-Text $buttonTheme "GetWindowSubclass(" "the shared component must not add the unavailable legacy comctl32 entry point"
Require-Text $core "ui::themed_push_buttons::Apply(root, g_config.ownerDrawUi, DrawThemeButton)" "application theme state and renderer must reach the shared component"
$dialogTheme = $core.Substring($core.IndexOf("void ApplyThemeToDialog(HWND hWnd)"))
$dialogTheme = $dialogTheme.Substring(0, $dialogTheme.IndexOf("namespace {"))
Require-Text $dialogTheme "ApplyThemeToButtons(hWnd);" "dialogs must use shared registration and drawing"
Forbid-Text $core "EnumChildOwnerDrawButtons" "dialogs must not keep a separate conversion path"

$builtInThemes = Get-SourceText "src/theme/built_in_theme.cpp"
foreach ($theme in @("cvd_red_green", "cvd_blue_yellow", "cvd_monochrome")) {
    Require-Text $builtInThemes $theme "color-vision option must be a built-in theme"
}

$generalControls = Get-SourceText "src/settings/settings_general_controls.cppinc"
Require-Text $generalControls "IsColorVisionThemeSelection" "settings must recognize color-vision themes"
Require-Text $generalControls "EnableWindow(ctx->cbToneVariant, isColorVisionTheme ? FALSE : TRUE);" "color tendency must be disabled for color-vision themes"
Require-Text $generalControls 'g_config.toneVariant = L"pure";' "color-vision themes must persist without a color tendency"
Forbid-Text $generalControls 'L"cvd_red_green", L"cvd_blue_yellow"' "color-vision themes must not be offered as color tendencies"

$generalDialog = Get-SourceText "src/settings/settings_general_dialog.cppinc"
Require-Text $generalDialog "addCombo(IDC_TONE_VARIANT" "color-vision profile selection must use a compact list"
Forbid-Text $generalDialog "addRadio(IDC_TONE_VARIANT" "color-vision profile selection must not use radio buttons"

$paletteEditor = Get-SourceText "src/settings/settings_palette.cppinc"
Require-Text $paletteEditor "IDC_PALETTE_EDITOR_RED" "palette editor must retain numeric RGB input"
Require-Text $paletteEditor "IDC_PALETTE_EDITOR_HEX" "palette editor must retain hexadecimal input"
Forbid-Text $paletteEditor "IDC_PALETTE_EDITOR_SV" "palette editor must not offer a click-and-drag color canvas"
Require-Text $paletteEditor "IDC_PALETTE_EDITOR_EYEDROPPER" "palette editor must offer one-shot screen color sampling"
Require-Text $paletteEditor "case WM_LBUTTONUP:" "eyedropper must pick a color only after the next click"
Forbid-Text $paletteEditor "case WM_MOUSEMOVE:" "eyedropper must not continuously sample cursor movement"

$fontList = Get-SourceText "src/core/font_list.h"
Require-Text $fontList "selected ? theme.selectionBg : theme.panelBg" "font combo selection must use the active theme"
Require-Text $fontList "selected ? theme.selectionText : theme.panelText" "font combo selection text must use the active theme"
Forbid-Text $fontList "GetSysColor((dis->itemState & ODS_SELECTED)" "font combo must not fall back to system selection colors"

$main = Get-SourceText "src/main.cpp"
Require-Text $main "bg = g_theme.selectionBg;" "left-pane selected rows must use selectionBg"
Require-Text $main "text = g_theme.selectionText;" "left-pane selected rows must use selectionText"
$applyMain = $main.Substring($main.IndexOf("void ApplyOwnerDrawUi(HWND hWnd) {"))
$applyMain = $applyMain.Substring(0, $applyMain.IndexOf("void EnsureOwnerDrawUi"))
Require-Text $applyMain "ApplyThemeToButtons(hWnd);" "main-window push buttons must use the same shared traversal"
$layout = Get-SourceText "src/ui/core/main_view_layout.cppinc"
foreach ($match in [regex]::Matches($applyMain, 'EnableOwnerDrawButton\((g_h\w+)\);')) {
    $name = $match.Groups[1].Value
    $creation = [regex]::Match($layout, '(?s)\b' + [regex]::Escape($name) + '\s*=\s*CreateWindowExW\(.*?;')
    if (-not $creation.Success -or $creation.Value -notmatch 'BS_(AUTO)?(CHECKBOX|RADIOBUTTON)') {
        throw "Ordinary or unclassified button must use shared theme traversal instead of a manual list: $name"
    }
}

$palette = Get-SourceText "src/settings/settings_palette.cppinc"
Require-Text $palette "borderColor = g_theme.accent;" "selected palette slot must use the theme accent"

$readonlyMain = Get-SourceText "src/readonly_viewer/main.cpp"
Require-Text $readonlyMain 'argument == L"--theme-inline") {' "legacy inline theme argument must be skipped"
Require-Text $readonlyMain 'if (index + 1 < argc) ++index;' "legacy theme value must not become an input path"
Require-Text $readonlyMain 'reader_next::DrawTab(' "read-only tabs must use reader chrome"
Forbid-Text $readonlyMain 'readonly_viewer::PdfPreviewPanel_GetTheme()' "read-only tabs must not inherit the PDF panel theme"
$readerChromeHeader = Get-SourceText "src/readonly_viewer/reader_chrome.h"
Require-Text $readerChromeHeader 'constexpr COLORREF kPaper = RGB(' "read-only reader chrome must use a fixed paper color"
Require-Text $readerChromeHeader 'constexpr COLORREF kSelection = RGB(' "read-only reader chrome must use a fixed selection color"
$readerChrome = Get-SourceText "src/readonly_viewer/reader_chrome.cpp"
Require-Text $readerChrome 'selected ? kPaper : RGB(207, 217, 227)' "read-only tab drawing must use fixed reader colors"

$automation = Get-SourceText "src/features/automation/main_ui_automation.cppinc"
Require-Text $automation "RunUiAutomationThemeSwitchScenario" "UI automation must exercise theme switching"
Require-Text $automation "ThemeBrushMatchesColor" "theme automation must inspect recreated theme brushes"
Require-Text $automation "automation:theme_switch_ok" "theme automation must report a successful switch"

Write-Host "Theme switching source tests passed." -ForegroundColor Green

# Optional focused native contract check. The fixture never shows a window or
# acquires focus; no application build, user workspace or physical input is used.
if ($NativeButtons) {
    $outputDir = Join-Path $repoRoot ('out/tests/theme_push_buttons_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $outputDir | Out-Null
    $testExe = Join-Path $outputDir 'themed_push_buttons_tests.exe'
    $compileLog = Join-Path $outputDir 'compile.log'
    & g++ -std=c++17 -O2 -Wall -Wextra -Werror -static -I (Join-Path $repoRoot 'src') `
        (Join-Path $repoRoot 'tests/unit/themed_push_buttons_tests.cpp') -lcomctl32 -lgdi32 -o $testExe *> $compileLog
    if ($LASTEXITCODE -ne 0) { throw "Button contract compilation failed. Evidence: $outputDir" }
    $process = Start-Process -FilePath $testExe -WorkingDirectory $outputDir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $outputDir 'stdout.log') -RedirectStandardError (Join-Path $outputDir 'stderr.log')
    $ownedHandle = $process.Handle
    if (-not $process.WaitForExit(30000)) {
        $process.Kill(); $process.WaitForExit()
        throw "Button contract timed out; only the owned fixture was stopped. Evidence: $outputDir"
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        throw ('Button contract failed (exit ' + $process.ExitCode + '). Evidence: ' + $outputDir + ': ' + (Get-Content (Join-Path $outputDir 'stderr.log') -Raw))
    }
    $result = Get-Content (Join-Path $outputDir 'stdout.log') -Raw
    if ($result -notmatch '(?m)^Themed push buttons PASS: \d+ checks;') { throw "Missing button contract result. Evidence: $outputDir" }
    Write-Output $result.TrimEnd()
    Write-Host "Button contract evidence: $outputDir"
}
