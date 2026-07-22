// file: settings/settings_core_api.cpp

#include "settings/settings.h"

#include "core/app_core.h"
#include "core/font_list.h"
#include "file_output/file_output.h"
#include "math/math_render.h"
#include "note_view/note_view.h"

#include <algorithm>

void ApplyNoteFont() {
    double pt = std::clamp(g_noteFontPt > 0.0 ? g_noteFontPt : 10.0, 6.0, 32.0);
    g_noteFontPt = pt;
    if (g_config.noteFontCustomization == 1) {
        std::wstring unifiedFace = g_noteRenderFontName.empty() ? g_noteFontName : g_noteRenderFontName;
        if (unifiedFace.empty()) unifiedFace = GetDefaultFontFaceName();
        g_noteFontName = unifiedFace;
        g_noteRenderFontName = unifiedFace;
        g_noteRenderJpFontName = unifiedFace;
        g_config.noteFontName = unifiedFace;
        g_config.noteRenderFontName = unifiedFace;
        g_config.noteRenderJpFontName = unifiedFace;
    }
    if (g_noteFontName.empty()) g_noteFontName = GetDefaultFontFaceName();
    HFONT rawFont = CreateFontFromFaceName(g_noteFontName, pt);
    if (rawFont) {
        HFONT old = g_hNoteFont;
        g_hNoteFont = rawFont;
        if (old) DeleteObject(old);
    }
    g_noteRenderFontPt = g_noteFontPt;
    std::wstring renderFace = g_noteRenderFontName;
    if (g_config.noteFontCustomization == 0) {
        renderFace = L"Segoe UI";
    } else if (renderFace.empty()) {
        renderFace = g_noteFontName;
    }
    HFONT renderFont = CreateFontFromFaceName(renderFace, g_noteRenderFontPt);
    if (renderFont) {
        HFONT old = g_hNoteRenderFont;
        g_hNoteRenderFont = renderFont;
        if (old) DeleteObject(old);
    }
    if (g_hNoteEdit) {
        HFONT active = (g_noteRenderEnabled && !g_noteRawOnly && g_hNoteRenderFont)
                           ? g_hNoteRenderFont
                           : g_hNoteFont;
        if (!active) active = g_hUIFont;
        SendMessageW(g_hNoteEdit, WM_SETFONT, reinterpret_cast<WPARAM>(active), TRUE);
        InvalidateRect(g_hNoteEdit, nullptr, TRUE);
        // Ensure markup caches reflect font-size-dependent tags like <s+10> / <s=-5>.
        RecomputeMathFromNote();
        return;
    }
    UpdateNoteLineSpacing();
}

void ApplyNoteSystem(HWND hWnd) {
    g_noteSystem = NoteSystem::Legacy;
    g_noteRenderEnabled = g_config.noteRenderEnabled;
    g_noteRawOnly = g_config.noteRawOnly;
    g_noteRenderMath = (g_noteRenderEnabled && !g_noteRawOnly && g_config.noteRenderMath);
    g_config.noteRenderMath = g_noteRenderMath;
    g_noteWrapEnabled = g_config.noteWrapEnabled;
    g_noteVimModeEnabled = g_config.noteVimModeEnabled;
    g_noteVimCaretLineRawTextVisible = g_config.noteVimCaretLineRawTextVisible;
    g_noteVimClickEntersInsertMode = g_config.noteVimClickEntersInsertMode;
    g_noteGridEnabled = g_config.noteGridEnabled;
    g_noteGridPitch = g_config.noteGridPitch;
    g_noteBgColor = g_config.noteBgColor;
    g_noteFgColor = g_config.noteFgColor;
    mathrender::SetSupSubGapSupPercent(g_config.noteMathSupSubGapSupPercent);
    if (!g_noteVimModeEnabled && g_noteNormalMode) {
        g_noteNormalMode = false;
        OnExitNoteNormalMode();
    }
    if (g_hNoteEdit) {
        RecomputeMathFromNote();
        UpdateNoteViewMode();
        InvalidateRect(g_hNoteEdit, nullptr, TRUE);
    }
}

void UpdateMathListVisibility() {
    const bool showAnnot = g_showMathList;
    if (g_hAnnotShow) ShowWindow(g_hAnnotShow, showAnnot ? SW_SHOW : SW_HIDE);
    if (g_hAnnotSettings) ShowWindow(g_hAnnotSettings, showAnnot ? SW_SHOW : SW_HIDE);
    if (g_hAnnotClear) ShowWindow(g_hAnnotClear, showAnnot ? SW_SHOW : SW_HIDE);
    if (g_hAnnotList) ShowWindow(g_hAnnotList, showAnnot ? SW_SHOW : SW_HIDE);
    if (g_hAnnotSummary) ShowWindow(g_hAnnotSummary, SW_HIDE);
    if (showAnnot) RefreshAnnotPanel();
}

void UpdateAutoSaveTimer(HWND hWnd) {
    file_output::ConfigureAutoStageSaveScheduling(hWnd);
}

void UpdateAutoIntegrateTimer(HWND hWnd) {
    file_output::ConfigureAutoIntegrateScheduling(hWnd);
}

namespace {

static void SetClientEdge(HWND hWnd, bool enable) {
    if (!hWnd) return;
    LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    LONG_PTR next = enable ? (ex | WS_EX_CLIENTEDGE) : (ex & ~WS_EX_CLIENTEDGE);
    if (next == ex) return;
    SetWindowLongPtrW(hWnd, GWL_EXSTYLE, next);
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

} // namespace

void ApplyBottomPaneEdgeStyle() {
    bool extendLegacyNote = (g_bottomPanePin == BottomPanePin::Note &&
                             g_bottomNoteMode == BottomNoteMode::Legacy);
    bool extendMath = (g_bottomPanePin == BottomPanePin::Math);
    bool extend = extendLegacyNote || extendMath;
    SetClientEdge(g_hBottomNote, !extend);
    SetClientEdge(g_hBottomMath, !extend);
}

void UpdateBottomPaneMenuChecks() {
    auto currentCommand = []() -> int {
        if (g_bottomPanePin == BottomPanePin::Math) return ID_VIEW_BOTTOM_MATH;
        switch (g_bottomNoteMode) {
        case BottomNoteMode::Headings: return ID_VIEW_BOTTOM_HEADINGS;
        case BottomNoteMode::Assist: return ID_VIEW_BOTTOM_ASSIST;
        default: return ID_VIEW_BOTTOM_NOTE;
        }
    };
    int checked = currentCommand();
    auto update = [&](HMENU menu) {
        if (!menu) return;
        CheckMenuRadioItem(menu, ID_VIEW_BOTTOM_NOTE, ID_VIEW_BOTTOM_ASSIST,
                           checked, MF_BYCOMMAND);
    };
    update(g_hBottomPaneMenu);
    update(g_hBottomPaneMenuSettings);
}

void UpdateScrollDirectionMenuChecks() {
    if (!g_hScrollDirectionMenu) return;

    int checked = ID_VIEW_SCROLL_DIR_V_TTB;
    if (g_config.pdfFlowMode == L"v_btu") {
        checked = ID_VIEW_SCROLL_DIR_V_BTU;
    } else if (g_config.pdfFlowMode == L"h_rtl") {
        checked = ID_VIEW_SCROLL_DIR_H_RTL;
    } else if (g_config.pdfFlowMode == L"h_ltr") {
        checked = ID_VIEW_SCROLL_DIR_H_LTR;
    }
    CheckMenuRadioItem(g_hScrollDirectionMenu,
                       ID_VIEW_SCROLL_DIR_V_TTB, ID_VIEW_SCROLL_DIR_H_LTR,
                       checked, MF_BYCOMMAND);
}

void UpdatePdfSinglePageModeMenuCheck() {
    HWND owner = g_hMainWnd;
    HMENU menu = owner ? GetMenu(owner) : g_hMainMenu;
    if (!menu) return;
    CheckMenuItem(menu, ID_VIEW_PDF_SINGLE_PAGE_MODE,
                  MF_BYCOMMAND | (g_config.pdfSinglePageMode ? MF_CHECKED : MF_UNCHECKED));
}
