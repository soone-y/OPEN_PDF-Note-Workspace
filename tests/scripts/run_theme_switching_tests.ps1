[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

$fontList = Get-SourceText "src/core/font_list.h"
Require-Text $fontList "selected ? theme.selectionBg : theme.panelBg" "font combo selection must use the active theme"
Require-Text $fontList "selected ? theme.selectionText : theme.panelText" "font combo selection text must use the active theme"
Forbid-Text $fontList "GetSysColor((dis->itemState & ODS_SELECTED)" "font combo must not fall back to system selection colors"

$main = Get-SourceText "src/main.cpp"
Require-Text $main "bg = g_theme.selectionBg;" "left-pane selected rows must use selectionBg"
Require-Text $main "text = g_theme.selectionText;" "left-pane selected rows must use selectionText"
Require-Text $main 'add(L"selectionBg", g_theme.selectionBg);' "read-only viewer launch must forward selectionBg"
Require-Text $main 'add(L"selectionText", g_theme.selectionText);' "read-only viewer launch must forward selectionText"

$palette = Get-SourceText "src/settings/settings_palette.cppinc"
Require-Text $palette "borderColor = g_theme.accent;" "selected palette slot must use the theme accent"

$readonlyHeader = Get-SourceText "src/readonly_viewer/pdf_preview_panel.h"
Require-Text $readonlyHeader "ThemeColors PdfPreviewPanel_GetTheme();" "read-only viewer must expose its active theme to owner-draw controls"

$readonlyPanel = Get-SourceText "src/readonly_viewer/pdf_preview_panel.cpp"
Require-Text $readonlyPanel 'else if (key == L"selectionBg") theme.selectionBg = value;' "read-only viewer must accept inline selectionBg"
Require-Text $readonlyPanel 'else if (key == L"selectionText") theme.selectionText = value;' "read-only viewer must accept inline selectionText"
Require-Text $readonlyPanel "ThemeColors PdfPreviewPanel_GetTheme()" "read-only viewer theme accessor must be implemented"

$readonlyMain = Get-SourceText "src/readonly_viewer/main.cpp"
Require-Text $readonlyMain "readonly_viewer::PdfPreviewPanel_GetTheme()" "read-only owner-draw tabs must read the active theme"
Require-Text $readonlyMain "isSelected ? theme.selectionBg : theme.toolbarBg" "read-only selected tabs must use selectionBg"
Require-Text $readonlyMain "isSelected ? theme.selectionText : theme.toolbarText" "read-only selected tab text must use selectionText"
Forbid-Text $readonlyMain "RGB(255, 255, 255) : RGB(220, 220, 220)" "read-only tabs must not use fixed selected-tab colors"

$automation = Get-SourceText "src/features/automation/main_ui_automation.cppinc"
Require-Text $automation "RunUiAutomationThemeSwitchScenario" "UI automation must exercise theme switching"
Require-Text $automation "ThemeBrushMatchesColor" "theme automation must inspect recreated theme brushes"
Require-Text $automation "automation:theme_switch_ok" "theme automation must report a successful switch"

Write-Host "Theme switching source tests passed." -ForegroundColor Green
